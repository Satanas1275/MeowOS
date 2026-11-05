#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
/* Set a /chosen property: overwrite it, or insert it if it is missing. */
int fdt_chosen_set_u64(uintptr_t fdt, const char *prop, uint64_t v);
int fdt_chosen_set_str(uintptr_t fdt, const char *prop, const char *s);
