// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2016 Socionext Inc.
 *   Author: Masahiro Yamada <yamada.masahiro@socionext.com>
 */

#include <linux/errno.h>
#include <dm.h>
#include <dm/pinctrl.h>

#include "init.h"

int uniphier_pin_init(const char *pinconfig_name)
{
	struct udevice *pctldev, *config;
	int ret;

	ret = uclass_first_device(UCLASS_PINCTRL, &pctldev);
	if (ret)
		return ret;

	device_foreach_child(config, pctldev) {
		if (strcmp(config->name, pinconfig_name))
			continue;

		return pinctrl_generic_set_state(pctldev, config);
	}

	return -ENODEV;
}
