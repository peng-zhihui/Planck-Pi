/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2013-2015
 * NVIDIA Corporation <www.nvidia.com>
 */

#ifndef _E2220_1170_H
#define _E2220_1170_H

#include <linux/sizes.h>

#include "tegra210-common.h"

/* High-level configuration options */
#define CONFIG_TEGRA_BOARD_STRING	"NVIDIA E2220-1170"

/* Board-specific serial config */
#define CONFIG_TEGRA_ENABLE_UARTA

/* Environment in eMMC, at the end of 2nd "boot sector" */
#define CONFIG_SYS_MMC_ENV_DEV		0
#define CONFIG_SYS_MMC_ENV_PART		2

/* SPI */
#define CONFIG_SPI_FLASH_SIZE		(4 << 20)

#include "tegra-common-usb-gadget.h"
#include "tegra-common-post.h"

#endif /* _E2220_1170_H */
