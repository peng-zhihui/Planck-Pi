/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * phycore_am335x_r2.h
 *
 * Phytec phyCORE-AM335x R2 (PCL060 / PCM060) boards information header
 *
 * Copyright (C) 2011, Texas Instruments, Incorporated - http://www.ti.com/
 * Copyright (C) 2013 Lars Poeschel, Lemonage Software GmbH
 * Copyright (C) 2019 DENX Software Engineering GmbH
 */

#ifndef __CONFIG_PHYCORE_AM335x_R2_H
#define __CONFIG_PHYCORE_AM335x_R2_H

#include <configs/ti_am335x_common.h>

#define CONFIG_MACH_TYPE		MACH_TYPE_SBC_PHYCORE_AM335X
#define CONFIG_SYS_MMC_ENV_DEV		0
#define CONFIG_SYS_MMCSD_FS_BOOT_PARTITION	1

#ifdef CONFIG_MTD_RAW_NAND
#define NANDARGS \
	"mtdids=" CONFIG_MTDIDS_DEFAULT "\0" \
	"mtdparts=" CONFIG_MTDPARTS_DEFAULT "\0" \
	"nandargs=setenv bootargs console=${console} " \
		"${optargs} " \
		"root=${nandroot} " \
		"rootfstype=${nandrootfstype}\0" \
	"nandroot=ubi0:root ubi.mtd=NAND.UBI\0" \
	"nandrootfstype=ubifs rootwait rw fsck.repair=yes\0" \
	"nandboot=echo Booting from nand ...; " \
		"run nandargs; " \
		"ubi part NAND.UBI; " \
		"ubi readvol ${fdtaddr} oftree; " \
		"ubi readvol ${loadaddr} kernel; " \
		"bootz ${loadaddr} - ${fdtaddr}\0"

#else
#define NANDARGS ""
#endif

/* set to negative value for no autoboot */
#define BOOTENV_DEV_LEGACY_MMC(devtypeu, devtypel, instance) \
	"bootcmd_" #devtypel #instance "=" \
	"setenv mmcdev " #instance "; "\
	"setenv bootpart " #instance ":1 ; "\
	"setenv rootpart " #instance ":2 ; "\
	"run mmcboot\0"

#define BOOTENV_DEV_NAME_LEGACY_MMC(devtypeu, devtypel, instance) \
	#devtypel #instance " "

#define BOOTENV_DEV_NAND(devtypeu, devtypel, instance) \
	"bootcmd_" #devtypel #instance "=" \
	"run nandboot\0"

#define BOOTENV_DEV_NAME_NAND(devtypeu, devtypel, instance) \
	#devtypel #instance " "

#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 0) \
	func(LEGACY_MMC, legacy_mmc, 0) \
	func(MMC, mmc, 1) \
	func(LEGACY_MMC, legacy_mmc, 1) \
	func(NAND, nand, 0)

#include <config_distro_bootcmd.h>
#include <environment/ti/dfu.h>
#include <environment/ti/mmc.h>

#define CONFIG_EXTRA_ENV_SETTINGS \
	DEFAULT_MMC_TI_ARGS \
	DEFAULT_LINUX_BOOT_ENV \
	"bootfile=zImage\0" \
	"fdtfile=" CONFIG_DEFAULT_FDT_FILE "\0" \
	"console=ttyS0,115200\0" \
	"optargs=\0" \
	"mmcrootfstype=ext2 rootwait\0" \
	"finduuid=part uuid mmc ${rootpart} uuid\0" \
	"boot_fit=0\0" \
	NANDARGS \
	BOOTENV

/* Clock Macros */
#define V_OSCK				25000000  /* Clock output from T2 */
#define V_SCLK				V_OSCK

#define CONFIG_POWER_TPS65910

#ifdef CONFIG_MTD_RAW_NAND
/* NAND: device related configs */
#define CONFIG_SYS_NAND_5_ADDR_CYCLE
#define CONFIG_SYS_NAND_PAGE_COUNT	(CONFIG_SYS_NAND_BLOCK_SIZE / \
					 CONFIG_SYS_NAND_PAGE_SIZE)
#define CONFIG_SYS_NAND_PAGE_SIZE	2048
#define CONFIG_SYS_NAND_OOBSIZE		64
#define CONFIG_SYS_NAND_BLOCK_SIZE	(128 * 1024)
/* NAND: driver related configs */
#define CONFIG_SYS_NAND_BAD_BLOCK_POS	NAND_LARGE_BADBLOCK_POS
#define CONFIG_SYS_NAND_ECCPOS		{ 2, 3, 4, 5, 6, 7, 8, 9, \
					 10, 11, 12, 13, 14, 15, 16, 17, \
					 18, 19, 20, 21, 22, 23, 24, 25, \
					 26, 27, 28, 29, 30, 31, 32, 33, \
					 34, 35, 36, 37, 38, 39, 40, 41, \
					 42, 43, 44, 45, 46, 47, 48, 49, \
					 50, 51, 52, 53, 54, 55, 56, 57, }

#define CONFIG_SYS_NAND_ECCSIZE		512
#define CONFIG_SYS_NAND_ECCBYTES	14
#define CONFIG_SYS_NAND_ONFI_DETECTION
#define CONFIG_NAND_OMAP_ECCSCHEME	OMAP_ECC_BCH8_CODE_HW

/* NAND: SPL related configs */
#ifdef CONFIG_SPL_OS_BOOT
#define CONFIG_SYS_NAND_SPL_KERNEL_OFFS	0x00200000 /* kernel offset */
#endif
#endif /* !CONFIG_MTD_RAW_NAND */

/* CPU */

#ifdef CONFIG_SPI_BOOT
#define CONFIG_SYS_SPI_U_BOOT_SIZE	0x40000
#elif defined(CONFIG_ENV_IS_IN_NAND)
#define CONFIG_SYS_ENV_SECT_SIZE	CONFIG_SYS_NAND_BLOCK_SIZE
#endif

#endif	/* ! __CONFIG_PHYCORE_AM335x_R2_H */
