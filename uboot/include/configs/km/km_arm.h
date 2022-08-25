/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2009
 * Marvell Semiconductor <www.marvell.com>
 * Prafulla Wadaskar <prafulla@marvell.com>
 *
 * (C) Copyright 2009
 * Stefan Roese, DENX Software Engineering, sr@denx.de.
 *
 * (C) Copyright 2010-2011
 * Heiko Schocher, DENX Software Engineering, hs@denx.de.
 */

/*
 * for linking errors see
 * http://lists.denx.de/pipermail/u-boot/2009-July/057350.html
 */

#ifndef _CONFIG_KM_ARM_H
#define _CONFIG_KM_ARM_H

/*
 * High Level Configuration Options (easy to change)
 */
#define CONFIG_FEROCEON_88FR131		/* CPU Core subversion */
#define CONFIG_KW88F6281		/* SOC Name */

#define CONFIG_MACH_TYPE	MACH_TYPE_KM_KIRKWOOD

#define CONFIG_NAND_ECC_BCH

/* include common defines/options for all Keymile boards */
#include "keymile-common.h"

/* Reserve 4 MB for malloc */
#define CONFIG_SYS_MALLOC_LEN		(4 * 1024 * 1024)

#include "asm/arch/config.h"

#define CONFIG_SYS_LOAD_ADDR	0x00800000	/* default load adr- 8M */

/* architecture specific default bootargs */
#define CONFIG_KM_DEF_BOOT_ARGS_CPU					\
		"bootcountaddr=${bootcountaddr} ${mtdparts}"		\
		" boardid=0x${IVM_BoardId} hwkey=0x${IVM_HWKey}"

#define CONFIG_KM_DEF_ENV_CPU						\
	"u-boot="CONFIG_HOSTNAME "/u-boot.kwb\0"		\
	CONFIG_KM_UPDATE_UBOOT						\
	"set_fdthigh=setenv fdt_high ${kernelmem}\0"			\
	"checkfdt="							\
		"if cramfsls fdt_0x${IVM_BoardId}_0x${IVM_HWKey}.dtb; "	\
		"then true; else setenv cramfsloadfdt true; "		\
		"setenv boot bootm ${load_addr_r}; "			\
		"echo No FDT found, booting with the kernel "		\
		"appended one; fi\0"					\
	""

#define CONFIG_SKIP_LOWLEVEL_INIT	/* disable board lowlevel_init */

/*
 * For booting Linux, the board info and command line data
 * have to be in the first 8 MB of memory, since this is
 * the maximum mapped by the Linux kernel during initialization.
 */
#define CONFIG_CMDLINE_TAG		/* enable passing of ATAGs  */
#define CONFIG_INITRD_TAG		/* enable INITRD tag */
#define CONFIG_SETUP_MEMORY_TAGS	/* enable memory tag */

/*
 * NAND Flash configuration
 */
#define CONFIG_SYS_MAX_NAND_DEVICE	1

/*
 * Other required minimal configurations
 */

/*
 * Ethernet Driver configuration
 */
#define PHY_ANEG_TIMEOUT	8000	/* PHY needs a longer autoneg timeout */
#define CONFIG_MVGBE_PORTS	{1, 0}	/* enable port 0 only */
#define CONFIG_PHY_BASE_ADR	0

/*
 * I2C related stuff
 */
#undef CONFIG_I2C_MVTWSI
#define CONFIG_SYS_I2C
#define	CONFIG_SYS_I2C_SOFT	/* I2C bit-banged	*/
#define CONFIG_SYS_I2C_INIT_BOARD

#define	CONFIG_KIRKWOOD_GPIO		/* Enable GPIO Support */
#define CONFIG_SYS_NUM_I2C_BUSES	6
#define CONFIG_SYS_I2C_MAX_HOPS		1
#define CONFIG_SYS_I2C_BUSES	{	{0, {I2C_NULL_HOP} }, \
					{0, {{I2C_MUX_PCA9547, 0x70, 1} } }, \
					{0, {{I2C_MUX_PCA9547, 0x70, 2} } }, \
					{0, {{I2C_MUX_PCA9547, 0x70, 3} } }, \
					{0, {{I2C_MUX_PCA9547, 0x70, 4} } }, \
					{0, {{I2C_MUX_PCA9547, 0x70, 5} } }, \
				}

#ifndef __ASSEMBLY__
#include <asm/arch/gpio.h>
#include <linux/delay.h>
#include <linux/stringify.h>
extern void __set_direction(unsigned pin, int high);
void set_sda(int state);
void set_scl(int state);
int get_sda(void);
int get_scl(void);
#define KM_KIRKWOOD_SDA_PIN	8
#define KM_KIRKWOOD_SCL_PIN	9
#define KM_KIRKWOOD_SOFT_I2C_GPIOS	0x0300
#define KM_KIRKWOOD_ENV_WP	38

#define I2C_ACTIVE	__set_direction(KM_KIRKWOOD_SDA_PIN, 0)
#define I2C_TRISTATE	__set_direction(KM_KIRKWOOD_SDA_PIN, 1)
#define I2C_READ	(kw_gpio_get_value(KM_KIRKWOOD_SDA_PIN) ? 1 : 0)
#define I2C_SDA(bit)	kw_gpio_set_value(KM_KIRKWOOD_SDA_PIN, bit)
#define I2C_SCL(bit)	kw_gpio_set_value(KM_KIRKWOOD_SCL_PIN, bit)
#endif

#define I2C_DELAY	udelay(1)
#define I2C_SOFT_DECLARATIONS

#define	CONFIG_SYS_I2C_SOFT_SLAVE	0x0
#define	CONFIG_SYS_I2C_SOFT_SPEED	100000

/* EEprom support 24C128, 24C256 valid for environment eeprom */
#define CONFIG_SYS_EEPROM_PAGE_WRITE_ENABLE
#define CONFIG_SYS_EEPROM_PAGE_WRITE_BITS	6 /* 64 Byte write page */
#define CONFIG_SYS_EEPROM_PAGE_WRITE_DELAY_MS	10

#define CONFIG_SYS_I2C_EEPROM_ADDR	0x50
#define CONFIG_SYS_I2C_EEPROM_ADDR_LEN	2

/*
 *  Environment variables configurations
 */
#if defined CONFIG_KM_ENV_IS_IN_SPI_NOR
#define CONFIG_ENV_TOTAL_SIZE		0x20000     /* no bracets! */
#else
#define CONFIG_SYS_DEF_EEPROM_ADDR	0x50
#define CONFIG_ENV_EEPROM_IS_ON_I2C
#define CONFIG_SYS_EEPROM_WREN
#define CONFIG_I2C_ENV_EEPROM_BUS 5 /* I2C2 (Mux-Port 5) */
#endif

#define KM_FLASH_GPIO_PIN	16

#define	CONFIG_KM_UPDATE_UBOOT						\
	"update="							\
		"sf probe 0;sf erase 0 +${filesize};"			\
		"sf write ${load_addr_r} 0 ${filesize};\0"

#if defined CONFIG_KM_ENV_IS_IN_SPI_NOR
#define CONFIG_KM_NEW_ENV						\
	"newenv=sf probe 0;"						\
		"sf erase " __stringify(CONFIG_ENV_OFFSET) " "		\
		__stringify(CONFIG_ENV_TOTAL_SIZE)"\0"
#else
#define CONFIG_KM_NEW_ENV						\
	"newenv=setenv addr 0x100000 && "				\
		"i2c dev " __stringify(CONFIG_I2C_ENV_EEPROM_BUS) "; "  \
		"mw.b ${addr} 0 4 && "					\
		"eeprom write " __stringify(CONFIG_SYS_DEF_EEPROM_ADDR)	\
		" ${addr} " __stringify(CONFIG_ENV_OFFSET) " 4 && "	\
		"eeprom write " __stringify(CONFIG_SYS_DEF_EEPROM_ADDR)	\
		" ${addr} " __stringify(CONFIG_ENV_OFFSET_REDUND) " 4\0"
#endif

#ifndef CONFIG_KM_BOARD_EXTRA_ENV
#define CONFIG_KM_BOARD_EXTRA_ENV       ""
#endif

/*
 * Default environment variables
 */
#define CONFIG_EXTRA_ENV_SETTINGS					\
	CONFIG_KM_BOARD_EXTRA_ENV					\
	CONFIG_KM_DEF_ENV						\
	CONFIG_KM_NEW_ENV						\
	"arch=arm\0"							\
	""

/* additions for new relocation code, must be added to all boards */
#define CONFIG_SYS_SDRAM_BASE		0x00000000

/* address for the bootcount (taken from end of RAM) */
#define BOOTCOUNT_ADDR          (CONFIG_KM_RESERVED_PRAM)

/* enable POST tests */
#define CONFIG_POST	(CONFIG_SYS_POST_MEM_REGIONS)
#define CONFIG_POST_SKIP_ENV_FLAGS
#define CONFIG_POST_EXTERNAL_WORD_FUNCS

#endif /* _CONFIG_KM_ARM_H */
