/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#ifndef _ASM_ARCH_SDRAM_H
#define _ASM_ARCH_SDRAM_H

enum {
	DDR4 = 0,
	DDR3 = 0x3,
	LPDDR2 = 0x5,
	LPDDR3 = 0x6,
	LPDDR4 = 0x7,
	UNUSED = 0xFF
};

/*
 * sys_reg2 bitfield struct
 * [31]		row_3_4_ch1
 * [30]		row_3_4_ch0
 * [29:28]	chinfo
 * [27]		rank_ch1
 * [26:25]	col_ch1
 * [24]		bk_ch1
 * [23:22]	low bits of cs0_row_ch1
 * [21:20]	low bits of cs1_row_ch1
 * [19:18]	bw_ch1
 * [17:16]	dbw_ch1;
 * [15:13]	ddrtype
 * [12]		channelnum
 * [11]		rank_ch0
 * [10:9]	col_ch0,
 * [8]		bk_ch0
 * [7:6]	low bits of cs0_row_ch0
 * [5:4]	low bits of cs1_row_ch0
 * [3:2]	bw_ch0
 * [1:0]	dbw_ch0
 */
#define SYS_REG_DDRTYPE_SHIFT		13
#define SYS_REG_DDRTYPE_MASK		7
#define SYS_REG_NUM_CH_SHIFT		12
#define SYS_REG_NUM_CH_MASK		1
#define SYS_REG_ROW_3_4_SHIFT(ch)	(30 + (ch))
#define SYS_REG_ROW_3_4_MASK		1
#define SYS_REG_CHINFO_SHIFT(ch)	(28 + (ch))
#define SYS_REG_RANK_SHIFT(ch)		(11 + (ch) * 16)
#define SYS_REG_RANK_MASK		1
#define SYS_REG_COL_SHIFT(ch)		(9 + (ch) * 16)
#define SYS_REG_COL_MASK		3
#define SYS_REG_BK_SHIFT(ch)		(8 + (ch) * 16)
#define SYS_REG_BK_MASK			1
#define SYS_REG_CS0_ROW_SHIFT(ch)	(6 + (ch) * 16)
#define SYS_REG_CS0_ROW_MASK		3
#define SYS_REG_CS1_ROW_SHIFT(ch)	(4 + (ch) * 16)
#define SYS_REG_CS1_ROW_MASK		3
#define SYS_REG_BW_SHIFT(ch)		(2 + (ch) * 16)
#define SYS_REG_BW_MASK			3
#define SYS_REG_DBW_SHIFT(ch)		((ch) * 16)
#define SYS_REG_DBW_MASK		3

/*
 * sys_reg3 bitfield struct
 * [7]		high bit of cs0_row_ch1
 * [6]		high bit of cs1_row_ch1
 * [5]		high bit of cs0_row_ch0
 * [4]		high bit of cs1_row_ch0
 * [3:2]	cs1_col_ch1
 * [1:0]	cs1_col_ch0
 */
#define SYS_REG_VERSION_SHIFT			28
#define SYS_REG_VERSION_MASK			0xf
#define SYS_REG_EXTEND_CS0_ROW_SHIFT(ch)	(5 + (ch) * 2)
#define SYS_REG_EXTEND_CS0_ROW_MASK		1
#define SYS_REG_EXTEND_CS1_ROW_SHIFT(ch)	(4 + (ch) * 2)
#define SYS_REG_EXTEND_CS1_ROW_MASK		1
#define SYS_REG_CS1_COL_SHIFT(ch)		(0 + (ch) * 2)
#define SYS_REG_CS1_COL_MASK			3

/* Get sdram size decode from reg */
size_t rockchip_sdram_size(phys_addr_t reg);

/* Called by U-Boot board_init_r for Rockchip SoCs */
int dram_init(void);

#endif
