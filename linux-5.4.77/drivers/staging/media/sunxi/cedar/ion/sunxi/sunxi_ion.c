/*
 * Allwinner SUNXI ION Driver
 *
 * Copyright (c) 2017 Allwinnertech.
 *
 * Author: fanqinghua <fanqinghua@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "Ion: " fmt

#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>
#include "../ion_priv.h"
#include "../ion.h"
#include "../ion_of.h"
#include "sunxi_ion_priv.h"

struct sunxi_ion_dev {
	struct ion_heap **heaps;
	struct ion_device *idev;
	struct ion_platform_data *data;
};
struct device *g_ion_dev;
struct ion_device *idev;
/* export for IMG GPU(sgx544) */
EXPORT_SYMBOL(idev);

static struct ion_of_heap sunxi_heaps[] = {
	PLATFORM_HEAP("allwinner,sys_user", 0, ION_HEAP_TYPE_SYSTEM,
		      "sys_user"),
	PLATFORM_HEAP("allwinner,sys_contig", 1, ION_HEAP_TYPE_SYSTEM_CONTIG,
		      "sys_contig"),
	PLATFORM_HEAP("allwinner,cma", ION_HEAP_TYPE_DMA, ION_HEAP_TYPE_DMA,
		      "cma"),
	PLATFORM_HEAP("allwinner,secure", ION_HEAP_TYPE_SECURE,
		      ION_HEAP_TYPE_SECURE, "secure"),
	{}
};

struct device *get_ion_dev(void)
{
	return g_ion_dev;
}

long sunxi_ion_ioctl(struct ion_client *client, unsigned int cmd,
		     unsigned long arg)
{
	long ret = 0;
	switch (cmd) {
	case ION_IOC_SUNXI_FLUSH_RANGE: {
		sunxi_cache_range range;
		if (copy_from_user(&range, (void __user *)arg,
				   sizeof(sunxi_cache_range))) {
			ret = -EINVAL;
			goto end;
		}
		
#if __LINUX_ARM_ARCH__ == 7
		if (flush_clean_user_range(range.start, range.end)) {
			ret = -EINVAL;
			goto end;
		}
#else
		dmac_flush_range((void*)range.start, (void*)range.end);
#endif
		break;
	}
#if __LINUX_ARM_ARCH__ == 7
	case ION_IOC_SUNXI_FLUSH_ALL: {
		flush_dcache_all();
		break;
	}
#endif
	case ION_IOC_SUNXI_PHYS_ADDR: {
		sunxi_phys_data data;
		struct ion_handle *handle;
		if (copy_from_user(&data, (void __user *)arg,
				   sizeof(sunxi_phys_data)))
			return -EFAULT;
		handle =
			ion_handle_get_by_id_nolock(client, data.handle.handle);
		/* FIXME Hardcoded CMA struct pointer */
		data.phys_addr =
			((struct ion_cma_buffer_info *)(handle->buffer
								->priv_virt))
				->handle;
		data.size = handle->buffer->size;
		if (copy_to_user((void __user *)arg, &data,
				 sizeof(sunxi_phys_data)))
			return -EFAULT;
		break;
	}
	
	default:
		return -ENOTTY;
	}
end:
	return ret;
}

static int sunxi_ion_probe(struct platform_device *pdev)
{
	struct sunxi_ion_dev *ipdev;
	int i;

	ipdev = devm_kzalloc(&pdev->dev, sizeof(*ipdev), GFP_KERNEL);
	if (!ipdev)
		return -ENOMEM;

	g_ion_dev = &pdev->dev;
	platform_set_drvdata(pdev, ipdev);

	ipdev->idev = ion_device_create(sunxi_ion_ioctl);
	if (IS_ERR(ipdev->idev))
		return PTR_ERR(ipdev->idev);

	idev = ipdev->idev;

	ipdev->data = ion_parse_dt(pdev, sunxi_heaps);
	if (IS_ERR(ipdev->data)) {
		pr_err("%s: ion_parse_dt error!\n", __func__);
		return PTR_ERR(ipdev->data);
	}

	ipdev->heaps = devm_kzalloc(&pdev->dev,
				    sizeof(struct ion_heap) * ipdev->data->nr,
				    GFP_KERNEL);
	if (!ipdev->heaps) {
		ion_destroy_platform_data(ipdev->data);
		return -ENOMEM;
	}

	for (i = 0; i < ipdev->data->nr; i++) {
		ipdev->heaps[i] = ion_heap_create(&ipdev->data->heaps[i]);
		if (!ipdev->heaps) {
			ion_destroy_platform_data(ipdev->data);
			return -ENOMEM;
		} else if (ipdev->heaps[i] == ERR_PTR(-EINVAL)) {
			return 0;
		}
		ion_device_add_heap(ipdev->idev, ipdev->heaps[i]);
	}
	return 0;
}

static int sunxi_ion_remove(struct platform_device *pdev)
{
	struct sunxi_ion_dev *ipdev;
	int i;

	ipdev = platform_get_drvdata(pdev);

	for (i = 0; i < ipdev->data->nr; i++)
		ion_heap_destroy(ipdev->heaps[i]);

	ion_destroy_platform_data(ipdev->data);
	ion_device_destroy(ipdev->idev);

	return 0;
}

static const struct of_device_id sunxi_ion_match_table[] = {
	{ .compatible = "allwinner,sunxi-ion" },
	{},
};

static struct platform_driver sunxi_ion_driver = {
	.probe = sunxi_ion_probe,
	.remove = sunxi_ion_remove,
	.driver = {
		.name = "ion-sunxi",
		.of_match_table = sunxi_ion_match_table,
	},
};

static int __init sunxi_ion_init(void)
{
	return platform_driver_register(&sunxi_ion_driver);
}
subsys_initcall(sunxi_ion_init);
