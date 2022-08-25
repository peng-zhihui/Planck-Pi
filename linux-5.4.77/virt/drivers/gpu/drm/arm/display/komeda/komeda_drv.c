// SPDX-License-Identifier: GPL-2.0
/*
 * (C) COPYRIGHT 2018 ARM Limited. All rights reserved.
 * Author: James.Qian.Wang <james.qian.wang@arm.com>
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/component.h>
#include <drm/drm_of.h>
#include "komeda_dev.h"
#include "komeda_kms.h"

struct komeda_drv {
	struct komeda_dev *mdev;
	struct komeda_kms_dev *kms;
};

struct komeda_dev *dev_to_mdev(struct device *dev)
{
	struct komeda_drv *mdrv = dev_get_drvdata(dev);

	return mdrv ? mdrv->mdev : NULL;
}

static void komeda_unbind(struct device *dev)
{
	struct komeda_drv *mdrv = dev_get_drvdata(dev);

	if (!mdrv)
		return;

	komeda_kms_detach(mdrv->kms);
	komeda_dev_destroy(mdrv->mdev);

	dev_set_drvdata(dev, NULL);
	devm_kfree(dev, mdrv);
}

static int komeda_bind(struct device *dev)
{
	struct komeda_drv *mdrv;
	int err;

	mdrv = devm_kzalloc(dev, sizeof(*mdrv), GFP_KERNEL);
	if (!mdrv)
		return -ENOMEM;

	mdrv->mdev = komeda_dev_create(dev);
	if (IS_ERR(mdrv->mdev)) {
		err = PTR_ERR(mdrv->mdev);
		goto free_mdrv;
	}

	mdrv->kms = komeda_kms_attach(mdrv->mdev);
	if (IS_ERR(mdrv->kms)) {
		err = PTR_ERR(mdrv->kms);
		goto destroy_mdev;
	}

	dev_set_drvdata(dev, mdrv);

	return 0;

destroy_mdev:
	komeda_dev_destroy(mdrv->mdev);

free_mdrv:
	devm_kfree(dev, mdrv);
	return err;
}

static const struct component_master_ops komeda_master_ops = {
	.bind	= komeda_bind,
	.unbind	= komeda_unbind,
};

static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static void komeda_add_slave(struct device *master,
			     struct component_match **match,
			     struct device_node *np,
			     u32 port, u32 endpoint)
{
	struct device_node *remote;

	remote = of_graph_get_remote_node(np, port, endpoint);
	if (remote) {
		drm_of_component_match_add(master, match, compare_of, remote);
		of_node_put(remote);
	}
}

static int komeda_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct device_node *child;

	if (!dev->of_node)
		return -ENODEV;

	for_each_available_child_of_node(dev->of_node, child) {
		if (of_node_cmp(child->name, "pipeline") != 0)
			continue;

		/* add connector */
		komeda_add_slave(dev, &match, child, KOMEDA_OF_PORT_OUTPUT, 0);
		komeda_add_slave(dev, &match, child, KOMEDA_OF_PORT_OUTPUT, 1);
	}

	return component_master_add_with_match(dev, &komeda_master_ops, match);
}

static int komeda_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &komeda_master_ops);
	return 0;
}

static const struct komeda_product_data komeda_products[] = {
	[MALI_D71] = {
		.product_id = MALIDP_D71_PRODUCT_ID,
		.identify = d71_identify,
	},
};

static const struct of_device_id komeda_of_match[] = {
	{ .compatible = "arm,mali-d71", .data = &komeda_products[MALI_D71], },
	{},
};

MODULE_DEVICE_TABLE(of, komeda_of_match);

static struct platform_driver komeda_platform_driver = {
	.probe	= komeda_platform_probe,
	.remove	= komeda_platform_remove,
	.driver	= {
		.name = "komeda",
		.of_match_table	= komeda_of_match,
		.pm = NULL,
	},
};

module_platform_driver(komeda_platform_driver);

MODULE_AUTHOR("James.Qian.Wang <james.qian.wang@arm.com>");
MODULE_DESCRIPTION("Komeda KMS driver");
MODULE_LICENSE("GPL v2");
