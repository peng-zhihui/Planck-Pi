// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cros_ec_dev - expose the Chrome OS Embedded Controller to user-space
 *
 * Copyright (C) 2014 Google, Inc.
 */

#include <linux/mfd/core.h>
#include <linux/mfd/cros_ec.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/platform_data/cros_ec_chardev.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/slab.h>

#define DRV_NAME "cros-ec-dev"

static struct class cros_class = {
	.owner          = THIS_MODULE,
	.name           = "chromeos",
};

/**
 * cros_feature_to_name - CrOS feature id to name/short description.
 * @id: The feature identifier.
 * @name: Device name associated with the feature id.
 * @desc: Short name that will be displayed.
 */
struct cros_feature_to_name {
	unsigned int id;
	const char *name;
	const char *desc;
};

/**
 * cros_feature_to_cells - CrOS feature id to mfd cells association.
 * @id: The feature identifier.
 * @mfd_cells: Pointer to the array of mfd cells that needs to be added.
 * @num_cells: Number of mfd cells into the array.
 */
struct cros_feature_to_cells {
	unsigned int id;
	const struct mfd_cell *mfd_cells;
	unsigned int num_cells;
};

static const struct cros_feature_to_name cros_mcu_devices[] = {
	{
		.id	= EC_FEATURE_FINGERPRINT,
		.name	= CROS_EC_DEV_FP_NAME,
		.desc	= "Fingerprint",
	},
	{
		.id	= EC_FEATURE_ISH,
		.name	= CROS_EC_DEV_ISH_NAME,
		.desc	= "Integrated Sensor Hub",
	},
	{
		.id	= EC_FEATURE_SCP,
		.name	= CROS_EC_DEV_SCP_NAME,
		.desc	= "System Control Processor",
	},
	{
		.id	= EC_FEATURE_TOUCHPAD,
		.name	= CROS_EC_DEV_TP_NAME,
		.desc	= "Touchpad",
	},
};

static const struct mfd_cell cros_ec_cec_cells[] = {
	{ .name = "cros-ec-cec", },
};

static const struct mfd_cell cros_ec_rtc_cells[] = {
	{ .name = "cros-ec-rtc", },
};

static const struct mfd_cell cros_usbpd_charger_cells[] = {
	{ .name = "cros-usbpd-charger", },
	{ .name = "cros-usbpd-logger", },
};

static const struct cros_feature_to_cells cros_subdevices[] = {
	{
		.id		= EC_FEATURE_CEC,
		.mfd_cells	= cros_ec_cec_cells,
		.num_cells	= ARRAY_SIZE(cros_ec_cec_cells),
	},
	{
		.id		= EC_FEATURE_RTC,
		.mfd_cells	= cros_ec_rtc_cells,
		.num_cells	= ARRAY_SIZE(cros_ec_rtc_cells),
	},
	{
		.id		= EC_FEATURE_USB_PD,
		.mfd_cells	= cros_usbpd_charger_cells,
		.num_cells	= ARRAY_SIZE(cros_usbpd_charger_cells),
	},
};

static const struct mfd_cell cros_ec_platform_cells[] = {
	{ .name = "cros-ec-chardev", },
	{ .name = "cros-ec-debugfs", },
	{ .name = "cros-ec-lightbar", },
	{ .name = "cros-ec-sysfs", },
};

static const struct mfd_cell cros_ec_vbc_cells[] = {
	{ .name = "cros-ec-vbc", }
};

static int cros_ec_check_features(struct cros_ec_dev *ec, int feature)
{
	struct cros_ec_command *msg;
	int ret;

	if (ec->features[0] == -1U && ec->features[1] == -1U) {
		/* features bitmap not read yet */
		msg = kzalloc(sizeof(*msg) + sizeof(ec->features), GFP_KERNEL);
		if (!msg)
			return -ENOMEM;

		msg->command = EC_CMD_GET_FEATURES + ec->cmd_offset;
		msg->insize = sizeof(ec->features);

		ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
		if (ret < 0) {
			dev_warn(ec->dev, "cannot get EC features: %d/%d\n",
				 ret, msg->result);
			memset(ec->features, 0, sizeof(ec->features));
		} else {
			memcpy(ec->features, msg->data, sizeof(ec->features));
		}

		dev_dbg(ec->dev, "EC features %08x %08x\n",
			ec->features[0], ec->features[1]);

		kfree(msg);
	}

	return ec->features[feature / 32] & EC_FEATURE_MASK_0(feature);
}

static void cros_ec_class_release(struct device *dev)
{
	kfree(to_cros_ec_dev(dev));
}

static void cros_ec_sensors_register(struct cros_ec_dev *ec)
{
	/*
	 * Issue a command to get the number of sensor reported.
	 * Build an array of sensors driver and register them all.
	 */
	int ret, i, id, sensor_num;
	struct mfd_cell *sensor_cells;
	struct cros_ec_sensor_platform *sensor_platforms;
	int sensor_type[MOTIONSENSE_TYPE_MAX];
	struct ec_params_motion_sense *params;
	struct ec_response_motion_sense *resp;
	struct cros_ec_command *msg;

	msg = kzalloc(sizeof(struct cros_ec_command) +
		      max(sizeof(*params), sizeof(*resp)), GFP_KERNEL);
	if (msg == NULL)
		return;

	msg->version = 2;
	msg->command = EC_CMD_MOTION_SENSE_CMD + ec->cmd_offset;
	msg->outsize = sizeof(*params);
	msg->insize = sizeof(*resp);

	params = (struct ec_params_motion_sense *)msg->data;
	params->cmd = MOTIONSENSE_CMD_DUMP;

	ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
	if (ret < 0) {
		dev_warn(ec->dev, "cannot get EC sensor information: %d/%d\n",
			 ret, msg->result);
		goto error;
	}

	resp = (struct ec_response_motion_sense *)msg->data;
	sensor_num = resp->dump.sensor_count;
	/*
	 * Allocate 2 extra sensors if lid angle sensor and/or FIFO are needed.
	 */
	sensor_cells = kcalloc(sensor_num + 2, sizeof(struct mfd_cell),
			       GFP_KERNEL);
	if (sensor_cells == NULL)
		goto error;

	sensor_platforms = kcalloc(sensor_num,
				   sizeof(struct cros_ec_sensor_platform),
				   GFP_KERNEL);
	if (sensor_platforms == NULL)
		goto error_platforms;

	memset(sensor_type, 0, sizeof(sensor_type));
	id = 0;
	for (i = 0; i < sensor_num; i++) {
		params->cmd = MOTIONSENSE_CMD_INFO;
		params->info.sensor_num = i;
		ret = cros_ec_cmd_xfer_status(ec->ec_dev, msg);
		if (ret < 0) {
			dev_warn(ec->dev, "no info for EC sensor %d : %d/%d\n",
				 i, ret, msg->result);
			continue;
		}
		switch (resp->info.type) {
		case MOTIONSENSE_TYPE_ACCEL:
			sensor_cells[id].name = "cros-ec-accel";
			break;
		case MOTIONSENSE_TYPE_BARO:
			sensor_cells[id].name = "cros-ec-baro";
			break;
		case MOTIONSENSE_TYPE_GYRO:
			sensor_cells[id].name = "cros-ec-gyro";
			break;
		case MOTIONSENSE_TYPE_MAG:
			sensor_cells[id].name = "cros-ec-mag";
			break;
		case MOTIONSENSE_TYPE_PROX:
			sensor_cells[id].name = "cros-ec-prox";
			break;
		case MOTIONSENSE_TYPE_LIGHT:
			sensor_cells[id].name = "cros-ec-light";
			break;
		case MOTIONSENSE_TYPE_ACTIVITY:
			sensor_cells[id].name = "cros-ec-activity";
			break;
		default:
			dev_warn(ec->dev, "unknown type %d\n", resp->info.type);
			continue;
		}
		sensor_platforms[id].sensor_num = i;
		sensor_cells[id].id = sensor_type[resp->info.type];
		sensor_cells[id].platform_data = &sensor_platforms[id];
		sensor_cells[id].pdata_size =
			sizeof(struct cros_ec_sensor_platform);

		sensor_type[resp->info.type]++;
		id++;
	}

	if (sensor_type[MOTIONSENSE_TYPE_ACCEL] >= 2)
		ec->has_kb_wake_angle = true;

	if (cros_ec_check_features(ec, EC_FEATURE_MOTION_SENSE_FIFO)) {
		sensor_cells[id].name = "cros-ec-ring";
		id++;
	}
	if (cros_ec_check_features(ec,
				EC_FEATURE_REFINED_TABLET_MODE_HYSTERESIS)) {
		sensor_cells[id].name = "cros-ec-lid-angle";
		id++;
	}

	ret = mfd_add_devices(ec->dev, 0, sensor_cells, id,
			      NULL, 0, NULL);
	if (ret)
		dev_err(ec->dev, "failed to add EC sensors\n");

	kfree(sensor_platforms);
error_platforms:
	kfree(sensor_cells);
error:
	kfree(msg);
}

static struct cros_ec_sensor_platform sensor_platforms[] = {
	{ .sensor_num = 0 },
	{ .sensor_num = 1 }
};

static const struct mfd_cell cros_ec_accel_legacy_cells[] = {
	{
		.name = "cros-ec-accel-legacy",
		.platform_data = &sensor_platforms[0],
		.pdata_size = sizeof(struct cros_ec_sensor_platform),
	},
	{
		.name = "cros-ec-accel-legacy",
		.platform_data = &sensor_platforms[1],
		.pdata_size = sizeof(struct cros_ec_sensor_platform),
	}
};

static void cros_ec_accel_legacy_register(struct cros_ec_dev *ec)
{
	struct cros_ec_device *ec_dev = ec->ec_dev;
	u8 status;
	int ret;

	/*
	 * ECs that need legacy support are the main EC, directly connected to
	 * the AP.
	 */
	if (ec->cmd_offset != 0)
		return;

	/*
	 * Check if EC supports direct memory reads and if EC has
	 * accelerometers.
	 */
	if (ec_dev->cmd_readmem) {
		ret = ec_dev->cmd_readmem(ec_dev, EC_MEMMAP_ACC_STATUS, 1,
					  &status);
		if (ret < 0) {
			dev_warn(ec->dev, "EC direct read error.\n");
			return;
		}

		/* Check if EC has accelerometers. */
		if (!(status & EC_MEMMAP_ACC_STATUS_PRESENCE_BIT)) {
			dev_info(ec->dev, "EC does not have accelerometers.\n");
			return;
		}
	}

	/*
	 * The device may still support accelerometers:
	 * it would be an older ARM based device that do not suppor the
	 * EC_CMD_GET_FEATURES command.
	 *
	 * Register 2 accelerometers, we will fail in the IIO driver if there
	 * are no sensors.
	 */
	ret = mfd_add_hotplug_devices(ec->dev, cros_ec_accel_legacy_cells,
				      ARRAY_SIZE(cros_ec_accel_legacy_cells));
	if (ret)
		dev_err(ec_dev->dev, "failed to add EC sensors\n");
}

static int ec_device_probe(struct platform_device *pdev)
{
	int retval = -ENOMEM;
	struct device_node *node;
	struct device *dev = &pdev->dev;
	struct cros_ec_platform *ec_platform = dev_get_platdata(dev);
	struct cros_ec_dev *ec = kzalloc(sizeof(*ec), GFP_KERNEL);
	int i;

	if (!ec)
		return retval;

	dev_set_drvdata(dev, ec);
	ec->ec_dev = dev_get_drvdata(dev->parent);
	ec->dev = dev;
	ec->cmd_offset = ec_platform->cmd_offset;
	ec->features[0] = -1U; /* Not cached yet */
	ec->features[1] = -1U; /* Not cached yet */
	device_initialize(&ec->class_dev);

	for (i = 0; i < ARRAY_SIZE(cros_mcu_devices); i++) {
		/*
		 * Check whether this is actually a dedicated MCU rather
		 * than an standard EC.
		 */
		if (cros_ec_check_features(ec, cros_mcu_devices[i].id)) {
			dev_info(dev, "CrOS %s MCU detected\n",
				 cros_mcu_devices[i].desc);
			/*
			 * Help userspace differentiating ECs from other MCU,
			 * regardless of the probing order.
			 */
			ec_platform->ec_name = cros_mcu_devices[i].name;
			break;
		}
	}

	/*
	 * Add the class device
	 */
	ec->class_dev.class = &cros_class;
	ec->class_dev.parent = dev;
	ec->class_dev.release = cros_ec_class_release;

	retval = dev_set_name(&ec->class_dev, "%s", ec_platform->ec_name);
	if (retval) {
		dev_err(dev, "dev_set_name failed => %d\n", retval);
		goto failed;
	}

	retval = device_add(&ec->class_dev);
	if (retval)
		goto failed;

	/* check whether this EC is a sensor hub. */
	if (cros_ec_check_features(ec, EC_FEATURE_MOTION_SENSE))
		cros_ec_sensors_register(ec);
	else
		/* Workaroud for older EC firmware */
		cros_ec_accel_legacy_register(ec);

	/*
	 * The following subdevices can be detected by sending the
	 * EC_FEATURE_GET_CMD Embedded Controller device.
	 */
	for (i = 0; i < ARRAY_SIZE(cros_subdevices); i++) {
		if (cros_ec_check_features(ec, cros_subdevices[i].id)) {
			retval = mfd_add_hotplug_devices(ec->dev,
						cros_subdevices[i].mfd_cells,
						cros_subdevices[i].num_cells);
			if (retval)
				dev_err(ec->dev,
					"failed to add %s subdevice: %d\n",
					cros_subdevices[i].mfd_cells->name,
					retval);
		}
	}

	/*
	 * The following subdevices cannot be detected by sending the
	 * EC_FEATURE_GET_CMD to the Embedded Controller device.
	 */
	retval = mfd_add_hotplug_devices(ec->dev, cros_ec_platform_cells,
					 ARRAY_SIZE(cros_ec_platform_cells));
	if (retval)
		dev_warn(ec->dev,
			 "failed to add cros-ec platform devices: %d\n",
			 retval);

	/* Check whether this EC instance has a VBC NVRAM */
	node = ec->ec_dev->dev->of_node;
	if (of_property_read_bool(node, "google,has-vbc-nvram")) {
		retval = mfd_add_hotplug_devices(ec->dev, cros_ec_vbc_cells,
						ARRAY_SIZE(cros_ec_vbc_cells));
		if (retval)
			dev_warn(ec->dev, "failed to add VBC devices: %d\n",
				 retval);
	}

	return 0;

failed:
	put_device(&ec->class_dev);
	return retval;
}

static int ec_device_remove(struct platform_device *pdev)
{
	struct cros_ec_dev *ec = dev_get_drvdata(&pdev->dev);

	mfd_remove_devices(ec->dev);
	device_unregister(&ec->class_dev);
	return 0;
}

static const struct platform_device_id cros_ec_id[] = {
	{ DRV_NAME, 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, cros_ec_id);

static struct platform_driver cros_ec_dev_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.id_table = cros_ec_id,
	.probe = ec_device_probe,
	.remove = ec_device_remove,
};

static int __init cros_ec_dev_init(void)
{
	int ret;

	ret  = class_register(&cros_class);
	if (ret) {
		pr_err(CROS_EC_DEV_NAME ": failed to register device class\n");
		return ret;
	}

	/* Register the driver */
	ret = platform_driver_register(&cros_ec_dev_driver);
	if (ret < 0) {
		pr_warn(CROS_EC_DEV_NAME ": can't register driver: %d\n", ret);
		goto failed_devreg;
	}
	return 0;

failed_devreg:
	class_unregister(&cros_class);
	return ret;
}

static void __exit cros_ec_dev_exit(void)
{
	platform_driver_unregister(&cros_ec_dev_driver);
	class_unregister(&cros_class);
}

module_init(cros_ec_dev_init);
module_exit(cros_ec_dev_exit);

MODULE_ALIAS("platform:" DRV_NAME);
MODULE_AUTHOR("Bill Richardson <wfrichar@chromium.org>");
MODULE_DESCRIPTION("Userspace interface to the Chrome OS Embedded Controller");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
