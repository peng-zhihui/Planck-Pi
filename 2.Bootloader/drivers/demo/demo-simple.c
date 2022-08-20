// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2013 Google, Inc
 *
 * (C) Copyright 2012
 * Pavel Herrmann <morpheus.ibis@gmail.com>
 */

#include <common.h>
#include <dm.h>
#include <dm-demo.h>
#include <mapmem.h>
#include <asm/io.h>

static int simple_hello(struct udevice *dev, int ch)
{
	const struct dm_demo_pdata *pdata = dev_get_platdata(dev);

	printf("Hello from %08x: %s %d\n", (uint)map_to_sysmem(dev), pdata->colour,
	       pdata->sides);

	return 0;
}

static const struct demo_ops simple_ops = {
	.hello = simple_hello,
};

static int demo_shape_ofdata_to_platdata(struct udevice *dev)
{
	/* Parse the data that is common with all demo devices */
	return demo_parse_dt(dev);
}

static const struct udevice_id demo_shape_id[] = {
	{ "demo-simple", 0 },
	{ },
};

U_BOOT_DRIVER(demo_simple_drv) = {
	.name	= "demo_simple_drv",
	.of_match = demo_shape_id,
	.id	= UCLASS_DEMO,
	.ofdata_to_platdata = demo_shape_ofdata_to_platdata,
	.ops	= &simple_ops,
	.platdata_auto_alloc_size = sizeof(struct dm_demo_pdata),
};
