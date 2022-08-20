// SPDX-License-Identifier: GPL-2.0-only
/*
 * UWB PAL support.
 *
 * Copyright (C) 2008 Cambridge Silicon Radio Ltd.
 */
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/export.h>

#include "uwb.h"
#include "uwb-internal.h"

/**
 * uwb_pal_init - initialize a UWB PAL
 * @pal: the PAL to initialize
 */
void uwb_pal_init(struct uwb_pal *pal)
{
	INIT_LIST_HEAD(&pal->node);
}
EXPORT_SYMBOL_GPL(uwb_pal_init);

/**
 * uwb_pal_register - register a UWB PAL
 * @pal: the PAL
 *
 * The PAL must be initialized with uwb_pal_init().
 */
int uwb_pal_register(struct uwb_pal *pal)
{
	struct uwb_rc *rc = pal->rc;
	int ret;

	if (pal->device) {
		/* create a link to the uwb_rc in the PAL device's directory. */
		ret = sysfs_create_link(&pal->device->kobj,
					&rc->uwb_dev.dev.kobj, "uwb_rc");
		if (ret < 0)
			return ret;
		/* create a link to the PAL in the UWB device's directory. */
		ret = sysfs_create_link(&rc->uwb_dev.dev.kobj,
					&pal->device->kobj, pal->name);
		if (ret < 0) {
			sysfs_remove_link(&pal->device->kobj, "uwb_rc");
			return ret;
		}
	}

	pal->debugfs_dir = uwb_dbg_create_pal_dir(pal);

	mutex_lock(&rc->uwb_dev.mutex);
	list_add(&pal->node, &rc->pals);
	mutex_unlock(&rc->uwb_dev.mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(uwb_pal_register);

static int find_rc(struct device *dev, const void *data)
{
	const struct uwb_rc *target_rc = data;
	struct uwb_rc *rc = dev_get_drvdata(dev);

	if (rc == NULL) {
		WARN_ON(1);
		return 0;
	}
	if (rc == target_rc) {
		if (rc->ready == 0)
			return 0;
		else
			return 1;
	}
	return 0;
}

/**
 * Given a radio controller descriptor see if it is registered.
 *
 * @returns false if the rc does not exist or is quiescing; true otherwise.
 */
static bool uwb_rc_class_device_exists(struct uwb_rc *target_rc)
{
	struct device *dev;

	dev = class_find_device(&uwb_rc_class, NULL, target_rc,	find_rc);

	put_device(dev);

	return (dev != NULL);
}

/**
 * uwb_pal_unregister - unregister a UWB PAL
 * @pal: the PAL
 */
void uwb_pal_unregister(struct uwb_pal *pal)
{
	struct uwb_rc *rc = pal->rc;

	uwb_radio_stop(pal);

	mutex_lock(&rc->uwb_dev.mutex);
	list_del(&pal->node);
	mutex_unlock(&rc->uwb_dev.mutex);

	debugfs_remove(pal->debugfs_dir);

	if (pal->device) {
		/* remove link to the PAL in the UWB device's directory. */
		if (uwb_rc_class_device_exists(rc))
			sysfs_remove_link(&rc->uwb_dev.dev.kobj, pal->name);

		/* remove link to uwb_rc in the PAL device's directory. */
		sysfs_remove_link(&pal->device->kobj, "uwb_rc");
	}
}
EXPORT_SYMBOL_GPL(uwb_pal_unregister);

/**
 * uwb_rc_pal_init - initialize the PAL related parts of a radio controller
 * @rc: the radio controller
 */
void uwb_rc_pal_init(struct uwb_rc *rc)
{
	INIT_LIST_HEAD(&rc->pals);
}
