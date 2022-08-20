/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2018 Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#ifndef __ROCK960_RK3399_H
#define __ROCK960_RK3399_H

#define ROCKCHIP_DEVICE_SETTINGS \
		"stdin=serial,usbkbd\0" \
		"stdout=serial,vidconsole\0" \
		"stderr=serial,vidconsole\0"

#include <configs/rk3399_common.h>

#define CONFIG_SYS_MMC_ENV_DEV		1

#define SDRAM_BANK_SIZE			(2UL << 30)

#endif
