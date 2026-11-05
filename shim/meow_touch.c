/*
 * MEOW OS touch: FocalTech FT5452 at 0x38 on QUP0 SE2 (GENI I2C), IRQ GPIO80,
 * reset GPIO86, AVDD GPIO36. Port of topaz-woa TopazTouch (hwio.c, touch.c) and
 * the topaz XNU patches 0083/0087: TLMM pins, GCC clock vote, polled FIFO-mode
 * GENI I2C, 63-byte frame reads, raw 1350x3000 -> panel 1080x2400.
 *
 * Polled, no interrupts: the frame is read every few ms and the IRQ GPIO is
 * only used as a hint, like the XNU driver.
 */
#include <stdint.h>
#include "meow_touch.h"

#define TLMM_BASE	0x00400000UL
#define TLMM_WEST	0x00100000UL
#define TLMM_EAST	0x00900000UL
#define TLMM_PIN(t, p)	(TLMM_BASE + (t) + (uint32_t)(p) * 0x1000UL)

#define GCC_BASE	0x01400000UL
#define GCC_QUP_VOTE	0x7900C		/* QUP0 wrapper + SE2 vote, bits 0x13c0 */
#define GCC_QUP0_S2_RCG	0x1F3A8		/* CFG +4, CBCR -4 (gcc-khaje.c) */

#define QUP0_SE2	0x04A88000UL

/* GENI SE registers */
#define SE_CTRL			0x20
#define SE_OUTPUT		0x24
#define SE_CGC			0x28
#define SE_STATUS		0x40
#define SE_MCLK			0x48
#define SE_FW_REV		0x68
#define SE_CLK_SEL		0x7C
#define SE_BYTE_GRAN		0x254
#define SE_DMA_MODE		0x258
#define SE_TX_PACK0		0x260
#define SE_TX_PACK1		0x264
#define SE_TX_LEN		0x26C
#define SE_RX_LEN		0x270
#define SE_SCL			0x278
#define SE_RX_PACK0		0x284
#define SE_RX_PACK1		0x288
#define SE_CMD			0x600
#define SE_CMD_CTRL		0x604
#define SE_IRQ_STATUS		0x610
#define SE_IRQ_EN		0x614
#define SE_IRQ_CLEAR		0x618
#define SE_S_IRQ_CLEAR		0x648
#define SE_TX_FIFO		0x700
#define SE_RX_FIFO		0x780
#define SE_TX_FIFO_STAT		0x800
#define SE_RX_FIFO_STAT		0x804
#define SE_RX_WM		0x810
#define SE_RX_RFR_WM		0x814
#define SE_DMA_TX_CLR		0xC44
#define SE_DMA_RX_CLR		0xD44
#define SE_GSI_EN		0xE18
#define SE_IRQ_EN_ALL		0xE1C
#define SE_HW_PARAM		0xE24

#define M_CMD_ACTIVE		(1u << 0)
#define M_CMD_ABORT		(1u << 1)
#define M_CMD_CANCEL		(1u << 2)
#define M_CMD_DONE		(1u << 0)
#define M_CMD_OVERRUN		(1u << 1)
#define M_ILLEGAL_CMD		(1u << 2)
#define M_CMD_FAILURE		(1u << 3)
#define M_CMD_CANCEL_EN		(1u << 4)
#define M_CMD_ABORT_EN		(1u << 5)
#define M_GP_IRQ_1		(1u << 10)	/* I2C NACK */
#define M_GP_IRQ_3		(1u << 12)
#define M_GP_IRQ_4		(1u << 13)
#define M_RX_FIFO_WM		(1u << 26)
#define M_RX_FIFO_LAST		(1u << 27)
#define M_TX_FIFO_WM		(1u << 30)
#define M_I2C_ERR (M_CMD_OVERRUN | M_ILLEGAL_CMD | M_CMD_FAILURE | M_CMD_ABORT_EN | \
		   M_GP_IRQ_1 | M_GP_IRQ_3 | M_GP_IRQ_4)
#define TX_FIFO_WC		0x0FFFFFFFu
#define RX_FIFO_WC		0x01FFFFFFu

#define I2C_OP_WRITE		0x1
#define I2C_OP_READ		0x2
#define M_OPCODE_SHIFT		27
#define I2C_STOP_STRETCH	(1u << 2)
#define I2C_SLV_ADDR_SHIFT	9

#define TS_ADDR			0x38
#define TS_PIN_SDA		6	/* WEST, func 1 = qup2 */
#define TS_PIN_SCL		7
#define TS_PIN_IRQ		80	/* WEST, active low, pull-up */
#define TS_PIN_RESET		86	/* WEST, active low */
#define TS_PIN_AVDD		36	/* EAST, output high */
#define TS_MAX			10
#define TS_RAW_MAX_X		1349
#define TS_RAW_MAX_Y		2999
#define TS_DATA_LEN		(3 + 6 * TS_MAX)
#define FTS_CHIP_ID		0xA3
#define FTS_TD_STATUS		0x02

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void delay_ms(uint32_t ms)
{
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000 * ms;
	while (rsr("cntpct_el0") < end)
		;
}

/* ---- TLMM ---- */

static void tlmm_cfg(uintptr_t tile, unsigned pin, unsigned func, unsigned pull,
		     unsigned drv, int out, int high)
{
	uintptr_t base = TLMM_PIN(tile, pin);
	if (out)
		wr32(base + 4, (rd32(base + 4) & ~2u) | (high ? 2u : 0u));
	uint32_t ctl = rd32(base);
	ctl &= ~0x3ffu;
	ctl |= (func << 2) | pull | (drv << 6) | (out ? 0x200u : 0u);
	wr32(base, ctl);
}

/* ---- GCC: QUP0 SE2 clock ---- */

static void gcc_qup0_se2(void)
{
	uintptr_t vote = GCC_BASE + (GCC_QUP_VOTE & ~0xFFFu);
	uintptr_t qup = GCC_BASE + (GCC_QUP0_S2_RCG & ~0xFFFu);
	wr32(vote + (GCC_QUP_VOTE & 0xFFFu), rd32(vote + (GCC_QUP_VOTE & 0xFFFu)) | 0x13c0u);
	wr32(qup + (GCC_QUP0_S2_RCG & 0xFFFu) + 4u, 1u);
	wr32(qup + (GCC_QUP0_S2_RCG & 0xFFFu), rd32(qup + (GCC_QUP0_S2_RCG & 0xFFFu)) | 1u);
	for (int i = 0; i < 100000 && (rd32(qup + (GCC_QUP0_S2_RCG & 0xFFFu)) & 1u); i++)
		;
}

/* ---- GENI I2C, FIFO mode, polled ---- */

static void geni_irq_clear(void)
{
	wr32(QUP0_SE2 + SE_GSI_EN, 0);
	wr32(QUP0_SE2 + SE_IRQ_CLEAR, 0xFFFFFFFF);
	wr32(QUP0_SE2 + SE_S_IRQ_CLEAR, 0xFFFFFFFF);
	wr32(QUP0_SE2 + SE_DMA_TX_CLR, 0xFFFFFFFF);
	wr32(QUP0_SE2 + SE_DMA_RX_CLR, 0xFFFFFFFF);
}

static void geni_packing(void)
{
	uint32_t cfg[4];
	int idx = 7;
	for (int i = 0; i < 4; i++) {
		cfg[i] = ((uint32_t)idx << 5) | (1u << 4) | (7u << 1);
		idx = ((i + 1) * 8) + 7;
	}
	cfg[3] |= 1u;
	uint32_t cfg0 = cfg[0] | (cfg[1] << 10);
	uint32_t cfg1 = cfg[2] | (cfg[3] << 10);
	wr32(QUP0_SE2 + SE_TX_PACK0, cfg0);
	wr32(QUP0_SE2 + SE_TX_PACK1, cfg1);
	wr32(QUP0_SE2 + SE_RX_PACK0, cfg0);
	wr32(QUP0_SE2 + SE_RX_PACK1, cfg1);
	wr32(QUP0_SE2 + SE_BYTE_GRAN, 0);
}

static int geni_init(void)
{
	uint32_t fw = rd32(QUP0_SE2 + SE_FW_REV);
	if (((fw >> 8) & 0xff) != 3)		/* protocol 3 = I2C */
		return 0;
	geni_irq_clear();
	wr32(QUP0_SE2 + SE_CGC, rd32(QUP0_SE2 + SE_CGC) | 0x7F);
	wr32(QUP0_SE2 + SE_OUTPUT, 0x7F);
	wr32(QUP0_SE2 + SE_CTRL, 1);
	wr32(QUP0_SE2 + SE_IRQ_EN_ALL, rd32(QUP0_SE2 + SE_IRQ_EN_ALL) | (1u << 2) | (1u << 3));
	wr32(QUP0_SE2 + SE_DMA_MODE, rd32(QUP0_SE2 + SE_DMA_MODE) & ~1u);
	wr32(QUP0_SE2 + SE_GSI_EN, 0);
	uint32_t depth = (rd32(QUP0_SE2 + SE_HW_PARAM) >> 16) & 0x3F;
	if (!depth)
		depth = 16;
	wr32(QUP0_SE2 + SE_RX_WM, depth - 1);
	wr32(QUP0_SE2 + SE_RX_RFR_WM, depth);
	wr32(QUP0_SE2 + SE_IRQ_EN, rd32(QUP0_SE2 + SE_IRQ_EN) | M_CMD_DONE | M_I2C_ERR |
					      M_RX_FIFO_WM | M_RX_FIFO_LAST);
	geni_packing();
	wr32(QUP0_SE2 + SE_CLK_SEL, 0);
	wr32(QUP0_SE2 + SE_MCLK, (2u << 4) | 1u);
	wr32(QUP0_SE2 + SE_SCL, (5u << 20) | (11u << 10) | 22u);	/* 400 kHz */
	return 1;
}

static void geni_cancel(void)
{
	wr32(QUP0_SE2 + SE_CMD_CTRL, M_CMD_CANCEL);
	for (int i = 0; i < 1000 && !(rd32(QUP0_SE2 + SE_IRQ_STATUS) & M_CMD_CANCEL_EN); i++)
		;
	if (rd32(QUP0_SE2 + SE_STATUS) & M_CMD_ACTIVE) {
		wr32(QUP0_SE2 + SE_CMD_CTRL, M_CMD_ABORT);
		for (int i = 0; i < 1000 && !(rd32(QUP0_SE2 + SE_IRQ_STATUS) & M_CMD_ABORT_EN); i++)
			;
	}
	wr32(QUP0_SE2 + SE_IRQ_CLEAR, 0xFFFFFFFF);
}

static int geni_wait_done(void)
{
	uint32_t st = 0;
	for (int i = 0; i < 20000; i++) {
		st = rd32(QUP0_SE2 + SE_IRQ_STATUS);
		if (st & (M_CMD_DONE | M_I2C_ERR))
			break;
	}
	wr32(QUP0_SE2 + SE_IRQ_CLEAR, st);
	if (st & M_I2C_ERR) {
		geni_cancel();
		return 0;
	}
	if (!(st & M_CMD_DONE)) {
		geni_cancel();
		return 0;
	}
	return 1;
}

static int geni_write(uint8_t addr, const uint8_t *buf, uint32_t len, int stop)
{
	uint32_t param = ((uint32_t)addr << I2C_SLV_ADDR_SHIFT) | (stop ? 0 : I2C_STOP_STRETCH);
	uint32_t done = 0;
	wr32(QUP0_SE2 + SE_IRQ_CLEAR, 0xFFFFFFFF);
	wr32(QUP0_SE2 + SE_TX_LEN, len);
	wr32(QUP0_SE2 + SE_CMD, ((uint32_t)I2C_OP_WRITE << M_OPCODE_SHIFT) | param);
	while (done < len) {
		uint32_t word = 0, n = len - done < 4 ? len - done : 4;
		for (int i = 0; i < 20000; i++)
			if ((rd32(QUP0_SE2 + SE_TX_FIFO_STAT) & TX_FIFO_WC) < 16)
				break;
		for (uint32_t i = 0; i < n; i++)
			word |= (uint32_t)buf[done + i] << (8 * i);
		wr32(QUP0_SE2 + SE_TX_FIFO, word);
		done += n;
	}
	return geni_wait_done();
}

static int geni_read(uint8_t addr, uint8_t *buf, uint32_t len)
{
	uint32_t param = (uint32_t)addr << I2C_SLV_ADDR_SHIFT;
	uint32_t got = 0;
	wr32(QUP0_SE2 + SE_IRQ_CLEAR, 0xFFFFFFFF);
	wr32(QUP0_SE2 + SE_RX_LEN, len);
	wr32(QUP0_SE2 + SE_CMD, ((uint32_t)I2C_OP_READ << M_OPCODE_SHIFT) | param);
	for (int spins = 0; spins < 20000 && got < len; ) {
		uint32_t words = rd32(QUP0_SE2 + SE_RX_FIFO_STAT) & RX_FIFO_WC;
		uint32_t st = rd32(QUP0_SE2 + SE_IRQ_STATUS);
		if (st & M_I2C_ERR)
			break;
		if (!words) {
			spins++;
			continue;
		}
		while (words-- && got < len) {
			uint32_t word = rd32(QUP0_SE2 + SE_RX_FIFO);
			uint32_t n = len - got < 4 ? len - got : 4;
			for (uint32_t i = 0; i < n; i++)
				buf[got + i] = (word >> (8 * i)) & 0xff;
			got += n;
		}
	}
	geni_wait_done();
	return got == len;
}

static int geni_read_reg(uint8_t reg, uint8_t *buf, uint32_t len)
{
	if (!geni_write(TS_ADDR, &reg, 1, 0))
		return 0;
	return geni_read(TS_ADDR, buf, len);
}

/* ---- FT5452 ---- */

static int touch_ok;

void meow_touch_init(void)
{
	tlmm_cfg(TLMM_WEST, TS_PIN_SDA, 1, 0, 0, 0, 0);
	tlmm_cfg(TLMM_WEST, TS_PIN_SCL, 1, 0, 0, 0, 0);
	tlmm_cfg(TLMM_WEST, TS_PIN_IRQ, 0, 3, 3, 0, 0);
	tlmm_cfg(TLMM_EAST, TS_PIN_AVDD, 0, 3, 3, 1, 1);
	gcc_qup0_se2();
	if (!geni_init())
		return;
	tlmm_cfg(TLMM_WEST, TS_PIN_RESET, 0, 3, 3, 1, 0);
	delay_ms(20);
	tlmm_cfg(TLMM_WEST, TS_PIN_RESET, 0, 3, 3, 1, 1);
	delay_ms(250);
	uint8_t id = 0;
	if (!geni_read_reg(FTS_CHIP_ID, &id, 1))
		return;
	touch_ok = 1;
}

/*
 * 1 = a contact is down (panel coordinates in x and y), 0 = read, no contact,
 * -1 = the controller did not answer. Every call reads the frame: the IRQ
 * line pulses are too short to be polled reliably, and edge detection needs
 * an answer on every call.
 */
int meow_touch_poll(int *x, int *y)
{
	uint8_t buf[TS_DATA_LEN];
	if (!touch_ok)
		return -1;
	if (!geni_read_reg(0x00, buf, TS_DATA_LEN))	/* TD_STATUS lands in buf[2] */
		return -1;
	unsigned td = buf[2] & 0x0f;
	if (td == 0 || td > TS_MAX)
		return 0;
	const uint8_t *p = buf + 3;
	unsigned px = ((unsigned)(p[0] & 0x0f) << 8) | p[1];
	unsigned py = ((unsigned)(p[2] & 0x0f) << 8) | p[3];
	if (px > TS_RAW_MAX_X || py > TS_RAW_MAX_Y)
		return 0;
	*x = (px * 4 + (p[4] >> 6)) / 5;
	*y = (py * 4 + ((p[4] >> 4) & 3)) / 5;
	if (*x > 1079) *x = 1079;
	if (*y > 2399) *y = 2399;
	return 1;
}
