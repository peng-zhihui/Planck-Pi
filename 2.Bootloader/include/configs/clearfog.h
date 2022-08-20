/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2015 Stefan Roese <sr@denx.de>
 */

#ifndef _CONFIG_CLEARFOG_H
#define _CONFIG_CLEARFOG_H

#include <linux/stringify.h>

/*
 * High Level Configuration Options (easy to change)
 */

/*
 * TEXT_BASE needs to be below 16MiB, since this area is scrubbed
 * for DDR ECC byte filling in the SPL before loading the main
 * U-Boot into it.
 */
#define CONFIG_SYS_TCLK		250000000	/* 250MHz */

/*
 * SDIO/MMC Card Configuration
 */
#define CONFIG_SYS_MMC_BASE		MVEBU_SDIO_BASE

/* USB/EHCI configuration */
#define CONFIG_EHCI_IS_TDI

#define CONFIG_ENV_MIN_ENTRIES		128

/* Environment in MMC */
#define CONFIG_SYS_MMC_ENV_DEV		0
/*
 * For SD - reserve 1 LBA for MBR + 1M for u-boot image. The MMC/eMMC
 * boot image starts @ LBA-0.
 * As result in MMC/eMMC case it will be a 1 sector gap between u-boot
 * image and environment
 */

#define PHY_ANEG_TIMEOUT	8000	/* PHY needs a longer aneg time */

/* PCIe support */
#ifndef CONFIG_SPL_BUILD
#define CONFIG_PCI_SCAN_SHOW
#endif

/* SATA support */
#ifdef CONFIG_SCSI
#define CONFIG_SCSI_AHCI_PLAT
#define CONFIG_SYS_SCSI_MAX_SCSI_ID	1
#define CONFIG_SYS_SCSI_MAX_LUN		1
#define CONFIG_SYS_SCSI_MAX_DEVICE	(CONFIG_SYS_SCSI_MAX_SCSI_ID * \
					CONFIG_SYS_SCSI_MAX_LUN)
#endif

/* Keep device tree and initrd in lower memory so the kernel can access them */
#define RELOCATION_LIMITS_ENV_SETTINGS	\
	"fdt_high=0x10000000\0"		\
	"initrd_high=0x10000000\0"

/* SPL */

/* Defines for SPL */
#define CONFIG_SPL_SIZE			(140 << 10)
#define CONFIG_SPL_MAX_SIZE		(CONFIG_SPL_SIZE - 0x0030)

#define CONFIG_SPL_BSS_START_ADDR	(0x40000000 + CONFIG_SPL_SIZE)
#define CONFIG_SPL_BSS_MAX_SIZE		(16 << 10)

#ifdef CONFIG_SPL_BUILD
#define CONFIG_SYS_MALLOC_SIMPLE
#endif

#define CONFIG_SPL_STACK		(0x40000000 + ((192 - 16) << 10))
#define CONFIG_SPL_BOOTROM_SAVE		(CONFIG_SPL_STACK + 4)

#if defined(CONFIG_MVEBU_SPL_BOOT_DEVICE_SPI)
/* SPL related SPI defines */
#define CONFIG_SYS_U_BOOT_OFFS		CONFIG_SYS_SPI_U_BOOT_OFFS
#elif defined(CONFIG_MVEBU_SPL_BOOT_DEVICE_MMC) || defined(CONFIG_MVEBU_SPL_BOOT_DEVICE_SATA)
/* SPL related MMC defines */
#define CONFIG_SYS_MMC_U_BOOT_OFFS		(160 << 10)
#define CONFIG_SYS_U_BOOT_OFFS			CONFIG_SYS_MMC_U_BOOT_OFFS
#ifdef CONFIG_SPL_BUILD
#define CONFIG_FIXED_SDHCI_ALIGNED_BUFFER	0x00180000	/* in SDRAM */
#endif
#endif

/*
 * mv-common.h should be defined after CMD configs since it used them
 * to enable certain macros
 */
#include "mv-common.h"

/* Include the common distro boot environment */
#ifndef CONFIG_SPL_BUILD

#ifdef CONFIG_MMC
#define BOOT_TARGET_DEVICES_MMC(func) func(MMC, mmc, 0)
#else
#define BOOT_TARGET_DEVICES_MMC(func)
#endif

#ifdef CONFIG_USB_STORAGE
#define BOOT_TARGET_DEVICES_USB(func) func(USB, usb, 0)
#else
#define BOOT_TARGET_DEVICES_USB(func)
#endif

#ifndef CONFIG_SCSI
#define BOOT_TARGET_DEVICES_SCSI_BUS0(func)
#define BOOT_TARGET_DEVICES_SCSI_BUS1(func)
#define BOOT_TARGET_DEVICES_SCSI_BUS2(func)
#else
/*
 * With SCSI enabled, M.2 SATA is always located on bus 0
 */
#define BOOT_TARGET_DEVICES_SCSI_BUS0(func) func(SCSI, scsi, 0)

/*
 * Either one or both mPCIe slots may be configured as mSATA interfaces. The
 * SCSI bus ids are assigned based on sequence of hardware present, not always
 * tied to hardware slot ids. As such, use second SCSI bus if either slot is
 * set for SATA, and only use third SCSI bus if both slots are SATA enabled.
 */
#if defined (CONFIG_CLEARFOG_CON2_SATA) || defined (CONFIG_CLEARFOG_CON3_SATA)
#define BOOT_TARGET_DEVICES_SCSI_BUS1(func) func(SCSI, scsi, 1)
#else
#define BOOT_TARGET_DEVICES_SCSI_BUS1(func)
#endif

#if defined (CONFIG_CLEARFOG_CON2_SATA) && defined (CONFIG_CLEARFOG_CON3_SATA)
#define BOOT_TARGET_DEVICES_SCSI_BUS2(func) func(SCSI, scsi, 2)
#else
#define BOOT_TARGET_DEVICES_SCSI_BUS2(func)
#endif

#endif /* CONFIG_SCSI */

/*
 * The SCSI buses are attempted in increasing bus order, there is no current
 * mechanism to alter the default bus priority order for booting.
 */
#define BOOT_TARGET_DEVICES(func) \
	BOOT_TARGET_DEVICES_MMC(func) \
	BOOT_TARGET_DEVICES_USB(func) \
	BOOT_TARGET_DEVICES_SCSI_BUS0(func) \
	BOOT_TARGET_DEVICES_SCSI_BUS1(func) \
	BOOT_TARGET_DEVICES_SCSI_BUS2(func) \
	func(PXE, pxe, na) \
	func(DHCP, dhcp, na)

#define KERNEL_ADDR_R	__stringify(0x800000)
#define FDT_ADDR_R	__stringify(0x100000)
#define RAMDISK_ADDR_R	__stringify(0x1800000)
#define SCRIPT_ADDR_R	__stringify(0x200000)
#define PXEFILE_ADDR_R	__stringify(0x300000)

#define LOAD_ADDRESS_ENV_SETTINGS \
	"kernel_addr_r=" KERNEL_ADDR_R "\0" \
	"fdt_addr_r=" FDT_ADDR_R "\0" \
	"ramdisk_addr_r=" RAMDISK_ADDR_R "\0" \
	"scriptaddr=" SCRIPT_ADDR_R "\0" \
	"pxefile_addr_r=" PXEFILE_ADDR_R "\0"

#include <config_distro_bootcmd.h>

#define CONFIG_EXTRA_ENV_SETTINGS \
	RELOCATION_LIMITS_ENV_SETTINGS \
	LOAD_ADDRESS_ENV_SETTINGS \
	"console=ttyS0,115200\0" \
	BOOTENV

#endif /* CONFIG_SPL_BUILD */

#endif /* _CONFIG_CLEARFOG_H */
