/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Sysam stmark2 board configuration
 *
 * (C) Copyright 2017  Angelo Dureghello <angelo@sysam.it>
 */

#ifndef __STMARK2_CONFIG_H
#define __STMARK2_CONFIG_H

#define CONFIG_HOSTNAME			"stmark2"

#define CONFIG_MCFUART
#define CONFIG_SYS_UART_PORT		0
#define CONFIG_SYS_BAUDRATE_TABLE	{ 9600 , 19200 , 38400 , 57600, 115200 }

#define LDS_BOARD_TEXT						\
	board/sysam/stmark2/sbf_dram_init.o (.text*)

#define CONFIG_TIMESTAMP

#define CONFIG_BOOTARGS						\
	"console=ttyS0,115200 root=/dev/ram0 rw "		\
		"rootfstype=ramfs "				\
		"rdinit=/bin/init "				\
		"devtmpfs.mount=1"

#define CONFIG_BOOTCOMMAND					\
	"sf probe 0:1 50000000; "				\
	"sf read ${loadaddr} 0x100000 ${kern_size}; "		\
	"bootm ${loadaddr}"

#define CONFIG_EXTRA_ENV_SETTINGS				\
	"kern_size=0x700000\0"					\
	"loadaddr=0x40001000\0"					\
		"-(rootfs)\0"					\
	"update_uboot=loady ${loadaddr}; "			\
		"sf probe 0:1 50000000; "			\
		"sf erase 0 0x80000; "				\
		"sf write ${loadaddr} 0 ${filesize}\0"		\
	"update_kernel=loady ${loadaddr}; "			\
		"setenv kern_size ${filesize}; saveenv; "	\
		"sf probe 0:1 50000000; "			\
		"sf erase 0x100000 0x700000; "			\
		"sf write ${loadaddr} 0x100000 ${filesize}\0"	\
	"update_rootfs=loady ${loadaddr}; "			\
		"sf probe 0:1 50000000; "			\
		"sf erase 0x00800000 0x100000; "		\
		"sf write ${loadaddr} 0x00800000 ${filesize}\0"	\
	""

/* Realtime clock */
#undef CONFIG_MCFRTC
#define CONFIG_RTC_MCFRRTC
#define CONFIG_SYS_MCFRRTC_BASE		0xFC0A8000

/* spi not partitions */
#define CONFIG_JFFS2_CMDLINE
#define CONFIG_JFFS2_DEV		"nor0"

/* Timer */
#define CONFIG_MCFTMR

/* DSPI and Serial Flash */
#define CONFIG_CF_DSPI
#define CONFIG_SERIAL_FLASH

#define CONFIG_SYS_SBFHDR_SIZE		0x7

/* Input, PCI, Flexbus, and VCO */
#define CONFIG_EXTRA_CLOCK

#define CONFIG_PRAM			2048	/* 2048 KB */
#define CONFIG_SYS_CBSIZE		256	/* Console I/O Buffer Size */

/* Print Buffer Size */
#define CONFIG_SYS_PBSIZE		(CONFIG_SYS_CBSIZE + \
					sizeof(CONFIG_SYS_PROMPT) + 16)
#define CONFIG_SYS_MAXARGS		16
/* Boot Argument Buffer Size    */
#define CONFIG_SYS_BARGSIZE		CONFIG_SYS_CBSIZE

#define CONFIG_SYS_LOAD_ADDR		(CONFIG_SYS_SDRAM_BASE + 0x10000)
#define CONFIG_SYS_MBAR			0xFC000000

/*
 * Definitions for initial stack pointer and data area (in internal SRAM)
 */
#define CONFIG_SYS_INIT_RAM_ADDR	0x80000000
/* End of used area in internal SRAM */
#define CONFIG_SYS_INIT_RAM_SIZE	0x10000
#define CONFIG_SYS_INIT_RAM_CTRL	0x221
#define CONFIG_SYS_GBL_DATA_OFFSET	((CONFIG_SYS_INIT_RAM_SIZE - \
					GENERATED_GBL_DATA_SIZE) - 32)
#define CONFIG_SYS_INIT_SP_OFFSET	CONFIG_SYS_GBL_DATA_OFFSET
#define CONFIG_SYS_SBFHDR_DATA_OFFSET	(CONFIG_SYS_INIT_RAM_SIZE - 32)

/*
 * Start addresses for the final memory configuration
 * (Set up by the startup code)
 * Please note that CONFIG_SYS_SDRAM_BASE _must_ start at 0
 */
#define CONFIG_SYS_SDRAM_BASE		0x40000000
#define CONFIG_SYS_SDRAM_SIZE		128	/* SDRAM size in MB */

#define CONFIG_SYS_DRAM_TEST

#if defined(CONFIG_CF_SBF)
#define CONFIG_SERIAL_BOOT
#endif

#if defined(CONFIG_SERIAL_BOOT)
#define CONFIG_SYS_MONITOR_BASE		(CONFIG_SYS_TEXT_BASE + 0x400)
#else
#define CONFIG_SYS_MONITOR_BASE		(CONFIG_SYS_FLASH_BASE + 0x400)
#endif

#define CONFIG_SYS_BOOTPARAMS_LEN	(64 * 1024)
/* Reserve 256 kB for Monitor */
#define CONFIG_SYS_MONITOR_LEN		(256 << 10)
/* Reserve 256 kB for malloc() */
#define CONFIG_SYS_MALLOC_LEN		(256 << 10)

/*
 * For booting Linux, the board info and command line data
 * have to be in the first 8 MB of memory, since this is
 * the maximum mapped by the Linux kernel during initialization ??
 */
/* Initial Memory map for Linux */
#define CONFIG_SYS_BOOTMAPSZ		(CONFIG_SYS_SDRAM_BASE + \
					(CONFIG_SYS_SDRAM_SIZE << 20))

/* Configuration for environment
 * Environment is embedded in u-boot in the second sector of the flash
 */

#if defined(CONFIG_CF_SBF)
#define CONFIG_ENV_IS_IN_SPI_FLASH	1
#endif

#undef CONFIG_ENV_OVERWRITE

/* Cache Configuration */
#define CONFIG_SYS_CACHELINE_SIZE	16
#define ICACHE_STATUS			(CONFIG_SYS_INIT_RAM_ADDR + \
					 CONFIG_SYS_INIT_RAM_SIZE - 8)
#define DCACHE_STATUS			(CONFIG_SYS_INIT_RAM_ADDR + \
					 CONFIG_SYS_INIT_RAM_SIZE - 4)
#define CONFIG_SYS_ICACHE_INV		(CF_CACR_BCINVA + CF_CACR_ICINVA)
#define CONFIG_SYS_DCACHE_INV		(CF_CACR_DCINVA)
#define CONFIG_SYS_CACHE_ACR2		(CONFIG_SYS_SDRAM_BASE | \
					 CF_ADDRMASK(CONFIG_SYS_SDRAM_SIZE) | \
					 CF_ACR_EN | CF_ACR_SM_ALL)
#define CONFIG_SYS_CACHE_ICACR		(CF_CACR_BEC | CF_CACR_IEC | \
					 CF_CACR_ICINVA | CF_CACR_EUSP)
#define CONFIG_SYS_CACHE_DCACR		((CONFIG_SYS_CACHE_ICACR | \
					 CF_CACR_DEC | CF_CACR_DDCM_P | \
					 CF_CACR_DCINVA) & ~CF_CACR_ICINVA)

#define CACR_STATUS			(CONFIG_SYS_INIT_RAM_ADDR + \
					CONFIG_SYS_INIT_RAM_SIZE - 12)

#ifdef CONFIG_MCFFEC
#define CONFIG_MII_INIT			1
#define CONFIG_SYS_DISCOVER_PHY
#define CONFIG_SYS_RX_ETH_BUFFER	8
#define CONFIG_SYS_FAULT_ECHO_LINK_DOWN
/* If CONFIG_SYS_DISCOVER_PHY is not defined - hardcoded */
#ifndef CONFIG_SYS_DISCOVER_PHY
#define FECDUPLEX			FULL
#define FECSPEED			_100BASET
#else
#ifndef CONFIG_SYS_FAULT_ECHO_LINK_DOWN
#define CONFIG_SYS_FAULT_ECHO_LINK_DOWN
#endif
#endif /* CONFIG_SYS_DISCOVER_PHY */
#endif
#endif /* __STMARK2_CONFIG_H */
