// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2012 Sascha Hauer, Pengutronix
 */

#include <linux/irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include "common.h"
#include "hardware.h"

static void __init imx25_init_early(void)
{
	mxc_set_cpu_type(MXC_CPU_MX25);
}

static void __init imx25_dt_init(void)
{
	imx_aips_allow_unprivileged_access("fsl,imx25-aips");
}

static void __init mx25_init_irq(void)
{
	struct device_node *np;
	void __iomem *avic_base;

	np = of_find_compatible_node(NULL, NULL, "fsl,avic");
	avic_base = of_iomap(np, 0);
	BUG_ON(!avic_base);
	mxc_init_irq(avic_base);
}

static const char * const imx25_dt_board_compat[] __initconst = {
	"fsl,imx25",
	NULL
};

DT_MACHINE_START(IMX25_DT, "Freescale i.MX25 (Device Tree Support)")
	.init_early	= imx25_init_early,
	.init_machine	= imx25_dt_init,
	.init_late      = imx25_pm_init,
	.init_irq	= mx25_init_irq,
	.dt_compat	= imx25_dt_board_compat,
MACHINE_END
