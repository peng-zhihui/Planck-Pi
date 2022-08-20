// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2015 Google, Inc
 * Written by Simon Glass <sjg@chromium.org>
 */

#include <common.h>
#include <cpu.h>
#include <dm.h>
#include <errno.h>
#include <log.h>
#include <dm/lists.h>
#include <dm/root.h>
#include <linux/err.h>

int cpu_probe_all(void)
{
	struct udevice *cpu;
	int ret;

	ret = uclass_first_device(UCLASS_CPU, &cpu);
	if (ret) {
		debug("%s: No CPU found (err = %d)\n", __func__, ret);
		return ret;
	}

	while (cpu) {
		ret = uclass_next_device(&cpu);
		if (ret) {
			debug("%s: Error while probing CPU (err = %d)\n",
			      __func__, ret);
			return ret;
		}
	}

	return 0;
}

int cpu_is_current(struct udevice *cpu)
{
	struct cpu_ops *ops = cpu_get_ops(cpu);

	if (ops->is_current) {
		if (ops->is_current(cpu))
			return 1;
	}

	return -ENOSYS;
}

struct udevice *cpu_get_current_dev(void)
{
	struct udevice *cpu;
	int ret;

	uclass_foreach_dev_probe(UCLASS_CPU, cpu) {
		if (cpu_is_current(cpu) > 0)
			return cpu;
	}

	/* If can't find current cpu device, use the first dev instead */
	ret = uclass_first_device_err(UCLASS_CPU, &cpu);
	if (ret) {
		debug("%s: Could not get CPU device (err = %d)\n",
		      __func__, ret);
		return NULL;
	}

	return cpu;
}

int cpu_get_desc(struct udevice *dev, char *buf, int size)
{
	struct cpu_ops *ops = cpu_get_ops(dev);

	if (!ops->get_desc)
		return -ENOSYS;

	return ops->get_desc(dev, buf, size);
}

int cpu_get_info(struct udevice *dev, struct cpu_info *info)
{
	struct cpu_ops *ops = cpu_get_ops(dev);

	if (!ops->get_info)
		return -ENOSYS;

	return ops->get_info(dev, info);
}

int cpu_get_count(struct udevice *dev)
{
	struct cpu_ops *ops = cpu_get_ops(dev);

	if (!ops->get_count)
		return -ENOSYS;

	return ops->get_count(dev);
}

int cpu_get_vendor(struct udevice *dev, char *buf, int size)
{
	struct cpu_ops *ops = cpu_get_ops(dev);

	if (!ops->get_vendor)
		return -ENOSYS;

	return ops->get_vendor(dev, buf, size);
}

U_BOOT_DRIVER(cpu_bus) = {
	.name	= "cpu_bus",
	.id	= UCLASS_SIMPLE_BUS,
	.per_child_platdata_auto_alloc_size = sizeof(struct cpu_platdata),
};

static int uclass_cpu_init(struct uclass *uc)
{
	struct udevice *dev;
	ofnode node;
	int ret;

	node = ofnode_path("/cpus");
	if (!ofnode_valid(node))
		return 0;

	ret = device_bind_driver_to_node(dm_root(), "cpu_bus", "cpus", node,
					 &dev);

	return ret;
}

UCLASS_DRIVER(cpu) = {
	.id		= UCLASS_CPU,
	.name		= "cpu",
	.flags		= DM_UC_FLAG_SEQ_ALIAS,
	.init		= uclass_cpu_init,
};
