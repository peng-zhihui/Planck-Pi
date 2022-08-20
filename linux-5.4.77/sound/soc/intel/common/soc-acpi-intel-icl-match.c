// SPDX-License-Identifier: GPL-2.0
/*
 * soc-acpi-intel-icl-match.c - tables and support for ICL ACPI enumeration.
 *
 * Copyright (c) 2018, Intel Corporation.
 *
 */

#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>
#include "../skylake/skl.h"

static struct skl_machine_pdata icl_pdata = {
	.use_tplg_pcm = true,
};

struct snd_soc_acpi_mach snd_soc_acpi_intel_icl_machines[] = {
	{
		.id = "INT34C2",
		.drv_name = "icl_rt274",
		.fw_filename = "intel/dsp_fw_icl.bin",
		.pdata = &icl_pdata,
		.sof_fw_filename = "sof-icl.ri",
		.sof_tplg_filename = "sof-icl-rt274.tplg",
	},
	{
		.id = "10EC5682",
		.drv_name = "sof_rt5682",
		.sof_fw_filename = "sof-icl.ri",
		.sof_tplg_filename = "sof-icl-rt5682.tplg",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_icl_machines);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Common ACPI Match module");
