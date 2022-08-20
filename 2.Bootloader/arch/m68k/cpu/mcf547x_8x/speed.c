// SPDX-License-Identifier: GPL-2.0+
/*
 *
 * (C) Copyright 2000-2003
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * Copyright (C) 2004-2007 Freescale Semiconductor, Inc.
 * TsiChung Liew (Tsi-Chung.Liew@freescale.com)
 */

#include <common.h>
#include <clock_legacy.h>
#include <asm/processor.h>

#include <asm/immap.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * get_clocks() fills in gd->cpu_clock and gd->bus_clk
 */
int get_clocks(void)
{
	gd->bus_clk = CONFIG_SYS_CLK;
	gd->cpu_clk = (gd->bus_clk * 2);

#ifdef CONFIG_SYS_I2C_FSL
	gd->arch.i2c1_clk = gd->bus_clk;
#endif

	return (0);
}
