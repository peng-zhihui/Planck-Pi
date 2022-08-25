/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Configuration for MediaTek MT7623 SoC
 *
 * Copyright (C) 2018 MediaTek Inc.
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef __MT7623_H
#define __MT7623_H

#include <linux/sizes.h>

/* Miscellaneous configurable options */
#define CONFIG_SETUP_MEMORY_TAGS
#define CONFIG_INITRD_TAG
#define CONFIG_CMDLINE_TAG

#define CONFIG_SYS_MAXARGS		8
#define CONFIG_SYS_BOOTM_LEN		SZ_64M
#define CONFIG_SYS_CBSIZE		SZ_1K
#define CONFIG_SYS_PBSIZE		(CONFIG_SYS_CBSIZE +	\
					sizeof(CONFIG_SYS_PROMPT) + 16)

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN		SZ_4M
#define CONFIG_SYS_NONCACHED_MEMORY	SZ_1M

/* Environment */
/* Allow to overwrite serial and ethaddr */
#define CONFIG_ENV_OVERWRITE

/* Preloader -> Uboot */
#define CONFIG_SYS_INIT_SP_ADDR		(CONFIG_SYS_TEXT_BASE + SZ_2M - \
					 GENERATED_GBL_DATA_SIZE)

/* UBoot -> Kernel */
#define CONFIG_LOADADDR			0x84000000
#define CONFIG_SYS_LOAD_ADDR		CONFIG_LOADADDR

/* MMC */
#define MMC_SUPPORTS_TUNING

/* DRAM */
#define CONFIG_SYS_SDRAM_BASE		0x80000000

/* This is needed for kernel booting */
#define FDT_HIGH			"0xac000000"

#define ENV_MEM_LAYOUT_SETTINGS				\
	"fdt_high=" FDT_HIGH "\0"			\
	"kernel_addr_r=0x84000000\0"			\
	"fdt_addr_r=" FDT_HIGH "\0"			\
	"fdtfile=mt7623n-bananapi-bpi-r2.dtb" "\0"

/* Ethernet */
#define CONFIG_IPADDR			192.168.1.1
#define CONFIG_SERVERIP			192.168.1.2

#define CONFIG_SYS_MMC_ENV_DEV		0

#ifdef CONFIG_DISTRO_DEFAULTS

#define BOOT_TARGET_DEVICES(func)	\
		func(MMC, mmc, 1)

#include <config_distro_bootcmd.h>

/* Extra environment variables */
#define CONFIG_EXTRA_ENV_SETTINGS	\
	ENV_MEM_LAYOUT_SETTINGS		\
	BOOTENV

#endif /* ifdef CONFIG_DISTRO_DEFAULTS*/

#endif
