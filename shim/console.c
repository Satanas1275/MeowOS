/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
static uint32_t con_bg;
static int row;

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
		for (int x = 0; x < FB_W; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (FB_STRIDE / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

/* ---- MEOW OS splash ---- */

void fill_rect(int x0, int y0, int w, int h, uint32_t c)
{
	if (x0 < 0) { w += x0; x0 = 0; }
	if (y0 < 0) { h += y0; y0 = 0; }
	if (x0 + w > FB_W) w = FB_W - x0;
	if (y0 + h > FB_H) h = FB_H - y0;
	for (int y = 0; y < h; y++) {
		volatile uint32_t *line = con_fb + (y0 + y) * (FB_STRIDE / 4) + x0;
		for (int x = 0; x < w; x++)
			line[x] = c;
	}
}

static void fill_circle(int cx, int cy, int r, uint32_t c)
{
	for (int y = -r; y <= r; y++) {
		int dx = 0;
		while ((dx + 1) * (dx + 1) + y * y <= r * r)
			dx++;
		fill_rect(cx - dx, cy + y, 2 * dx + 1, 1, c);
	}
}

/* Isoceles triangle: apex at (cx, ytop), half-width w at ytop + h. */
static void fill_tri(int cx, int ytop, int w, int h, uint32_t c)
{
	for (int y = 0; y < h; y++)
		fill_rect(cx - w * y / h, ytop + y, 2 * (w * y / h) + 1, 1, c);
}

static void draw_line(int x0, int y0, int x1, int y1, int th, uint32_t c)
{
	int dx = x1 - x0, dy = y1 - y0;
	int adx = dx < 0 ? -dx : dx;
	int ady = dy < 0 ? -dy : dy;
	int steps = adx > ady ? adx : ady;
	if (steps < 1)
		steps = 1;
	for (int i = 0; i <= steps; i++)
		fill_rect(x0 + dx * i / steps - th / 2, y0 + dy * i / steps - th / 2, th, th, c);
}

void put_char_at(int x, int y, char ch, uint32_t fg, int scale)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			if (g[gy] & (0x80 >> gx))
				fill_rect(x + gx * scale, y + gy * scale, scale, scale, fg);
}

void put_str_at(int x, int y, const char *s, uint32_t fg, int scale)
{
	while (*s) {
		put_char_at(x, y, *s++, fg, scale);
		x += 8 * scale;
	}
}

static void put_str_centered(int y, const char *s, uint32_t fg, int scale)
{
	int w = 0;
	for (const char *p = s; *p; p++)
		w += 8 * scale;
	put_str_at((FB_W - w) / 2, y, s, fg, scale);
}

void con_splash(void)
{
	con_clear(BLACK);

	/* Cat: head, ears, face, whiskers. */
	fill_circle(540, 980, 240, GINGER);
	fill_tri(370, 690, 105, 190, GINGER);
	fill_tri(710, 690, 105, 190, GINGER);
	fill_tri(370, 725, 52, 115, PINK);
	fill_tri(710, 725, 52, 115, PINK);
	fill_circle(450, 960, 42, BLACK);
	fill_circle(630, 960, 42, BLACK);
	fill_circle(463, 946, 14, WHITE);
	fill_circle(643, 946, 14, WHITE);
	fill_tri(540, 1030, 30, 32, PINK);
	draw_line(345, 1015, 180, 975, 5, WHITE);
	draw_line(345, 1040, 175, 1040, 5, WHITE);
	draw_line(345, 1065, 180, 1105, 5, WHITE);
	draw_line(735, 1015, 900, 975, 5, WHITE);
	draw_line(735, 1040, 905, 1040, 5, WHITE);
	draw_line(735, 1065, 900, 1105, 5, WHITE);

	put_str_centered(1400, "MEOW OS", WHITE, 10);
	put_str_centered(1620, "a purring operating system", 0xffaaaaaaU, 3);
	put_str_centered(1700, "0.1 / topaz", 0xff888888U, 2);
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}
