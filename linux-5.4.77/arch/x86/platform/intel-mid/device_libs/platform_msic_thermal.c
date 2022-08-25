// SPDX-License-Identifier: GPL-2.0-only
/*
 * platform_msic_thermal.c: msic_thermal platform data initialization file
 *
 * (C) Copyright 2013 Intel Corporation
 * Author: Sathyanarayanan Kuppuswamy <sathyanarayanan.kuppuswamy@intel.com>
 */

#include <linux/input.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/mfd/intel_msic.h>
#include <asm/intel-mid.h>

#include "platform_msic.h"

static void __init *msic_thermal_platform_data(void *info)
{
	return msic_generic_platform_data(info, INTEL_MSIC_BLOCK_THERMAL);
}

static const struct devs_id msic_thermal_dev_id __initconst = {
	.name = "msic_thermal",
	.type = SFI_DEV_TYPE_IPC,
	.delay = 1,
	.msic = 1,
	.get_platform_data = &msic_thermal_platform_data,
};

sfi_device(msic_thermal_dev_id);
