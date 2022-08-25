// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

#include <common.h>
#include <dm.h>
#include <pch.h>
#include <spl.h>
#include <asm/lpc_common.h>

#define BIOS_CTRL	0xdc

static int apl_set_spi_protect(struct udevice *dev, bool protect)
{
	if (spl_phase() == PHASE_SPL)
		return lpc_set_spi_protect(dev, BIOS_CTRL, protect);

	return 0;
}

static const struct pch_ops apl_pch_ops = {
	.set_spi_protect = apl_set_spi_protect,
};

static const struct udevice_id apl_pch_ids[] = {
	{ .compatible = "intel,apl-pch" },
	{ }
};

U_BOOT_DRIVER(apl_pch) = {
	.name		= "apl_pch",
	.id		= UCLASS_PCH,
	.of_match	= apl_pch_ids,
	.ops		= &apl_pch_ops,
};
