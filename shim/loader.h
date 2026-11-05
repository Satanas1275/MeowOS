#pragma once
#include <stdint.h>

/* Boot the payload found in the ABL ramdisk or appended to the Image. */
const char *loader_boot(uintptr_t dtb);
/* MEOW OS: boot a Linux payload appended to the Image, if there is one. */
void loader_try_linux(uintptr_t dtb);
