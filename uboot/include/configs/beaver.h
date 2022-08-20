/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <linux/sizes.h>

#include "tegra30-common.h"

/* VDD core PMIC */
#define CONFIG_TEGRA_VDD_CORE_TPS62366A_SET1

/* High-level configuration options */
#define CONFIG_TEGRA_BOARD_STRING	"NVIDIA Beaver"

/* Board-specific serial config */
#define CONFIG_TEGRA_ENABLE_UARTA
#define CONFIG_SYS_NS16550_COM1		NV_PA_APB_UARTA_BASE

#define CONFIG_MACH_TYPE		MACH_TYPE_BEAVER

/* Environment in eMMC, at the end of 2nd "boot sector" */
#define CONFIG_SYS_MMC_ENV_DEV		0
#define CONFIG_SYS_MMC_ENV_PART		2

/* SPI */
#define CONFIG_TEGRA_SLINK_CTRLS       6
#define CONFIG_SPI_FLASH_SIZE          (4 << 20)

#include "tegra-common-usb-gadget.h"
#include "tegra-common-post.h"

#endif /* __CONFIG_H */
