/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author(s): Patrice Chotard, <patrice.chotard@st.com> for STMicroelectronics.
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <config.h>
#include <linux/sizes.h>

/* For booting Linux, use the first 16MB of memory */
#define CONFIG_SYS_BOOTMAPSZ		SZ_16M

#define CONFIG_SYS_FLASH_BASE		0x08000000
#define CONFIG_SYS_INIT_SP_ADDR		0x24040000

/*
 * Configuration of the external SDRAM memory
 */
#define CONFIG_SYS_LOAD_ADDR		0xD0400000
#define CONFIG_LOADADDR			0xD0400000

#define CONFIG_SYS_HZ_CLOCK		1000000

#define CONFIG_CMDLINE_TAG
#define CONFIG_SETUP_MEMORY_TAGS
#define CONFIG_INITRD_TAG
#define CONFIG_REVISION_TAG

#define CONFIG_SYS_MAXARGS		16
#define CONFIG_SYS_MALLOC_LEN		(1 * 1024 * 1024)

#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 0)

#include <config_distro_bootcmd.h>
#define CONFIG_EXTRA_ENV_SETTINGS				\
			"kernel_addr_r=0xD0008000\0"		\
			"fdtfile=stm32h743i-disco.dtb\0"	\
			"fdt_addr_r=0xD0408000\0"		\
			"scriptaddr=0xD0418000\0"		\
			"pxefile_addr_r=0xD0428000\0" \
			"ramdisk_addr_r=0xD0438000\0"		\
			BOOTENV

#endif /* __CONFIG_H */
