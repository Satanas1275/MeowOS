#pragma once
#include <stdint.h>

#define FB_BASE		0x5c000000UL	/* reserved-memory cont_splash_region */
#define FB_W		1080
#define FB_H		2400
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define BLACK	0xff000000U
#define GINGER	0xffff9933U
#define PINK	0xffff9ccdU

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
void con_splash(void);
/* drawing primitives used by the MEOW OS terminal/keyboard */
void fill_rect(int x0, int y0, int w, int h, uint32_t c);
void put_char_at(int x, int y, char ch, uint32_t fg, int scale);
void put_str_at(int x, int y, const char *s, uint32_t fg, int scale);
