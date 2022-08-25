// SPDX-License-Identifier: GPL-2.0
/*
 * RTC subsystem, nvmem interface
 *
 * Copyright (C) 2017 Alexandre Belloni
 */

#include <linux/err.h>
#include <linux/types.h>
#include <linux/nvmem-consumer.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

/*
 * Deprecated ABI compatibility, this should be removed at some point
 */

static const char nvram_warning[] = "Deprecated ABI, please use nvmem";

static ssize_t
rtc_nvram_read(struct file *filp, struct kobject *kobj,
	       struct bin_attribute *attr,
	       char *buf, loff_t off, size_t count)
{
	dev_warn_once(kobj_to_dev(kobj), nvram_warning);

	return nvmem_device_read(attr->private, off, count, buf);
}

static ssize_t
rtc_nvram_write(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr,
		char *buf, loff_t off, size_t count)
{
	dev_warn_once(kobj_to_dev(kobj), nvram_warning);

	return nvmem_device_write(attr->private, off, count, buf);
}

static int rtc_nvram_register(struct rtc_device *rtc,
			      struct nvmem_device *nvmem, size_t size)
{
	int err;

	rtc->nvram = kzalloc(sizeof(*rtc->nvram), GFP_KERNEL);
	if (!rtc->nvram)
		return -ENOMEM;

	rtc->nvram->attr.name = "nvram";
	rtc->nvram->attr.mode = 0644;
	rtc->nvram->private = nvmem;

	sysfs_bin_attr_init(rtc->nvram);

	rtc->nvram->read = rtc_nvram_read;
	rtc->nvram->write = rtc_nvram_write;
	rtc->nvram->size = size;

	err = sysfs_create_bin_file(&rtc->dev.parent->kobj,
				    rtc->nvram);
	if (err) {
		kfree(rtc->nvram);
		rtc->nvram = NULL;
	}

	return err;
}

static void rtc_nvram_unregister(struct rtc_device *rtc)
{
	sysfs_remove_bin_file(&rtc->dev.parent->kobj, rtc->nvram);
	kfree(rtc->nvram);
	rtc->nvram = NULL;
}

/*
 * New ABI, uses nvmem
 */
int rtc_nvmem_register(struct rtc_device *rtc,
		       struct nvmem_config *nvmem_config)
{
	struct nvmem_device *nvmem;

	if (!nvmem_config)
		return -ENODEV;

	nvmem_config->dev = rtc->dev.parent;
	nvmem_config->owner = rtc->owner;
	nvmem = devm_nvmem_register(rtc->dev.parent, nvmem_config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	/* Register the old ABI */
	if (rtc->nvram_old_abi)
		rtc_nvram_register(rtc, nvmem, nvmem_config->size);

	return 0;
}
EXPORT_SYMBOL_GPL(rtc_nvmem_register);

void rtc_nvmem_unregister(struct rtc_device *rtc)
{
	/* unregister the old ABI */
	if (rtc->nvram)
		rtc_nvram_unregister(rtc);
}
