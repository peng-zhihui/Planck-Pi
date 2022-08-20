/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2013 Siemens Schweiz AG
 * (C) Heiko Schocher, DENX Software Engineering, hs@denx.de.
 *
 * Based on:
 * U-Boot file:/include/configs/am335x_evm.h
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 */

#ifndef __CONFIG_RASTABAN_H
#define __CONFIG_RASTABAN_H

#include "siemens-am33x-common.h"

#define DDR_PLL_FREQ	303

/* FWD Button = 27
 * SRV Button = 87 */
#define BOARD_DFU_BUTTON_GPIO	27
#define GPIO_LAN9303_NRST	88	/* GPIO2_24 = gpio88 */
/* In dfu mode keep led1 on */
#define CONFIG_ENV_SETTINGS_BUTTONS_AND_LEDS \
	"button_dfu0=27\0" \
	"button_dfu1=87\0" \
	"led0=3,0,1\0" \
	"led1=4,0,0\0" \
	"led2=5,0,1\0" \
	"led3=62,0,1\0" \
	"led4=60,0,1\0" \
	"led5=63,0,1\0"

 /* Physical Memory Map */
#define CONFIG_MAX_RAM_BANK_SIZE	(1024 << 20)	/* 1GB */

/* I2C Configuration */
#define CONFIG_SYS_I2C_SPEED		100000

#define CONFIG_SYS_I2C_EEPROM_ADDR              0x50
#define EEPROM_ADDR_DDR3 0x90
#define EEPROM_ADDR_CHIP 0x120

#define CONFIG_FACTORYSET

/* Define own nand partitions */
#define CONFIG_ENV_RANGE		(4 * CONFIG_SYS_ENV_SECT_SIZE)

#ifndef CONFIG_SPL_BUILD

/* Default env settings */
#define CONFIG_EXTRA_ENV_SETTINGS \
	"hostname=rastaban\0" \
	"ubi_off=2048\0"\
	"nand_img_size=0x400000\0" \
	"optargs=\0" \
	"preboot=draco_led 0\0" \
	CONFIG_ENV_SETTINGS_BUTTONS_AND_LEDS \
	CONFIG_ENV_SETTINGS_V2 \
	CONFIG_ENV_SETTINGS_NAND_V2

#ifndef CONFIG_RESTORE_FLASH
/* set to negative value for no autoboot */

#define CONFIG_BOOTCOMMAND \
"if dfubutton; then " \
	"run dfu_start; " \
	"reset; " \
"fi;" \
"run nand_boot;" \
"run nand_boot_backup;" \
"reset;"

#else

#define CONFIG_BOOTCOMMAND			\
	"setenv autoload no; "			\
	"dhcp; "				\
	"if tftp 80000000 debrick.scr; then "	\
		"source 80000000; "		\
	"fi"
#endif
#endif	/* CONFIG_SPL_BUILD */
#endif	/* ! __CONFIG_RASTABAN_H */
