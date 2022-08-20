/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2018 NXP
 */

#ifndef __LS1012AFRWY_H__
#define __LS1012AFRWY_H__

#include "ls1012a_common.h"

#undef CONFIG_SYS_BOARD
#define CONFIG_SYS_BOARD "ls1012afrwy"

/* Board Rev*/
#define BOARD_REV_A_B			0x0
#define BOARD_REV_C			0x00080000
#define BOARD_REV_MASK			0x001A0000
/* DDR */
#define CONFIG_DIMM_SLOTS_PER_CTLR	1
#define CONFIG_CHIP_SELECTS_PER_CTRL	1
#define SYS_SDRAM_SIZE_512		0x20000000
#define SYS_SDRAM_SIZE_1024		0x40000000
#define CONFIG_CHIP_SELECTS_PER_CTRL	1

/* ENV */
#define CONFIG_SYS_FSL_QSPI_BASE	0x40000000

#ifndef CONFIG_SPL_BUILD
#undef BOOT_TARGET_DEVICES
#define BOOT_TARGET_DEVICES(func) \
	func(MMC, mmc, 0) \
	func(USB, usb, 0) \
	func(DHCP, dhcp, na)
#endif

/*  MMC  */
#ifdef CONFIG_MMC
#define CONFIG_SYS_FSL_MMC_HAS_CAPBLT_VS33
#endif

#define CONFIG_PCIE1		/* PCIE controller 1 */

#define CONFIG_PCI_SCAN_SHOW

#undef CONFIG_EXTRA_ENV_SETTINGS
#define CONFIG_EXTRA_ENV_SETTINGS		\
	"verify=no\0"				\
	"fdt_high=0xffffffffffffffff\0"		\
	"initrd_high=0xffffffffffffffff\0"	\
	"fdt_addr=0x00f00000\0"			\
	"kernel_addr=0x01000000\0"		\
	"kernel_size_sd=0x16000\0"		\
	"kernelhdr_size_sd=0x10\0"		\
	"kernel_addr_sd=0x8000\0"		\
	"kernelhdr_addr_sd=0x4000\0"		\
	"kernelheader_addr=0x1fc000\0"		\
	"kernelheader_addr=0x1fc000\0"		\
	"scriptaddr=0x80000000\0"		\
	"scripthdraddr=0x80080000\0"		\
	"fdtheader_addr_r=0x80100000\0"		\
	"kernelheader_addr_r=0x80200000\0"	\
	"kernelheader_size=0x40000\0"		\
	"kernel_addr_r=0x92000000\0"		\
	"fdt_addr_r=0x90000000\0"		\
	"load_addr=0x92000000\0"		\
	"kernel_size=0x2800000\0"		\
	"kernelheader_size=0x40000\0"		\
	"console=ttyS0,115200\0"		\
	"BOARD=ls1012afrwy\0"			\
	BOOTENV					\
	"boot_scripts=ls1012afrwy_boot.scr\0"	\
	"boot_script_hdr=hdr_ls1012afrwy_bs.out\0"	\
	"scan_dev_for_boot_part="		\
	     "part list ${devtype} ${devnum} devplist; "	\
	     "env exists devplist || setenv devplist 1; "	\
	     "for distro_bootpart in ${devplist}; do "		\
		  "if fstype ${devtype} "			\
		      "${devnum}:${distro_bootpart} "		\
		      "bootfstype; then "			\
		      "run scan_dev_for_boot; "	\
		  "fi; "			\
	      "done\0"				\
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
			"${scripthdraddr} ${prefix}${boot_script_hdr}; " \
			"env exists secureboot "	\
			"&& esbc_validate ${scripthdraddr};"    \
		"source ${scriptaddr}\0"	  \
	"sd_bootcmd=pfe stop; echo Trying load from sd card..;"		\
		"mmcinfo; mmc read $load_addr "			\
		"$kernel_addr_sd $kernel_size_sd ;"		\
		"env exists secureboot && mmc read $kernelheader_addr_r "\
		"$kernelhdr_addr_sd $kernelhdr_size_sd "		\
		" && esbc_validate ${kernelheader_addr_r};"	\
		"bootm $load_addr#$BOARD\0"

#undef CONFIG_BOOTCOMMAND
#ifdef CONFIG_TFABOOT
#undef QSPI_NOR_BOOTCOMMAND
#define QSPI_NOR_BOOTCOMMAND "pfe stop; run distro_bootcmd; run sd_bootcmd; "\
			     "env exists secureboot && esbc_halt;"
#else
#define CONFIG_BOOTCOMMAND "pfe stop; run distro_bootcmd; run sd_bootcmd; "\
			   "env exists secureboot && esbc_halt;"
#endif

#include <asm/fsl_secure_boot.h>

#include <asm/fsl_secure_boot.h>
#endif /* __LS1012AFRWY_H__ */
