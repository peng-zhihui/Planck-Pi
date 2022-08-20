/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2016-2018 Toradex AG
 *
 * Configuration settings for the Colibri iMX7 module.
 *
 * based on mx7dsabresd.h:
 * Copyright (C) 2015 Freescale Semiconductor, Inc.
 */

#ifndef __COLIBRI_IMX7_CONFIG_H
#define __COLIBRI_IMX7_CONFIG_H

#include "mx7_common.h"

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN		(32 * SZ_1M)

/* MMC Config*/
#define CONFIG_SYS_FSL_ESDHC_ADDR	0
#ifdef CONFIG_TARGET_COLIBRI_IMX7_NAND
#define CONFIG_SYS_FSL_USDHC_NUM	1
#elif CONFIG_TARGET_COLIBRI_IMX7_EMMC
#define CONFIG_SYS_FSL_USDHC_NUM	2
#endif

/* I2C configs */
#define CONFIG_SYS_I2C_MXC
#define CONFIG_SYS_I2C_SPEED		100000

#define CONFIG_IPADDR			192.168.10.2
#define CONFIG_NETMASK			255.255.255.0
#define CONFIG_SERVERIP			192.168.10.1

#if defined(CONFIG_TARGET_COLIBRI_IMX7_EMMC)
#define UBOOT_UPDATE \
	"uboot_hwpart=1\0" \
	"uboot_blk=2\0" \
	"set_blkcnt=setexpr blkcnt ${filesize} + 0x1ff && " \
		"setexpr blkcnt ${blkcnt} / 0x200\0" \
	"update_uboot=run set_blkcnt && mmc dev 0 ${uboot_hwpart} && " \
		"mmc write ${loadaddr} ${uboot_blk} ${blkcnt}\0"
#elif defined(CONFIG_TARGET_COLIBRI_IMX7_NAND)
#define UBOOT_UPDATE \
	"update_uboot=nand erase.part u-boot1 && " \
		"nand write ${loadaddr} u-boot1 ${filesize} && " \
		"nand erase.part u-boot2 && " \
		"nand write ${loadaddr} u-boot2 ${filesize}\0"
#endif

#ifndef PARTS_DEFAULT
/* Define the default GPT table for eMMC */
#define PARTS_DEFAULT \
	/* Android partitions */ \
	"partitions_android=" \
	"uuid_disk=${uuid_gpt_disk};" \
	"name=boot,start=1M,size=32M,uuid=${uuid_gpt_boot};" \
	"name=environment,size=4M,uuid=${uuid_gpt_environment};" \
	"name=recovery,size=16M,uuid=${uuid_gpt_recovery};" \
	"name=system,size=1536M,uuid=${uuid_gpt_system};" \
	"name=cache,size=512M,uuid=${uuid_gpt_cache};" \
	"name=device,size=8M,uuid=${uuid_gpt_device};" \
	"name=misc,size=4M,uuid=${uuid_gpt_misc};" \
	"name=datafooter,size=2M,uuid=${uuid_gpt_datafooter};" \
	"name=metadata,size=2M,uuid=${uuid_gpt_metadata};" \
	"name=persistdata,size=2M,uuid=${uuid_gpt_persistdata};" \
	"name=userdata,size=128M,uuid=${uuid_gpt_userdata};" \
	"name=fbmisc,size=-,uuid=${uuid_gpt_fbmisc}\0"
#endif /* PARTS_DEFAULT */

#define EMMC_ANDROID_BOOTCMD \
	"android_args=androidboot.storage_type=emmc\0" \
	PARTS_DEFAULT \
	"android_fdt_addr=0x83700000\0" \
	"android_mmc_dev=0\0" \
	"m4binary=rpmsg_imu_freertos.elf\0" \
	"androidboot=ext4load mmc 0:a ${loadaddr} media/0/${m4binary}; "\
		"bootaux ${loadaddr}; " \
		"setenv loadaddr 0x88000000; " \
		"setenv bootm_boot_mode sec;" \
		"setenv bootargs androidboot.serialno=${serial#} " \
			"$android_args; " \
		"part start mmc ${android_mmc_dev} boot boot_start; " \
		"part size mmc ${android_mmc_dev} boot boot_size; " \
		"mmc read ${loadaddr} ${boot_start} ${boot_size}; " \
		"part start mmc ${android_mmc_dev} environment env_start; " \
		"part size mmc ${android_mmc_dev} environment env_size; " \
		"mmc read ${android_fdt_addr} ${env_start} ${env_size}; " \
		"bootm ${loadaddr} ${loadaddr} ${android_fdt_addr}\0 "

#define MEM_LAYOUT_ENV_SETTINGS \
	"bootm_size=0x10000000\0" \
	"fdt_addr_r=0x82000000\0" \
	"kernel_addr_r=0x81000000\0" \
	"pxefile_addr_r=0x87100000\0" \
	"ramdisk_addr_r=0x82100000\0" \
	"scriptaddr=0x87000000\0"

#define NFS_BOOTCMD \
	"nfsargs=ip=:::::eth0: root=/dev/nfs\0" \
	"nfsboot=run setup; " \
		"setenv bootargs ${defargs} ${nfsargs} " \
		"${setupargs} ${vidargs}; echo Booting from NFS...;" \
		"dhcp ${kernel_addr_r} && " \
		"tftp ${fdt_addr_r} ${soc}-colibri${variant}-${fdt_board}.dtb && " \
		"run fdt_fixup && bootz ${kernel_addr_r} - ${fdt_addr_r}\0" \

#define UBI_BOOTCMD	\
	"ubiargs=ubi.mtd=ubi root=ubi0:rootfs rootfstype=ubifs " \
		"ubi.fm_autoconvert=1\0" \
	"ubiboot=run setup; " \
		"setenv bootargs ${defargs} ${ubiargs} " \
		"${setupargs} ${vidargs}; echo Booting from NAND...; " \
		"ubi part ubi && run m4boot && " \
		"ubi read ${kernel_addr_r} kernel && " \
		"ubi read ${fdt_addr_r} dtb && " \
		"run fdt_fixup && bootz ${kernel_addr_r} - ${fdt_addr_r}\0" \

#if defined(CONFIG_TARGET_COLIBRI_IMX7_NAND)
#define CONFIG_BOOTCOMMAND "run ubiboot ; echo ; echo ubiboot failed ; " \
	"setenv fdtfile ${soc}-colibri-${fdt_board}.dtb && run distro_bootcmd;"
#define MODULE_EXTRA_ENV_SETTINGS \
	"mtdparts=" CONFIG_MTDPARTS_DEFAULT "\0" \
	UBI_BOOTCMD
#elif defined(CONFIG_TARGET_COLIBRI_IMX7_EMMC)
#define CONFIG_BOOTCOMMAND \
	"setenv fdtfile ${soc}-colibri-emmc-${fdt_board}.dtb && run distro_bootcmd;"
#define MODULE_EXTRA_ENV_SETTINGS \
	"variant=-emmc\0" \
	EMMC_ANDROID_BOOTCMD
#endif

#if defined(CONFIG_TARGET_COLIBRI_IMX7_NAND)
#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 0) \
	func(USB, usb, 0) \
	func(DHCP, dhcp, na)
#elif defined(CONFIG_TARGET_COLIBRI_IMX7_EMMC)
#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 1) \
	func(MMC, mmc, 0) \
	func(USB, usb, 0) \
	func(DHCP, dhcp, na)
#endif
#include <config_distro_bootcmd.h>

#define CONFIG_EXTRA_ENV_SETTINGS \
	BOOTENV \
	MEM_LAYOUT_ENV_SETTINGS \
	NFS_BOOTCMD \
	MODULE_EXTRA_ENV_SETTINGS \
	UBOOT_UPDATE \
	"boot_file=zImage\0" \
	"bootubipart=ubi\0" \
	"console=ttymxc0\0" \
	"defargs=\0" \
	"fdt_board=eval-v3\0" \
	"fdt_fixup=;\0" \
	"m4boot=;\0" \
	"ip_dyn=yes\0" \
	"kernel_file=zImage\0" \
	"setethupdate=if env exists ethaddr; then; else setenv ethaddr " \
		"00:14:2d:00:00:00; fi; tftpboot ${loadaddr} " \
		"${board}/flash_eth.img && source ${loadaddr}\0" \
	"setsdupdate=mmc rescan && setenv interface mmc && " \
		"fatload ${interface} 0:1 ${loadaddr} " \
		"${board}/flash_blk.img && source ${loadaddr}\0" \
	"setup=setenv setupargs " \
		"console=tty1 console=${console}" \
		",${baudrate}n8 ${memargs} consoleblank=0\0" \
	"setupdate=run setsdupdate || run setusbupdate || run setethupdate\0" \
	"setusbupdate=usb start && setenv interface usb && " \
		"fatload ${interface} 0:1 ${loadaddr} " \
		"${board}/flash_blk.img && source ${loadaddr}\0" \
	"splashpos=m,m\0" \
	"videomode=video=ctfb:x:640,y:480,depth:18,pclk:39722,le:48,ri:16,up:33,lo:10,hs:96,vs:2,sync:0,vmode:0\0" \
	"updlevel=2\0"

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

#if defined(CONFIG_ENV_IS_IN_MMC)
/* Environment in eMMC, before config block at the end of 1st "boot sector" */
#define CONFIG_SYS_MMC_ENV_DEV		0
#define CONFIG_SYS_MMC_ENV_PART		1
#endif

#ifdef CONFIG_TARGET_COLIBRI_IMX7_NAND
/* NAND stuff */
#define CONFIG_SYS_MAX_NAND_DEVICE	1
#define CONFIG_SYS_NAND_BASE		0x40000000
#define CONFIG_SYS_NAND_5_ADDR_CYCLE
#define CONFIG_SYS_NAND_ONFI_DETECTION
#define CONFIG_SYS_NAND_MX7_GPMI_62_ECC_BYTES
#endif

/* USB Configs */
#define CONFIG_EHCI_HCD_INIT_AFTER_RESET

#define CONFIG_MXC_USB_PORTSC		(PORT_PTS_UTMI | PORT_PTS_PTW)
#define CONFIG_MXC_USB_FLAGS		0
#define CONFIG_USB_MAX_CONTROLLER_COUNT 2

#define CONFIG_IMX_THERMAL

#define CONFIG_USBD_HS

#if defined(CONFIG_VIDEO) || defined(CONFIG_DM_VIDEO)
#define CONFIG_VIDEO_MXS
#define CONFIG_VIDEO_LOGO
#define CONFIG_SPLASH_SCREEN
#define CONFIG_SPLASH_SCREEN_ALIGN
#define CONFIG_BMP_16BPP
#define CONFIG_VIDEO_BMP_RLE8
#define CONFIG_VIDEO_BMP_LOGO
#endif

#endif
