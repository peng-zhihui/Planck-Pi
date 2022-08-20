// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019
 * Author(s): Giulio Benetti <giulio.benetti@benettiengineering.com>
 */

#include <common.h>
#include <init.h>
#include <asm/io.h>
#include <asm/armv7_mpu.h>
#include <linux/bitops.h>

int arch_cpu_init(void)
{
	int i;

	struct mpu_region_config imxrt1050_region_config[] = {
		{ 0x00000000, REGION_0, XN_DIS, PRIV_RW_USR_RW,
		  STRONG_ORDER, REGION_4GB },
		{ PHYS_SDRAM, REGION_1, XN_DIS, PRIV_RW_USR_RW,
		  O_I_WB_RD_WR_ALLOC, (ffs(PHYS_SDRAM_SIZE) - 2) },
		{ DMAMEM_BASE,
		  REGION_2, XN_DIS, PRIV_RW_USR_RW,
		  STRONG_ORDER, (ffs(DMAMEM_SZ_ALL) - 2) },
	};

	/*
	 * Configure the memory protection unit (MPU) to allow full access to
	 * the whole 4GB address space.
	 */
	disable_mpu();
	for (i = 0; i < ARRAY_SIZE(imxrt1050_region_config); i++)
		mpu_config(&imxrt1050_region_config[i]);
	enable_mpu();

	return 0;
}
