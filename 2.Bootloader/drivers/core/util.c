// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2013 Google, Inc
 */

#include <common.h>
#include <dm/device.h>
#include <dm/ofnode.h>
#include <dm/read.h>
#include <dm/util.h>
#include <linux/libfdt.h>
#include <vsprintf.h>

#ifdef CONFIG_DM_WARN
void dm_warn(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}
#endif

int list_count_items(struct list_head *head)
{
	struct list_head *node;
	int count = 0;

	list_for_each(node, head)
		count++;

	return count;
}

#if !CONFIG_IS_ENABLED(OF_PLATDATA)
int pci_get_devfn(struct udevice *dev)
{
	struct fdt_pci_addr addr;
	int ret;

	/* Extract the devfn from fdt_pci_addr */
	ret = ofnode_read_pci_addr(dev_ofnode(dev), FDT_PCI_SPACE_CONFIG,
				   "reg", &addr);
	if (ret) {
		if (ret != -ENOENT)
			return -EINVAL;
	}

	return addr.phys_hi & 0xff00;
}
#endif
