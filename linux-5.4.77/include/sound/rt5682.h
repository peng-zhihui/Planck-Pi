/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/sound/rt5682.h -- Platform data for RT5682
 *
 * Copyright 2018 Realtek Microelectronics
 */

#ifndef __LINUX_SND_RT5682_H
#define __LINUX_SND_RT5682_H

enum rt5682_dmic1_data_pin {
	RT5682_DMIC1_NULL,
	RT5682_DMIC1_DATA_GPIO2,
	RT5682_DMIC1_DATA_GPIO5,
};

enum rt5682_dmic1_clk_pin {
	RT5682_DMIC1_CLK_GPIO1,
	RT5682_DMIC1_CLK_GPIO3,
};

enum rt5682_jd_src {
	RT5682_JD_NULL,
	RT5682_JD1,
};

struct rt5682_platform_data {

	int ldo1_en; /* GPIO for LDO1_EN */

	enum rt5682_dmic1_data_pin dmic1_data_pin;
	enum rt5682_dmic1_clk_pin dmic1_clk_pin;
	enum rt5682_jd_src jd_src;
};

#endif

