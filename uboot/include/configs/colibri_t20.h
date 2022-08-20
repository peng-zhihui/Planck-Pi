/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2012 Lucas Stach
 *
 * Configuration settings for the Toradex Colibri T20 modules.
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include "tegra20-common.h"

/* Board-specific serial config */
#define CONFIG_TEGRA_ENABLE_UARTA
#define CONFIG_TEGRA_UARTA_SDIO1
#define CONFIG_SYS_NS16550_COM1		NV_PA_APB_UARTA_BASE

#define CONFIG_MACH_TYPE		MACH_TYPE_COLIBRI_TEGRA2

/* General networking support */
#define CONFIG_TFTP_TSIZE

/* LCD support */
#define CONFIG_LCD_LOGO

/* NAND support */
#define CONFIG_TEGRA_NAND
#define CONFIG_SYS_MAX_NAND_DEVICE	1

#define UBOOT_UPDATE \
	"update_uboot=nand erase.part u-boot && " \
		"nand write ${loadaddr} u-boot ${filesize}\0" \

/* Environment in NAND, 64K is a bit excessive but erase block is 512K anyway */
#define BOARD_EXTRA_ENV_SETTINGS \
	"mtdparts=" CONFIG_MTDPARTS_DEFAULT "\0" \
	UBOOT_UPDATE

/* Increase console I/O buffer size */
#undef CONFIG_SYS_CBSIZE
#define CONFIG_SYS_CBSIZE		1024

/* Increase arguments buffer size */
#undef CONFIG_SYS_BARGSIZE
#define CONFIG_SYS_BARGSIZE CONFIG_SYS_CBSIZE

/* Increase maximum number of arguments */
#undef CONFIG_SYS_MAXARGS
#define CONFIG_SYS_MAXARGS		32

#include "tegra-common-usb-gadget.h"
#include "tegra-common-post.h"

#endif /* __CONFIG_H */
