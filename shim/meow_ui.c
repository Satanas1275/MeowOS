/*
 * MEOW OS UI: a scrollback terminal and an on-screen AZERTY keyboard, drawn
 * straight on the ABL splash framebuffer. Input comes from meow_touch.c
 * (FocalTech FT5452 on QUP0 SE2).
 *
 * Redraws are incremental: the text area is only repainted when the content
 * changes, the cursor blinks by repainting one cell, and a pressed key is
 * highlighted by repainting that key. A full repaint costs ~100 ms with the
 * MMU off and a few ms with the framebuffer mapped Normal-NC.
 */
#include <stdint.h>
#include "console.h"
#include "meow_ui.h"
#ifdef __aarch64__
#include "meow_touch.h"
#endif

#define UI_SCALE	2
#define CH_W		(8 * UI_SCALE)
#define CH_H		(16 * UI_SCALE)
#define TERM_COLS	64
#define TERM_ROWS	40
#define TERM_X		28
#define TERM_Y		16
#define KB_TOP		1400
#define KB_H		(FB_H - KB_TOP)
#define MAX_LINES	256

#ifdef __aarch64__
#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#else
/* host_test: no timer, no touch hardware */
static inline uint64_t rsr(const char *r) { (void)r; return 0; }
static void meow_touch_init(void) { }
static int meow_touch_poll(int *x, int *y) { (void)x; (void)y; return 0; }
#endif

struct key {
	char label[8];
	int x, y, w, h;
	char code;
};

static char lines[MAX_LINES][TERM_COLS];
static int linelen[MAX_LINES];
static int nlines, vtop;
static char input[TERM_COLS];
static int in_len;
static int cur_on, cur_x, cur_y;
static int dot_x = -1, dot_y = -1;
static struct key *last_key;

static void *meow_memcpy(void *d, const void *s, unsigned n)
{
	char *dd = d;
	const char *ss = s;
	while (n--)
		*dd++ = *ss++;
	return d;
}

static int meow_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static void delay_ms(uint32_t ms)
{
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000 * ms;
	while (rsr("cntpct_el0") < end)
		;
}

/* ---- terminal ---- */

static void term_clear(void)
{
	nlines = vtop = 0;
	in_len = 0;
	input[0] = 0;
	dot_x = dot_y = -1;
}

static void term_add_line(void)
{
	if (nlines < MAX_LINES) {
		nlines++;
	} else {
		for (int i = 0; i < MAX_LINES - 1; i++) {
			meow_memcpy(lines[i], lines[i + 1], TERM_COLS);
			linelen[i] = linelen[i + 1];
		}
	}
	linelen[nlines - 1] = 0;
	lines[nlines - 1][0] = 0;
}

static void term_putc(char c)
{
	if (c == '\n' || nlines == 0) {
		term_add_line();
		if (c == '\n')
			return;
	}
	if (linelen[nlines - 1] >= TERM_COLS - 1)
		term_add_line();
	lines[nlines - 1][linelen[nlines - 1]++] = c;
}

static void term_puts(const char *s)
{
	while (*s)
		term_putc(*s++);
}

static void term_num(int v, unsigned base)
{
	char b[12];
	int i = 0;
	unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
	if (u == 0)
		term_putc('0');
	while (u) {
		unsigned d = u % base;
		b[i++] = d < 10 ? '0' + d : 'a' + d - 10;
		u /= base;
	}
	if (v < 0)
		term_putc('-');
	while (i)
		term_putc(b[--i]);
}

static void term_autoscroll(void)
{
	vtop = nlines > TERM_ROWS ? nlines - TERM_ROWS : 0;
}

/* ---- incremental painting ---- */

static void cursor_show(int on)
{
	cur_on = on;
	fill_rect(cur_x, cur_y + CH_H - 8, CH_W - 4, 5, on ? GREEN : BLACK);
}

static void input_draw(void)
{
	int iy = KB_TOP - CH_H - 12;
	fill_rect(TERM_X, iy - 6, FB_W - 2 * TERM_X, CH_H + 12, BLACK);
	put_str_at(TERM_X, iy, "> ", GREEN, UI_SCALE);
	put_str_at(TERM_X + 2 * CH_W, iy, input, WHITE, UI_SCALE);
	cur_x = TERM_X + (2 + in_len) * CH_W;
	cur_y = iy;
	cursor_show(cur_on);
}

/* Debug marker, terminal area only (the keyboard is not a flat background). */
static void dot_show(int x, int y)
{
	if (dot_x >= 0 && dot_y < KB_TOP)
		fill_rect(dot_x - 8, dot_y - 8, 16, 16, BLACK);
	dot_x = x;
	dot_y = y;
	if (x >= 0 && y < KB_TOP)
		fill_rect(x - 8, y - 8, 16, 16, RED);
}

static void term_draw(void)
{
	fill_rect(0, 0, FB_W, KB_TOP, BLACK);
	int first = vtop > 0 ? vtop : 0;
	int y = TERM_Y;
	for (int r = 0; r < TERM_ROWS; r++) {
		int l = first + r;
		if (l >= nlines)
			break;
		put_str_at(TERM_X, y, lines[l], WHITE, UI_SCALE);
		y += CH_H;
	}
	input_draw();
}

/* ---- keyboard ---- */

static struct key keys[48];
static int nkeys;
static int shifted;

static void kb_add(const char *label, int x, int y, int w, int h, char code)
{
	struct key *k = &keys[nkeys++];
	int i = 0;
	while (label[i] && i < 7) {
		k->label[i] = label[i];
		i++;
	}
	k->label[i] = 0;
	k->x = x;
	k->y = y;
	k->w = w;
	k->h = h;
	k->code = code;
}

static void kb_build(void)
{
	static const char rows[3][12] = { "azertyuiop", "qsdfghjklm", "wxcvbn,.!?" };
	nkeys = 0;
	int kh = (KB_H - 80) / 4;
	for (int r = 0; r < 3; r++) {
		int y = KB_TOP + 24 + r * (kh + 10);
		int kw = (FB_W - 24 - 9 * 6) / 10;
		for (int i = 0; rows[r][i]; i++) {
			char lb[2] = { rows[r][i], 0 };
			kb_add(lb, 12 + i * (kw + 6), y, kw, kh, rows[r][i]);
		}
	}
	int y = KB_TOP + 24 + 3 * (kh + 10);
	kb_add("MAJ", 12, y, 200, kh, 0x01);
	kb_add("espace", 220, y, 400, kh, ' ');
	kb_add("<=", 628, y, 200, kh, '\b');
	kb_add("OK", 836, y, 232, kh, '\n');
}

static void key_draw(struct key *k, int down)
{
	fill_rect(k->x, k->y, k->w, k->h, down ? 0xff707070 : 0xff383838);
	fill_rect(k->x, k->y, k->w, 3, down ? 0xffa0a0a0 : 0xff585858);
	int lw = 0;
	while (k->label[lw])
		lw++;
	int scale = k->h >= 120 ? 3 : 2;
	int tw = lw * 8 * scale;
	put_str_at(k->x + (k->w - tw) / 2, k->y + (k->h - 16 * scale) / 2,
		  k->label, WHITE, scale);
}

static void kb_draw(void)
{
	fill_rect(0, KB_TOP, FB_W, KB_H, 0xff181818);
	for (int i = 0; i < nkeys; i++)
		key_draw(&keys[i], shifted && keys[i].code == 0x01);
}

static struct key *kb_find(int x, int y)
{
	for (int i = 0; i < nkeys; i++)
		if (x >= keys[i].x && x < keys[i].x + keys[i].w &&
		    y >= keys[i].y && y < keys[i].y + keys[i].h)
			return &keys[i];
	return 0;
}

/* ---- commands ---- */

static void meow_command(const char *cmd)
{
	if (!meow_strcmp(cmd, "help")) {
		term_puts("help clear meow uptime reboot boot\n");
	} else if (!meow_strcmp(cmd, "clear")) {
		term_clear();
	} else if (!meow_strcmp(cmd, "meow")) {
		term_puts(" /\\_/\\ \n( o.o )\n > ^ <\n");
	} else if (!meow_strcmp(cmd, "uptime")) {
		term_num((int)(rsr("cntpct_el0") / (rsr("cntfrq_el0") / 1000)), 10);
		term_puts(" ms\n");
	} else if (!meow_strcmp(cmd, "reboot")) {
		term_puts("not wired yet\n");
	} else if (!meow_strcmp(cmd, "boot")) {
		term_puts("xnu hand-off not built into meow\n");
	} else if (cmd[0]) {
		term_puts("unknown: ");
		term_puts(cmd);
		term_putc('\n');
	}
	term_autoscroll();
	term_draw();
}

static void meow_key(char code)
{
	if (code == 0x01) {
		shifted = !shifted;
		kb_draw();
		return;
	}
	if (code == '\b') {
		if (in_len)
			input[--in_len] = 0;
		input_draw();
		return;
	}
	if (code == '\n') {
		term_puts("> ");
		term_puts(input);
		term_putc('\n');
		meow_command(input);
		in_len = 0;
		input[0] = 0;
		input_draw();
		return;
	}
	if (in_len < TERM_COLS - 2) {
		if (shifted && code >= 'a' && code <= 'z')
			code -= 32;
		input[in_len++] = code;
	}
	input_draw();
}

/* ---- main ---- */

void meow_ui_render(void)
{
	term_draw();
	kb_draw();
}

void meow_ui_demo(void)
{
	term_clear();
	kb_build();
	term_puts("MEOW OS 0.2 / topaz\n");
	term_puts("terminal + clavier tactile\n");
	term_puts("tape 'help'\n\n");
	term_puts("> help\n");
	term_puts("help clear meow uptime reboot boot\n");
	meow_ui_render();
}

void meow_run(void)
{
	term_clear();
	kb_build();
	term_puts("MEOW OS 0.2 / topaz\n");
	term_puts("terminal + clavier tactile\n");
	term_puts("tape 'help'\n\n");
	meow_touch_init();
	meow_ui_render();
	unsigned ticks = 0;
	int was_down = 0;
	int up_cnt = 0;
	for (;;) {
		int x = -1, y = -1;
		int r = meow_touch_poll(&x, &y);
		if (r == 1) {
			up_cnt = 0;
			if (!was_down) {
				was_down = 1;
				struct key *k = kb_find(x, y);
				if (k) {
					key_draw(k, 1);
					last_key = k;
					meow_key(k->code);
				} else {
					dot_show(x, y);
				}
			} else if (!last_key) {
				dot_show(x, y);
			}
		} else if (r == 0 && was_down) {
			/*
			 * Debounce the release: a tap shorter than the controller's own
			 * frame (~21 ms) can read as contact / no-contact / contact and
			 * would otherwise register as several presses.
			 */
			if (++up_cnt >= 5) {
				was_down = 0;
				up_cnt = 0;
				if (last_key) {
					key_draw(last_key, 0);
					last_key = 0;
				}
				dot_show(-1, -1);
			}
		}
		if ((ticks++ % 100) == 0)
			cursor_show(!cur_on);
		delay_ms(3);
	}
}
