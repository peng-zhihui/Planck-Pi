/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2016 - AmLogic, Inc.
 * Copyright 2018 - Beniamino Galvani <b.galvani@gmail.com>
 * Copyright 2018 - BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 */
#ifndef _ARCH_MESON_CLOCK_AXG_H_
#define _ARCH_MESON_CLOCK_AXG_H_

/*
 * Clock controller register offsets
 *
 * Register offsets from the data sheet are listed in comment blocks below.
 * Those offsets must be multiplied by 4 before adding them to the base address
 * to get the right value
 */
#define HHI_GP0_PLL_CNTL		0x40
#define HHI_GP0_PLL_CNTL2		0x44
#define HHI_GP0_PLL_CNTL3		0x48
#define HHI_GP0_PLL_CNTL4		0x4c
#define HHI_GP0_PLL_CNTL5		0x50
#define HHI_GP0_PLL_STS			0x54
#define HHI_GP0_PLL_CNTL1		0x58
#define HHI_HIFI_PLL_CNTL		0x80
#define HHI_HIFI_PLL_CNTL2		0x84
#define HHI_HIFI_PLL_CNTL3		0x88
#define HHI_HIFI_PLL_CNTL4		0x8C
#define HHI_HIFI_PLL_CNTL5		0x90
#define HHI_HIFI_PLL_STS		0x94
#define HHI_HIFI_PLL_CNTL1		0x98

#define HHI_XTAL_DIVN_CNTL		0xbc
#define HHI_GCLK2_MPEG0			0xc0
#define HHI_GCLK2_MPEG1			0xc4
#define HHI_GCLK2_MPEG2			0xc8
#define HHI_GCLK2_OTHER			0xd0
#define HHI_GCLK2_AO			0xd4
#define HHI_PCIE_PLL_CNTL		0xd8
#define HHI_PCIE_PLL_CNTL1		0xdC
#define HHI_PCIE_PLL_CNTL2		0xe0
#define HHI_PCIE_PLL_CNTL3		0xe4
#define HHI_PCIE_PLL_CNTL4		0xe8
#define HHI_PCIE_PLL_CNTL5		0xec
#define HHI_PCIE_PLL_CNTL6		0xf0
#define HHI_PCIE_PLL_STS		0xf4

#define HHI_MEM_PD_REG0			0x100
#define HHI_VPU_MEM_PD_REG0		0x104
#define HHI_VIID_CLK_DIV		0x128
#define HHI_VIID_CLK_CNTL		0x12c

#define HHI_GCLK_MPEG0			0x140
#define HHI_GCLK_MPEG1			0x144
#define HHI_GCLK_MPEG2			0x148
#define HHI_GCLK_OTHER			0x150
#define HHI_GCLK_AO			0x154
#define HHI_SYS_CPU_CLK_CNTL1		0x15c
#define HHI_SYS_CPU_RESET_CNTL		0x160
#define HHI_VID_CLK_DIV			0x164
#define HHI_SPICC_HCLK_CNTL		0x168

#define HHI_MPEG_CLK_CNTL		0x174
#define HHI_VID_CLK_CNTL		0x17c
#define HHI_TS_CLK_CNTL			0x190
#define HHI_VID_CLK_CNTL2		0x194
#define HHI_SYS_CPU_CLK_CNTL0		0x19c
#define HHI_VID_PLL_CLK_DIV		0x1a0
#define HHI_VPU_CLK_CNTL		0x1bC

#define HHI_VAPBCLK_CNTL		0x1F4

#define HHI_GEN_CLK_CNTL		0x228

#define HHI_VDIN_MEAS_CLK_CNTL		0x250
#define HHI_NAND_CLK_CNTL		0x25C
#define HHI_SD_EMMC_CLK_CNTL		0x264

#define HHI_MPLL_CNTL			0x280
#define HHI_MPLL_CNTL2			0x284
#define HHI_MPLL_CNTL3			0x288
#define HHI_MPLL_CNTL4			0x28C
#define HHI_MPLL_CNTL5			0x290
#define HHI_MPLL_CNTL6			0x294
#define HHI_MPLL_CNTL7			0x298
#define HHI_MPLL_CNTL8			0x29C
#define HHI_MPLL_CNTL9			0x2A0
#define HHI_MPLL_CNTL10			0x2A4

#define HHI_MPLL3_CNTL0			0x2E0
#define HHI_MPLL3_CNTL1			0x2E4
#define HHI_PLL_TOP_MISC		0x2E8

#define HHI_SYS_PLL_CNTL1		0x2FC
#define HHI_SYS_PLL_CNTL		0x300
#define HHI_SYS_PLL_CNTL2		0x304
#define HHI_SYS_PLL_CNTL3		0x308
#define HHI_SYS_PLL_CNTL4		0x30c
#define HHI_SYS_PLL_CNTL5		0x310
#define HHI_SYS_PLL_STS			0x314
#define HHI_DPLL_TOP_I			0x318
#define HHI_DPLL_TOP2_I			0x31C

#endif
