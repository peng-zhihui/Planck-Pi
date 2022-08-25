// SPDX-License-Identifier: GPL-2.0
/*
 * board/renesas/stout/stout.c
 *     This file is Stout board support.
 *
 * Copyright (C) 2015 Renesas Electronics Europe GmbH
 * Copyright (C) 2015 Renesas Electronics Corporation
 * Copyright (C) 2015 Cogent Embedded, Inc.
 */

#include <common.h>
#include <env.h>
#include <init.h>
#include <malloc.h>
#include <netdev.h>
#include <dm.h>
#include <dm/platform_data/serial_sh.h>
#include <env_internal.h>
#include <asm/processor.h>
#include <asm/mach-types.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <asm/arch/sys_proto.h>
#include <asm/gpio.h>
#include <asm/arch/rmobile.h>
#include <asm/arch/rcar-mstp.h>
#include <asm/arch/mmc.h>
#include <asm/arch/sh_sdhi.h>
#include <miiphy.h>
#include <i2c.h>
#include <mmc.h>
#include "qos.h"
#include "cpld.h"

DECLARE_GLOBAL_DATA_PTR;

#define CLK2MHZ(clk)	(clk / 1000 / 1000)
void s_init(void)
{
	struct rcar_rwdt *rwdt = (struct rcar_rwdt *)RWDT_BASE;
	struct rcar_swdt *swdt = (struct rcar_swdt *)SWDT_BASE;

	/* Watchdog init */
	writel(0xA5A5A500, &rwdt->rwtcsra);
	writel(0xA5A5A500, &swdt->swtcsra);

	/* CPU frequency setting. Set to 1.4GHz */
	if (rmobile_get_cpu_rev_integer() >= R8A7790_CUT_ES2X) {
		u32 stat = 0;
		u32 stc = ((1400 / CLK2MHZ(CONFIG_SYS_CLK_FREQ)) - 1)
			<< PLL0_STC_BIT;
		clrsetbits_le32(PLL0CR, PLL0_STC_MASK, stc);

		do {
			stat = readl(PLLECR) & PLL0ST;
		} while (stat == 0x0);
	}

	/* QoS(Quality-of-Service) Init */
	qos_init();
}

#define TMU0_MSTP125	BIT(25)

#define SD2CKCR		0xE6150078
#define SD2_97500KHZ	0x7

int board_early_init_f(void)
{
	/* TMU0 */
	mstp_clrbits_le32(MSTPSR1, SMSTPCR1, TMU0_MSTP125);

	/*
	 * SD0 clock is set to 97.5MHz by default.
	 * Set SD2 to the 97.5MHz as well.
	 */
	writel(SD2_97500KHZ, SD2CKCR);

	return 0;
}

#define ETHERNET_PHY_RESET	123	/* GPIO 3 31 */

int board_init(void)
{
	/* adress of boot parameters */
	gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + 0x100;

	cpld_init();

	/* Force ethernet PHY out of reset */
	gpio_request(ETHERNET_PHY_RESET, "phy_reset");
	gpio_direction_output(ETHERNET_PHY_RESET, 0);
	mdelay(20);
	gpio_direction_output(ETHERNET_PHY_RESET, 1);

	return 0;
}

int dram_init(void)
{
	if (fdtdec_setup_mem_size_base() != 0)
		return -EINVAL;

	return 0;
}

int dram_init_banksize(void)
{
	fdtdec_setup_memory_banksize();

	return 0;
}

/* Stout has KSZ8041NL/RNL */
#define PHY_CONTROL1		0x1E
#define PHY_LED_MODE		0xC000
#define PHY_LED_MODE_ACK	0x4000
int board_phy_config(struct phy_device *phydev)
{
	int ret = phy_read(phydev, MDIO_DEVAD_NONE, PHY_CONTROL1);
	ret &= ~PHY_LED_MODE;
	ret |= PHY_LED_MODE_ACK;
	ret = phy_write(phydev, MDIO_DEVAD_NONE, PHY_CONTROL1, (u16)ret);

	return 0;
}

enum env_location env_get_location(enum env_operation op, int prio)
{
	const u32 load_magic = 0xb33fc0de;

	/* Block environment access if loaded using JTAG */
	if ((readl(CONFIG_SPL_TEXT_BASE + 0x24) == load_magic) &&
	    (op != ENVOP_INIT))
		return ENVL_UNKNOWN;

	if (prio)
		return ENVL_UNKNOWN;

	return ENVL_SPI_FLASH;
}
