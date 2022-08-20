// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2016 - 2018 Xilinx, Inc.
 * Michal Simek <michal.simek@xilinx.com>
 */

#include <common.h>
#include <init.h>
#include <time.h>

DECLARE_GLOBAL_DATA_PTR;

#ifdef CONFIG_CLOCKS
/**
 * set_cpu_clk_info - Initialize clock framework
 *
 * Return: 0 always.
 *
 * This function is called from common code after relocation and sets up the
 * clock framework. The framework must not be used before this function had been
 * called.
 */
int set_cpu_clk_info(void)
{
	gd->cpu_clk = get_tbclk();

	gd->bd->bi_arm_freq = gd->cpu_clk / 1000000;
	gd->bd->bi_dsp_freq = 0;

	return 0;
}
#endif
