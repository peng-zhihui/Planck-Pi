/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2013 Gabor Juhos <juhosg@openwrt.org>
 */

#ifndef _MALTA_CONFIG_H
#define _MALTA_CONFIG_H

/*
 * System configuration
 */
#define CONFIG_MALTA

#define CONFIG_MEMSIZE_IN_BYTES

#define CONFIG_PCI_GT64120
#define CONFIG_PCI_MSC01
#define CONFIG_PCNET
#define PCNET_HAS_PROM

#define CONFIG_SYS_ISA_IO_BASE_ADDRESS	0

/*
 * CPU Configuration
 */
#define CONFIG_SYS_MHZ			250	/* arbitrary value */
#define CONFIG_SYS_MIPS_TIMER_FREQ	(CONFIG_SYS_MHZ * 1000000)

/*
 * Memory map
 */
#define CONFIG_SYS_MONITOR_BASE		CONFIG_SYS_TEXT_BASE

#ifdef CONFIG_64BIT
# define CONFIG_SYS_SDRAM_BASE		0xffffffff80000000
#else
# define CONFIG_SYS_SDRAM_BASE		0x80000000
#endif
#define CONFIG_SYS_MEM_SIZE		(256 * 1024 * 1024)

#define CONFIG_SYS_INIT_SP_OFFSET	0x400000

#define CONFIG_SYS_LOAD_ADDR		(CONFIG_SYS_SDRAM_BASE + 0x01000000)

#define CONFIG_SYS_MALLOC_LEN		(128 * 1024)
#define CONFIG_SYS_BOOTPARAMS_LEN	(128 * 1024)
#define CONFIG_SYS_BOOTM_LEN		(64 * 1024 * 1024)

/*
 * Serial driver
 */
#define CONFIG_SYS_NS16550_PORT_MAPPED

/*
 * Flash configuration
 */
#ifdef CONFIG_64BIT
# define CONFIG_SYS_FLASH_BASE		0xffffffffbe000000
#else
# define CONFIG_SYS_FLASH_BASE		0xbe000000
#endif
#define CONFIG_SYS_MAX_FLASH_BANKS	1
#define CONFIG_SYS_MAX_FLASH_SECT	128

/*
 * Environment
 */

/*
 * IDE/ATA
 */
#define CONFIG_SYS_IDE_MAXBUS		1
#define CONFIG_SYS_IDE_MAXDEVICE	2
#define CONFIG_SYS_ATA_BASE_ADDR	CONFIG_SYS_ISA_IO_BASE_ADDRESS
#define CONFIG_SYS_ATA_IDE0_OFFSET	0x01f0
#define CONFIG_SYS_ATA_DATA_OFFSET	0
#define CONFIG_SYS_ATA_REG_OFFSET	0

/*
 * Commands
 */

#endif /* _MALTA_CONFIG_H */
