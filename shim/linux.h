#pragma once
#include <stdint.h>

/*
 * Load a Linux kernel Image + initrd and jump to it (arm64 Linux boot protocol:
 * x0 = device tree, x1-x3 = 0, MMU off, DAIF masked).
 * Returns a diagnostic string, or 0 if the kernel took over.
 */
const char *meow_linux_boot(uintptr_t payload, uintptr_t dtb);
