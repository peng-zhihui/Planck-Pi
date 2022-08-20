// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2019, Microchip Technology, Inc.
 * Author: Eugen Hristev <eugen.hristev@microchip.com>
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <log.h>
#include <misc.h>
#include <asm/io.h>
#include <linux/err.h>

struct microchip_flexcom_regs {
	u32 cr;
};

struct microchip_flexcom_platdata {
	struct microchip_flexcom_regs *regs;
	u32 flexcom_mode;
};

static int microchip_flexcom_ofdata_to_platdata(struct udevice *dev)
{
	struct microchip_flexcom_platdata *plat = dev_get_platdata(dev);
	int ret;

	plat->regs = map_physmem(devfdt_get_addr(dev),
				 sizeof(struct microchip_flexcom_regs),
				MAP_NOCACHE);

	ret = dev_read_u32(dev, "atmel,flexcom-mode", &plat->flexcom_mode);

	if (IS_ERR_VALUE(ret)) {
		debug("Missing atmel,flexcom-mode property\n");
		return ret;
	}

	/*
	 * The mode must have only 2 bits. If any other bits are set,
	 * the value is not supported.
	 */
	if (plat->flexcom_mode & 0xfffffffc) {
		debug("Wrong atmel,flexcom-mode property\n");
		return -EINVAL;
	}

	writel(plat->flexcom_mode, &plat->regs->cr);

	return 0;
}

static const struct udevice_id microchip_flexcom_ids[] = {
	{ .compatible = "atmel,sama5d2-flexcom" },
	{ .compatible = "microchip,flexcom" },
	{}
};

U_BOOT_DRIVER(microchip_flexcom) = {
	.name	= "microchip_flexcom",
	.id	= UCLASS_MISC,
	.of_match = microchip_flexcom_ids,
	.ofdata_to_platdata = microchip_flexcom_ofdata_to_platdata,
	.platdata_auto_alloc_size = sizeof(struct microchip_flexcom_platdata),
};
