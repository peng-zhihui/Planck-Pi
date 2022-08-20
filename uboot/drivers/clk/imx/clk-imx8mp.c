// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 NXP
 * Peng Fan <peng.fan@nxp.com>
 */

#include <common.h>
#include <clk.h>
#include <clk-uclass.h>
#include <dm.h>
#include <log.h>
#include <asm/arch/clock.h>
#include <asm/arch/imx-regs.h>
#include <dt-bindings/clock/imx8mp-clock.h>

#include "clk.h"

#define PLL_1416X_RATE(_rate, _m, _p, _s)		\
	{						\
		.rate	=	(_rate),		\
		.mdiv	=	(_m),			\
		.pdiv	=	(_p),			\
		.sdiv	=	(_s),			\
	}

#define PLL_1443X_RATE(_rate, _m, _p, _s, _k)		\
	{						\
		.rate	=	(_rate),		\
		.mdiv	=	(_m),			\
		.pdiv	=	(_p),			\
		.sdiv	=	(_s),			\
		.kdiv	=	(_k),			\
	}

static const struct imx_pll14xx_rate_table imx8mp_pll1416x_tbl[] = {
	PLL_1416X_RATE(1800000000U, 225, 3, 0),
	PLL_1416X_RATE(1600000000U, 200, 3, 0),
	PLL_1416X_RATE(1200000000U, 300, 3, 1),
	PLL_1416X_RATE(1000000000U, 250, 3, 1),
	PLL_1416X_RATE(800000000U,  200, 3, 1),
	PLL_1416X_RATE(750000000U,  250, 2, 2),
	PLL_1416X_RATE(700000000U,  350, 3, 2),
	PLL_1416X_RATE(600000000U,  300, 3, 2),
};

static const struct imx_pll14xx_rate_table imx8mp_drampll_tbl[] = {
	PLL_1443X_RATE(650000000U, 325, 3, 2, 0),
};

static struct imx_pll14xx_clk imx8mp_dram_pll __initdata = {
		.type = PLL_1443X,
		.rate_table = imx8mp_drampll_tbl,
		.rate_count = ARRAY_SIZE(imx8mp_drampll_tbl),
};

static struct imx_pll14xx_clk imx8mp_arm_pll __initdata = {
		.type = PLL_1416X,
		.rate_table = imx8mp_pll1416x_tbl,
		.rate_count = ARRAY_SIZE(imx8mp_pll1416x_tbl),
};

static struct imx_pll14xx_clk imx8mp_sys_pll __initdata = {
		.type = PLL_1416X,
		.rate_table = imx8mp_pll1416x_tbl,
		.rate_count = ARRAY_SIZE(imx8mp_pll1416x_tbl),
};

static const char *pll_ref_sels[] = { "clock-osc-24m", "dummy", "dummy", "dummy", };
static const char *dram_pll_bypass_sels[] = {"dram_pll", "dram_pll_ref_sel", };
static const char *arm_pll_bypass_sels[] = {"arm_pll", "arm_pll_ref_sel", };
static const char *sys_pll1_bypass_sels[] = {"sys_pll1", "sys_pll1_ref_sel", };
static const char *sys_pll2_bypass_sels[] = {"sys_pll2", "sys_pll2_ref_sel", };
static const char *sys_pll3_bypass_sels[] = {"sys_pll3", "sys_pll3_ref_sel", };

static const char *imx8mp_a53_sels[] = {"clock-osc-24m", "arm_pll_out", "sys_pll2_500m",
					"sys_pll2_1000m", "sys_pll1_800m", "sys_pll1_400m",
					"audio_pll1_out", "sys_pll3_out", };

static const char *imx8mp_main_axi_sels[] = {"clock-osc-24m", "sys_pll2_333m", "sys_pll1_800m",
					     "sys_pll2_250m", "sys_pll2_1000m", "audio_pll1_out",
					     "video_pll1_out", "sys_pll1_100m",};

static const char *imx8mp_nand_usdhc_sels[] = {"clock-osc-24m", "sys_pll1_266m", "sys_pll1_800m",
					       "sys_pll2_200m", "sys_pll1_133m", "sys_pll3_out",
					       "sys_pll2_250m", "audio_pll1_out", };

static const char *imx8mp_noc_sels[] = {"clock-osc-24m", "sys_pll1_800m", "sys_pll3_out",
					"sys_pll2_1000m", "sys_pll2_500m", "audio_pll1_out",
					"video_pll1_out", "audio_pll2_out", };

static const char *imx8mp_noc_io_sels[] = {"clock-osc-24m", "sys_pll1_800m", "sys_pll3_out",
					   "sys_pll2_1000m", "sys_pll2_500m", "audio_pll1_out",
					   "video_pll1_out", "audio_pll2_out", };

static const char *imx8mp_ahb_sels[] = {"clock-osc-24m", "sys_pll1_133m", "sys_pll1_800m",
					"sys_pll1_400m", "sys_pll2_125m", "sys_pll3_out",
					"audio_pll1_out", "video_pll1_out", };

static const char *imx8mp_dram_alt_sels[] = {"clock-osc-24m", "sys_pll1_800m", "sys_pll1_100m",
					     "sys_pll2_500m", "sys_pll2_1000m", "sys_pll3_out",
					     "audio_pll1_out", "sys_pll1_266m", };

static const char *imx8mp_dram_apb_sels[] = {"clock-osc-24m", "sys_pll2_200m", "sys_pll1_40m",
					     "sys_pll1_160m", "sys_pll1_800m", "sys_pll3_out",
					     "sys_pll2_250m", "audio_pll2_out", };

static const char *imx8mp_i2c5_sels[] = {"clock-osc-24m", "sys_pll1_160m", "sys_pll2_50m",
					 "sys_pll3_out", "audio_pll1_out", "video_pll1_out",
					 "audio_pll2_out", "sys_pll1_133m", };

static const char *imx8mp_i2c6_sels[] = {"clock-osc-24m", "sys_pll1_160m", "sys_pll2_50m",
					 "sys_pll3_out", "audio_pll1_out", "video_pll1_out",
					 "audio_pll2_out", "sys_pll1_133m", };

static const char *imx8mp_usdhc1_sels[] = {"clock-osc-24m", "sys_pll1_400m", "sys_pll1_800m",
					   "sys_pll2_500m", "sys_pll3_out", "sys_pll1_266m",
					   "audio_pll2_out", "sys_pll1_100m", };

static const char *imx8mp_usdhc2_sels[] = {"clock-osc-24m", "sys_pll1_400m", "sys_pll1_800m",
					   "sys_pll2_500m", "sys_pll3_out", "sys_pll1_266m",
					   "audio_pll2_out", "sys_pll1_100m", };

static const char *imx8mp_i2c1_sels[] = {"clock-osc-24m", "sys_pll1_160m", "sys_pll2_50m",
					 "sys_pll3_out", "audio_pll1_out", "video_pll1_out",
					 "audio_pll2_out", "sys_pll1_133m", };

static const char *imx8mp_i2c2_sels[] = {"clock-osc-24m", "sys_pll1_160m", "sys_pll2_50m",
					 "sys_pll3_out", "audio_pll1_out", "video_pll1_out",
					 "audio_pll2_out", "sys_pll1_133m", };

static const char *imx8mp_i2c3_sels[] = {"clock-osc-24m", "sys_pll1_160m", "sys_pll2_50m",
					 "sys_pll3_out", "audio_pll1_out", "video_pll1_out",
					 "audio_pll2_out", "sys_pll1_133m", };

static const char *imx8mp_i2c4_sels[] = {"clock-osc-24m", "sys_pll1_160m", "sys_pll2_50m",
					 "sys_pll3_out", "audio_pll1_out", "video_pll1_out",
					 "audio_pll2_out", "sys_pll1_133m", };

static const char *imx8mp_uart1_sels[] = {"clock-osc-24m", "sys_pll1_80m", "sys_pll2_200m",
					  "sys_pll2_100m", "sys_pll3_out", "clk_ext2",
					  "clk_ext4", "audio_pll2_out", };

static const char *imx8mp_uart2_sels[] = {"clock-osc-24m", "sys_pll1_80m", "sys_pll2_200m",
					  "sys_pll2_100m", "sys_pll3_out", "clk_ext2",
					  "clk_ext3", "audio_pll2_out", };

static const char *imx8mp_uart3_sels[] = {"clock-osc-24m", "sys_pll1_80m", "sys_pll2_200m",
					  "sys_pll2_100m", "sys_pll3_out", "clk_ext2",
					  "clk_ext4", "audio_pll2_out", };

static const char *imx8mp_uart4_sels[] = {"clock-osc-24m", "sys_pll1_80m", "sys_pll2_200m",
					  "sys_pll2_100m", "sys_pll3_out", "clk_ext2",
					  "clk_ext3", "audio_pll2_out", };

static const char *imx8mp_gic_sels[] = {"clock-osc-24m", "sys_pll2_200m", "sys_pll1_40m",
					"sys_pll2_100m", "sys_pll1_800m",
					"sys_pll2_500m", "clk_ext4", "audio_pll2_out" };

static const char *imx8mp_wdog_sels[] = {"clock-osc-24m", "sys_pll1_133m", "sys_pll1_160m",
					 "vpu_pll_out", "sys_pll2_125m", "sys_pll3_out",
					 "sys_pll1_80m", "sys_pll2_166m" };

static const char *imx8mp_usdhc3_sels[] = {"clock-osc-24m", "sys_pll1_400m", "sys_pll1_800m",
					   "sys_pll2_500m", "sys_pll3_out", "sys_pll1_266m",
					   "audio_pll2_out", "sys_pll1_100m", };

static const char *imx8mp_dram_core_sels[] = {"dram_pll_out", "dram_alt_root", };


static ulong imx8mp_clk_get_rate(struct clk *clk)
{
	struct clk *c;
	int ret;

	debug("%s(#%lu)\n", __func__, clk->id);

	ret = clk_get_by_id(clk->id, &c);
	if (ret)
		return ret;

	return clk_get_rate(c);
}

static ulong imx8mp_clk_set_rate(struct clk *clk, unsigned long rate)
{
	struct clk *c;
	int ret;

	debug("%s(#%lu), rate: %lu\n", __func__, clk->id, rate);

	ret = clk_get_by_id(clk->id, &c);
	if (ret)
		return ret;

	return clk_set_rate(c, rate);
}

static int __imx8mp_clk_enable(struct clk *clk, bool enable)
{
	struct clk *c;
	int ret;

	debug("%s(#%lu) en: %d\n", __func__, clk->id, enable);

	ret = clk_get_by_id(clk->id, &c);
	if (ret)
		return ret;

	if (enable)
		ret = clk_enable(c);
	else
		ret = clk_disable(c);

	return ret;
}

static int imx8mp_clk_disable(struct clk *clk)
{
	return __imx8mp_clk_enable(clk, 0);
}

static int imx8mp_clk_enable(struct clk *clk)
{
	return __imx8mp_clk_enable(clk, 1);
}

static struct clk_ops imx8mp_clk_ops = {
	.set_rate = imx8mp_clk_set_rate,
	.get_rate = imx8mp_clk_get_rate,
	.enable = imx8mp_clk_enable,
	.disable = imx8mp_clk_disable,
};

static int imx8mp_clk_probe(struct udevice *dev)
{
	void __iomem *base;

	base = (void *)ANATOP_BASE_ADDR;

	clk_dm(IMX8MP_DRAM_PLL_REF_SEL, imx_clk_mux("dram_pll_ref_sel", base + 0x50, 0, 2, pll_ref_sels, ARRAY_SIZE(pll_ref_sels)));
	clk_dm(IMX8MP_ARM_PLL_REF_SEL, imx_clk_mux("arm_pll_ref_sel", base + 0x84, 0, 2, pll_ref_sels, ARRAY_SIZE(pll_ref_sels)));
	clk_dm(IMX8MP_SYS_PLL1_REF_SEL, imx_clk_mux("sys_pll1_ref_sel", base + 0x94, 0, 2, pll_ref_sels, ARRAY_SIZE(pll_ref_sels)));
	clk_dm(IMX8MP_SYS_PLL2_REF_SEL, imx_clk_mux("sys_pll2_ref_sel", base + 0x104, 0, 2, pll_ref_sels, ARRAY_SIZE(pll_ref_sels)));
	clk_dm(IMX8MP_SYS_PLL3_REF_SEL, imx_clk_mux("sys_pll3_ref_sel", base + 0x114, 0, 2, pll_ref_sels, ARRAY_SIZE(pll_ref_sels)));

	clk_dm(IMX8MP_DRAM_PLL, imx_clk_pll14xx("dram_pll", "dram_pll_ref_sel", base + 0x50, &imx8mp_dram_pll));
	clk_dm(IMX8MP_ARM_PLL, imx_clk_pll14xx("arm_pll", "arm_pll_ref_sel", base + 0x84, &imx8mp_arm_pll));
	clk_dm(IMX8MP_SYS_PLL1, imx_clk_pll14xx("sys_pll1", "sys_pll1_ref_sel", base + 0x94, &imx8mp_sys_pll));
	clk_dm(IMX8MP_SYS_PLL2, imx_clk_pll14xx("sys_pll2", "sys_pll2_ref_sel", base + 0x104, &imx8mp_sys_pll));
	clk_dm(IMX8MP_SYS_PLL3, imx_clk_pll14xx("sys_pll3", "sys_pll3_ref_sel", base + 0x114, &imx8mp_sys_pll));

	clk_dm(IMX8MP_DRAM_PLL_BYPASS, imx_clk_mux_flags("dram_pll_bypass", base + 0x50, 4, 1, dram_pll_bypass_sels, ARRAY_SIZE(dram_pll_bypass_sels), CLK_SET_RATE_PARENT));
	clk_dm(IMX8MP_ARM_PLL_BYPASS, imx_clk_mux_flags("arm_pll_bypass", base + 0x84, 4, 1, arm_pll_bypass_sels, ARRAY_SIZE(arm_pll_bypass_sels), CLK_SET_RATE_PARENT));
	clk_dm(IMX8MP_SYS_PLL1_BYPASS, imx_clk_mux_flags("sys_pll1_bypass", base + 0x94, 4, 1, sys_pll1_bypass_sels, ARRAY_SIZE(sys_pll1_bypass_sels), CLK_SET_RATE_PARENT));
	clk_dm(IMX8MP_SYS_PLL2_BYPASS, imx_clk_mux_flags("sys_pll2_bypass", base + 0x104, 4, 1, sys_pll2_bypass_sels, ARRAY_SIZE(sys_pll2_bypass_sels), CLK_SET_RATE_PARENT));
	clk_dm(IMX8MP_SYS_PLL3_BYPASS, imx_clk_mux_flags("sys_pll3_bypass", base + 0x114, 4, 1, sys_pll3_bypass_sels, ARRAY_SIZE(sys_pll3_bypass_sels), CLK_SET_RATE_PARENT));

	clk_dm(IMX8MP_DRAM_PLL_OUT, imx_clk_gate("dram_pll_out", "dram_pll_bypass", base + 0x50, 13));
	clk_dm(IMX8MP_ARM_PLL_OUT, imx_clk_gate("arm_pll_out", "arm_pll_bypass", base + 0x84, 11));
	clk_dm(IMX8MP_SYS_PLL1_OUT, imx_clk_gate("sys_pll1_out", "sys_pll1_bypass", base + 0x94, 11));
	clk_dm(IMX8MP_SYS_PLL2_OUT, imx_clk_gate("sys_pll2_out", "sys_pll2_bypass", base + 0x104, 11));
	clk_dm(IMX8MP_SYS_PLL3_OUT, imx_clk_gate("sys_pll3_out", "sys_pll3_bypass", base + 0x114, 11));

	clk_dm(IMX8MP_SYS_PLL1_40M, imx_clk_fixed_factor("sys_pll1_40m", "sys_pll1_out", 1, 20));
	clk_dm(IMX8MP_SYS_PLL1_80M, imx_clk_fixed_factor("sys_pll1_80m", "sys_pll1_out", 1, 10));
	clk_dm(IMX8MP_SYS_PLL1_100M, imx_clk_fixed_factor("sys_pll1_100m", "sys_pll1_out", 1, 8));
	clk_dm(IMX8MP_SYS_PLL1_133M, imx_clk_fixed_factor("sys_pll1_133m", "sys_pll1_out", 1, 6));
	clk_dm(IMX8MP_SYS_PLL1_160M, imx_clk_fixed_factor("sys_pll1_160m", "sys_pll1_out", 1, 5));
	clk_dm(IMX8MP_SYS_PLL1_200M, imx_clk_fixed_factor("sys_pll1_200m", "sys_pll1_out", 1, 4));
	clk_dm(IMX8MP_SYS_PLL1_266M, imx_clk_fixed_factor("sys_pll1_266m", "sys_pll1_out", 1, 3));
	clk_dm(IMX8MP_SYS_PLL1_400M, imx_clk_fixed_factor("sys_pll1_400m", "sys_pll1_out", 1, 2));
	clk_dm(IMX8MP_SYS_PLL1_800M, imx_clk_fixed_factor("sys_pll1_800m", "sys_pll1_out", 1, 1));

	clk_dm(IMX8MP_SYS_PLL2_50M, imx_clk_fixed_factor("sys_pll2_50m", "sys_pll2_out", 1, 20));
	clk_dm(IMX8MP_SYS_PLL2_100M, imx_clk_fixed_factor("sys_pll2_100m", "sys_pll2_out", 1, 10));
	clk_dm(IMX8MP_SYS_PLL2_125M, imx_clk_fixed_factor("sys_pll2_125m", "sys_pll2_out", 1, 8));
	clk_dm(IMX8MP_SYS_PLL2_166M, imx_clk_fixed_factor("sys_pll2_166m", "sys_pll2_out", 1, 6));
	clk_dm(IMX8MP_SYS_PLL2_200M, imx_clk_fixed_factor("sys_pll2_200m", "sys_pll2_out", 1, 5));
	clk_dm(IMX8MP_SYS_PLL2_250M, imx_clk_fixed_factor("sys_pll2_250m", "sys_pll2_out", 1, 4));
	clk_dm(IMX8MP_SYS_PLL2_333M, imx_clk_fixed_factor("sys_pll2_333m", "sys_pll2_out", 1, 3));
	clk_dm(IMX8MP_SYS_PLL2_500M, imx_clk_fixed_factor("sys_pll2_500m", "sys_pll2_out", 1, 2));
	clk_dm(IMX8MP_SYS_PLL2_1000M, imx_clk_fixed_factor("sys_pll2_1000m", "sys_pll2_out", 1, 1));

	base = dev_read_addr_ptr(dev);
	if (base == (void *)FDT_ADDR_T_NONE)
		return -EINVAL;

	clk_dm(IMX8MP_CLK_A53_SRC, imx_clk_mux2("arm_a53_src", base + 0x8000, 24, 3, imx8mp_a53_sels, ARRAY_SIZE(imx8mp_a53_sels)));
	clk_dm(IMX8MP_CLK_A53_CG, imx_clk_gate3("arm_a53_cg", "arm_a53_src", base + 0x8000, 28));
	clk_dm(IMX8MP_CLK_A53_DIV, imx_clk_divider2("arm_a53_div", "arm_a53_cg", base + 0x8000, 0, 3));

	clk_dm(IMX8MP_CLK_MAIN_AXI, imx8m_clk_composite_critical("main_axi", imx8mp_main_axi_sels, base + 0x8800));
	clk_dm(IMX8MP_CLK_NAND_USDHC_BUS, imx8m_clk_composite_critical("nand_usdhc_bus", imx8mp_nand_usdhc_sels, base + 0x8900));
	clk_dm(IMX8MP_CLK_NOC, imx8m_clk_composite_critical("noc", imx8mp_noc_sels, base + 0x8d00));
	clk_dm(IMX8MP_CLK_NOC_IO, imx8m_clk_composite_critical("noc_io", imx8mp_noc_io_sels, base + 0x8d80));

	clk_dm(IMX8MP_CLK_AHB, imx8m_clk_composite_critical("ahb_root", imx8mp_ahb_sels, base + 0x9000));

	clk_dm(IMX8MP_CLK_IPG_ROOT, imx_clk_divider2("ipg_root", "ahb_root", base + 0x9080, 0, 1));

	clk_dm(IMX8MP_CLK_DRAM_ALT, imx8m_clk_composite("dram_alt", imx8mp_dram_alt_sels, base + 0xa000));
	clk_dm(IMX8MP_CLK_DRAM_APB, imx8m_clk_composite_critical("dram_apb", imx8mp_dram_apb_sels, base + 0xa080));
	clk_dm(IMX8MP_CLK_I2C5, imx8m_clk_composite("i2c5", imx8mp_i2c5_sels, base + 0xa480));
	clk_dm(IMX8MP_CLK_I2C6, imx8m_clk_composite("i2c6", imx8mp_i2c6_sels, base + 0xa500));
	clk_dm(IMX8MP_CLK_USDHC1, imx8m_clk_composite("usdhc1", imx8mp_usdhc1_sels, base + 0xac00));
	clk_dm(IMX8MP_CLK_USDHC2, imx8m_clk_composite("usdhc2", imx8mp_usdhc2_sels, base + 0xac80));
	clk_dm(IMX8MP_CLK_I2C1, imx8m_clk_composite("i2c1", imx8mp_i2c1_sels, base + 0xad00));
	clk_dm(IMX8MP_CLK_I2C2, imx8m_clk_composite("i2c2", imx8mp_i2c2_sels, base + 0xad80));
	clk_dm(IMX8MP_CLK_I2C3, imx8m_clk_composite("i2c3", imx8mp_i2c3_sels, base + 0xae00));
	clk_dm(IMX8MP_CLK_I2C4, imx8m_clk_composite("i2c4", imx8mp_i2c4_sels, base + 0xae80));

	clk_dm(IMX8MP_CLK_UART1, imx8m_clk_composite("uart1", imx8mp_uart1_sels, base + 0xaf00));
	clk_dm(IMX8MP_CLK_UART2, imx8m_clk_composite("uart2", imx8mp_uart2_sels, base + 0xaf80));
	clk_dm(IMX8MP_CLK_UART3, imx8m_clk_composite("uart3", imx8mp_uart3_sels, base + 0xb000));
	clk_dm(IMX8MP_CLK_UART4, imx8m_clk_composite("uart4", imx8mp_uart4_sels, base + 0xb080));
	clk_dm(IMX8MP_CLK_GIC, imx8m_clk_composite_critical("gic", imx8mp_gic_sels, base + 0xb200));

	clk_dm(IMX8MP_CLK_WDOG, imx8m_clk_composite("wdog", imx8mp_wdog_sels, base + 0xb900));
	clk_dm(IMX8MP_CLK_USDHC3, imx8m_clk_composite("usdhc3", imx8mp_usdhc3_sels, base + 0xbc80));

	clk_dm(IMX8MP_CLK_DRAM_ALT_ROOT, imx_clk_fixed_factor("dram_alt_root", "dram_alt", 1, 4));
	clk_dm(IMX8MP_CLK_DRAM_CORE, imx_clk_mux2_flags("dram_core_clk", base + 0x9800, 24, 1, imx8mp_dram_core_sels, ARRAY_SIZE(imx8mp_dram_core_sels), CLK_IS_CRITICAL));

	clk_dm(IMX8MP_CLK_DRAM1_ROOT, imx_clk_gate4_flags("dram1_root_clk", "dram_core_clk", base + 0x4050, 0, CLK_IS_CRITICAL));
	clk_dm(IMX8MP_CLK_GPIO1_ROOT, imx_clk_gate4("gpio1_root_clk", "ipg_root", base + 0x40b0, 0));
	clk_dm(IMX8MP_CLK_GPIO2_ROOT, imx_clk_gate4("gpio2_root_clk", "ipg_root", base + 0x40c0, 0));
	clk_dm(IMX8MP_CLK_GPIO3_ROOT, imx_clk_gate4("gpio3_root_clk", "ipg_root", base + 0x40d0, 0));
	clk_dm(IMX8MP_CLK_GPIO4_ROOT, imx_clk_gate4("gpio4_root_clk", "ipg_root", base + 0x40e0, 0));
	clk_dm(IMX8MP_CLK_GPIO5_ROOT, imx_clk_gate4("gpio5_root_clk", "ipg_root", base + 0x40f0, 0));
	clk_dm(IMX8MP_CLK_I2C1_ROOT, imx_clk_gate4("i2c1_root_clk", "i2c1", base + 0x4170, 0));
	clk_dm(IMX8MP_CLK_I2C2_ROOT, imx_clk_gate4("i2c2_root_clk", "i2c2", base + 0x4180, 0));
	clk_dm(IMX8MP_CLK_I2C3_ROOT, imx_clk_gate4("i2c3_root_clk", "i2c3", base + 0x4190, 0));
	clk_dm(IMX8MP_CLK_I2C4_ROOT, imx_clk_gate4("i2c4_root_clk", "i2c4", base + 0x41a0, 0));
	clk_dm(IMX8MP_CLK_I2C5_ROOT, imx_clk_gate2("i2c5_root_clk", "i2c5", base + 0x4330, 0));
	clk_dm(IMX8MP_CLK_I2C6_ROOT, imx_clk_gate2("i2c6_root_clk", "i2c6", base + 0x4340, 0));
	clk_dm(IMX8MP_CLK_UART1_ROOT, imx_clk_gate4("uart1_root_clk", "uart1", base + 0x4490, 0));
	clk_dm(IMX8MP_CLK_UART2_ROOT, imx_clk_gate4("uart2_root_clk", "uart2", base + 0x44a0, 0));
	clk_dm(IMX8MP_CLK_UART3_ROOT, imx_clk_gate4("uart3_root_clk", "uart3", base + 0x44b0, 0));
	clk_dm(IMX8MP_CLK_UART4_ROOT, imx_clk_gate4("uart4_root_clk", "uart4", base + 0x44c0, 0));
	clk_dm(IMX8MP_CLK_USDHC1_ROOT, imx_clk_gate4("usdhc1_root_clk", "usdhc1", base + 0x4510, 0));
	clk_dm(IMX8MP_CLK_USDHC2_ROOT, imx_clk_gate4("usdhc2_root_clk", "usdhc2", base + 0x4520, 0));
	clk_dm(IMX8MP_CLK_WDOG1_ROOT, imx_clk_gate4("wdog1_root_clk", "wdog", base + 0x4530, 0));
	clk_dm(IMX8MP_CLK_WDOG2_ROOT, imx_clk_gate4("wdog2_root_clk", "wdog", base + 0x4540, 0));
	clk_dm(IMX8MP_CLK_WDOG3_ROOT, imx_clk_gate4("wdog3_root_clk", "wdog", base + 0x4550, 0));

	clk_dm(IMX8MP_CLK_USDHC3_ROOT, imx_clk_gate4("usdhc3_root_clk", "usdhc3", base + 0x45e0, 0));

	return 0;
}

static const struct udevice_id imx8mp_clk_ids[] = {
	{ .compatible = "fsl,imx8mp-ccm" },
	{ },
};

U_BOOT_DRIVER(imx8mp_clk) = {
	.name = "clk_imx8mp",
	.id = UCLASS_CLK,
	.of_match = imx8mp_clk_ids,
	.ops = &imx8mp_clk_ops,
	.probe = imx8mp_clk_probe,
	.flags = DM_FLAG_PRE_RELOC,
};
