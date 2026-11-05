#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
/* Set a /chosen property: overwrite it, or insert it if it is missing. */
int fdt_chosen_set_u64(uintptr_t fdt, const char *prop, uint64_t v);
int fdt_chosen_set_str(uintptr_t fdt, const char *prop, const char *s);
/* Print what ABL handed us (the kernel has no console yet). */
void fdt_diag(uintptr_t fdt);
/* Add a simple-framebuffer node pointing at the ABL panel. */
int fdt_add_framebuffer(uintptr_t fdt, uint64_t addr, uint64_t size,
			uint32_t w, uint32_t h, uint32_t stride);
