/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2016 Rockchip Electronics Co., Ltd
 */

#ifndef __EVB_RK3399_H
#define __EVB_RK3399_H

#define ROCKCHIP_DEVICE_SETTINGS \
		"stdin=serial,usbkbd\0" \
		"stdout=serial,vidconsole\0" \
		"stderr=serial,vidconsole\0"

#include <configs/rk3399_common.h>

#if defined(CONFIG_ENV_IS_IN_MMC)
# define CONFIG_SYS_MMC_ENV_DEV		0
#endif

#define SDRAM_BANK_SIZE			(2UL << 30)

#endif
