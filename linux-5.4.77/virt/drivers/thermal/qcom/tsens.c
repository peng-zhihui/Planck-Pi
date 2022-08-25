// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include "tsens.h"

static int tsens_get_temp(void *data, int *temp)
{
	const struct tsens_sensor *s = data;
	struct tsens_priv *priv = s->priv;

	return priv->ops->get_temp(priv, s->id, temp);
}

static int tsens_get_trend(void *data, int trip, enum thermal_trend *trend)
{
	const struct tsens_sensor *s = data;
	struct tsens_priv *priv = s->priv;

	if (priv->ops->get_trend)
		return priv->ops->get_trend(priv, s->id, trend);

	return -ENOTSUPP;
}

static int  __maybe_unused tsens_suspend(struct device *dev)
{
	struct tsens_priv *priv = dev_get_drvdata(dev);

	if (priv->ops && priv->ops->suspend)
		return priv->ops->suspend(priv);

	return 0;
}

static int __maybe_unused tsens_resume(struct device *dev)
{
	struct tsens_priv *priv = dev_get_drvdata(dev);

	if (priv->ops && priv->ops->resume)
		return priv->ops->resume(priv);

	return 0;
}

static SIMPLE_DEV_PM_OPS(tsens_pm_ops, tsens_suspend, tsens_resume);

static const struct of_device_id tsens_table[] = {
	{
		.compatible = "qcom,msm8916-tsens",
		.data = &data_8916,
	}, {
		.compatible = "qcom,msm8974-tsens",
		.data = &data_8974,
	}, {
		.compatible = "qcom,msm8996-tsens",
		.data = &data_8996,
	}, {
		.compatible = "qcom,tsens-v1",
		.data = &data_tsens_v1,
	}, {
		.compatible = "qcom,tsens-v2",
		.data = &data_tsens_v2,
	},
	{}
};
MODULE_DEVICE_TABLE(of, tsens_table);

static const struct thermal_zone_of_device_ops tsens_of_ops = {
	.get_temp = tsens_get_temp,
	.get_trend = tsens_get_trend,
};

static int tsens_register(struct tsens_priv *priv)
{
	int i;
	struct thermal_zone_device *tzd;

	for (i = 0;  i < priv->num_sensors; i++) {
		priv->sensor[i].priv = priv;
		priv->sensor[i].id = i;
		tzd = devm_thermal_zone_of_sensor_register(priv->dev, i,
							   &priv->sensor[i],
							   &tsens_of_ops);
		if (IS_ERR(tzd))
			continue;
		priv->sensor[i].tzd = tzd;
		if (priv->ops->enable)
			priv->ops->enable(priv, i);
	}
	return 0;
}

static int tsens_probe(struct platform_device *pdev)
{
	int ret, i;
	struct device *dev;
	struct device_node *np;
	struct tsens_priv *priv;
	const struct tsens_plat_data *data;
	const struct of_device_id *id;
	u32 num_sensors;

	if (pdev->dev.of_node)
		dev = &pdev->dev;
	else
		dev = pdev->dev.parent;

	np = dev->of_node;

	id = of_match_node(tsens_table, np);
	if (id)
		data = id->data;
	else
		data = &data_8960;

	num_sensors = data->num_sensors;

	if (np)
		of_property_read_u32(np, "#qcom,sensors", &num_sensors);

	if (num_sensors <= 0) {
		dev_err(dev, "invalid number of sensors\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(dev,
			     struct_size(priv, sensor, num_sensors),
			     GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->num_sensors = num_sensors;
	priv->ops = data->ops;
	for (i = 0;  i < priv->num_sensors; i++) {
		if (data->hw_ids)
			priv->sensor[i].hw_id = data->hw_ids[i];
		else
			priv->sensor[i].hw_id = i;
	}
	priv->feat = data->feat;
	priv->fields = data->fields;

	if (!priv->ops || !priv->ops->init || !priv->ops->get_temp)
		return -EINVAL;

	ret = priv->ops->init(priv);
	if (ret < 0) {
		dev_err(dev, "tsens init failed\n");
		return ret;
	}

	if (priv->ops->calibrate) {
		ret = priv->ops->calibrate(priv);
		if (ret < 0) {
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "tsens calibration failed\n");
			return ret;
		}
	}

	ret = tsens_register(priv);

	platform_set_drvdata(pdev, priv);

	return ret;
}

static int tsens_remove(struct platform_device *pdev)
{
	struct tsens_priv *priv = platform_get_drvdata(pdev);

	if (priv->ops->disable)
		priv->ops->disable(priv);

	return 0;
}

static struct platform_driver tsens_driver = {
	.probe = tsens_probe,
	.remove = tsens_remove,
	.driver = {
		.name = "qcom-tsens",
		.pm	= &tsens_pm_ops,
		.of_match_table = tsens_table,
	},
};
module_platform_driver(tsens_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QCOM Temperature Sensor driver");
MODULE_ALIAS("platform:qcom-tsens");
