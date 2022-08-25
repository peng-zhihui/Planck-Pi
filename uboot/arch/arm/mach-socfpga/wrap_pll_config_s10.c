// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016-2018 Intel Corporation <www.intel.com>
 *
 */

#include <common.h>
#include <asm/arch/clock_manager.h>
#include <asm/io.h>
#include <asm/arch/handoff_s10.h>
#include <asm/arch/system_manager.h>

const struct cm_config * const cm_get_default_config(void)
{
	struct cm_config *cm_handoff_cfg = (struct cm_config *)
		(S10_HANDOFF_CLOCK + S10_HANDOFF_OFFSET_DATA);
	u32 *conversion = (u32 *)cm_handoff_cfg;
	u32 i;
	u32 handoff_clk = readl(S10_HANDOFF_CLOCK);

	if (swab32(handoff_clk) == S10_HANDOFF_MAGIC_CLOCK) {
		writel(swab32(handoff_clk), S10_HANDOFF_CLOCK);
		for (i = 0; i < (sizeof(*cm_handoff_cfg) / sizeof(u32)); i++)
			conversion[i] = swab32(conversion[i]);
		return cm_handoff_cfg;
	} else if (handoff_clk == S10_HANDOFF_MAGIC_CLOCK) {
		return cm_handoff_cfg;
	}

	return NULL;
}

const unsigned int cm_get_osc_clk_hz(void)
{
#ifdef CONFIG_SPL_BUILD

	u32 clock = readl(HANDOFF_CLOCK_OSC);

	writel(clock,
	       socfpga_get_sysmgr_addr() + SYSMGR_SOC64_BOOT_SCRATCH_COLD1);
#endif
	return readl(socfpga_get_sysmgr_addr() +
		     SYSMGR_SOC64_BOOT_SCRATCH_COLD1);
}

const unsigned int cm_get_intosc_clk_hz(void)
{
	return CLKMGR_INTOSC_HZ;
}

const unsigned int cm_get_fpga_clk_hz(void)
{
#ifdef CONFIG_SPL_BUILD
	u32 clock = readl(HANDOFF_CLOCK_FPGA);

	writel(clock,
	       socfpga_get_sysmgr_addr() + SYSMGR_SOC64_BOOT_SCRATCH_COLD2);
#endif
	return readl(socfpga_get_sysmgr_addr() +
		     SYSMGR_SOC64_BOOT_SCRATCH_COLD2);
}
