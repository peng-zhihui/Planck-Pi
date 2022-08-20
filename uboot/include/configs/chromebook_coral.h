/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2019 Google LLC
 */

/*
 * board/config.h - configuration options, board-specific
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#define CONFIG_BOOTCOMMAND	\
	"fatload mmc 1:c 1000000 syslinux/vmlinuz.A; zboot 1000000"

#include <configs/x86-common.h>
#include <configs/x86-chromebook.h>

#undef CONFIG_STD_DEVICES_SETTINGS
#define CONFIG_STD_DEVICES_SETTINGS     "stdin=usbkbd,i8042-kbd,serial\0" \
					"stdout=vidconsole,serial\0" \
					"stderr=vidconsole,serial\0"

#define CONFIG_ENV_SECT_SIZE		0x1000
#define CONFIG_ENV_OFFSET		0x003f8000

#define CONFIG_TPL_TEXT_BASE		0xffff8000

#define CONFIG_SYS_NS16550_MEM32
#undef CONFIG_SYS_NS16550_PORT_MAPPED

#endif	/* __CONFIG_H */
