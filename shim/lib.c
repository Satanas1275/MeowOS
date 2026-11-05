/*
 * The shim is freestanding (-nostdlib), so the few string routines fdt.c needs
 * live here. Keep them small and 8-byte aligned where it is safe to do so.
 */
#include <stdint.h>
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n)
{
	volatile uint8_t *dd = (volatile uint8_t *)d;
	volatile const uint8_t *ss = (volatile const uint8_t *)s;
	while (n--)
		*dd++ = *ss++;
	return d;
}

void *memset(void *d, int c, size_t n)
{
	volatile uint8_t *dd = (volatile uint8_t *)d;
	while (n--)
		*dd++ = (uint8_t)c;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	volatile uint8_t *dd = (volatile uint8_t *)d;
	volatile const uint8_t *ss = (volatile const uint8_t *)s;
	if (dd < ss) {
		while (n--)
			*dd++ = *ss++;
	} else {
		dd += n;
		ss += n;
		while (n--)
			*--dd = *--ss;
	}
	return d;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}
