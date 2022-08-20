// SPDX-License-Identifier: GPL-2.0+
/*
 * XEA iMX28 board
 *
 * Copyright (C) 2019 DENX Software Engineering
 * Lukasz Majewski, DENX Software Engineering, lukma@denx.de
 *
 * Copyright (C) 2018 DENX Software Engineering
 * Måns Rullgård, DENX Software Engineering, mans@mansr.com
 *
 * Copyright (C) 2011 Marek Vasut <marek.vasut@gmail.com>
 * on behalf of DENX Software Engineering GmbH
 *
 */

#include <common.h>
#include <fdt_support.h>
#include <init.h>
#include <log.h>
#include <net.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/iomux-mx28.h>
#include <asm/arch/clock.h>
#include <asm/arch/sys_proto.h>
#include <linux/delay.h>
#include <linux/mii.h>
#include <miiphy.h>
#include <netdev.h>
#include <errno.h>
#include <usb.h>
#include <serial.h>

#ifdef CONFIG_SPL_BUILD
#include <spl.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

/*
 * Functions
 */

static void init_clocks(void)
{
	/* IO0 clock at 480MHz */
	mxs_set_ioclk(MXC_IOCLK0, 480000);
	/* IO1 clock at 480MHz */
	mxs_set_ioclk(MXC_IOCLK1, 480000);

	/* SSP0 clock at 96MHz */
	mxs_set_sspclk(MXC_SSPCLK0, 96000, 0);
	/* SSP2 clock at 160MHz */
	mxs_set_sspclk(MXC_SSPCLK2, 160000, 0);
	/* SSP3 clock at 96MHz */
	mxs_set_sspclk(MXC_SSPCLK3, 96000, 0);
}

#ifdef CONFIG_SPL_BUILD
void board_init_f(ulong arg)
{
	init_clocks();
	preloader_console_init();
}

static int boot_tiva0, boot_tiva1;

/* Check if TIVAs request booting via U-Boot proper */
void spl_board_init(void)
{
	struct gpio_desc btiva0, btiva1, en_3_3v;
	int ret;

	/*
	 * Setup GPIO0_0 (TIVA power enable pin) to be output high
	 * to allow TIVA startup.
	 */
	ret = dm_gpio_lookup_name("GPIO0_0", &en_3_3v);
	if (ret)
		printf("Cannot get GPIO0_0\n");

	ret = dm_gpio_request(&en_3_3v, "pwr_3_3v");
	if (ret)
		printf("Cannot request GPIO0_0\n");

	/* Set GPIO0_0 to HIGH */
	dm_gpio_set_dir_flags(&en_3_3v, GPIOD_IS_OUT | GPIOD_IS_OUT_ACTIVE);

	ret = dm_gpio_lookup_name("GPIO0_23", &btiva0);
	if (ret)
		printf("Cannot get GPIO0_23\n");

	ret = dm_gpio_lookup_name("GPIO0_25", &btiva1);
	if (ret)
		printf("Cannot get GPIO0_25\n");

	ret = dm_gpio_request(&btiva0, "boot-tiva0");
	if (ret)
		printf("Cannot request GPIO0_23\n");

	ret = dm_gpio_request(&btiva1, "boot-tiva1");
	if (ret)
		printf("Cannot request GPIO0_25\n");

	dm_gpio_set_dir_flags(&btiva0, GPIOD_IS_IN);
	dm_gpio_set_dir_flags(&btiva1, GPIOD_IS_IN);

	udelay(1000);

	boot_tiva0 = dm_gpio_get_value(&btiva0);
	boot_tiva1 = dm_gpio_get_value(&btiva1);
}

void board_boot_order(u32 *spl_boot_list)
{
	spl_boot_list[0] = BOOT_DEVICE_MMC1;
	spl_boot_list[1] = BOOT_DEVICE_SPI;
}

int spl_start_uboot(void)
{
	/* break into full u-boot on 'c' */
	if (serial_tstc() && serial_getc() == 'c')
		return 1;

	debug("%s: btiva0: %d btiva1: %d\n", __func__, boot_tiva0, boot_tiva1);
	return !boot_tiva0 || !boot_tiva1;
}
#else

int board_early_init_f(void)
{
	init_clocks();

	return 0;
}

int board_init(void)
{
	struct gpio_desc phy_rst;
	int ret;

	/* Address of boot parameters */
	gd->bd->bi_boot_params = PHYS_SDRAM_1 + 0x100;

	cpu_eth_init(NULL);

	/* PHY INT#/PWDN# */
	ret = dm_gpio_lookup_name("GPIO4_13", &phy_rst);
	if (ret) {
		printf("Cannot get GPIO4_13\n");
		return ret;
	}

	ret = dm_gpio_request(&phy_rst, "phy-rst");
	if (ret) {
		printf("Cannot request GPIO4_13\n");
		return ret;
	}

	dm_gpio_set_dir_flags(&phy_rst, GPIOD_IS_IN);
	udelay(1000);

	return 0;
}

int dram_init(void)
{
	return mxs_dram_init();
}

#ifdef CONFIG_OF_BOARD_SETUP
static int fdt_fixup_l2switch(void *blob)
{
	u8 ethaddr[6];
	int ret;

	if (eth_env_get_enetaddr("ethaddr", ethaddr)) {
		ret = fdt_find_and_setprop(blob,
					   "/ahb@80080000/switch@800f0000",
					   "local-mac-address", ethaddr, 6, 1);
		if (ret < 0)
			printf("%s: can't find usbether@1 node: %d\n",
			       __func__, ret);
	}

	return 0;
}

int ft_board_setup(void *blob, bd_t *bd)
{
	/*
	 * i.MX28 L2 switch needs manual update (fixup) of eth MAC address
	 * (in 'local-mac-address' property) as it uses "switch@800f0000"
	 * node, not set by default FIT image handling code in
	 * "ethernet@800f0000"
	 */
	fdt_fixup_l2switch(blob);

	return 0;
}
#endif

#endif	/* CONFIG_SPL_BUILD */
