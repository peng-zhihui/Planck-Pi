/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2017-2019 A. Karas, SomLabs
 * Copyright (C) 2015-2016 Freescale Semiconductor, Inc.
 *
 * Configuration settings for the SoMlabs VisionSOM 6ULL board.
 */
#ifndef __SOMLABS_VISIONSOM_6ULL_H
#define __SOMLABS_VISIONSOM_6ULL_H

#include <asm/arch/imx-regs.h>
#include <linux/sizes.h>
#include "mx6_common.h"
#include <asm/mach-imx/gpio.h>

/* SPL options */
#include "imx6_spl.h"

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN		(16 * SZ_1M)

#define CONFIG_MXC_UART
#define CONFIG_MXC_UART_BASE		UART1_BASE

/* MMC Configs */
#ifdef CONFIG_FSL_USDHC
#define CONFIG_SYS_FSL_ESDHC_ADDR	USDHC2_BASE_ADDR

#define CONFIG_SYS_FSL_USDHC_NUM	1
#endif /* CONFIG_FSL_USDHC */

#define CONFIG_EXTRA_ENV_SETTINGS \
	"bootm_size=0x10000000\0" \
	"console=ttymxc0\0" \
	"initrd_addr=0x86800000\0" \
	"fdt_addr=0x83000000\0" \
	"script=boot.scr\0" \
	"image=zImage\0" \
	"splashimage=0x80000000\0" \
	"splashfile=/boot/splash.bmp\0" \
	"mmcdev=1\0" \
	"mmcpart=1\0" \
	"mmcroot=/dev/mmcblk1p1 rootwait rw\0" \
	"setrootmmc=setenv rootspec root=${mmcroot}\0" \
	"setbootscriptmmc=setenv loadbootscript " \
		"load mmc ${mmcdev}:${mmcpart} " \
		"${loadaddr} /boot/${script};\0" \
	"setloadmmc=setenv loadimage load mmc ${mmcdev}:${mmcpart} " \
		"${loadaddr} /boot/${image}; " \
		"setenv loadfdt load mmc ${mmcdev}:${mmcpart} " \
		"${fdt_addr} /boot/${fdt_file};\0" \
	"setbootargs=setenv bootargs console=${console},${baudrate} " \
		"${rootspec}\0" \
	"execbootscript=echo Running bootscript...; source\0" \
	"setfdtfile=setenv fdt_file somlabs-visionsom-6ull.dtb\0" \
	"checkbootdev=run setbootscriptmmc; " \
		"run setrootmmc; " \
		"run setloadmmc; " \

#define CONFIG_BOOTCOMMAND \
	"run setfdtfile; " \
	"run checkbootdev; " \
	"run loadfdt;" \
	"if run loadbootscript; then " \
		"run bootscript; " \
	"else " \
		"if run loadimage; then " \
			"run setbootargs; " \
			"bootz ${loadaddr} - ${fdt_addr}; " \
		"fi; " \
	"fi"

/* Miscellaneous configurable options */

#define CONFIG_SYS_LOAD_ADDR		CONFIG_LOADADDR
#define CONFIG_SYS_HZ			1000

/* Physical Memory Map */
#define PHYS_SDRAM			MMDC0_ARB_BASE_ADDR

#define CONFIG_SYS_SDRAM_BASE		PHYS_SDRAM
#define CONFIG_SYS_INIT_RAM_ADDR	IRAM_BASE_ADDR
#define CONFIG_SYS_INIT_RAM_SIZE	IRAM_SIZE

#define CONFIG_SYS_INIT_SP_OFFSET \
	(CONFIG_SYS_INIT_RAM_SIZE - GENERATED_GBL_DATA_SIZE)
#define CONFIG_SYS_INIT_SP_ADDR \
	(CONFIG_SYS_INIT_RAM_ADDR + CONFIG_SYS_INIT_SP_OFFSET)

/* environment organization */
#define CONFIG_SYS_MMC_ENV_DEV		1	/* USDHC2 */
#define CONFIG_SYS_MMC_ENV_PART		0	/* user area */

/* USB Configs */
#ifdef CONFIG_CMD_USB
#define CONFIG_EHCI_HCD_INIT_AFTER_RESET
#define CONFIG_MXC_USB_PORTSC  (PORT_PTS_UTMI | PORT_PTS_PTW)
#define CONFIG_MXC_USB_FLAGS   0
#define CONFIG_USB_MAX_CONTROLLER_COUNT 2
#endif

#ifdef CONFIG_CMD_NET
#define CONFIG_FEC_MXC
#define IMX_FEC_BASE			ENET_BASE_ADDR
#define CONFIG_FEC_MXC_PHYADDR		0x1
#define CONFIG_FEC_XCV_TYPE		RMII
#define CONFIG_ETHPRIME			"eth0"
#endif

#define CONFIG_IMX_THERMAL

#endif
