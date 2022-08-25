// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// Copyright 2019 NXP
//
// Author: Daniel Baluta <daniel.baluta@nxp.com>
//

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <sound/sof.h>

#include "ops.h"

extern struct snd_sof_dsp_ops sof_imx8_ops;

/* platform specific devices */
#if IS_ENABLED(CONFIG_SND_SOC_SOF_IMX8)
static struct sof_dev_desc sof_of_imx8qxp_desc = {
	.default_fw_path = "imx/sof",
	.default_tplg_path = "imx/sof-tplg",
	.nocodec_fw_filename = "sof-imx8.ri",
	.nocodec_tplg_filename = "sof-imx8-nocodec.tplg",
	.ops = &sof_imx8_ops,
};
#endif

static const struct dev_pm_ops sof_of_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(snd_sof_suspend, snd_sof_resume)
	SET_RUNTIME_PM_OPS(snd_sof_runtime_suspend, snd_sof_runtime_resume,
			   NULL)
};

static void sof_of_probe_complete(struct device *dev)
{
	/* allow runtime_pm */
	pm_runtime_set_autosuspend_delay(dev, SND_SOF_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
}

static int sof_of_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct sof_dev_desc *desc;
	/*TODO: create a generic snd_soc_xxx_mach */
	struct snd_soc_acpi_mach *mach;
	struct snd_sof_pdata *sof_pdata;
	const struct snd_sof_dsp_ops *ops;
	int ret;

	dev_info(&pdev->dev, "DT DSP detected");

	sof_pdata = devm_kzalloc(dev, sizeof(*sof_pdata), GFP_KERNEL);
	if (!sof_pdata)
		return -ENOMEM;

	desc = device_get_match_data(dev);
	if (!desc)
		return -ENODEV;

	/* get ops for platform */
	ops = desc->ops;
	if (!ops) {
		dev_err(dev, "error: no matching DT descriptor ops\n");
		return -ENODEV;
	}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_FORCE_NOCODEC_MODE)
	/* force nocodec mode */
	dev_warn(dev, "Force to use nocodec mode\n");
	mach = devm_kzalloc(dev, sizeof(*mach), GFP_KERNEL);
	if (!mach)
		return -ENOMEM;
	ret = sof_nocodec_setup(dev, sof_pdata, mach, desc, ops);
	if (ret < 0)
		return ret;
#else
	/* TODO: implement case where we actually have a codec */
	return -ENODEV;
#endif

	if (mach)
		mach->mach_params.platform = dev_name(dev);

	sof_pdata->machine = mach;
	sof_pdata->desc = desc;
	sof_pdata->dev = &pdev->dev;
	sof_pdata->platform = dev_name(dev);

	/* TODO: read alternate fw and tplg filenames from DT */
	sof_pdata->fw_filename_prefix = sof_pdata->desc->default_fw_path;
	sof_pdata->tplg_filename_prefix = sof_pdata->desc->default_tplg_path;

#if IS_ENABLED(CONFIG_SND_SOC_SOF_PROBE_WORK_QUEUE)
	/* set callback to enable runtime_pm */
	sof_pdata->sof_probe_complete = sof_of_probe_complete;
#endif
	 /* call sof helper for DSP hardware probe */
	ret = snd_sof_device_probe(dev, sof_pdata);
	if (ret) {
		dev_err(dev, "error: failed to probe DSP hardware\n");
		return ret;
	}

#if !IS_ENABLED(CONFIG_SND_SOC_SOF_PROBE_WORK_QUEUE)
	sof_of_probe_complete(dev);
#endif

	return ret;
}

static int sof_of_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);

	/* call sof helper for DSP hardware remove */
	snd_sof_device_remove(&pdev->dev);

	return 0;
}

static const struct of_device_id sof_of_ids[] = {
#if IS_ENABLED(CONFIG_SND_SOC_SOF_IMX8)
	{ .compatible = "fsl,imx8qxp-dsp", .data = &sof_of_imx8qxp_desc},
#endif
	{ }
};
MODULE_DEVICE_TABLE(of, sof_of_ids);

/* DT driver definition */
static struct platform_driver snd_sof_of_driver = {
	.probe = sof_of_probe,
	.remove = sof_of_remove,
	.driver = {
		.name = "sof-audio-of",
		.pm = &sof_of_pm,
		.of_match_table = sof_of_ids,
	},
};
module_platform_driver(snd_sof_of_driver);

MODULE_LICENSE("Dual BSD/GPL");
