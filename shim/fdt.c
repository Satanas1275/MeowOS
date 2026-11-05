/*
 * Just enough flattened-device-tree handling to find and patch what ABL put in
 * /chosen (initrd location, bootargs). Big-endian, 4-byte aligned tokens.
 *
 * The structure block is: FDT_BEGIN_NODE <name> properties... FDT_END_NODE.
 * A property is: FDT_PROP <len> <nameoff> <value...>.
 *
 * Adding a property rebuilds the blob in place: the property goes right before
 * the /chosen FDT_END_NODE, the tail of the structure block shifts up, and the
 * property name is appended to the strings block (which dtc puts last).
 */
#include "fdt.h"
#include <string.h>

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static void wr32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = __builtin_bswap32(v);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

/*
 * Walk to the /chosen node. Returns the offset of its first token (the
 * FDT_BEGIN_NODE) and, if end is not null, the offset of its FDT_END_NODE.
 */
static int find_chosen(uintptr_t fdt, uintptr_t *node, uintptr_t *end)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t stop = p + be32(fdt + 36);
	int depth = 0;

	while (p < stop) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			if (depth == 2 && streq(name, "chosen") && node)
				*node = p - 4;
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (depth == 2 && end && node && *node && !*end)
				*end = p - 4;
			depth--;
			break;
		case FDT_PROP:
			p = align4(p + 8 + be32(p));
			break;
		case FDT_NOP:
			break;
		case FDT_END:
			return node && *node;
		default:
			return 0;
		}
	}
	return node && *node;
}

static uint32_t find_string(uintptr_t fdt, const char *name, uint32_t *len_out)
{
	uintptr_t strings = fdt + be32(fdt + 12);
	uint32_t size = be32(fdt + 32);
	uint32_t off = 0;

	while (off < size) {
		const char *s = (const char *)(strings + off);
		uint32_t l = (uint32_t)strlen(s) + 1;
		if (streq(s, name)) {
			if (len_out)
				*len_out = l;
			return off;
		}
		off += l;
	}
	return 0xffffffffu;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	uintptr_t stop = p + be32(fdt + 36);
	int depth = 0, in_chosen = 0;

	while (p < stop) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}
	return 0;
}

/* Offset of the value of a /chosen property, or 0 if it is not there. */
static uintptr_t chosen_prop_val(uintptr_t fdt, const char *prop, uint32_t *len_out)
{
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	uintptr_t stop = p + be32(fdt + 36);
	int depth = 0, in_chosen = 0;

	while (p < stop) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len_out)
					*len_out = len;
				return val;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}
	return 0;
}

/*
 * Set a /chosen property. An existing property is overwritten in place (the new
 * value must fit). A missing one is inserted before /chosen's FDT_END_NODE.
 * Returns 1 on success.
 */
int fdt_chosen_set(uintptr_t fdt, const char *prop, const void *val, uint32_t len)
{
	uintptr_t node = 0, end = 0;
	if (!find_chosen(fdt, &node, &end))
		return 0;

	uint32_t oldlen = 0;
	uintptr_t old = chosen_prop_val(fdt, prop, &oldlen);
	if (old) {
		if (len > oldlen)
			return 0;
		memcpy((void *)old, val, len);
		memset((void *)(old + len), 0, oldlen - len);
		return 1;
	}

	/* Append the property name to the strings block (dtc puts it last). */
	uint32_t name_len = (uint32_t)strlen(prop) + 1;
	uint32_t nameoff = find_string(fdt, prop, 0);
	if (nameoff == 0xffffffffu) {
		uint32_t ssize = be32(fdt + 32);
		uint32_t total = be32(fdt + 4);
		uintptr_t strings = fdt + be32(fdt + 12);
		if (strings + ssize != fdt + total) {
			/*
			 * The strings block is not last, so there is nowhere to put
			 * a new name. Reuse a zero hole in the block if there is one
			 * that is big enough.
			 */
			uint32_t hole = 0, best = 0;
			for (uint32_t o = 0; o + name_len <= ssize; o++) {
				if (*(volatile uint8_t *)(strings + o) == 0) {
					uint32_t n = 0;
					while (o + n < ssize &&
					       *(volatile uint8_t *)(strings + o + n) == 0)
						n++;
					if (n >= name_len && (!best || n < best)) {
						best = n;
						hole = o;
					}
					o += n;
				}
			}
			if (!best)
				return 0;
			nameoff = hole;
			memcpy((void *)(strings + hole), prop, name_len);
		} else {
			nameoff = ssize;
			memcpy((void *)(strings + ssize), prop, name_len);
			wr32(fdt + 32, ssize + name_len);
			wr32(fdt + 4, total + name_len);
		}
	}

	/*
	 * Insert the property before /chosen's FDT_END_NODE. Everything after it
	 * moves up by ins bytes -- the rest of the structure block *and* the
	 * strings block, which follows it.
	 */
	uint32_t ins = 12 + ((len + 3) & ~3u);
	uint32_t total = be32(fdt + 4);
	uint32_t struct_off = be32(fdt + 8);
	uint32_t ssize = be32(fdt + 36);
	uint32_t stroff = be32(fdt + 12);
	if (stroff >= struct_off + ssize)
		wr32(fdt + 12, stroff + ins);
	memmove((void *)(end + ins), (void *)end, (fdt + total) - end);
	wr32(end, FDT_PROP);
	wr32(end + 4, len);
	wr32(end + 8, nameoff);
	memcpy((void *)(end + 12), val, len);
	wr32(fdt + 36, ssize + ins);
	wr32(fdt + 4, total + ins);
	return 1;
}

int fdt_chosen_set_u64(uintptr_t fdt, const char *prop, uint64_t v)
{
	uint64_t be = __builtin_bswap64(v);	/* the blob stores big-endian */
	return fdt_chosen_set(fdt, prop, &be, 8);
}

int fdt_chosen_set_str(uintptr_t fdt, const char *prop, const char *s)
{
	return fdt_chosen_set(fdt, prop, s, (uint32_t)strlen(s) + 1);
}
