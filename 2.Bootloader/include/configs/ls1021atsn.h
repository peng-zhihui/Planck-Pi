/* SPDX-License-Identifier: GPL-2.0
 * Copyright 2016-2019 NXP Semiconductors
 * Copyright 2019 Vladimir Oltean <olteanv@gmail.com>
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#define CONFIG_ARMV7_SECURE_BASE	OCRAM_BASE_S_ADDR

#define CONFIG_SYS_FSL_CLK

#define CONFIG_DEEP_SLEEP

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN		(CONFIG_ENV_SIZE + 16 * 1024 * 1024)

#define CONFIG_SYS_INIT_RAM_ADDR	OCRAM_BASE_ADDR
#define CONFIG_SYS_INIT_RAM_SIZE	OCRAM_SIZE

/* XHCI Support - enabled by default */
#define CONFIG_USB_MAX_CONTROLLER_COUNT	1

#define CONFIG_SYS_CLK_FREQ		100000000
#define CONFIG_DDR_CLK_FREQ		100000000

#define DDR_SDRAM_CFG			0x470c0008
#define DDR_CS0_BNDS			0x008000bf
#define DDR_CS0_CONFIG			0x80014302
#define DDR_TIMING_CFG_0		0x50550004
#define DDR_TIMING_CFG_1		0xbcb38c56
#define DDR_TIMING_CFG_2		0x0040d120
#define DDR_TIMING_CFG_3		0x010e1000
#define DDR_TIMING_CFG_4		0x00000001
#define DDR_TIMING_CFG_5		0x03401400
#define DDR_SDRAM_CFG_2			0x00401010
#define DDR_SDRAM_MODE			0x00061c60
#define DDR_SDRAM_MODE_2		0x00180000
#define DDR_SDRAM_INTERVAL		0x18600618
#define DDR_DDR_WRLVL_CNTL		0x8655f605
#define DDR_DDR_WRLVL_CNTL_2		0x05060607
#define DDR_DDR_WRLVL_CNTL_3		0x05050505
#define DDR_DDR_CDR1			0x80040000
#define DDR_DDR_CDR2			0x00000001
#define DDR_SDRAM_CLK_CNTL		0x02000000
#define DDR_DDR_ZQ_CNTL			0x89080600
#define DDR_CS0_CONFIG_2		0
#define DDR_SDRAM_CFG_MEM_EN		0x80000000
#define SDRAM_CFG2_D_INIT		0x00000010
#define DDR_CDR2_VREF_TRAIN_EN		0x00000080
#define SDRAM_CFG2_FRC_SR		0x80000000
#define SDRAM_CFG_BI			0x00000001

#ifdef CONFIG_RAMBOOT_PBL
#define CONFIG_SYS_FSL_PBL_PBI	\
		"board/freescale/ls1021atsn/ls102xa_pbi.cfg"
#endif

#ifdef CONFIG_SD_BOOT
#define CONFIG_SYS_FSL_PBL_RCW	\
		"board/freescale/ls1021atsn/ls102xa_rcw_sd.cfg"

#ifdef CONFIG_SECURE_BOOT
#define CONFIG_U_BOOT_HDR_SIZE		(16 << 10)
#endif /* ifdef CONFIG_SECURE_BOOT */

#define CONFIG_SPL_MAX_SIZE		0x1a000
#define CONFIG_SPL_STACK		0x1001d000
#define CONFIG_SPL_PAD_TO		0x1c000

#define CONFIG_SYS_SPL_MALLOC_START	(CONFIG_SYS_TEXT_BASE + \
		CONFIG_SYS_MONITOR_LEN)
#define CONFIG_SYS_SPL_MALLOC_SIZE	0x100000
#define CONFIG_SPL_BSS_START_ADDR	0x80100000
#define CONFIG_SPL_BSS_MAX_SIZE		0x80000

#ifdef CONFIG_U_BOOT_HDR_SIZE
/*
 * HDR would be appended at end of image and copied to DDR along
 * with U-Boot image. Here u-boot max. size is 512K. So if binary
 * size increases then increase this size in case of secure boot as
 * it uses raw U-Boot image instead of FIT image.
 */
#define CONFIG_SYS_MONITOR_LEN		(0x100000 + CONFIG_U_BOOT_HDR_SIZE)
#else
#define CONFIG_SYS_MONITOR_LEN		0x100000
#endif /* ifdef CONFIG_U_BOOT_HDR_SIZE */
#endif

#define CONFIG_NR_DRAM_BANKS		1
#define PHYS_SDRAM			0x80000000
#define PHYS_SDRAM_SIZE			(1u * 1024 * 1024 * 1024)

#define CONFIG_SYS_DDR_SDRAM_BASE	0x80000000UL
#define CONFIG_SYS_SDRAM_BASE		CONFIG_SYS_DDR_SDRAM_BASE

#define CONFIG_CHIP_SELECTS_PER_CTRL	4

/* Serial Port */
#define CONFIG_CONS_INDEX		1
#define CONFIG_SYS_NS16550_SERIAL
#ifndef CONFIG_DM_SERIAL
#define CONFIG_SYS_NS16550_REG_SIZE	1
#endif
#define CONFIG_SYS_NS16550_CLK		get_serial_clock()

#define CONFIG_BAUDRATE			115200

/* I2C */
#ifndef CONFIG_DM_I2C
#define CONFIG_SYS_I2C
#else
#define CONFIG_I2C_SET_DEFAULT_BUS_NUM
#define CONFIG_I2C_DEFAULT_BUS_NUMBER 0
#endif
#define CONFIG_SYS_I2C_MXC
#define CONFIG_SYS_I2C_MXC_I2C1		/* enable I2C bus 1 */
#define CONFIG_SYS_I2C_MXC_I2C2		/* enable I2C bus 2 */
#define CONFIG_SYS_I2C_MXC_I2C3		/* enable I2C bus 3 */

/* EEPROM */
#define CONFIG_ID_EEPROM
#define CONFIG_SYS_I2C_EEPROM_NXID
#define CONFIG_SYS_EEPROM_BUS_NUM	0
#define CONFIG_SYS_I2C_EEPROM_ADDR	0x51
#define CONFIG_SYS_I2C_EEPROM_ADDR_LEN	2

/* QSPI */
#define FSL_QSPI_FLASH_SIZE		(1 << 24)
#define FSL_QSPI_FLASH_NUM		2

/* PCIe */
#define CONFIG_PCIE1			/* PCIE controller 1 */
#define CONFIG_PCIE2			/* PCIE controller 2 */
#define FSL_PCIE_COMPAT			"fsl,ls1021a-pcie"
#ifdef CONFIG_PCI
#define CONFIG_PCI_SCAN_SHOW
#endif

#define CONFIG_LAYERSCAPE_NS_ACCESS
#define COUNTER_FREQUENCY		12500000

#define CONFIG_HWCONFIG
#define HWCONFIG_BUFFER_SIZE		256

#define CONFIG_FSL_DEVICE_DISABLE

#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 0) \
	func(USB, usb, 0) \
	func(DHCP, dhcp, na)
#include <config_distro_bootcmd.h>

#define CONFIG_EXTRA_ENV_SETTINGS					\
	"bootargs=root=/dev/ram0 rw console=ttyS0,115200\0"		\
	"initrd_high=0xffffffff\0"					\
	"fdt_addr=0x64f00000\0"						\
	"kernel_addr=0x61000000\0"					\
	"kernelheader_addr=0x60800000\0"				\
	"scriptaddr=0x80000000\0"					\
	"scripthdraddr=0x80080000\0"					\
	"fdtheader_addr_r=0x80100000\0"					\
	"kernelheader_addr_r=0x80200000\0"				\
	"kernel_addr_r=0x80008000\0"					\
	"kernelheader_size=0x40000\0"					\
	"fdt_addr_r=0x8f000000\0"					\
	"ramdisk_addr_r=0xa0000000\0"					\
	"load_addr=0x80008000\0"					\
	"kernel_size=0x2800000\0"					\
	"kernel_addr_sd=0x8000\0"					\
	"kernel_size_sd=0x14000\0"					\
	"kernelhdr_addr_sd=0x4000\0"					\
	"kernelhdr_size_sd=0x10\0"					\
	BOOTENV								\
	"boot_scripts=ls1021atsn_boot.scr\0"				\
	"boot_script_hdr=hdr_ls1021atsn_bs.out\0"			\
		"scan_dev_for_boot_part="				\
			"part list ${devtype} ${devnum} devplist; "	\
			"env exists devplist || setenv devplist 1; "	\
			"for distro_bootpart in ${devplist}; do "	\
			"if fstype ${devtype} "				\
				"${devnum}:${distro_bootpart} "		\
				"bootfstype; then "			\
				"run scan_dev_for_boot; "		\
			"fi; "						\
		"done\0"						\
	"scan_dev_for_boot="						\
		"echo Scanning ${devtype} "				\
				"${devnum}:${distro_bootpart}...; "	\
		"for prefix in ${boot_prefixes}; do "			\
			"run scan_dev_for_scripts; "			\
			"run scan_dev_for_extlinux; "			\
		"done;"							\
		"\0"							\
	"boot_a_script="						\
		"load ${devtype} ${devnum}:${distro_bootpart} "		\
			"${scriptaddr} ${prefix}${script}; "		\
		"env exists secureboot && load ${devtype} "		\
			"${devnum}:${distro_bootpart} "			\
			"${scripthdraddr} ${prefix}${boot_script_hdr} "	\
			"&& esbc_validate ${scripthdraddr};"		\
		"source ${scriptaddr}\0"				\
	"qspi_bootcmd=echo Trying load from qspi..;"			\
		"sf probe && sf read $load_addr "			\
		"$kernel_addr $kernel_size; env exists secureboot "	\
		"&& sf read $kernelheader_addr_r $kernelheader_addr "	\
		"$kernelheader_size && esbc_validate ${kernelheader_addr_r}; " \
		"bootm $load_addr#$board\0"				\
	"sd_bootcmd=echo Trying load from SD ..;"			\
		"mmcinfo && mmc read $load_addr "			\
		"$kernel_addr_sd $kernel_size_sd && "			\
		"env exists secureboot && mmc read $kernelheader_addr_r " \
		"$kernelhdr_addr_sd $kernelhdr_size_sd "		\
		" && esbc_validate ${kernelheader_addr_r};"		\
		"bootm $load_addr#$board\0"

/* Miscellaneous configurable options */
#define CONFIG_SYS_BOOTMAPSZ		(256 << 20)

#define CONFIG_SYS_CBSIZE		256	/* Console I/O Buffer Size */
#define CONFIG_SYS_PBSIZE		\
		(CONFIG_SYS_CBSIZE + sizeof(CONFIG_SYS_PROMPT) + 16)
#define CONFIG_SYS_MAXARGS		16	/* max number of command args */
#define CONFIG_SYS_BARGSIZE		CONFIG_SYS_CBSIZE

#define CONFIG_SYS_LOAD_ADDR		0x82000000

#define CONFIG_LS102XA_STREAM_ID

#define CONFIG_SYS_INIT_SP_OFFSET \
	(CONFIG_SYS_INIT_RAM_SIZE - GENERATED_GBL_DATA_SIZE)
#define CONFIG_SYS_INIT_SP_ADDR \
	(CONFIG_SYS_INIT_RAM_ADDR + CONFIG_SYS_INIT_SP_OFFSET)

#ifdef CONFIG_SPL_BUILD
#define CONFIG_SYS_MONITOR_BASE CONFIG_SPL_TEXT_BASE
#else
#define CONFIG_SYS_MONITOR_BASE CONFIG_SYS_TEXT_BASE    /* start of monitor */
#endif

/* Environment */
#define CONFIG_ENV_OVERWRITE

#if defined(CONFIG_SD_BOOT)
#define CONFIG_SYS_MMC_ENV_DEV		0
#endif

#define CONFIG_SYS_BOOTM_LEN		0x8000000 /* 128 MB */

#endif
