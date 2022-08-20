// SPDX-License-Identifier: GPL-2.0
/*
 * board/renesas/lager/lager_spl.c
 *
 * Copyright (C) 2018 Marek Vasut <marek.vasut@gmail.com>
 */

#include <common.h>
#include <cpu_func.h>
#include <init.h>
#include <malloc.h>
#include <dm/platform_data/serial_sh.h>
#include <asm/processor.h>
#include <asm/mach-types.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <asm/arch/sys_proto.h>
#include <asm/gpio.h>
#include <asm/arch/rmobile.h>
#include <asm/arch/rcar-mstp.h>

#include <spl.h>

#define TMU0_MSTP125	BIT(25)
#define SCIF0_MSTP721	BIT(21)
#define QSPI_MSTP917	BIT(17)

#define SD2CKCR		0xE615026C
#define SD_97500KHZ	0x7

struct reg_config {
	u16	off;
	u32	val;
};

static void dbsc_wait(u16 reg)
{
	static const u32 dbsc3_0_base = DBSC3_0_BASE;

	while (!(readl(dbsc3_0_base + reg) & BIT(0)))
		;
}

static void spl_init_sys(void)
{
	u32 r0 = 0;

	writel(0xa5a5a500, 0xe6020004);
	writel(0xa5a5a500, 0xe6030004);

	asm volatile(
		/* ICIALLU - Invalidate I$ to PoU */
		"mcr	15, 0, %0, cr7, cr5, 0	\n"
		/* BPIALL - Invalidate branch predictors */
		"mcr	15, 0, %0, cr7, cr5, 6	\n"
		/* Set SCTLR[IZ] */
		"mrc	15, 0, %0, cr1, cr0, 0	\n"
		"orr	%0, #0x1800		\n"
		"mcr	15, 0, %0, cr1, cr0, 0	\n"
		"isb	sy			\n"
		:"=r"(r0));
}

static void spl_init_pfc(void)
{
	static const struct reg_config pfc_with_unlock[] = {
		{ 0x0090, 0x00000000 },
		{ 0x0094, 0x00000000 },
		{ 0x0098, 0xc0000000 },
		{ 0x0020, 0x00000000 },
		{ 0x0024, 0x00000000 },
		{ 0x0028, 0x00000000 },
		{ 0x002c, 0x20000000 },
		{ 0x0030, 0x00001249 },
		{ 0x0034, 0x00000278 },
		{ 0x0038, 0x00000841 },
		{ 0x003c, 0x00000000 },
		{ 0x0040, 0x00000000 },
		{ 0x0044, 0x10000000 },
		{ 0x0048, 0x00000001 },
		{ 0x004c, 0x0004aab0 },
		{ 0x0050, 0x37301b00 },
		{ 0x0054, 0x00048da3 },
		{ 0x0058, 0x089044a1 },
		{ 0x005c, 0x2a3a55b4 },
		{ 0x0160, 0x00000003 },
		{ 0x0004, 0xffffffff },
		{ 0x0008, 0x2aef3fff },
		{ 0x000c, 0x3fffffff },
		{ 0x0010, 0xff7fc07f },
		{ 0x0014, 0x7f3ff3f8 },
		{ 0x0018, 0x1cfdfff7 },
	};

	static const struct reg_config pfc_without_unlock[] = {
		{ 0x0100, 0x1fffffff },
		{ 0x0104, 0xffff0318 },
		{ 0x0108, 0x387fffe1 },
		{ 0x010c, 0x00803f80 },
		{ 0x0110, 0x1520009f },
		{ 0x0114, 0x00000000 },
		{ 0x0118, 0x00000000 },
	};

	static const u32 pfc_base = 0xe6060000;

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pfc_with_unlock); i++) {
		writel(~pfc_with_unlock[i].val, pfc_base);
		writel(pfc_with_unlock[i].val,
		       pfc_base | pfc_with_unlock[i].off);
	}

	for (i = 0; i < ARRAY_SIZE(pfc_without_unlock); i++)
		writel(pfc_without_unlock[i].val,
		       pfc_base | pfc_without_unlock[i].off);
}

static void spl_init_gpio(void)
{
	static const u16 gpio_offs[] = {
		0x1000, 0x3000, 0x4000, 0x5000
	};

	static const struct reg_config gpio_set[] = {
		{ 0x4000, 0x00c00000 },
		{ 0x5000, 0x63020000 },
	};

	static const struct reg_config gpio_clr[] = {
		{ 0x1000, 0x00000000 },
		{ 0x3000, 0x00000000 },
		{ 0x4000, 0x00c00000 },
		{ 0x5000, 0xe3020000 },
	};

	static const u32 gpio_base = 0xe6050000;

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gpio_offs); i++)
		writel(0, gpio_base | 0x20 | gpio_offs[i]);

	for (i = 0; i < ARRAY_SIZE(gpio_offs); i++)
		writel(0, gpio_base | 0x00 | gpio_offs[i]);

	for (i = 0; i < ARRAY_SIZE(gpio_set); i++)
		writel(gpio_set[i].val, gpio_base | 0x08 | gpio_set[i].off);

	for (i = 0; i < ARRAY_SIZE(gpio_clr); i++)
		writel(gpio_clr[i].val, gpio_base | 0x04 | gpio_clr[i].off);
}

static void spl_init_lbsc(void)
{
	static const struct reg_config lbsc_config[] = {
		{ 0x00, 0x00000020 },
		{ 0x08, 0x00002020 },
		{ 0x30, 0x02150326 },
		{ 0x38, 0x077f077f },
	};

	static const u16 lbsc_offs[] = {
		0x80, 0x84, 0x88, 0x8c, 0xa0, 0xc0, 0xc4, 0xc8, 0x180
	};

	static const u32 lbsc_base = 0xfec00200;

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(lbsc_config); i++) {
		writel(lbsc_config[i].val,
		       lbsc_base | lbsc_config[i].off);
		writel(lbsc_config[i].val,
		       lbsc_base | (lbsc_config[i].off + 4));
	}

	for (i = 0; i < ARRAY_SIZE(lbsc_offs); i++)
		writel(0, lbsc_base | lbsc_offs[i]);
}

static void spl_init_dbsc(void)
{
	static const struct reg_config dbsc_config1[] = {
		{ 0x0018, 0x21000000 },
		{ 0x0018, 0x11000000 },
		{ 0x0018, 0x10000000 },
		{ 0x0280, 0x0000a55a },
		{ 0x0290, 0x00000001 },
		{ 0x02a0, 0x80000000 },
		{ 0x0290, 0x00000004 },
	};

	static const struct reg_config dbsc_config4[] = {
		{ 0x0290, 0x00000010 },
		{ 0x02a0, 0xf004649b },
		{ 0x0290, 0x0000000f },
		{ 0x02a0, 0x00181ee4 },
		{ 0x0290, 0x00000060 },
		{ 0x02a0, 0x330657b2 },
		{ 0x0290, 0x00000001 },
		{ 0x02a0, 0x00000071 },
		{ 0x0020, 0x00000007 },
		{ 0x0024, 0x10030a02 },
		{ 0x0030, 0x00000001 },
		{ 0x00b0, 0x00000000 },
		{ 0x0040, 0x0000000b },
		{ 0x0044, 0x00000008 },
		{ 0x0048, 0x00000000 },
		{ 0x0050, 0x0000000b },
		{ 0x0054, 0x000c000b },
		{ 0x0058, 0x00000027 },
		{ 0x005c, 0x0000001c },
		{ 0x0060, 0x00000005 },
		{ 0x0064, 0x00000018 },
		{ 0x0068, 0x00000008 },
		{ 0x006c, 0x0000000c },
		{ 0x0070, 0x00000009 },
		{ 0x0074, 0x00000012 },
		{ 0x0078, 0x000000d0 },
		{ 0x007c, 0x00140005 },
		{ 0x0080, 0x00050004 },
		{ 0x0084, 0x70233005 },
		{ 0x0088, 0x000c0000 },
		{ 0x008c, 0x00000300 },
		{ 0x0090, 0x00000040 },
		{ 0x0100, 0x00000001 },
		{ 0x00c0, 0x00020001 },
		{ 0x00c8, 0x20082008 },
		{ 0x0380, 0x00020002 },
		{ 0x0390, 0x0000000f },
	};

	static const struct reg_config dbsc_config5[] = {
		{ 0x0244, 0x00000011 },
		{ 0x0290, 0x00000006 },
		{ 0x02a0, 0x0005c000 },
		{ 0x0290, 0x00000003 },
		{ 0x02a0, 0x0300c481 },
		{ 0x0290, 0x00000023 },
		{ 0x02a0, 0x00fdb6c0 },
		{ 0x0290, 0x00000011 },
		{ 0x02a0, 0x1000040b },
		{ 0x0290, 0x00000012 },
		{ 0x02a0, 0x9d5cbb66 },
		{ 0x0290, 0x00000013 },
		{ 0x02a0, 0x1a868300 },
		{ 0x0290, 0x00000014 },
		{ 0x02a0, 0x300214d8 },
		{ 0x0290, 0x00000015 },
		{ 0x02a0, 0x00000d70 },
		{ 0x0290, 0x00000016 },
		{ 0x02a0, 0x00000006 },
		{ 0x0290, 0x00000017 },
		{ 0x02a0, 0x00000018 },
		{ 0x0290, 0x0000001a },
		{ 0x02a0, 0x910035c7 },
		{ 0x0290, 0x00000004 },
	};

	static const struct reg_config dbsc_config6[] = {
		{ 0x0290, 0x00000001 },
		{ 0x02a0, 0x00000181 },
		{ 0x0018, 0x11000000 },
		{ 0x0290, 0x00000004 },
	};

	static const struct reg_config dbsc_config7[] = {
		{ 0x0290, 0x00000001 },
		{ 0x02a0, 0x0000fe01 },
		{ 0x0290, 0x00000004 },
	};

	static const struct reg_config dbsc_config8[] = {
		{ 0x0304, 0x00000000 },
		{ 0x00f4, 0x01004c20 },
		{ 0x00f8, 0x014000aa },
		{ 0x00e0, 0x00000140 },
		{ 0x00e4, 0x00081860 },
		{ 0x00e8, 0x00010000 },
		{ 0x0014, 0x00000001 },
		{ 0x0010, 0x00000001 },
		{ 0x0280, 0x00000000 },
	};

	static const u32 dbsc3_0_base = DBSC3_0_BASE;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dbsc_config1); i++)
		writel(dbsc_config1[i].val, dbsc3_0_base | dbsc_config1[i].off);

	dbsc_wait(0x2a0);

	for (i = 0; i < ARRAY_SIZE(dbsc_config4); i++)
		writel(dbsc_config4[i].val, dbsc3_0_base | dbsc_config4[i].off);

	dbsc_wait(0x240);

	for (i = 0; i < ARRAY_SIZE(dbsc_config5); i++)
		writel(dbsc_config5[i].val, dbsc3_0_base | dbsc_config5[i].off);

	dbsc_wait(0x2a0);

	for (i = 0; i < ARRAY_SIZE(dbsc_config6); i++)
		writel(dbsc_config6[i].val, dbsc3_0_base | dbsc_config6[i].off);

	dbsc_wait(0x2a0);

	for (i = 0; i < ARRAY_SIZE(dbsc_config7); i++)
		writel(dbsc_config7[i].val, dbsc3_0_base | dbsc_config7[i].off);

	dbsc_wait(0x2a0);

	for (i = 0; i < ARRAY_SIZE(dbsc_config8); i++)
		writel(dbsc_config8[i].val, dbsc3_0_base | dbsc_config8[i].off);

}

static void spl_init_qspi(void)
{
	mstp_clrbits_le32(MSTPSR9, SMSTPCR9, QSPI_MSTP917);

	static const u32 qspi_base = 0xe6b10000;

	writeb(0x08, qspi_base + 0x00);
	writeb(0x00, qspi_base + 0x01);
	writeb(0x06, qspi_base + 0x02);
	writeb(0x01, qspi_base + 0x0a);
	writeb(0x00, qspi_base + 0x0b);
	writeb(0x00, qspi_base + 0x0c);
	writeb(0x00, qspi_base + 0x0d);
	writeb(0x00, qspi_base + 0x0e);

	writew(0xe080, qspi_base + 0x10);

	writeb(0xc0, qspi_base + 0x18);
	writeb(0x00, qspi_base + 0x18);
	writeb(0x00, qspi_base + 0x08);
	writeb(0x48, qspi_base + 0x00);
}

void board_init_f(ulong dummy)
{
	mstp_clrbits_le32(MSTPSR1, SMSTPCR1, TMU0_MSTP125);
	mstp_clrbits_le32(MSTPSR7, SMSTPCR7, SCIF0_MSTP721);

	/*
	 * SD0 clock is set to 97.5MHz by default.
	 * Set SD2 to the 97.5MHz as well.
	 */
	writel(SD_97500KHZ, SD2CKCR);

	spl_init_sys();
	spl_init_pfc();
	spl_init_gpio();
	spl_init_lbsc();
	spl_init_dbsc();
	spl_init_qspi();
}

void spl_board_init(void)
{
	/* UART clocks enabled and gd valid - init serial console */
	preloader_console_init();
}

void board_boot_order(u32 *spl_boot_list)
{
	const u32 jtag_magic = 0x1337c0de;
	const u32 load_magic = 0xb33fc0de;

	/*
	 * If JTAG probe sets special word at 0xe6300020, then it must
	 * put U-Boot into RAM and SPL will start it from RAM.
	 */
	if (readl(CONFIG_SPL_TEXT_BASE + 0x20) == jtag_magic) {
		printf("JTAG boot detected!\n");

		while (readl(CONFIG_SPL_TEXT_BASE + 0x24) != load_magic)
			;

		spl_boot_list[0] = BOOT_DEVICE_RAM;
		spl_boot_list[1] = BOOT_DEVICE_NONE;

		return;
	}

	/* Boot from SPI NOR with YMODEM UART fallback. */
	spl_boot_list[0] = BOOT_DEVICE_SPI;
	spl_boot_list[1] = BOOT_DEVICE_UART;
	spl_boot_list[2] = BOOT_DEVICE_NONE;
}

void reset_cpu(ulong addr)
{
}
