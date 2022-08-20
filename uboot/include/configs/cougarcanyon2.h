/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2016, Bin Meng <bmeng.cn@gmail.com>
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <configs/x86-common.h>

#define CONFIG_SYS_MONITOR_LEN		(2 << 20)

#define CONFIG_SMSC_SIO1007

#define CONFIG_STD_DEVICES_SETTINGS	"stdin=serial,i8042-kbd,usbkbd\0" \
					"stdout=serial,vga\0" \
					"stderr=serial,vga\0"

/* Environment configuration */

#endif	/* __CONFIG_H */
