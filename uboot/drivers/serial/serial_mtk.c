// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek High-speed UART driver
 *
 * Copyright (C) 2018 MediaTek Inc.
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#include <clk.h>
#include <common.h>
#include <div64.h>
#include <dm.h>
#include <errno.h>
#include <log.h>
#include <serial.h>
#include <watchdog.h>
#include <asm/io.h>
#include <asm/types.h>
#include <linux/err.h>

struct mtk_serial_regs {
	u32 rbr;
	u32 ier;
	u32 fcr;
	u32 lcr;
	u32 mcr;
	u32 lsr;
	u32 msr;
	u32 spr;
	u32 mdr1;
	u32 highspeed;
	u32 sample_count;
	u32 sample_point;
	u32 fracdiv_l;
	u32 fracdiv_m;
	u32 escape_en;
	u32 guard;
	u32 rx_sel;
};

#define thr rbr
#define iir fcr
#define dll rbr
#define dlm ier

#define UART_LCR_WLS_8	0x03		/* 8 bit character length */
#define UART_LCR_DLAB	0x80		/* Divisor latch access bit */

#define UART_LSR_DR	0x01		/* Data ready */
#define UART_LSR_THRE	0x20		/* Xmit holding register empty */
#define UART_LSR_TEMT	0x40		/* Xmitter empty */

#define UART_MCR_DTR	0x01		/* DTR   */
#define UART_MCR_RTS	0x02		/* RTS   */

#define UART_FCR_FIFO_EN	0x01	/* Fifo enable */
#define UART_FCR_RXSR		0x02	/* Receiver soft reset */
#define UART_FCR_TXSR		0x04	/* Transmitter soft reset */

#define UART_MCRVAL (UART_MCR_DTR | \
		     UART_MCR_RTS)

/* Clear & enable FIFOs */
#define UART_FCRVAL (UART_FCR_FIFO_EN | \
		     UART_FCR_RXSR |	\
		     UART_FCR_TXSR)

/* the data is correct if the real baud is within 3%. */
#define BAUD_ALLOW_MAX(baud)	((baud) + (baud) * 3 / 100)
#define BAUD_ALLOW_MIX(baud)	((baud) - (baud) * 3 / 100)

struct mtk_serial_priv {
	struct mtk_serial_regs __iomem *regs;
	u32 clock;
};

static void _mtk_serial_setbrg(struct mtk_serial_priv *priv, int baud)
{
	bool support_clk12m_baud115200;
	u32 quot, samplecount, realbaud;

	if ((baud <= 115200) && (priv->clock == 12000000))
		support_clk12m_baud115200 = true;
	else
		support_clk12m_baud115200 = false;

	if (baud <= 115200) {
		writel(0, &priv->regs->highspeed);
		quot = DIV_ROUND_CLOSEST(priv->clock, 16 * baud);

		if (support_clk12m_baud115200) {
			writel(3, &priv->regs->highspeed);
			quot = DIV_ROUND_CLOSEST(priv->clock, 256 * baud);
			if (quot == 0)
				quot = 1;

			samplecount = DIV_ROUND_CLOSEST(priv->clock,
							quot * baud);
			if (samplecount != 0) {
				realbaud = priv->clock / samplecount / quot;
				if ((realbaud > BAUD_ALLOW_MAX(baud)) ||
				    (realbaud < BAUD_ALLOW_MIX(baud))) {
					pr_info("baud %d can't be handled\n",
						baud);
				}
			} else {
				pr_info("samplecount is 0\n");
			}
		}
	} else if (baud <= 576000) {
		writel(2, &priv->regs->highspeed);

		/* Set to next lower baudrate supported */
		if ((baud == 500000) || (baud == 576000))
			baud = 460800;
		quot = DIV_ROUND_UP(priv->clock, 4 * baud);
	} else {
		writel(3, &priv->regs->highspeed);
		quot = DIV_ROUND_UP(priv->clock, 256 * baud);
	}

	/* set divisor */
	writel(UART_LCR_WLS_8 | UART_LCR_DLAB, &priv->regs->lcr);
	writel(quot & 0xff, &priv->regs->dll);
	writel((quot >> 8) & 0xff, &priv->regs->dlm);
	writel(UART_LCR_WLS_8, &priv->regs->lcr);

	if (baud > 460800) {
		u32 tmp;

		tmp = DIV_ROUND_CLOSEST(priv->clock, quot * baud);
		writel(tmp - 1, &priv->regs->sample_count);
		writel((tmp - 2) >> 1, &priv->regs->sample_point);
	} else {
		writel(0, &priv->regs->sample_count);
		writel(0xff, &priv->regs->sample_point);
	}

	if (support_clk12m_baud115200) {
		writel(samplecount - 1, &priv->regs->sample_count);
		writel((samplecount - 2) >> 1, &priv->regs->sample_point);
	}
}

static int _mtk_serial_putc(struct mtk_serial_priv *priv, const char ch)
{
	if (!(readl(&priv->regs->lsr) & UART_LSR_THRE))
		return -EAGAIN;

	writel(ch, &priv->regs->thr);

	if (ch == '\n')
		WATCHDOG_RESET();

	return 0;
}

static int _mtk_serial_getc(struct mtk_serial_priv *priv)
{
	if (!(readl(&priv->regs->lsr) & UART_LSR_DR))
		return -EAGAIN;

	return readl(&priv->regs->rbr);
}

static int _mtk_serial_pending(struct mtk_serial_priv *priv, bool input)
{
	if (input)
		return (readl(&priv->regs->lsr) & UART_LSR_DR) ? 1 : 0;
	else
		return (readl(&priv->regs->lsr) & UART_LSR_THRE) ? 0 : 1;
}

#if defined(CONFIG_DM_SERIAL) && \
	(!defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_DM))
static int mtk_serial_setbrg(struct udevice *dev, int baudrate)
{
	struct mtk_serial_priv *priv = dev_get_priv(dev);

	_mtk_serial_setbrg(priv, baudrate);

	return 0;
}

static int mtk_serial_putc(struct udevice *dev, const char ch)
{
	struct mtk_serial_priv *priv = dev_get_priv(dev);

	return _mtk_serial_putc(priv, ch);
}

static int mtk_serial_getc(struct udevice *dev)
{
	struct mtk_serial_priv *priv = dev_get_priv(dev);

	return _mtk_serial_getc(priv);
}

static int mtk_serial_pending(struct udevice *dev, bool input)
{
	struct mtk_serial_priv *priv = dev_get_priv(dev);

	return _mtk_serial_pending(priv, input);
}

static int mtk_serial_probe(struct udevice *dev)
{
	struct mtk_serial_priv *priv = dev_get_priv(dev);

	/* Disable interrupt */
	writel(0, &priv->regs->ier);

	writel(UART_MCRVAL, &priv->regs->mcr);
	writel(UART_FCRVAL, &priv->regs->fcr);

	return 0;
}

static int mtk_serial_ofdata_to_platdata(struct udevice *dev)
{
	struct mtk_serial_priv *priv = dev_get_priv(dev);
	fdt_addr_t addr;
	struct clk clk;
	int err;

	addr = dev_read_addr(dev);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;

	priv->regs = map_physmem(addr, 0, MAP_NOCACHE);

	err = clk_get_by_index(dev, 0, &clk);
	if (!err) {
		err = clk_get_rate(&clk);
		if (!IS_ERR_VALUE(err))
			priv->clock = err;
	} else if (err != -ENOENT && err != -ENODEV && err != -ENOSYS) {
		debug("mtk_serial: failed to get clock\n");
		return err;
	}

	if (!priv->clock)
		priv->clock = dev_read_u32_default(dev, "clock-frequency", 0);

	if (!priv->clock) {
		debug("mtk_serial: clock not defined\n");
		return -EINVAL;
	}

	return 0;
}

static const struct dm_serial_ops mtk_serial_ops = {
	.putc = mtk_serial_putc,
	.pending = mtk_serial_pending,
	.getc = mtk_serial_getc,
	.setbrg = mtk_serial_setbrg,
};

static const struct udevice_id mtk_serial_ids[] = {
	{ .compatible = "mediatek,hsuart" },
	{ .compatible = "mediatek,mt6577-uart" },
	{ }
};

U_BOOT_DRIVER(serial_mtk) = {
	.name = "serial_mtk",
	.id = UCLASS_SERIAL,
	.of_match = mtk_serial_ids,
	.ofdata_to_platdata = mtk_serial_ofdata_to_platdata,
	.priv_auto_alloc_size = sizeof(struct mtk_serial_priv),
	.probe = mtk_serial_probe,
	.ops = &mtk_serial_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
#else

DECLARE_GLOBAL_DATA_PTR;

#define DECLARE_HSUART_PRIV(port) \
	static struct mtk_serial_priv mtk_hsuart##port = { \
	.regs = (struct mtk_serial_regs *)CONFIG_SYS_NS16550_COM##port, \
	.clock = CONFIG_SYS_NS16550_CLK \
};

#define DECLARE_HSUART_FUNCTIONS(port) \
	static int mtk_serial##port##_init(void) \
	{ \
		writel(0, &mtk_hsuart##port.regs->ier); \
		writel(UART_MCRVAL, &mtk_hsuart##port.regs->mcr); \
		writel(UART_FCRVAL, &mtk_hsuart##port.regs->fcr); \
		_mtk_serial_setbrg(&mtk_hsuart##port, gd->baudrate); \
		return 0 ; \
	} \
	static void mtk_serial##port##_setbrg(void) \
	{ \
		_mtk_serial_setbrg(&mtk_hsuart##port, gd->baudrate); \
	} \
	static int mtk_serial##port##_getc(void) \
	{ \
		int err; \
		do { \
			err = _mtk_serial_getc(&mtk_hsuart##port); \
			if (err == -EAGAIN) \
				WATCHDOG_RESET(); \
		} while (err == -EAGAIN); \
		return err >= 0 ? err : 0; \
	} \
	static int mtk_serial##port##_tstc(void) \
	{ \
		return _mtk_serial_pending(&mtk_hsuart##port, true); \
	} \
	static void mtk_serial##port##_putc(const char c) \
	{ \
		int err; \
		if (c == '\n') \
			mtk_serial##port##_putc('\r'); \
		do { \
			err = _mtk_serial_putc(&mtk_hsuart##port, c); \
		} while (err == -EAGAIN); \
	} \
	static void mtk_serial##port##_puts(const char *s) \
	{ \
		while (*s) { \
			mtk_serial##port##_putc(*s++); \
		} \
	}

/* Serial device descriptor */
#define INIT_HSUART_STRUCTURE(port, __name) {	\
	.name	= __name,			\
	.start	= mtk_serial##port##_init,	\
	.stop	= NULL,				\
	.setbrg	= mtk_serial##port##_setbrg,	\
	.getc	= mtk_serial##port##_getc,	\
	.tstc	= mtk_serial##port##_tstc,	\
	.putc	= mtk_serial##port##_putc,	\
	.puts	= mtk_serial##port##_puts,	\
}

#define DECLARE_HSUART(port, __name) \
	DECLARE_HSUART_PRIV(port); \
	DECLARE_HSUART_FUNCTIONS(port); \
	struct serial_device mtk_hsuart##port##_device = \
		INIT_HSUART_STRUCTURE(port, __name);

#if !defined(CONFIG_CONS_INDEX)
#elif (CONFIG_CONS_INDEX < 1) || (CONFIG_CONS_INDEX > 6)
#error	"Invalid console index value."
#endif

#if CONFIG_CONS_INDEX == 1 && !defined(CONFIG_SYS_NS16550_COM1)
#error	"Console port 1 defined but not configured."
#elif CONFIG_CONS_INDEX == 2 && !defined(CONFIG_SYS_NS16550_COM2)
#error	"Console port 2 defined but not configured."
#elif CONFIG_CONS_INDEX == 3 && !defined(CONFIG_SYS_NS16550_COM3)
#error	"Console port 3 defined but not configured."
#elif CONFIG_CONS_INDEX == 4 && !defined(CONFIG_SYS_NS16550_COM4)
#error	"Console port 4 defined but not configured."
#elif CONFIG_CONS_INDEX == 5 && !defined(CONFIG_SYS_NS16550_COM5)
#error	"Console port 5 defined but not configured."
#elif CONFIG_CONS_INDEX == 6 && !defined(CONFIG_SYS_NS16550_COM6)
#error	"Console port 6 defined but not configured."
#endif

#if defined(CONFIG_SYS_NS16550_COM1)
DECLARE_HSUART(1, "mtk-hsuart0");
#endif
#if defined(CONFIG_SYS_NS16550_COM2)
DECLARE_HSUART(2, "mtk-hsuart1");
#endif
#if defined(CONFIG_SYS_NS16550_COM3)
DECLARE_HSUART(3, "mtk-hsuart2");
#endif
#if defined(CONFIG_SYS_NS16550_COM4)
DECLARE_HSUART(4, "mtk-hsuart3");
#endif
#if defined(CONFIG_SYS_NS16550_COM5)
DECLARE_HSUART(5, "mtk-hsuart4");
#endif
#if defined(CONFIG_SYS_NS16550_COM6)
DECLARE_HSUART(6, "mtk-hsuart5");
#endif

__weak struct serial_device *default_serial_console(void)
{
#if CONFIG_CONS_INDEX == 1
	return &mtk_hsuart1_device;
#elif CONFIG_CONS_INDEX == 2
	return &mtk_hsuart2_device;
#elif CONFIG_CONS_INDEX == 3
	return &mtk_hsuart3_device;
#elif CONFIG_CONS_INDEX == 4
	return &mtk_hsuart4_device;
#elif CONFIG_CONS_INDEX == 5
	return &mtk_hsuart5_device;
#elif CONFIG_CONS_INDEX == 6
	return &mtk_hsuart6_device;
#else
#error "Bad CONFIG_CONS_INDEX."
#endif
}

void mtk_serial_initialize(void)
{
#if defined(CONFIG_SYS_NS16550_COM1)
	serial_register(&mtk_hsuart1_device);
#endif
#if defined(CONFIG_SYS_NS16550_COM2)
	serial_register(&mtk_hsuart2_device);
#endif
#if defined(CONFIG_SYS_NS16550_COM3)
	serial_register(&mtk_hsuart3_device);
#endif
#if defined(CONFIG_SYS_NS16550_COM4)
	serial_register(&mtk_hsuart4_device);
#endif
#if defined(CONFIG_SYS_NS16550_COM5)
	serial_register(&mtk_hsuart5_device);
#endif
#if defined(CONFIG_SYS_NS16550_COM6)
	serial_register(&mtk_hsuart6_device);
#endif
}

#endif

#ifdef CONFIG_DEBUG_UART_MTK

#include <debug_uart.h>

static inline void _debug_uart_init(void)
{
	struct mtk_serial_priv priv;

	priv.regs = (void *) CONFIG_DEBUG_UART_BASE;
	priv.clock = CONFIG_DEBUG_UART_CLOCK;

	writel(0, &priv.regs->ier);
	writel(UART_MCRVAL, &priv.regs->mcr);
	writel(UART_FCRVAL, &priv.regs->fcr);

	_mtk_serial_setbrg(&priv, CONFIG_BAUDRATE);
}

static inline void _debug_uart_putc(int ch)
{
	struct mtk_serial_regs __iomem *regs =
		(void *) CONFIG_DEBUG_UART_BASE;

	while (!(readl(&regs->lsr) & UART_LSR_THRE))
		;

	writel(ch, &regs->thr);
}

DEBUG_UART_FUNCS

#endif
