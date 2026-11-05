/* Render the console layout on the host: ./out/host_test, then view the ppm files in out/ */
#include <stdio.h>
#include "console.h"
#include "meow_ui.h"

static uint32_t host_fb[FB_H * FB_STRIDE / 4];

static void save(const char *path)
{
	FILE *f = fopen(path, "wb");
	fprintf(f, "P6 %d %d 255\n", FB_W, FB_H);
	for (int i = 0; i < FB_W * FB_H; i++) {
		unsigned char px[3] = { host_fb[i] >> 16, host_fb[i] >> 8, host_fb[i] };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

int main(void)
{
	con_fb = host_fb;
	con_splash();
	save("out/splash.ppm");

	con_clear(RED);
	con_str("SHIM", "MEOW OS milestone 4: xnu loader");
	con_hex("MMFR0", 0x0000000000101122ULL);
	con_hex2("PSCI", "ver=", 0x10001, "smccc=", 0x10002);
	con_hex2("CPU7", "ret=", 0, "mpidr=", 0x81000700);
	save("out/host.ppm");

	con_clear(BLUE);
	con_str("PANIC", "sync exception");
	con_hex("ESR", 0x96000010);
	save("out/panic.ppm");

	meow_ui_demo();
	save("out/ui.ppm");
	return 0;
}
