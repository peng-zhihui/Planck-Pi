/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2018, Bin Meng <bmeng.cn@gmail.com>
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <linux/sizes.h>

#ifdef CONFIG_SPL

#define CONFIG_SPL_MAX_SIZE		0x00100000
#define CONFIG_SPL_BSS_START_ADDR	0x84000000
#define CONFIG_SPL_BSS_MAX_SIZE		0x00100000
#define CONFIG_SYS_SPL_MALLOC_START	0x84100000
#define CONFIG_SYS_SPL_MALLOC_SIZE	0x00100000

#define CONFIG_SPL_LOAD_FIT_ADDRESS	0x80200000

#endif

#define CONFIG_SYS_SDRAM_BASE		0x80000000
#define CONFIG_SYS_INIT_SP_ADDR		(CONFIG_SYS_SDRAM_BASE + SZ_2M)

#define CONFIG_SYS_LOAD_ADDR		(CONFIG_SYS_SDRAM_BASE + SZ_2M)

#define CONFIG_SYS_MALLOC_LEN		SZ_8M

#define CONFIG_SYS_BOOTM_LEN		SZ_64M

#define CONFIG_STANDALONE_LOAD_ADDR	0x80200000

/* Environment options */

#ifndef CONFIG_SPL_BUILD
#define BOOT_TARGET_DEVICES(func) \
	func(QEMU, qemu, na) \
	func(VIRTIO, virtio, 0) \
	func(DHCP, dhcp, na)

#include <config_distro_bootcmd.h>

#define BOOTENV_DEV_QEMU(devtypeu, devtypel, instance) \
	"bootcmd_qemu=" \
		"if env exists kernel_start; then " \
			"bootm ${kernel_start} - ${fdtcontroladdr};" \
		"fi;\0"

#define BOOTENV_DEV_NAME_QEMU(devtypeu, devtypel, instance) \
	"qemu "

#define CONFIG_EXTRA_ENV_SETTINGS \
	"fdt_high=0xffffffffffffffff\0" \
	"initrd_high=0xffffffffffffffff\0" \
	"kernel_addr_r=0x84000000\0" \
	"fdt_addr_r=0x88000000\0" \
	"scriptaddr=0x88100000\0" \
	"pxefile_addr_r=0x88200000\0" \
	"ramdisk_addr_r=0x88300000\0" \
	BOOTENV
#endif

#endif /* __CONFIG_H */
