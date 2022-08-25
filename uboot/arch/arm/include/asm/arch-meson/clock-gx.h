/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2016 - AmLogic, Inc.
 * Copyright 2018 - Beniamino Galvani <b.galvani@gmail.com>
 */
#ifndef _ARCH_MESON_CLOCK_GX_H_
#define _ARCH_MESON_CLOCK_GX_H_

/*
 * Clock controller register offsets
 *
 * Register offsets from the data sheet are listed in comment blocks below.
 * Those offsets must be multiplied by 4 before adding them to the base address
 * to get the right value
 */
#define SCR				0x2C /* 0x0b offset in data sheet */
#define TIMEOUT_VALUE			0x3c /* 0x0f offset in data sheet */

#define HHI_GP0_PLL_CNTL		0x40 /* 0x10 offset in data sheet */
#define HHI_GP0_PLL_CNTL2		0x44 /* 0x11 offset in data sheet */
#define HHI_GP0_PLL_CNTL3		0x48 /* 0x12 offset in data sheet */
#define HHI_GP0_PLL_CNTL4		0x4c /* 0x13 offset in data sheet */
#define	HHI_GP0_PLL_CNTL5		0x50 /* 0x14 offset in data sheet */
#define	HHI_GP0_PLL_CNTL1		0x58 /* 0x16 offset in data sheet */

#define	HHI_XTAL_DIVN_CNTL		0xbc /* 0x2f offset in data sheet */
#define	HHI_TIMER90K			0xec /* 0x3b offset in data sheet */

#define	HHI_MEM_PD_REG0			0x100 /* 0x40 offset in data sheet */
#define	HHI_MEM_PD_REG1			0x104 /* 0x41 offset in data sheet */
#define	HHI_VPU_MEM_PD_REG1		0x108 /* 0x42 offset in data sheet */
#define	HHI_VIID_CLK_DIV		0x128 /* 0x4a offset in data sheet */
#define	HHI_VIID_CLK_CNTL		0x12c /* 0x4b offset in data sheet */

#define HHI_GCLK_MPEG0			0x140 /* 0x50 offset in data sheet */
#define HHI_GCLK_MPEG1			0x144 /* 0x51 offset in data sheet */
#define HHI_GCLK_MPEG2			0x148 /* 0x52 offset in data sheet */
#define HHI_GCLK_OTHER			0x150 /* 0x54 offset in data sheet */
#define HHI_GCLK_AO			0x154 /* 0x55 offset in data sheet */
#define HHI_SYS_OSCIN_CNTL		0x158 /* 0x56 offset in data sheet */
#define HHI_SYS_CPU_CLK_CNTL1		0x15c /* 0x57 offset in data sheet */
#define HHI_SYS_CPU_RESET_CNTL		0x160 /* 0x58 offset in data sheet */
#define HHI_VID_CLK_DIV			0x164 /* 0x59 offset in data sheet */

#define HHI_MPEG_CLK_CNTL		0x174 /* 0x5d offset in data sheet */
#define HHI_AUD_CLK_CNTL		0x178 /* 0x5e offset in data sheet */
#define HHI_VID_CLK_CNTL		0x17c /* 0x5f offset in data sheet */
#define HHI_AUD_CLK_CNTL2		0x190 /* 0x64 offset in data sheet */
#define HHI_VID_CLK_CNTL2		0x194 /* 0x65 offset in data sheet */
#define HHI_SYS_CPU_CLK_CNTL0		0x19c /* 0x67 offset in data sheet */
#define HHI_VID_PLL_CLK_DIV		0x1a0 /* 0x68 offset in data sheet */
#define HHI_AUD_CLK_CNTL3		0x1a4 /* 0x69 offset in data sheet */
#define HHI_MALI_CLK_CNTL		0x1b0 /* 0x6c offset in data sheet */
#define HHI_VPU_CLK_CNTL		0x1bC /* 0x6f offset in data sheet */

#define HHI_HDMI_CLK_CNTL		0x1CC /* 0x73 offset in data sheet */
#define HHI_VDEC_CLK_CNTL		0x1E0 /* 0x78 offset in data sheet */
#define HHI_VDEC2_CLK_CNTL		0x1E4 /* 0x79 offset in data sheet */
#define HHI_VDEC3_CLK_CNTL		0x1E8 /* 0x7a offset in data sheet */
#define HHI_VDEC4_CLK_CNTL		0x1EC /* 0x7b offset in data sheet */
#define HHI_HDCP22_CLK_CNTL		0x1F0 /* 0x7c offset in data sheet */
#define HHI_VAPBCLK_CNTL		0x1F4 /* 0x7d offset in data sheet */

#define HHI_VPU_CLKB_CNTL		0x20C /* 0x83 offset in data sheet */
#define HHI_USB_CLK_CNTL		0x220 /* 0x88 offset in data sheet */
#define HHI_32K_CLK_CNTL		0x224 /* 0x89 offset in data sheet */
#define HHI_GEN_CLK_CNTL		0x228 /* 0x8a offset in data sheet */
#define HHI_GEN_CLK_CNTL		0x228 /* 0x8a offset in data sheet */

#define HHI_PCM_CLK_CNTL		0x258 /* 0x96 offset in data sheet */
#define HHI_NAND_CLK_CNTL		0x25C /* 0x97 offset in data sheet */
#define HHI_SD_EMMC_CLK_CNTL		0x264 /* 0x99 offset in data sheet */

#define HHI_MPLL_CNTL			0x280 /* 0xa0 offset in data sheet */
#define HHI_MPLL_CNTL2			0x284 /* 0xa1 offset in data sheet */
#define HHI_MPLL_CNTL3			0x288 /* 0xa2 offset in data sheet */
#define HHI_MPLL_CNTL4			0x28C /* 0xa3 offset in data sheet */
#define HHI_MPLL_CNTL5			0x290 /* 0xa4 offset in data sheet */
#define HHI_MPLL_CNTL6			0x294 /* 0xa5 offset in data sheet */
#define HHI_MPLL_CNTL7			0x298 /* 0xa6 offset in data sheet */
#define HHI_MPLL_CNTL8			0x29C /* 0xa7 offset in data sheet */
#define HHI_MPLL_CNTL9			0x2A0 /* 0xa8 offset in data sheet */
#define HHI_MPLL_CNTL10			0x2A4 /* 0xa9 offset in data sheet */

#define HHI_MPLL3_CNTL0			0x2E0 /* 0xb8 offset in data sheet */
#define HHI_MPLL3_CNTL1			0x2E4 /* 0xb9 offset in data sheet */
#define HHI_VDAC_CNTL0			0x2F4 /* 0xbd offset in data sheet */
#define HHI_VDAC_CNTL1			0x2F8 /* 0xbe offset in data sheet */

#define HHI_SYS_PLL_CNTL		0x300 /* 0xc0 offset in data sheet */
#define HHI_SYS_PLL_CNTL2		0x304 /* 0xc1 offset in data sheet */
#define HHI_SYS_PLL_CNTL3		0x308 /* 0xc2 offset in data sheet */
#define HHI_SYS_PLL_CNTL4		0x30c /* 0xc3 offset in data sheet */
#define HHI_SYS_PLL_CNTL5		0x310 /* 0xc4 offset in data sheet */
#define HHI_DPLL_TOP_I			0x318 /* 0xc6 offset in data sheet */
#define HHI_DPLL_TOP2_I			0x31C /* 0xc7 offset in data sheet */
#define HHI_HDMI_PLL_CNTL		0x320 /* 0xc8 offset in data sheet */
#define HHI_HDMI_PLL_CNTL2		0x324 /* 0xc9 offset in data sheet */
#define HHI_HDMI_PLL_CNTL3		0x328 /* 0xca offset in data sheet */
#define HHI_HDMI_PLL_CNTL4		0x32C /* 0xcb offset in data sheet */
#define HHI_HDMI_PLL_CNTL5		0x330 /* 0xcc offset in data sheet */
#define HHI_HDMI_PLL_CNTL6		0x334 /* 0xcd offset in data sheet */
#define HHI_HDMI_PLL_CNTL_I		0x338 /* 0xce offset in data sheet */
#define HHI_HDMI_PLL_CNTL7		0x33C /* 0xcf offset in data sheet */

#define HHI_HDMI_PHY_CNTL0		0x3A0 /* 0xe8 offset in data sheet */
#define HHI_HDMI_PHY_CNTL1		0x3A4 /* 0xe9 offset in data sheet */
#define HHI_HDMI_PHY_CNTL2		0x3A8 /* 0xea offset in data sheet */
#define HHI_HDMI_PHY_CNTL3		0x3AC /* 0xeb offset in data sheet */

#define HHI_VID_LOCK_CLK_CNTL		0x3C8 /* 0xf2 offset in data sheet */
#define HHI_BT656_CLK_CNTL		0x3D4 /* 0xf5 offset in data sheet */
#define HHI_SAR_CLK_CNTL		0x3D8 /* 0xf6 offset in data sheet */

ulong meson_measure_clk_rate(unsigned int clk);

#endif
