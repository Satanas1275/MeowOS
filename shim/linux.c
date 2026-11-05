/*
 * MEOW OS Linux loader.
 *
 * The payload is built by tools/mklinux.py:
 *
 *   0x000  char magic[8] = "MEOXLINUX"
 *   0x008  u32 version = 1, u32 flags
 *   0x010  u64 kernel_pa    2MB-aligned load address of the kernel Image
 *   0x018  u64 kernel_off   offset from the payload start
 *   0x020  u64 kernel_size
 *   0x028  u64 initrd_pa    0 = take the address from the DTB's /chosen
 *   0x030  u64 initrd_off
 *   0x038  u64 initrd_size
 *   0x040  u64 dtb_pa       0 = use the DTB ABL handed us
 *   0x048  u64 dtb_off
 *   0x050  u64 dtb_size
 *   0x058  char cmdline[1024]
 *   0x458  data
 *
 * The kernel is entered at kernel_pa (its header's code0 branches to stext).
 * The initrd is written to /chosen/linux,initrd-start and -end, and the
 * bootargs to /chosen/bootargs, so the kernel finds both.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "linux.h"

#define MEOW_LINUX_HDR	0x458

struct meow_linux_hdr {
	char magic[8];
	uint32_t version, flags;
	uint64_t kernel_pa, kernel_off, kernel_size;
	uint64_t initrd_pa, initrd_off, initrd_size;
	uint64_t dtb_pa, dtb_off, dtb_size;
	char cmdline[1024];
};

static void lcopy(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

const char *meow_linux_boot(uintptr_t payload, uintptr_t dtb)
{
	const struct meow_linux_hdr *h = (const struct meow_linux_hdr *)payload;

	if (h->version != 1)
		return "bad linux payload version";
	if (h->kernel_pa & 0x1fffff)
		return "kernel_pa is not 2MB aligned";
	if (h->kernel_off + h->kernel_size > 0x40000000ULL ||
	    h->initrd_off + h->initrd_size > 0x40000000ULL)
		return "linux payload overflow";

	uintptr_t kernel_pa = h->kernel_pa;
	con_hex2("LINUX", "kernel=", kernel_pa, "size=", h->kernel_size);
	lcopy(kernel_pa, payload + h->kernel_off, h->kernel_size);

	uintptr_t initrd_pa = h->initrd_pa;
	if (!initrd_pa) {
		uint64_t s = 0, e = 0;
		if (!fdt_chosen_u64(dtb, "linux,initrd-start", &s) ||
		    !fdt_chosen_u64(dtb, "linux,initrd-end", &e) || e <= s)
			return "no initrd address in /chosen";
		initrd_pa = (uintptr_t)s;
		if (e - s < h->initrd_size)
			return "initrd does not fit the DTB window";
	}
	con_hex2("LINUX", "initrd=", initrd_pa, "size=", h->initrd_size);
	lcopy(initrd_pa, payload + h->initrd_off, h->initrd_size);

	/* Tell the kernel where the initrd is and what to boot with. */
	if (!fdt_chosen_set_u64(dtb, "linux,initrd-start", initrd_pa) ||
	    !fdt_chosen_set_u64(dtb, "linux,initrd-end", initrd_pa + h->initrd_size))
		return "could not set /chosen initrd";
	if (h->cmdline[0] && !fdt_chosen_set_str(dtb, "bootargs", h->cmdline))
		return "could not set /chosen bootargs";
	con_hex("DTB", dtb);
	fdt_diag(dtb);

	/* Clean the kernel, the initrd and the DTB to the PoC. */
	for (uint64_t a = kernel_pa; a < kernel_pa + h->kernel_size; a += 64)
		__asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
	for (uint64_t a = initrd_pa; a < initrd_pa + h->initrd_size; a += 64)
		__asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
	if (dtb) {
		uint32_t dtb_size = __builtin_bswap32(*(volatile uint32_t *)(dtb + 4));
		for (uint64_t a = dtb; a < dtb + dtb_size; a += 64)
			__asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
	}
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");

	con_str("LINUX", "booting");
	__asm__ volatile(
		"mov x1, #0\n"
		"mov x2, #0\n"
		"mov x3, #0\n"
		"mov x0, %0\n"
		"msr daifset, #0xf\n"
		"br %1\n"
		:
		: "r"(dtb), "r"(kernel_pa)
		: "x0", "x1", "x2", "x3", "memory");
	return "linux returned";
}
