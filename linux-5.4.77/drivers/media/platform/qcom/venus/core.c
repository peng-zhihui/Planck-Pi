// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
 * Copyright (C) 2017 Linaro Ltd.
 */
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/pm_runtime.h>
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-ioctl.h>

#include "core.h"
#include "vdec.h"
#include "venc.h"
#include "firmware.h"

static void venus_event_notify(struct venus_core *core, u32 event)
{
	struct venus_inst *inst;

	switch (event) {
	case EVT_SYS_WATCHDOG_TIMEOUT:
	case EVT_SYS_ERROR:
		break;
	default:
		return;
	}

	mutex_lock(&core->lock);
	core->sys_error = true;
	list_for_each_entry(inst, &core->instances, list)
		inst->ops->event_notify(inst, EVT_SESSION_ERROR, NULL);
	mutex_unlock(&core->lock);

	disable_irq_nosync(core->irq);

	/*
	 * Delay recovery to ensure venus has completed any pending cache
	 * operations. Without this sleep, we see device reset when firmware is
	 * unloaded after a system error.
	 */
	schedule_delayed_work(&core->work, msecs_to_jiffies(100));
}

static const struct hfi_core_ops venus_core_ops = {
	.event_notify = venus_event_notify,
};

static void venus_sys_error_handler(struct work_struct *work)
{
	struct venus_core *core =
			container_of(work, struct venus_core, work.work);
	int ret = 0;

	dev_warn(core->dev, "system error has occurred, starting recovery!\n");

	pm_runtime_get_sync(core->dev);

	hfi_core_deinit(core, true);
	hfi_destroy(core);
	mutex_lock(&core->lock);
	venus_shutdown(core);

	pm_runtime_put_sync(core->dev);

	ret |= hfi_create(core, &venus_core_ops);

	pm_runtime_get_sync(core->dev);

	ret |= venus_boot(core);

	ret |= hfi_core_resume(core, true);

	enable_irq(core->irq);

	mutex_unlock(&core->lock);

	ret |= hfi_core_init(core);

	pm_runtime_put_sync(core->dev);

	if (ret) {
		disable_irq_nosync(core->irq);
		dev_warn(core->dev, "recovery failed (%d)\n", ret);
		schedule_delayed_work(&core->work, msecs_to_jiffies(10));
		return;
	}

	mutex_lock(&core->lock);
	core->sys_error = false;
	mutex_unlock(&core->lock);
}

static int venus_clks_get(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	struct device *dev = core->dev;
	unsigned int i;

	for (i = 0; i < res->clks_num; i++) {
		core->clks[i] = devm_clk_get(dev, res->clks[i]);
		if (IS_ERR(core->clks[i]))
			return PTR_ERR(core->clks[i]);
	}

	return 0;
}

static int venus_clks_enable(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	unsigned int i;
	int ret;

	for (i = 0; i < res->clks_num; i++) {
		ret = clk_prepare_enable(core->clks[i]);
		if (ret)
			goto err;
	}

	return 0;
err:
	while (i--)
		clk_disable_unprepare(core->clks[i]);

	return ret;
}

static void venus_clks_disable(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	unsigned int i = res->clks_num;

	while (i--)
		clk_disable_unprepare(core->clks[i]);
}

static u32 to_v4l2_codec_type(u32 codec)
{
	switch (codec) {
	case HFI_VIDEO_CODEC_H264:
		return V4L2_PIX_FMT_H264;
	case HFI_VIDEO_CODEC_H263:
		return V4L2_PIX_FMT_H263;
	case HFI_VIDEO_CODEC_MPEG1:
		return V4L2_PIX_FMT_MPEG1;
	case HFI_VIDEO_CODEC_MPEG2:
		return V4L2_PIX_FMT_MPEG2;
	case HFI_VIDEO_CODEC_MPEG4:
		return V4L2_PIX_FMT_MPEG4;
	case HFI_VIDEO_CODEC_VC1:
		return V4L2_PIX_FMT_VC1_ANNEX_G;
	case HFI_VIDEO_CODEC_VP8:
		return V4L2_PIX_FMT_VP8;
	case HFI_VIDEO_CODEC_VP9:
		return V4L2_PIX_FMT_VP9;
	case HFI_VIDEO_CODEC_DIVX:
	case HFI_VIDEO_CODEC_DIVX_311:
		return V4L2_PIX_FMT_XVID;
	default:
		return 0;
	}
}

static int venus_enumerate_codecs(struct venus_core *core, u32 type)
{
	const struct hfi_inst_ops dummy_ops = {};
	struct venus_inst *inst;
	u32 codec, codecs;
	unsigned int i;
	int ret;

	if (core->res->hfi_version != HFI_VERSION_1XX)
		return 0;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	mutex_init(&inst->lock);
	inst->core = core;
	inst->session_type = type;
	if (type == VIDC_SESSION_TYPE_DEC)
		codecs = core->dec_codecs;
	else
		codecs = core->enc_codecs;

	ret = hfi_session_create(inst, &dummy_ops);
	if (ret)
		goto err;

	for (i = 0; i < MAX_CODEC_NUM; i++) {
		codec = (1UL << i) & codecs;
		if (!codec)
			continue;

		ret = hfi_session_init(inst, to_v4l2_codec_type(codec));
		if (ret)
			goto done;

		ret = hfi_session_deinit(inst);
		if (ret)
			goto done;
	}

done:
	hfi_session_destroy(inst);
err:
	mutex_destroy(&inst->lock);
	kfree(inst);

	return ret;
}

static int venus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct venus_core *core;
	struct resource *r;
	int ret;

	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->dev = dev;
	platform_set_drvdata(pdev, core);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	core->base = devm_ioremap_resource(dev, r);
	if (IS_ERR(core->base))
		return PTR_ERR(core->base);

	core->irq = platform_get_irq(pdev, 0);
	if (core->irq < 0)
		return core->irq;

	core->res = of_device_get_match_data(dev);
	if (!core->res)
		return -ENODEV;

	ret = venus_clks_get(core);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, core->res->dma_mask);
	if (ret)
		return ret;

	if (!dev->dma_parms) {
		dev->dma_parms = devm_kzalloc(dev, sizeof(*dev->dma_parms),
					      GFP_KERNEL);
		if (!dev->dma_parms)
			return -ENOMEM;
	}
	dma_set_max_seg_size(dev, DMA_BIT_MASK(32));

	INIT_LIST_HEAD(&core->instances);
	mutex_init(&core->lock);
	INIT_DELAYED_WORK(&core->work, venus_sys_error_handler);

	ret = devm_request_threaded_irq(dev, core->irq, hfi_isr, hfi_isr_thread,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"venus", core);
	if (ret)
		return ret;

	ret = hfi_create(core, &venus_core_ops);
	if (ret)
		return ret;

	pm_runtime_enable(dev);

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		goto err_runtime_disable;

	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret)
		goto err_runtime_disable;

	ret = venus_firmware_init(core);
	if (ret)
		goto err_runtime_disable;

	ret = venus_boot(core);
	if (ret)
		goto err_runtime_disable;

	ret = hfi_core_resume(core, true);
	if (ret)
		goto err_venus_shutdown;

	ret = hfi_core_init(core);
	if (ret)
		goto err_venus_shutdown;

	ret = venus_enumerate_codecs(core, VIDC_SESSION_TYPE_DEC);
	if (ret)
		goto err_venus_shutdown;

	ret = venus_enumerate_codecs(core, VIDC_SESSION_TYPE_ENC);
	if (ret)
		goto err_venus_shutdown;

	ret = v4l2_device_register(dev, &core->v4l2_dev);
	if (ret)
		goto err_core_deinit;

	ret = pm_runtime_put_sync(dev);
	if (ret) {
		pm_runtime_get_noresume(dev);
		goto err_dev_unregister;
	}

	return 0;

err_dev_unregister:
	v4l2_device_unregister(&core->v4l2_dev);
err_core_deinit:
	hfi_core_deinit(core, false);
err_venus_shutdown:
	venus_shutdown(core);
err_runtime_disable:
	pm_runtime_put_noidle(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_disable(dev);
	hfi_destroy(core);
	return ret;
}

static int venus_remove(struct platform_device *pdev)
{
	struct venus_core *core = platform_get_drvdata(pdev);
	struct device *dev = core->dev;
	int ret;

	ret = pm_runtime_get_sync(dev);
	WARN_ON(ret < 0);

	ret = hfi_core_deinit(core, true);
	WARN_ON(ret);

	hfi_destroy(core);
	venus_shutdown(core);
	of_platform_depopulate(dev);

	venus_firmware_deinit(core);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);

	v4l2_device_unregister(&core->v4l2_dev);

	return ret;
}

static __maybe_unused int venus_runtime_suspend(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret;

	ret = hfi_core_suspend(core);

	venus_clks_disable(core);

	return ret;
}

static __maybe_unused int venus_runtime_resume(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret;

	ret = venus_clks_enable(core);
	if (ret)
		return ret;

	ret = hfi_core_resume(core, false);
	if (ret)
		goto err_clks_disable;

	return 0;

err_clks_disable:
	venus_clks_disable(core);
	return ret;
}

static const struct dev_pm_ops venus_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(venus_runtime_suspend, venus_runtime_resume, NULL)
};

static const struct freq_tbl msm8916_freq_table[] = {
	{ 352800, 228570000 },	/* 1920x1088 @ 30 + 1280x720 @ 30 */
	{ 244800, 160000000 },	/* 1920x1088 @ 30 */
	{ 108000, 100000000 },	/* 1280x720 @ 30 */
};

static const struct reg_val msm8916_reg_preset[] = {
	{ 0xe0020, 0x05555556 },
	{ 0xe0024, 0x05555556 },
	{ 0x80124, 0x00000003 },
};

static const struct venus_resources msm8916_res = {
	.freq_tbl = msm8916_freq_table,
	.freq_tbl_size = ARRAY_SIZE(msm8916_freq_table),
	.reg_tbl = msm8916_reg_preset,
	.reg_tbl_size = ARRAY_SIZE(msm8916_reg_preset),
	.clks = { "core", "iface", "bus", },
	.clks_num = 3,
	.max_load = 352800, /* 720p@30 + 1080p@30 */
	.hfi_version = HFI_VERSION_1XX,
	.vmem_id = VIDC_RESOURCE_NONE,
	.vmem_size = 0,
	.vmem_addr = 0,
	.dma_mask = 0xddc00000 - 1,
	.fwname = "qcom/venus-1.8/venus.mdt",
};

static const struct freq_tbl msm8996_freq_table[] = {
	{ 1944000, 520000000 },	/* 4k UHD @ 60 (decode only) */
	{  972000, 520000000 },	/* 4k UHD @ 30 */
	{  489600, 346666667 },	/* 1080p @ 60 */
	{  244800, 150000000 },	/* 1080p @ 30 */
	{  108000,  75000000 },	/* 720p @ 30 */
};

static const struct reg_val msm8996_reg_preset[] = {
	{ 0x80010, 0xffffffff },
	{ 0x80018, 0x00001556 },
	{ 0x8001C, 0x00001556 },
};

static const struct venus_resources msm8996_res = {
	.freq_tbl = msm8996_freq_table,
	.freq_tbl_size = ARRAY_SIZE(msm8996_freq_table),
	.reg_tbl = msm8996_reg_preset,
	.reg_tbl_size = ARRAY_SIZE(msm8996_reg_preset),
	.clks = {"core", "iface", "bus", "mbus" },
	.clks_num = 4,
	.max_load = 2563200,
	.hfi_version = HFI_VERSION_3XX,
	.vmem_id = VIDC_RESOURCE_NONE,
	.vmem_size = 0,
	.vmem_addr = 0,
	.dma_mask = 0xddc00000 - 1,
	.fwname = "qcom/venus-4.2/venus.mdt",
};

static const struct freq_tbl sdm845_freq_table[] = {
	{ 3110400, 533000000 },	/* 4096x2160@90 */
	{ 2073600, 444000000 },	/* 4096x2160@60 */
	{ 1944000, 404000000 },	/* 3840x2160@60 */
	{  972000, 330000000 },	/* 3840x2160@30 */
	{  489600, 200000000 },	/* 1920x1080@60 */
	{  244800, 100000000 },	/* 1920x1080@30 */
};

static const struct venus_resources sdm845_res = {
	.freq_tbl = sdm845_freq_table,
	.freq_tbl_size = ARRAY_SIZE(sdm845_freq_table),
	.clks = {"core", "iface", "bus" },
	.clks_num = 3,
	.max_load = 3110400,	/* 4096x2160@90 */
	.hfi_version = HFI_VERSION_4XX,
	.vmem_id = VIDC_RESOURCE_NONE,
	.vmem_size = 0,
	.vmem_addr = 0,
	.dma_mask = 0xe0000000 - 1,
	.fwname = "qcom/venus-5.2/venus.mdt",
};

static const struct of_device_id venus_dt_match[] = {
	{ .compatible = "qcom,msm8916-venus", .data = &msm8916_res, },
	{ .compatible = "qcom,msm8996-venus", .data = &msm8996_res, },
	{ .compatible = "qcom,sdm845-venus", .data = &sdm845_res, },
	{ }
};
MODULE_DEVICE_TABLE(of, venus_dt_match);

static struct platform_driver qcom_venus_driver = {
	.probe = venus_probe,
	.remove = venus_remove,
	.driver = {
		.name = "qcom-venus",
		.of_match_table = venus_dt_match,
		.pm = &venus_pm_ops,
	},
};
module_platform_driver(qcom_venus_driver);

MODULE_ALIAS("platform:qcom-venus");
MODULE_DESCRIPTION("Qualcomm Venus video encoder and decoder driver");
MODULE_LICENSE("GPL v2");
