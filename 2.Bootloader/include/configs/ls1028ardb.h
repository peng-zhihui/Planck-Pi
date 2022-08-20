/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2019 NXP
 */

#ifndef __LS1028A_RDB_H
#define __LS1028A_RDB_H

#include "ls1028a_common.h"

#define CONFIG_SYS_CLK_FREQ		100000000
#define CONFIG_DDR_CLK_FREQ		100000000
#define COUNTER_FREQUENCY_REAL		(CONFIG_SYS_CLK_FREQ / 4)

#define CONFIG_SYS_RTC_BUS_NUM         0

/* Store environment at top of flash */

#define CONFIG_DIMM_SLOTS_PER_CTLR          1

#define CONFIG_SYS_MONITOR_BASE CONFIG_SYS_TEXT_BASE

#define CONFIG_QIXIS_I2C_ACCESS

/*
 * QIXIS Definitions
 */
#define CONFIG_FSL_QIXIS

#ifdef CONFIG_FSL_QIXIS
#define QIXIS_BASE			0x7fb00000
#define QIXIS_BASE_PHYS			QIXIS_BASE
#define CONFIG_SYS_I2C_FPGA_ADDR	0x66
#define QIXIS_LBMAP_SWITCH		2
#define QIXIS_LBMAP_MASK		0xe0
#define QIXIS_LBMAP_SHIFT		0x5
#define QIXIS_LBMAP_DFLTBANK		0x00
#define QIXIS_LBMAP_ALTBANK		0x00
#define QIXIS_LBMAP_SD			0x00
#define QIXIS_LBMAP_EMMC		0x00
#define QIXIS_LBMAP_QSPI		0x00
#define QIXIS_RCW_SRC_SD		0xf8
#define QIXIS_RCW_SRC_EMMC		0xf9
#define QIXIS_RCW_SRC_QSPI		0xff
#define QIXIS_RST_CTL_RESET		0x31
#define QIXIS_RCFG_CTL_RECONFIG_IDLE	0x10
#define QIXIS_RCFG_CTL_RECONFIG_START	0x11
#define QIXIS_RCFG_CTL_WATCHDOG_ENBLE	0x08
#define QIXIS_RST_FORCE_MEM		0x01

#define CONFIG_SYS_FPGA_CSPR_EXT	(0x0)
#define CONFIG_SYS_FPGA_CSPR		(CSPR_PHYS_ADDR(QIXIS_BASE_PHYS) | \
					CSPR_PORT_SIZE_8 | \
					CSPR_MSEL_GPCM | \
					CSPR_V)
#define CONFIG_SYS_FPGA_CSOR		(CSOR_NOR_ADM_SHIFT(4) | \
					CSOR_NOR_NOR_MODE_AVD_NOR | \
					CSOR_NOR_TRHZ_80)
#endif

/* SATA */
#define CONFIG_SYS_SCSI_MAX_SCSI_ID		1
#define CONFIG_SYS_SCSI_MAX_LUN			1
#define CONFIG_SYS_SCSI_MAX_DEVICE		(CONFIG_SYS_SCSI_MAX_SCSI_ID * \
						CONFIG_SYS_SCSI_MAX_LUN)
#define SCSI_VEND_ID 0x1b4b
#define SCSI_DEV_ID  0x9170
#define CONFIG_SCSI_DEV_LIST {SCSI_VEND_ID, SCSI_DEV_ID}
#define CONFIG_SCSI_AHCI_PLAT
#define CONFIG_SYS_SATA1                        AHCI_BASE_ADDR1

/* Initial environment variables */
#ifndef SPL_NO_ENV
#undef CONFIG_EXTRA_ENV_SETTINGS
#define CONFIG_EXTRA_ENV_SETTINGS		\
	"board=ls1028ardb\0"			\
	"hwconfig=fsl_ddr:bank_intlv=auto\0"	\
	"ramdisk_addr=0x800000\0"		\
	"ramdisk_size=0x2000000\0"		\
	"bootm_size=0x10000000\0"		\
	"fdt_addr=0x00f00000\0"                 \
	"kernel_addr=0x01000000\0"              \
	"scriptaddr=0x80000000\0"               \
	"scripthdraddr=0x80080000\0"		\
	"fdtheader_addr_r=0x80100000\0"         \
	"kernelheader_addr_r=0x80200000\0"      \
	"load_addr=0xa0000000\0"            \
	"kernel_addr_r=0x81000000\0"            \
	"fdt_addr_r=0x90000000\0"               \
	"ramdisk_addr_r=0xa0000000\0"           \
	"kernel_start=0x1000000\0"		\
	"kernelheader_start=0x600000\0"		\
	"kernel_load=0xa0000000\0"		\
	"kernel_size=0x2800000\0"		\
	"kernelheader_size=0x40000\0"		\
	"kernel_addr_sd=0x8000\0"		\
	"kernel_size_sd=0x14000\0"		\
	"kernelhdr_addr_sd=0x3000\0"		\
	"kernelhdr_size_sd=0x20\0"		\
	"console=ttyS0,115200\0"                \
	"mtdparts=" CONFIG_MTDPARTS_DEFAULT "\0"	\
	BOOTENV					\
	"boot_scripts=ls1028ardb_boot.scr\0"    \
	"boot_script_hdr=hdr_ls1028ardb_bs.out\0"	\
	"scan_dev_for_boot_part="               \
		"part list ${devtype} ${devnum} devplist; "   \
		"env exists devplist || setenv devplist 1; "  \
		"for distro_bootpart in ${devplist}; do "     \
		  "if fstype ${devtype} "                  \
			"${devnum}:${distro_bootpart} "      \
			"bootfstype; then "                  \
			"run scan_dev_for_boot; "            \
		  "fi; "                                   \
		"done\0"                                   \
	"scan_dev_for_boot="				  \
		"echo Scanning ${devtype} "		  \
				"${devnum}:${distro_bootpart}...; "  \
		"for prefix in ${boot_prefixes}; do "	  \
			"run scan_dev_for_scripts; "	  \
		"done;"					  \
		"\0"					  \
	"boot_a_script="				  \
		"load ${devtype} ${devnum}:${distro_bootpart} "  \
			"${scriptaddr} ${prefix}${script}; "    \
		"env exists secureboot && load ${devtype} "     \
			"${devnum}:${distro_bootpart} "		\
			"${scripthdraddr} ${prefix}${boot_script_hdr} " \
			"&& esbc_validate ${scripthdraddr};"    \
		"source ${scriptaddr}\0"	  \
	"xspi_bootcmd=echo Trying load from FlexSPI flash ...;" \
		"sf probe 0:0 && sf read $load_addr " \
		"$kernel_start $kernel_size ; env exists secureboot &&" \
		"sf read $kernelheader_addr_r $kernelheader_start " \
		"$kernelheader_size && esbc_validate ${kernelheader_addr_r}; "\
		" bootm $load_addr#$board\0" \
	"xspi_hdploadcmd=echo Trying load HDP firmware from FlexSPI...;" \
		"sf probe 0:0 && sf read $load_addr 0x940000 0x30000 " \
		"&& hdp load $load_addr 0x2000\0"			\
	"sd_bootcmd=echo Trying load from SD ...;" \
		"mmc dev 0;mmcinfo; mmc read $load_addr "		\
		"$kernel_addr_sd $kernel_size_sd && "	\
		"env exists secureboot && mmc read $kernelheader_addr_r " \
		"$kernelhdr_addr_sd $kernelhdr_size_sd "		\
		" && esbc_validate ${kernelheader_addr_r};"	\
		"bootm $load_addr#$board\0"		\
	"sd_hdploadcmd=echo Trying load HDP firmware from SD..;"	\
		"mmc dev 0;mmcinfo;mmc read $load_addr 0x4a00 0x200 "	\
		"&& hdp load $load_addr 0x2000\0"	\
	"emmc_bootcmd=echo Trying load from EMMC ..;"	\
		"mmc dev 1;mmcinfo; mmc read $load_addr "		\
		"$kernel_addr_sd $kernel_size_sd && "	\
		"env exists secureboot && mmc read $kernelheader_addr_r " \
		"$kernelhdr_addr_sd $kernelhdr_size_sd "		\
		" && esbc_validate ${kernelheader_addr_r};"	\
		"bootm $load_addr#$board\0"			\
	"emmc_hdploadcmd=echo Trying load HDP firmware from EMMC..;"      \
		"mmc dev 1;mmcinfo;mmc read $load_addr 0x4a00 0x200 "	\
		"&& hdp load $load_addr 0x2000\0"
#endif
#endif /* __LS1028A_RDB_H */
