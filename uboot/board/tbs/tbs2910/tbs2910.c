// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2014 Soeren Moch <smoch@web.de>
 */

#include <init.h>
#include <asm/arch/clock.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/iomux.h>
#include <asm/arch/mx6-pins.h>
#include <linux/errno.h>
#include <asm/gpio.h>
#include <asm/mach-imx/iomux-v3.h>
#include <asm/mach-imx/boot_mode.h>
#include <asm/mach-imx/video.h>
#include <mmc.h>
#include <fsl_esdhc_imx.h>
#include <asm/arch/mxc_hdmi.h>
#include <asm/arch/crm_regs.h>
#include <asm/io.h>
#include <asm/arch/sys_proto.h>
DECLARE_GLOBAL_DATA_PTR;

#define UART_PAD_CTRL  (PAD_CTL_PUS_100K_UP |			\
	PAD_CTL_SPEED_MED | PAD_CTL_DSE_40ohm |			\
	PAD_CTL_SRE_FAST  | PAD_CTL_HYS)

static iomux_v3_cfg_t const uart1_pads[] = {
	MX6_PAD_CSI0_DAT10__UART1_TX_DATA | MUX_PAD_CTRL(UART_PAD_CTRL),
	MX6_PAD_CSI0_DAT11__UART1_RX_DATA | MUX_PAD_CTRL(UART_PAD_CTRL),
};

static iomux_v3_cfg_t const uart2_pads[] = {
	MX6_PAD_EIM_D26__UART2_TX_DATA | MUX_PAD_CTRL(UART_PAD_CTRL),
	MX6_PAD_EIM_D27__UART2_RX_DATA | MUX_PAD_CTRL(UART_PAD_CTRL),
};

int dram_init(void)
{
	gd->ram_size = 2048ul * 1024 * 1024;
	return 0;
}

static void setup_iomux_uart(void)
{
	imx_iomux_v3_setup_multiple_pads(uart1_pads, ARRAY_SIZE(uart1_pads));
	imx_iomux_v3_setup_multiple_pads(uart2_pads, ARRAY_SIZE(uart2_pads));
}

#ifdef CONFIG_FSL_ESDHC_IMX
/* set environment device to boot device when booting from SD */
int board_mmc_get_env_dev(int devno)
{
	return devno - 1;
}

int board_mmc_get_env_part(int devno)
{
	return (devno == 3) ? 1 : 0; /* part 0 for SD2 / SD3, part 1 for eMMC */
}
#endif /* CONFIG_FSL_ESDHC_IMX */

#ifdef CONFIG_VIDEO_IPUV3
static void do_enable_hdmi(struct display_info_t const *dev)
{
	imx_enable_hdmi_phy();
}

struct display_info_t const displays[] = {{
	.bus	= -1,
	.addr	= 0,
	.pixfmt	= IPU_PIX_FMT_RGB24,
	.detect	= detect_hdmi,
	.enable	= do_enable_hdmi,
	.mode	= {
		.name           = "HDMI",
		/* 1024x768@60Hz (VESA)*/
		.refresh        = 60,
		.xres           = 1024,
		.yres           = 768,
		.pixclock       = 15384,
		.left_margin    = 160,
		.right_margin   = 24,
		.upper_margin   = 29,
		.lower_margin   = 3,
		.hsync_len      = 136,
		.vsync_len      = 6,
		.sync           = FB_SYNC_EXT,
		.vmode          = FB_VMODE_NONINTERLACED
} } };
size_t display_count = ARRAY_SIZE(displays);

static void setup_display(void)
{
	struct mxc_ccm_reg *ccm = (struct mxc_ccm_reg *)CCM_BASE_ADDR;
	int reg;
	s32 timeout = 100000;

	enable_ipu_clock();
	imx_setup_hdmi();

	/* set video pll to 455MHz (24MHz * (37+11/12) / 2) */
	reg = readl(&ccm->analog_pll_video);
	reg |= BM_ANADIG_PLL_VIDEO_POWERDOWN;
	writel(reg, &ccm->analog_pll_video);

	reg &= ~BM_ANADIG_PLL_VIDEO_DIV_SELECT;
	reg |= BF_ANADIG_PLL_VIDEO_DIV_SELECT(37);
	reg &= ~BM_ANADIG_PLL_VIDEO_POST_DIV_SELECT;
	reg |= BF_ANADIG_PLL_VIDEO_POST_DIV_SELECT(1);
	writel(reg, &ccm->analog_pll_video);

	writel(BF_ANADIG_PLL_VIDEO_NUM_A(11), &ccm->analog_pll_video_num);
	writel(BF_ANADIG_PLL_VIDEO_DENOM_B(12), &ccm->analog_pll_video_denom);

	reg &= ~BM_ANADIG_PLL_VIDEO_POWERDOWN;
	writel(reg, &ccm->analog_pll_video);

	while (timeout--)
		if (readl(&ccm->analog_pll_video) & BM_ANADIG_PLL_VIDEO_LOCK)
			break;
	if (timeout < 0)
		printf("Warning: video pll lock timeout!\n");

	reg = readl(&ccm->analog_pll_video);
	reg |= BM_ANADIG_PLL_VIDEO_ENABLE;
	reg &= ~BM_ANADIG_PLL_VIDEO_BYPASS;
	writel(reg, &ccm->analog_pll_video);

	/* gate ipu1_di0_clk */
	reg = readl(&ccm->CCGR3);
	reg &= ~MXC_CCM_CCGR3_LDB_DI0_MASK;
	writel(reg, &ccm->CCGR3);

	/* select video_pll clock / 7  for ipu1_di0_clk -> 65MHz pixclock */
	reg = readl(&ccm->chsccdr);
	reg &= ~(MXC_CCM_CHSCCDR_IPU1_DI0_PRE_CLK_SEL_MASK |
		 MXC_CCM_CHSCCDR_IPU1_DI0_PODF_MASK |
		 MXC_CCM_CHSCCDR_IPU1_DI0_CLK_SEL_MASK);
	reg |= (2 << MXC_CCM_CHSCCDR_IPU1_DI0_PRE_CLK_SEL_OFFSET) |
	       (6 << MXC_CCM_CHSCCDR_IPU1_DI0_PODF_OFFSET) |
	       (0 << MXC_CCM_CHSCCDR_IPU1_DI0_CLK_SEL_OFFSET);
	writel(reg, &ccm->chsccdr);

	/* enable ipu1_di0_clk */
	reg = readl(&ccm->CCGR3);
	reg |= MXC_CCM_CCGR3_LDB_DI0_MASK;
	writel(reg, &ccm->CCGR3);
}
#endif /* CONFIG_VIDEO_IPUV3 */

int board_early_init_f(void)
{
	setup_iomux_uart();
	return 0;
}

#ifdef CONFIG_CMD_BMODE
static const struct boot_mode board_boot_modes[] = {
	/* 4 bit bus width */
	{"sd2",	 MAKE_CFGVAL(0x40, 0x28, 0x00, 0x00)},
	{"sd3",	 MAKE_CFGVAL(0x40, 0x30, 0x00, 0x00)},
	/* 8 bit bus width */
	{"emmc", MAKE_CFGVAL(0x60, 0x58, 0x00, 0x00)},
	{NULL,	 0},
};
#endif

int board_init(void)
{
	/* address of boot parameters */
	gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + 0x100;

#ifdef CONFIG_VIDEO_IPUV3
	setup_display();
#endif
#ifdef CONFIG_CMD_BMODE
	add_board_boot_modes(board_boot_modes);
#endif
	return 0;
}
