/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2007, Guennadi Liakhovetski <lg@denx.de>
 *
 * (C) Copyright 2009 Freescale Semiconductor, Inc.
 *
 * Configuration settings for the MX51EVK Board
 */

#ifndef __CONFIG_H
#define __CONFIG_H

 /* High Level Configuration Options */

#define CONFIG_SYS_FSL_CLK

#include <asm/arch/imx-regs.h>

#define CONFIG_CMDLINE_TAG			/* enable passing of ATAGs */
#define CONFIG_SETUP_MEMORY_TAGS
#define CONFIG_INITRD_TAG
#define CONFIG_REVISION_TAG

#define CONFIG_MACH_TYPE	MACH_TYPE_MX51_BABBAGE
/*
 * Size of malloc() pool
 */
#define CONFIG_SYS_MALLOC_LEN		(10 * 1024 * 1024)

/*
 * Hardware drivers
 */
#define CONFIG_FSL_IIM

#define CONFIG_MXC_UART
#define CONFIG_MXC_UART_BASE	UART1_BASE

/* PMIC Controller */
#define CONFIG_POWER
#define CONFIG_POWER_SPI
#define CONFIG_POWER_FSL
#define CONFIG_FSL_PMIC_BUS	0
#define CONFIG_FSL_PMIC_CS	0
#define CONFIG_FSL_PMIC_CLK	2500000
#define CONFIG_FSL_PMIC_MODE	(SPI_MODE_0 | SPI_CS_HIGH)
#define CONFIG_FSL_PMIC_BITLEN	32
#define CONFIG_RTC_MC13XXX

/*
 * MMC Configs
 * */
#define CONFIG_SYS_FSL_ESDHC_ADDR	MMC_SDHC1_BASE_ADDR
#define CONFIG_SYS_FSL_ESDHC_NUM	2

/*
 * Eth Configs
 */

#define CONFIG_FEC_MXC
#define IMX_FEC_BASE	FEC_BASE_ADDR
#define CONFIG_FEC_MXC_PHYADDR	0x1F

/* USB Configs */
#define CONFIG_MXC_USB_PORT	1
#define CONFIG_MXC_USB_PORTSC	PORT_PTS_ULPI
#define CONFIG_MXC_USB_FLAGS	MXC_EHCI_POWER_PINS_ENABLED

/* Framebuffer and LCD */
#define CONFIG_VIDEO_BMP_RLE8
#define CONFIG_SPLASH_SCREEN
#define CONFIG_BMP_16BPP
#define CONFIG_VIDEO_LOGO

/* allow to overwrite serial and ethaddr */
#define CONFIG_ENV_OVERWRITE

#define CONFIG_ETHPRIME		"FEC0"

#define CONFIG_LOADADDR		0x92000000	/* loadaddr env var */

#define CONFIG_EXTRA_ENV_SETTINGS \
	"script=boot.scr\0" \
	"image=zImage\0" \
	"fdt_file=imx51-babbage.dtb\0" \
	"fdt_addr=0x91000000\0" \
	"boot_fdt=try\0" \
	"ip_dyn=yes\0" \
	"mmcdev=0\0" \
	"mmcpart=1\0" \
	"mmcroot=/dev/mmcblk0p2 rootwait rw\0" \
	"mmcargs=setenv bootargs console=ttymxc0,${baudrate} " \
		"root=${mmcroot}\0" \
	"loadbootscript=" \
		"fatload mmc ${mmcdev}:${mmcpart} ${loadaddr} ${script};\0" \
	"bootscript=echo Running bootscript from mmc ...; " \
		"source\0" \
	"loadimage=fatload mmc ${mmcdev}:${mmcpart} ${loadaddr} ${image}\0" \
	"loadfdt=fatload mmc ${mmcdev}:${mmcpart} ${fdt_addr} ${fdt_file}\0" \
	"mmcboot=echo Booting from mmc ...; " \
		"run mmcargs; " \
		"if test ${boot_fdt} = yes || test ${boot_fdt} = try; then " \
			"if run loadfdt; then " \
				"bootz ${loadaddr} - ${fdt_addr}; " \
			"else " \
				"if test ${boot_fdt} = try; then " \
					"bootz; " \
				"else " \
					"echo WARN: Cannot load the DT; " \
				"fi; " \
			"fi; " \
		"else " \
			"bootz; " \
		"fi;\0" \
	"netargs=setenv bootargs console=ttymxc0,${baudrate} " \
		"root=/dev/nfs " \
		"ip=dhcp nfsroot=${serverip}:${nfsroot},v3,tcp\0" \
	"netboot=echo Booting from net ...; " \
		"run netargs; " \
		"if test ${ip_dyn} = yes; then " \
			"setenv get_cmd dhcp; " \
		"else " \
			"setenv get_cmd tftp; " \
		"fi; " \
		"${get_cmd} ${image}; " \
		"if test ${boot_fdt} = yes ||  test ${boot_fdt} = try; then " \
			"if ${get_cmd} ${fdt_addr} ${fdt_file}; then " \
				"bootz ${loadaddr} - ${fdt_addr}; " \
			"else " \
				"if test ${boot_fdt} = try; then " \
					"bootz; " \
				"else " \
					"echo ERROR: Cannot load the DT; " \
					"exit; " \
				"fi; " \
			"fi; " \
		"else " \
			"bootz; " \
		"fi;\0"

#define CONFIG_BOOTCOMMAND \
	"mmc dev ${mmcdev}; if mmc rescan; then " \
		"if run loadbootscript; then " \
			"run bootscript; " \
		"else " \
			"if run loadimage; then " \
				"run mmcboot; " \
			"else run netboot; " \
			"fi; " \
		"fi; " \
	"else run netboot; fi"

#define CONFIG_ARP_TIMEOUT	200UL

/*
 * Miscellaneous configurable options
 */

#define CONFIG_SYS_LOAD_ADDR		CONFIG_LOADADDR

/*-----------------------------------------------------------------------
 * Physical Memory Map
 */
#define PHYS_SDRAM_1		CSD0_BASE_ADDR
#define PHYS_SDRAM_1_SIZE	(512 * 1024 * 1024)

#define CONFIG_SYS_SDRAM_BASE		(PHYS_SDRAM_1)
#define CONFIG_SYS_INIT_RAM_ADDR	(IRAM_BASE_ADDR)
#define CONFIG_SYS_INIT_RAM_SIZE	(IRAM_SIZE)

#define CONFIG_SYS_INIT_SP_OFFSET \
	(CONFIG_SYS_INIT_RAM_SIZE - GENERATED_GBL_DATA_SIZE)
#define CONFIG_SYS_INIT_SP_ADDR \
	(CONFIG_SYS_INIT_RAM_ADDR + CONFIG_SYS_INIT_SP_OFFSET)

#define CONFIG_SYS_DDR_CLKSEL	0
#define CONFIG_SYS_CLKTL_CBCDR	0x59E35100
#define CONFIG_SYS_MAIN_PWR_ON

/*-----------------------------------------------------------------------
 * environment organization
 */

/*
 * Environment starts at CONFIG_ENV_OFFSET=0xC0000 = 768k = 768 * 1024 = 786432
 *
 * Detect overlap between U-Boot image and environment area in build-time
 *
 * CONFIG_BOARD_SIZE_LIMIT = CONFIG_ENV_OFFSET - u-boot.imx offset
 * CONFIG_BOARD_SIZE_LIMIT = 768k - 1k = 767k = 785408
 *
 * Currently CONFIG_BOARD_SIZE_LIMIT does not handle expressions, so
 * write the direct value here
 */
#define CONFIG_BOARD_SIZE_LIMIT		785408
#define CONFIG_SYS_MMC_ENV_DEV 0

#endif
