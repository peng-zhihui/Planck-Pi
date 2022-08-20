// SPDX-License-Identifier: GPL-2.0-only
/*
 * Analog TV Connector driver
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include "../dss/omapdss.h"

struct panel_drv_data {
	struct omap_dss_device dssdev;

	struct device *dev;
};

#define to_panel_data(x) container_of(x, struct panel_drv_data, dssdev)

static int tvc_connect(struct omap_dss_device *src,
		       struct omap_dss_device *dst)
{
	return 0;
}

static void tvc_disconnect(struct omap_dss_device *src,
			   struct omap_dss_device *dst)
{
}

static const struct omap_dss_device_ops tvc_ops = {
	.connect		= tvc_connect,
	.disconnect		= tvc_disconnect,
};

static int tvc_probe(struct platform_device *pdev)
{
	struct panel_drv_data *ddata;
	struct omap_dss_device *dssdev;

	ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);
	ddata->dev = &pdev->dev;

	dssdev = &ddata->dssdev;
	dssdev->ops = &tvc_ops;
	dssdev->dev = &pdev->dev;
	dssdev->type = OMAP_DISPLAY_TYPE_VENC;
	dssdev->display = true;
	dssdev->owner = THIS_MODULE;
	dssdev->of_ports = BIT(0);

	omapdss_display_init(dssdev);
	omapdss_device_register(dssdev);

	return 0;
}

static int __exit tvc_remove(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);

	omapdss_device_unregister(&ddata->dssdev);

	return 0;
}

static const struct of_device_id tvc_of_match[] = {
	{ .compatible = "omapdss,svideo-connector", },
	{ .compatible = "omapdss,composite-video-connector", },
	{},
};

MODULE_DEVICE_TABLE(of, tvc_of_match);

static struct platform_driver tvc_connector_driver = {
	.probe	= tvc_probe,
	.remove	= __exit_p(tvc_remove),
	.driver	= {
		.name	= "connector-analog-tv",
		.of_match_table = tvc_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(tvc_connector_driver);

MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ti.com>");
MODULE_DESCRIPTION("Analog TV Connector driver");
MODULE_LICENSE("GPL");
