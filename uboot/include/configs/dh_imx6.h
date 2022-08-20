/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * DHCOM DH-iMX6 PDK board configuration
 *
 * Copyright (C) 2017 Marek Vasut <marex@denx.de>
 */

#ifndef __DH_IMX6_CONFIG_H
#define __DH_IMX6_CONFIG_H

#include <asm/arch/imx-regs.h>

#include "mx6_common.h"

/*
 * SPI NOR layout:
 * 0x00_0000-0x00_ffff ... U-Boot SPL
 * 0x01_0000-0x0f_ffff ... U-Boot
 * 0x10_0000-0x10_ffff ... U-Boot env #1
 * 0x11_0000-0x11_ffff ... U-Boot env #2
 * 0x12_0000-0x1f_ffff ... UNUSED
 */

/* SPL */
#include "imx6_spl.h"			/* common IMX6 SPL configuration */
#define CONFIG_SPL_TARGET		"u-boot-with-spl.imx"

/* Miscellaneous configurable options */

#define CONFIG_CMDLINE_TAG
#define CONFIG_SETUP_MEMORY_TAGS
#define CONFIG_INITRD_TAG
#define CONFIG_REVISION_TAG

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN		(4 * SZ_1M)

/* Bootcounter */
#define CONFIG_SYS_BOOTCOUNT_BE

/* FEC ethernet */
#define IMX_FEC_BASE			ENET_BASE_ADDR
#define CONFIG_FEC_XCV_TYPE		RMII
#define CONFIG_ETHPRIME			"FEC"
#define CONFIG_FEC_MXC_PHYADDR		0
#define CONFIG_ARP_TIMEOUT		200UL

/* MMC Configs */
#define CONFIG_SYS_FSL_ESDHC_ADDR	0
#define CONFIG_SYS_FSL_USDHC_NUM	3
#define CONFIG_SYS_MMC_ENV_DEV		2 /* 1 = SDHC3, 2 = SDHC4 (eMMC) */

/* SATA Configs */
#define CONFIG_LBA48

/* SPI Flash Configs */
#if defined(CONFIG_SPL_BUILD)
#undef CONFIG_DM_SPI
#undef CONFIG_DM_SPI_FLASH
#endif

/* UART */
#define CONFIG_MXC_UART
#define CONFIG_MXC_UART_BASE		UART1_BASE
#define CONFIG_BAUDRATE			115200

/* USB Configs */
#ifdef CONFIG_CMD_USB
#define CONFIG_EHCI_HCD_INIT_AFTER_RESET
#define CONFIG_USB_HOST_ETHER
#define CONFIG_USB_ETHER_ASIX
#define CONFIG_MXC_USB_PORTSC		(PORT_PTS_UTMI | PORT_PTS_PTW)
#define CONFIG_MXC_USB_FLAGS		0
#define CONFIG_USB_MAX_CONTROLLER_COUNT	2 /* Enabled USB controller number */

/* USB Gadget (DFU, UMS) */
#if defined(CONFIG_CMD_DFU) || defined(CONFIG_CMD_USB_MASS_STORAGE)
#define CONFIG_SYS_DFU_DATA_BUF_SIZE	(16 * 1024 * 1024)
#define DFU_DEFAULT_POLL_TIMEOUT	300

/* USB IDs */
#define CONFIG_G_DNL_UMS_VENDOR_NUM	0x0525
#define CONFIG_G_DNL_UMS_PRODUCT_NUM	0xA4A5
#endif
#endif

/* Watchdog */
#if defined(CONFIG_SPL_BUILD)
#undef CONFIG_WDT
#undef CONFIG_WATCHDOG
#define CONFIG_HW_WATCHDOG
#endif

/* allow to overwrite serial and ethaddr */
#define CONFIG_ENV_OVERWRITE

#define CONFIG_LOADADDR			0x12000000
#define CONFIG_SYS_LOAD_ADDR		CONFIG_LOADADDR

#ifndef CONFIG_SPL_BUILD
#define CONFIG_EXTRA_ENV_SETTINGS	\
	"console=ttymxc0,115200\0"	\
	"fdt_addr=0x18000000\0"		\
	"fdt_high=0xffffffff\0"		\
	"initrd_high=0xffffffff\0"	\
	"kernel_addr_r=0x10008000\0"	\
	"fdt_addr_r=0x13000000\0"	\
	"ramdisk_addr_r=0x18000000\0"	\
	"scriptaddr=0x14000000\0"	\
	"fdtfile=imx6q-dhcom-pdk2.dtb\0"\
	BOOTENV

#define CONFIG_BOOTCOMMAND		"run distro_bootcmd"

#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 0) \
	func(MMC, mmc, 2) \
	func(USB, usb, 1) \
	func(SATA, sata, 0) \
	func(DHCP, dhcp, na)

#include <config_distro_bootcmd.h>
#endif

/* Physical Memory Map */
#define PHYS_SDRAM			MMDC0_ARB_BASE_ADDR

#define CONFIG_SYS_SDRAM_BASE		PHYS_SDRAM
#define CONFIG_SYS_INIT_RAM_ADDR	IRAM_BASE_ADDR
#define CONFIG_SYS_INIT_RAM_SIZE	IRAM_SIZE

#define CONFIG_SYS_INIT_SP_OFFSET \
	(CONFIG_SYS_INIT_RAM_SIZE - GENERATED_GBL_DATA_SIZE)

#define CONFIG_SYS_INIT_SP_ADDR \
	(CONFIG_SYS_INIT_RAM_ADDR + CONFIG_SYS_INIT_SP_OFFSET)

/* Environment */

#endif	/* __DH_IMX6_CONFIG_H */
