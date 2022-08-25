/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2013 Freescale Semiconductor, Inc.
 *
 * Configuration settings for the pico-imx6 board.
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include "mx6_common.h"

#include "imx6_spl.h"

#ifdef CONFIG_SPL_OS_BOOT
/* Falcon Mode */
#define CONFIG_SPL_FS_LOAD_ARGS_NAME   "args"
#define CONFIG_SPL_FS_LOAD_KERNEL_NAME "uImage"
#define CONFIG_SYS_SPL_ARGS_ADDR   0x18000000

/* Falcon Mode - MMC support: args@1MB kernel@2MB */
#define CONFIG_SYS_MMCSD_RAW_MODE_ARGS_SECTOR  0x800   /* 1MB */
#define CONFIG_SYS_MMCSD_RAW_MODE_ARGS_SECTORS (CONFIG_CMD_SPL_WRITE_SIZE / 512)
#define CONFIG_SYS_MMCSD_RAW_MODE_KERNEL_SECTOR        0x1000  /* 2MB */
#endif

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN		(35 * SZ_1M) /* Increase due to DFU */

#define CONFIG_MXC_UART
#define CONFIG_MXC_UART_BASE		UART1_BASE

/* MMC Configuration */
#define CONFIG_SYS_FSL_ESDHC_ADDR	USDHC3_BASE_ADDR
#define CONFIG_SUPPORT_EMMC_BOOT
#define CONFIG_SYS_FSL_ESDHC_HAS_DDR_MODE

/* USB Configs */
#define CONFIG_MXC_USB_PORTSC		(PORT_PTS_UTMI | PORT_PTS_PTW)
#define CONFIG_MXC_USB_FLAGS		0

#define CONFIG_SYS_DFU_DATA_BUF_SIZE SZ_16M
#define DFU_DEFAULT_POLL_TIMEOUT 300

#define CONFIG_DFU_ENV_SETTINGS \
	"dfu_alt_info=" \
		"spl raw 0x2 0x400;" \
		"u-boot raw 0x8a 0x1000;" \
		"/boot/zImage ext4 0 1;" \
		"rootfs part 0 1\0" \

#define BOOTMENU_ENV \
	"bootmenu_0=Boot using PICO-Hobbit baseboard=" \
		"setenv baseboard hobbit; saveenv; run base_boot\0" \
	"bootmenu_1=Boot using PICO-Pi baseboard=" \
		"setenv baseboard pi; saveenv; run base_boot\0" \
	"bootmenu_2=Boot using PICO-Dwarf baseboard=" \
		"setenv baseboard dwarf; saveenv; run base_boot\0" \
	"bootmenu_3=Boot using PICO-Nymph baseboard=" \
		"setenv baseboard nymph; saveenv; run base_boot\0" \

#define CONFIG_EXTRA_ENV_SETTINGS \
	"console=ttymxc0\0" \
	"fdtfile=" CONFIG_DEFAULT_FDT_FILE "\0" \
	BOOTMENU_ENV \
	"fdt_high=0xffffffff\0" \
	"initrd_high=0xffffffff\0" \
	"fdt_addr_r=0x18000000\0" \
	"fdt_addr=0x18000000\0" \
	"mmcdev=" __stringify(CONFIG_SYS_MMC_ENV_DEV) "\0" \
	CONFIG_DFU_ENV_SETTINGS \
	"finduuid=part uuid mmc 0:1 uuid\0" \
	"findfdt="\
		"if test $baseboard = hobbit && test $board_rev = MX6Q ; then " \
			"setenv fdtfile imx6q-pico-hobbit.dtb; fi; " \
		"if test $baseboard = pi && test $board_rev = MX6Q ; then " \
			"setenv fdtfile imx6q-pico-pi.dtb; fi; " \
		"if test $baseboard = dwarf && test $board_rev = MX6Q ; then " \
			"setenv fdtfile imx6q-pico-dwarf.dtb; fi; " \
		"if test $baseboard = nymph && test $board_rev = MX6Q ; then " \
			"setenv fdtfile imx6q-pico-nymph.dtb; fi; " \
		"if test $baseboard = hobbit && test $board_rev = MX6DL ; then " \
			"setenv fdtfile imx6dl-pico-hobbit.dtb; fi; " \
		"if test $baseboard = pi && test $board_rev = MX6DL ; then " \
			"setenv fdtfile imx6dl-pico-pi.dtb; fi; " \
		"if test $baseboard = dwarf && test $board_rev = MX6DL ; then " \
			"setenv fdtfile imx6dl-pico-dwarf.dtb; fi; " \
		"if test $baseboard = nymph && test $board_rev = MX6DL ; then " \
			"setenv fdtfile imx6dl-pico-nymph.dtb; fi; " \
		"if test $fdtfile = ask; then " \
			"echo WARNING: Could not determine dtb to use; fi; \0" \
	"default_boot=" \
		"if test $baseboard = ask ; then " \
			"bootmenu -1; " \
		"else " \
			"run base_boot;" \
		"fi; \0" \
	"base_boot=run findfdt; run finduuid; run distro_bootcmd\0" \
	"kernel_addr_r=" __stringify(CONFIG_LOADADDR) "\0" \
	"pxefile_addr_r=" __stringify(CONFIG_LOADADDR) "\0" \
	"ramdisk_addr_r=0x13000000\0" \
	"ramdiskaddr=0x13000000\0" \
	"scriptaddr=" __stringify(CONFIG_LOADADDR) "\0" \
	BOOTENV

#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 0) \
	func(USB, usb, 0)

#include <config_distro_bootcmd.h>

/* Physical Memory Map */
#define PHYS_SDRAM			MMDC0_ARB_BASE_ADDR

#define CONFIG_SYS_SDRAM_BASE		PHYS_SDRAM
#define CONFIG_SYS_INIT_RAM_ADDR	IRAM_BASE_ADDR
#define CONFIG_SYS_INIT_RAM_SIZE	IRAM_SIZE

#define CONFIG_SYS_INIT_SP_OFFSET \
	(CONFIG_SYS_INIT_RAM_SIZE - GENERATED_GBL_DATA_SIZE)
#define CONFIG_SYS_INIT_SP_ADDR \
	(CONFIG_SYS_INIT_RAM_ADDR + CONFIG_SYS_INIT_SP_OFFSET)

/* Environment organization */

/* Environment starts at 768k = 768 * 1024 = 786432 */
/*
 * Detect overlap between U-Boot image and environment area in build-time
 *
 * CONFIG_BOARD_SIZE_LIMIT = CONFIG_ENV_OFFSET - u-boot.img offset
 * CONFIG_BOARD_SIZE_LIMIT = 768k - 69k = 699k = 715776
 *
 * Currently CONFIG_BOARD_SIZE_LIMIT does not handle expressions, so
 * write the direct value here
 */
#define CONFIG_BOARD_SIZE_LIMIT		715776
#define CONFIG_SYS_MMC_ENV_DEV		0

/* Ethernet Configuration */
#define CONFIG_FEC_MXC
#define CONFIG_MII
#define IMX_FEC_BASE			ENET_BASE_ADDR
#define CONFIG_FEC_XCV_TYPE		RGMII
#define CONFIG_ETHPRIME			"FEC"
#define CONFIG_FEC_MXC_PHYADDR		1

/* Framebuffer */
#define CONFIG_VIDEO_BMP_RLE8
#define CONFIG_SPLASH_SCREEN
#define CONFIG_SPLASH_SCREEN_ALIGN
#define CONFIG_BMP_16BPP
#define CONFIG_VIDEO_LOGO
#define CONFIG_VIDEO_BMP_LOGO
#define CONFIG_IMX_HDMI
#define CONFIG_IMX_VIDEO_SKIP

#endif			       /* __CONFIG_H * */
