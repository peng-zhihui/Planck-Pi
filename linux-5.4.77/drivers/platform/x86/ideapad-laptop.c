// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  ideapad-laptop.c - Lenovo IdeaPad ACPI Extras
 *
 *  Copyright © 2010 Intel Corporation
 *  Copyright © 2010 David Woodhouse <dwmw2@infradead.org>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/acpi.h>
#include <linux/rfkill.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/i8042.h>
#include <linux/dmi.h>
#include <linux/device.h>
#include <acpi/video.h>

#define IDEAPAD_RFKILL_DEV_NUM	(3)

#define BM_CONSERVATION_BIT (5)
#define HA_FNLOCK_BIT       (10)

#define CFG_BT_BIT	(16)
#define CFG_3G_BIT	(17)
#define CFG_WIFI_BIT	(18)
#define CFG_CAMERA_BIT	(19)

#if IS_ENABLED(CONFIG_ACPI_WMI)
static const char *const ideapad_wmi_fnesc_events[] = {
	"26CAB2E5-5CF1-46AE-AAC3-4A12B6BA50E6", /* Yoga 3 */
	"56322276-8493-4CE8-A783-98C991274F5E", /* Yoga 700 */
};
#endif

enum {
	BMCMD_CONSERVATION_ON = 3,
	BMCMD_CONSERVATION_OFF = 5,
	HACMD_FNLOCK_ON = 0xe,
	HACMD_FNLOCK_OFF = 0xf,
};

enum {
	VPCCMD_R_VPC1 = 0x10,
	VPCCMD_R_BL_MAX,
	VPCCMD_R_BL,
	VPCCMD_W_BL,
	VPCCMD_R_WIFI,
	VPCCMD_W_WIFI,
	VPCCMD_R_BT,
	VPCCMD_W_BT,
	VPCCMD_R_BL_POWER,
	VPCCMD_R_NOVO,
	VPCCMD_R_VPC2,
	VPCCMD_R_TOUCHPAD,
	VPCCMD_W_TOUCHPAD,
	VPCCMD_R_CAMERA,
	VPCCMD_W_CAMERA,
	VPCCMD_R_3G,
	VPCCMD_W_3G,
	VPCCMD_R_ODD, /* 0x21 */
	VPCCMD_W_FAN,
	VPCCMD_R_RF,
	VPCCMD_W_RF,
	VPCCMD_R_FAN = 0x2B,
	VPCCMD_R_SPECIAL_BUTTONS = 0x31,
	VPCCMD_W_BL_POWER = 0x33,
};

struct ideapad_rfk_priv {
	int dev;
	struct ideapad_private *priv;
};

struct ideapad_private {
	struct acpi_device *adev;
	struct rfkill *rfk[IDEAPAD_RFKILL_DEV_NUM];
	struct ideapad_rfk_priv rfk_priv[IDEAPAD_RFKILL_DEV_NUM];
	struct platform_device *platform_device;
	struct input_dev *inputdev;
	struct backlight_device *blightdev;
	struct dentry *debug;
	unsigned long cfg;
	bool has_hw_rfkill_switch;
	const char *fnesc_guid;
};

static bool no_bt_rfkill;
module_param(no_bt_rfkill, bool, 0444);
MODULE_PARM_DESC(no_bt_rfkill, "No rfkill for bluetooth.");

/*
 * ACPI Helpers
 */
#define IDEAPAD_EC_TIMEOUT (200) /* in ms */

static int read_method_int(acpi_handle handle, const char *method, int *val)
{
	acpi_status status;
	unsigned long long result;

	status = acpi_evaluate_integer(handle, (char *)method, NULL, &result);
	if (ACPI_FAILURE(status)) {
		*val = -1;
		return -1;
	}
	*val = result;
	return 0;

}

static int method_gbmd(acpi_handle handle, unsigned long *ret)
{
	int result, val;

	result = read_method_int(handle, "GBMD", &val);
	*ret = val;
	return result;
}

static int method_int1(acpi_handle handle, char *method, int cmd)
{
	acpi_status status;

	status = acpi_execute_simple_method(handle, method, cmd);
	return ACPI_FAILURE(status) ? -1 : 0;
}

static int method_vpcr(acpi_handle handle, int cmd, int *ret)
{
	acpi_status status;
	unsigned long long result;
	struct acpi_object_list params;
	union acpi_object in_obj;

	params.count = 1;
	params.pointer = &in_obj;
	in_obj.type = ACPI_TYPE_INTEGER;
	in_obj.integer.value = cmd;

	status = acpi_evaluate_integer(handle, "VPCR", &params, &result);

	if (ACPI_FAILURE(status)) {
		*ret = -1;
		return -1;
	}
	*ret = result;
	return 0;

}

static int method_vpcw(acpi_handle handle, int cmd, int data)
{
	struct acpi_object_list params;
	union acpi_object in_obj[2];
	acpi_status status;

	params.count = 2;
	params.pointer = in_obj;
	in_obj[0].type = ACPI_TYPE_INTEGER;
	in_obj[0].integer.value = cmd;
	in_obj[1].type = ACPI_TYPE_INTEGER;
	in_obj[1].integer.value = data;

	status = acpi_evaluate_object(handle, "VPCW", &params, NULL);
	if (status != AE_OK)
		return -1;
	return 0;
}

static int read_ec_data(acpi_handle handle, int cmd, unsigned long *data)
{
	int val;
	unsigned long int end_jiffies;

	if (method_vpcw(handle, 1, cmd))
		return -1;

	for (end_jiffies = jiffies+(HZ)*IDEAPAD_EC_TIMEOUT/1000+1;
	     time_before(jiffies, end_jiffies);) {
		schedule();
		if (method_vpcr(handle, 1, &val))
			return -1;
		if (val == 0) {
			if (method_vpcr(handle, 0, &val))
				return -1;
			*data = val;
			return 0;
		}
	}
	pr_err("timeout in %s\n", __func__);
	return -1;
}

static int write_ec_cmd(acpi_handle handle, int cmd, unsigned long data)
{
	int val;
	unsigned long int end_jiffies;

	if (method_vpcw(handle, 0, data))
		return -1;
	if (method_vpcw(handle, 1, cmd))
		return -1;

	for (end_jiffies = jiffies+(HZ)*IDEAPAD_EC_TIMEOUT/1000+1;
	     time_before(jiffies, end_jiffies);) {
		schedule();
		if (method_vpcr(handle, 1, &val))
			return -1;
		if (val == 0)
			return 0;
	}
	pr_err("timeout in %s\n", __func__);
	return -1;
}

/*
 * debugfs
 */
static int debugfs_status_show(struct seq_file *s, void *data)
{
	struct ideapad_private *priv = s->private;
	unsigned long value;

	if (!priv)
		return -EINVAL;

	if (!read_ec_data(priv->adev->handle, VPCCMD_R_BL_MAX, &value))
		seq_printf(s, "Backlight max:\t%lu\n", value);
	if (!read_ec_data(priv->adev->handle, VPCCMD_R_BL, &value))
		seq_printf(s, "Backlight now:\t%lu\n", value);
	if (!read_ec_data(priv->adev->handle, VPCCMD_R_BL_POWER, &value))
		seq_printf(s, "BL power value:\t%s\n", value ? "On" : "Off");
	seq_printf(s, "=====================\n");

	if (!read_ec_data(priv->adev->handle, VPCCMD_R_RF, &value))
		seq_printf(s, "Radio status:\t%s(%lu)\n",
			   value ? "On" : "Off", value);
	if (!read_ec_data(priv->adev->handle, VPCCMD_R_WIFI, &value))
		seq_printf(s, "Wifi status:\t%s(%lu)\n",
			   value ? "On" : "Off", value);
	if (!read_ec_data(priv->adev->handle, VPCCMD_R_BT, &value))
		seq_printf(s, "BT status:\t%s(%lu)\n",
			   value ? "On" : "Off", value);
	if (!read_ec_data(priv->adev->handle, VPCCMD_R_3G, &value))
		seq_printf(s, "3G status:\t%s(%lu)\n",
			   value ? "On" : "Off", value);
	seq_printf(s, "=====================\n");

	if (!read_ec_data(priv->adev->handle, VPCCMD_R_TOUCHPAD, &value))
		seq_printf(s, "Touchpad status:%s(%lu)\n",
			   value ? "On" : "Off", value);
	if (!read_ec_data(priv->adev->handle, VPCCMD_R_CAMERA, &value))
		seq_printf(s, "Camera status:\t%s(%lu)\n",
			   value ? "On" : "Off", value);
	seq_puts(s, "=====================\n");

	if (!method_gbmd(priv->adev->handle, &value)) {
		seq_printf(s, "Conservation mode:\t%s(%lu)\n",
			   test_bit(BM_CONSERVATION_BIT, &value) ? "On" : "Off",
			   value);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(debugfs_status);

static int debugfs_cfg_show(struct seq_file *s, void *data)
{
	struct ideapad_private *priv = s->private;

	if (!priv) {
		seq_printf(s, "cfg: N/A\n");
	} else {
		seq_printf(s, "cfg: 0x%.8lX\n\nCapability: ",
			   priv->cfg);
		if (test_bit(CFG_BT_BIT, &priv->cfg))
			seq_printf(s, "Bluetooth ");
		if (test_bit(CFG_3G_BIT, &priv->cfg))
			seq_printf(s, "3G ");
		if (test_bit(CFG_WIFI_BIT, &priv->cfg))
			seq_printf(s, "Wireless ");
		if (test_bit(CFG_CAMERA_BIT, &priv->cfg))
			seq_printf(s, "Camera ");
		seq_printf(s, "\nGraphic: ");
		switch ((priv->cfg)&0x700) {
		case 0x100:
			seq_printf(s, "Intel");
			break;
		case 0x200:
			seq_printf(s, "ATI");
			break;
		case 0x300:
			seq_printf(s, "Nvidia");
			break;
		case 0x400:
			seq_printf(s, "Intel and ATI");
			break;
		case 0x500:
			seq_printf(s, "Intel and Nvidia");
			break;
		}
		seq_printf(s, "\n");
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(debugfs_cfg);

static void ideapad_debugfs_init(struct ideapad_private *priv)
{
	struct dentry *dir;

	dir = debugfs_create_dir("ideapad", NULL);
	priv->debug = dir;

	debugfs_create_file("cfg", S_IRUGO, dir, priv, &debugfs_cfg_fops);
	debugfs_create_file("status", S_IRUGO, dir, priv, &debugfs_status_fops);
}

static void ideapad_debugfs_exit(struct ideapad_private *priv)
{
	debugfs_remove_recursive(priv->debug);
	priv->debug = NULL;
}

/*
 * sysfs
 */
static ssize_t show_ideapad_cam(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	unsigned long result;
	struct ideapad_private *priv = dev_get_drvdata(dev);

	if (read_ec_data(priv->adev->handle, VPCCMD_R_CAMERA, &result))
		return sprintf(buf, "-1\n");
	return sprintf(buf, "%lu\n", result);
}

static ssize_t store_ideapad_cam(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret, state;
	struct ideapad_private *priv = dev_get_drvdata(dev);

	if (!count)
		return 0;
	if (sscanf(buf, "%i", &state) != 1)
		return -EINVAL;
	ret = write_ec_cmd(priv->adev->handle, VPCCMD_W_CAMERA, state);
	if (ret < 0)
		return -EIO;
	return count;
}

static DEVICE_ATTR(camera_power, 0644, show_ideapad_cam, store_ideapad_cam);

static ssize_t show_ideapad_fan(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	unsigned long result;
	struct ideapad_private *priv = dev_get_drvdata(dev);

	if (read_ec_data(priv->adev->handle, VPCCMD_R_FAN, &result))
		return sprintf(buf, "-1\n");
	return sprintf(buf, "%lu\n", result);
}

static ssize_t store_ideapad_fan(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret, state;
	struct ideapad_private *priv = dev_get_drvdata(dev);

	if (!count)
		return 0;
	if (sscanf(buf, "%i", &state) != 1)
		return -EINVAL;
	if (state < 0 || state > 4 || state == 3)
		return -EINVAL;
	ret = write_ec_cmd(priv->adev->handle, VPCCMD_W_FAN, state);
	if (ret < 0)
		return -EIO;
	return count;
}

static DEVICE_ATTR(fan_mode, 0644, show_ideapad_fan, store_ideapad_fan);

static ssize_t touchpad_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct ideapad_private *priv = dev_get_drvdata(dev);
	unsigned long result;

	if (read_ec_data(priv->adev->handle, VPCCMD_R_TOUCHPAD, &result))
		return sprintf(buf, "-1\n");
	return sprintf(buf, "%lu\n", result);
}

/* Switch to RO for now: It might be revisited in the future */
static ssize_t __maybe_unused touchpad_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct ideapad_private *priv = dev_get_drvdata(dev);
	bool state;
	int ret;

	ret = kstrtobool(buf, &state);
	if (ret)
		return ret;

	ret = write_ec_cmd(priv->adev->handle, VPCCMD_W_TOUCHPAD, state);
	if (ret < 0)
		return -EIO;
	return count;
}

static DEVICE_ATTR_RO(touchpad);

static ssize_t conservation_mode_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct ideapad_private *priv = dev_get_drvdata(dev);
	unsigned long result;

	if (method_gbmd(priv->adev->handle, &result))
		return sprintf(buf, "-1\n");
	return sprintf(buf, "%u\n", test_bit(BM_CONSERVATION_BIT, &result));
}

static ssize_t conservation_mode_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct ideapad_private *priv = dev_get_drvdata(dev);
	bool state;
	int ret;

	ret = kstrtobool(buf, &state);
	if (ret)
		return ret;

	ret = method_int1(priv->adev->handle, "SBMC", state ?
					      BMCMD_CONSERVATION_ON :
					      BMCMD_CONSERVATION_OFF);
	if (ret < 0)
		return -EIO;
	return count;
}

static DEVICE_ATTR_RW(conservation_mode);

static ssize_t fn_lock_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct ideapad_private *priv = dev_get_drvdata(dev);
	unsigned long result;
	int hals;
	int fail = read_method_int(priv->adev->handle, "HALS", &hals);

	if (fail)
		return sprintf(buf, "-1\n");

	result = hals;
	return sprintf(buf, "%u\n", test_bit(HA_FNLOCK_BIT, &result));
}

static ssize_t fn_lock_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ideapad_private *priv = dev_get_drvdata(dev);
	bool state;
	int ret;

	ret = kstrtobool(buf, &state);
	if (ret)
		return ret;

	ret = method_int1(priv->adev->handle, "SALS", state ?
			  HACMD_FNLOCK_ON :
			  HACMD_FNLOCK_OFF);
	if (ret < 0)
		return -EIO;
	return count;
}

static DEVICE_ATTR_RW(fn_lock);


static struct attribute *ideapad_attributes[] = {
	&dev_attr_camera_power.attr,
	&dev_attr_fan_mode.attr,
	&dev_attr_touchpad.attr,
	&dev_attr_conservation_mode.attr,
	&dev_attr_fn_lock.attr,
	NULL
};

static umode_t ideapad_is_visible(struct kobject *kobj,
				 struct attribute *attr,
				 int idx)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct ideapad_private *priv = dev_get_drvdata(dev);
	bool supported;

	if (attr == &dev_attr_camera_power.attr)
		supported = test_bit(CFG_CAMERA_BIT, &(priv->cfg));
	else if (attr == &dev_attr_fan_mode.attr) {
		unsigned long value;
		supported = !read_ec_data(priv->adev->handle, VPCCMD_R_FAN,
					  &value);
	} else if (attr == &dev_attr_conservation_mode.attr) {
		supported = acpi_has_method(priv->adev->handle, "GBMD") &&
			    acpi_has_method(priv->adev->handle, "SBMC");
	} else if (attr == &dev_attr_fn_lock.attr) {
		supported = acpi_has_method(priv->adev->handle, "HALS") &&
			acpi_has_method(priv->adev->handle, "SALS");
	} else
		supported = true;

	return supported ? attr->mode : 0;
}

static const struct attribute_group ideapad_attribute_group = {
	.is_visible = ideapad_is_visible,
	.attrs = ideapad_attributes
};

/*
 * Rfkill
 */
struct ideapad_rfk_data {
	char *name;
	int cfgbit;
	int opcode;
	int type;
};

static const struct ideapad_rfk_data ideapad_rfk_data[] = {
	{ "ideapad_wlan",    CFG_WIFI_BIT, VPCCMD_W_WIFI, RFKILL_TYPE_WLAN },
	{ "ideapad_bluetooth", CFG_BT_BIT, VPCCMD_W_BT, RFKILL_TYPE_BLUETOOTH },
	{ "ideapad_3g",        CFG_3G_BIT, VPCCMD_W_3G, RFKILL_TYPE_WWAN },
};

static int ideapad_rfk_set(void *data, bool blocked)
{
	struct ideapad_rfk_priv *priv = data;
	int opcode = ideapad_rfk_data[priv->dev].opcode;

	return write_ec_cmd(priv->priv->adev->handle, opcode, !blocked);
}

static const struct rfkill_ops ideapad_rfk_ops = {
	.set_block = ideapad_rfk_set,
};

static void ideapad_sync_rfk_state(struct ideapad_private *priv)
{
	unsigned long hw_blocked = 0;
	int i;

	if (priv->has_hw_rfkill_switch) {
		if (read_ec_data(priv->adev->handle, VPCCMD_R_RF, &hw_blocked))
			return;
		hw_blocked = !hw_blocked;
	}

	for (i = 0; i < IDEAPAD_RFKILL_DEV_NUM; i++)
		if (priv->rfk[i])
			rfkill_set_hw_state(priv->rfk[i], hw_blocked);
}

static int ideapad_register_rfkill(struct ideapad_private *priv, int dev)
{
	int ret;
	unsigned long sw_blocked;

	if (no_bt_rfkill &&
	    (ideapad_rfk_data[dev].type == RFKILL_TYPE_BLUETOOTH)) {
		/* Force to enable bluetooth when no_bt_rfkill=1 */
		write_ec_cmd(priv->adev->handle,
			     ideapad_rfk_data[dev].opcode, 1);
		return 0;
	}
	priv->rfk_priv[dev].dev = dev;
	priv->rfk_priv[dev].priv = priv;

	priv->rfk[dev] = rfkill_alloc(ideapad_rfk_data[dev].name,
				      &priv->platform_device->dev,
				      ideapad_rfk_data[dev].type,
				      &ideapad_rfk_ops,
				      &priv->rfk_priv[dev]);
	if (!priv->rfk[dev])
		return -ENOMEM;

	if (read_ec_data(priv->adev->handle, ideapad_rfk_data[dev].opcode-1,
			 &sw_blocked)) {
		rfkill_init_sw_state(priv->rfk[dev], 0);
	} else {
		sw_blocked = !sw_blocked;
		rfkill_init_sw_state(priv->rfk[dev], sw_blocked);
	}

	ret = rfkill_register(priv->rfk[dev]);
	if (ret) {
		rfkill_destroy(priv->rfk[dev]);
		return ret;
	}
	return 0;
}

static void ideapad_unregister_rfkill(struct ideapad_private *priv, int dev)
{
	if (!priv->rfk[dev])
		return;

	rfkill_unregister(priv->rfk[dev]);
	rfkill_destroy(priv->rfk[dev]);
}

/*
 * Platform device
 */
static int ideapad_sysfs_init(struct ideapad_private *priv)
{
	return sysfs_create_group(&priv->platform_device->dev.kobj,
				    &ideapad_attribute_group);
}

static void ideapad_sysfs_exit(struct ideapad_private *priv)
{
	sysfs_remove_group(&priv->platform_device->dev.kobj,
			   &ideapad_attribute_group);
}

/*
 * input device
 */
static const struct key_entry ideapad_keymap[] = {
	{ KE_KEY, 6,  { KEY_SWITCHVIDEOMODE } },
	{ KE_KEY, 7,  { KEY_CAMERA } },
	{ KE_KEY, 8,  { KEY_MICMUTE } },
	{ KE_KEY, 11, { KEY_F16 } },
	{ KE_KEY, 13, { KEY_WLAN } },
	{ KE_KEY, 16, { KEY_PROG1 } },
	{ KE_KEY, 17, { KEY_PROG2 } },
	{ KE_KEY, 64, { KEY_PROG3 } },
	{ KE_KEY, 65, { KEY_PROG4 } },
	{ KE_KEY, 66, { KEY_TOUCHPAD_OFF } },
	{ KE_KEY, 67, { KEY_TOUCHPAD_ON } },
	{ KE_KEY, 128, { KEY_ESC } },

	{ KE_END, 0 },
};

static int ideapad_input_init(struct ideapad_private *priv)
{
	struct input_dev *inputdev;
	int error;

	inputdev = input_allocate_device();
	if (!inputdev)
		return -ENOMEM;

	inputdev->name = "Ideapad extra buttons";
	inputdev->phys = "ideapad/input0";
	inputdev->id.bustype = BUS_HOST;
	inputdev->dev.parent = &priv->platform_device->dev;

	error = sparse_keymap_setup(inputdev, ideapad_keymap, NULL);
	if (error) {
		pr_err("Unable to setup input device keymap\n");
		goto err_free_dev;
	}

	error = input_register_device(inputdev);
	if (error) {
		pr_err("Unable to register input device\n");
		goto err_free_dev;
	}

	priv->inputdev = inputdev;
	return 0;

err_free_dev:
	input_free_device(inputdev);
	return error;
}

static void ideapad_input_exit(struct ideapad_private *priv)
{
	input_unregister_device(priv->inputdev);
	priv->inputdev = NULL;
}

static void ideapad_input_report(struct ideapad_private *priv,
				 unsigned long scancode)
{
	sparse_keymap_report_event(priv->inputdev, scancode, 1, true);
}

static void ideapad_input_novokey(struct ideapad_private *priv)
{
	unsigned long long_pressed;

	if (read_ec_data(priv->adev->handle, VPCCMD_R_NOVO, &long_pressed))
		return;
	if (long_pressed)
		ideapad_input_report(priv, 17);
	else
		ideapad_input_report(priv, 16);
}

static void ideapad_check_special_buttons(struct ideapad_private *priv)
{
	unsigned long bit, value;

	read_ec_data(priv->adev->handle, VPCCMD_R_SPECIAL_BUTTONS, &value);

	for (bit = 0; bit < 16; bit++) {
		if (test_bit(bit, &value)) {
			switch (bit) {
			case 0:	/* Z580 */
			case 6:	/* Z570 */
				/* Thermal Management button */
				ideapad_input_report(priv, 65);
				break;
			case 1:
				/* OneKey Theater button */
				ideapad_input_report(priv, 64);
				break;
			default:
				pr_info("Unknown special button: %lu\n", bit);
				break;
			}
		}
	}
}

/*
 * backlight
 */
static int ideapad_backlight_get_brightness(struct backlight_device *blightdev)
{
	struct ideapad_private *priv = bl_get_data(blightdev);
	unsigned long now;

	if (!priv)
		return -EINVAL;

	if (read_ec_data(priv->adev->handle, VPCCMD_R_BL, &now))
		return -EIO;
	return now;
}

static int ideapad_backlight_update_status(struct backlight_device *blightdev)
{
	struct ideapad_private *priv = bl_get_data(blightdev);

	if (!priv)
		return -EINVAL;

	if (write_ec_cmd(priv->adev->handle, VPCCMD_W_BL,
			 blightdev->props.brightness))
		return -EIO;
	if (write_ec_cmd(priv->adev->handle, VPCCMD_W_BL_POWER,
			 blightdev->props.power == FB_BLANK_POWERDOWN ? 0 : 1))
		return -EIO;

	return 0;
}

static const struct backlight_ops ideapad_backlight_ops = {
	.get_brightness = ideapad_backlight_get_brightness,
	.update_status = ideapad_backlight_update_status,
};

static int ideapad_backlight_init(struct ideapad_private *priv)
{
	struct backlight_device *blightdev;
	struct backlight_properties props;
	unsigned long max, now, power;

	if (read_ec_data(priv->adev->handle, VPCCMD_R_BL_MAX, &max))
		return -EIO;
	if (read_ec_data(priv->adev->handle, VPCCMD_R_BL, &now))
		return -EIO;
	if (read_ec_data(priv->adev->handle, VPCCMD_R_BL_POWER, &power))
		return -EIO;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.max_brightness = max;
	props.type = BACKLIGHT_PLATFORM;
	blightdev = backlight_device_register("ideapad",
					      &priv->platform_device->dev,
					      priv,
					      &ideapad_backlight_ops,
					      &props);
	if (IS_ERR(blightdev)) {
		pr_err("Could not register backlight device\n");
		return PTR_ERR(blightdev);
	}

	priv->blightdev = blightdev;
	blightdev->props.brightness = now;
	blightdev->props.power = power ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;
	backlight_update_status(blightdev);

	return 0;
}

static void ideapad_backlight_exit(struct ideapad_private *priv)
{
	backlight_device_unregister(priv->blightdev);
	priv->blightdev = NULL;
}

static void ideapad_backlight_notify_power(struct ideapad_private *priv)
{
	unsigned long power;
	struct backlight_device *blightdev = priv->blightdev;

	if (!blightdev)
		return;
	if (read_ec_data(priv->adev->handle, VPCCMD_R_BL_POWER, &power))
		return;
	blightdev->props.power = power ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;
}

static void ideapad_backlight_notify_brightness(struct ideapad_private *priv)
{
	unsigned long now;

	/* if we control brightness via acpi video driver */
	if (priv->blightdev == NULL) {
		read_ec_data(priv->adev->handle, VPCCMD_R_BL, &now);
		return;
	}

	backlight_force_update(priv->blightdev, BACKLIGHT_UPDATE_HOTKEY);
}

/*
 * module init/exit
 */
static void ideapad_sync_touchpad_state(struct ideapad_private *priv)
{
	unsigned long value;

	/* Without reading from EC touchpad LED doesn't switch state */
	if (!read_ec_data(priv->adev->handle, VPCCMD_R_TOUCHPAD, &value)) {
		/* Some IdeaPads don't really turn off touchpad - they only
		 * switch the LED state. We (de)activate KBC AUX port to turn
		 * touchpad off and on. We send KEY_TOUCHPAD_OFF and
		 * KEY_TOUCHPAD_ON to not to get out of sync with LED */
		unsigned char param;
		i8042_command(&param, value ? I8042_CMD_AUX_ENABLE :
			      I8042_CMD_AUX_DISABLE);
		ideapad_input_report(priv, value ? 67 : 66);
	}
}

static void ideapad_acpi_notify(acpi_handle handle, u32 event, void *data)
{
	struct ideapad_private *priv = data;
	unsigned long vpc1, vpc2, vpc_bit;

	if (read_ec_data(handle, VPCCMD_R_VPC1, &vpc1))
		return;
	if (read_ec_data(handle, VPCCMD_R_VPC2, &vpc2))
		return;

	vpc1 = (vpc2 << 8) | vpc1;
	for (vpc_bit = 0; vpc_bit < 16; vpc_bit++) {
		if (test_bit(vpc_bit, &vpc1)) {
			switch (vpc_bit) {
			case 9:
				ideapad_sync_rfk_state(priv);
				break;
			case 13:
			case 11:
			case 8:
			case 7:
			case 6:
				ideapad_input_report(priv, vpc_bit);
				break;
			case 5:
				ideapad_sync_touchpad_state(priv);
				break;
			case 4:
				ideapad_backlight_notify_brightness(priv);
				break;
			case 3:
				ideapad_input_novokey(priv);
				break;
			case 2:
				ideapad_backlight_notify_power(priv);
				break;
			case 0:
				ideapad_check_special_buttons(priv);
				break;
			case 1:
				/* Some IdeaPads report event 1 every ~20
				 * seconds while on battery power; some
				 * report this when changing to/from tablet
				 * mode. Squelch this event.
				 */
				break;
			default:
				pr_info("Unknown event: %lu\n", vpc_bit);
			}
		}
	}
}

#if IS_ENABLED(CONFIG_ACPI_WMI)
static void ideapad_wmi_notify(u32 value, void *context)
{
	switch (value) {
	case 128:
		ideapad_input_report(context, value);
		break;
	default:
		pr_info("Unknown WMI event %u\n", value);
	}
}
#endif

/*
 * Some ideapads have a hardware rfkill switch, but most do not have one.
 * Reading VPCCMD_R_RF always results in 0 on models without a hardware rfkill,
 * switch causing ideapad_laptop to wrongly report all radios as hw-blocked.
 * There used to be a long list of DMI ids for models without a hw rfkill
 * switch here, but that resulted in playing whack a mole.
 * More importantly wrongly reporting the wifi radio as hw-blocked, results in
 * non working wifi. Whereas not reporting it hw-blocked, when it actually is
 * hw-blocked results in an empty SSID list, which is a much more benign
 * failure mode.
 * So the default now is the much safer option of assuming there is no
 * hardware rfkill switch. This default also actually matches most hardware,
 * since having a hw rfkill switch is quite rare on modern hardware, so this
 * also leads to a much shorter list.
 */
static const struct dmi_system_id hw_rfkill_list[] = {
	{}
};

static int ideapad_acpi_add(struct platform_device *pdev)
{
	int ret, i;
	int cfg;
	struct ideapad_private *priv;
	struct acpi_device *adev;

	ret = acpi_bus_get_device(ACPI_HANDLE(&pdev->dev), &adev);
	if (ret)
		return -ENODEV;

	if (read_method_int(adev->handle, "_CFG", &cfg))
		return -ENODEV;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, priv);
	priv->cfg = cfg;
	priv->adev = adev;
	priv->platform_device = pdev;
	priv->has_hw_rfkill_switch = dmi_check_system(hw_rfkill_list);

	ret = ideapad_sysfs_init(priv);
	if (ret)
		return ret;

	ideapad_debugfs_init(priv);

	ret = ideapad_input_init(priv);
	if (ret)
		goto input_failed;

	/*
	 * On some models without a hw-switch (the yoga 2 13 at least)
	 * VPCCMD_W_RF must be explicitly set to 1 for the wifi to work.
	 */
	if (!priv->has_hw_rfkill_switch)
		write_ec_cmd(priv->adev->handle, VPCCMD_W_RF, 1);

	for (i = 0; i < IDEAPAD_RFKILL_DEV_NUM; i++)
		if (test_bit(ideapad_rfk_data[i].cfgbit, &priv->cfg))
			ideapad_register_rfkill(priv, i);

	ideapad_sync_rfk_state(priv);
	ideapad_sync_touchpad_state(priv);

	if (acpi_video_get_backlight_type() == acpi_backlight_vendor) {
		ret = ideapad_backlight_init(priv);
		if (ret && ret != -ENODEV)
			goto backlight_failed;
	}
	ret = acpi_install_notify_handler(adev->handle,
		ACPI_DEVICE_NOTIFY, ideapad_acpi_notify, priv);
	if (ret)
		goto notification_failed;

#if IS_ENABLED(CONFIG_ACPI_WMI)
	for (i = 0; i < ARRAY_SIZE(ideapad_wmi_fnesc_events); i++) {
		ret = wmi_install_notify_handler(ideapad_wmi_fnesc_events[i],
						 ideapad_wmi_notify, priv);
		if (ret == AE_OK) {
			priv->fnesc_guid = ideapad_wmi_fnesc_events[i];
			break;
		}
	}
	if (ret != AE_OK && ret != AE_NOT_EXIST)
		goto notification_failed_wmi;
#endif

	return 0;
#if IS_ENABLED(CONFIG_ACPI_WMI)
notification_failed_wmi:
	acpi_remove_notify_handler(priv->adev->handle,
		ACPI_DEVICE_NOTIFY, ideapad_acpi_notify);
#endif
notification_failed:
	ideapad_backlight_exit(priv);
backlight_failed:
	for (i = 0; i < IDEAPAD_RFKILL_DEV_NUM; i++)
		ideapad_unregister_rfkill(priv, i);
	ideapad_input_exit(priv);
input_failed:
	ideapad_debugfs_exit(priv);
	ideapad_sysfs_exit(priv);
	return ret;
}

static int ideapad_acpi_remove(struct platform_device *pdev)
{
	struct ideapad_private *priv = dev_get_drvdata(&pdev->dev);
	int i;

#if IS_ENABLED(CONFIG_ACPI_WMI)
	if (priv->fnesc_guid)
		wmi_remove_notify_handler(priv->fnesc_guid);
#endif
	acpi_remove_notify_handler(priv->adev->handle,
		ACPI_DEVICE_NOTIFY, ideapad_acpi_notify);
	ideapad_backlight_exit(priv);
	for (i = 0; i < IDEAPAD_RFKILL_DEV_NUM; i++)
		ideapad_unregister_rfkill(priv, i);
	ideapad_input_exit(priv);
	ideapad_debugfs_exit(priv);
	ideapad_sysfs_exit(priv);
	dev_set_drvdata(&pdev->dev, NULL);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int ideapad_acpi_resume(struct device *device)
{
	struct ideapad_private *priv;

	if (!device)
		return -EINVAL;
	priv = dev_get_drvdata(device);

	ideapad_sync_rfk_state(priv);
	ideapad_sync_touchpad_state(priv);
	return 0;
}
#endif
static SIMPLE_DEV_PM_OPS(ideapad_pm, NULL, ideapad_acpi_resume);

static const struct acpi_device_id ideapad_device_ids[] = {
	{ "VPC2004", 0},
	{ "", 0},
};
MODULE_DEVICE_TABLE(acpi, ideapad_device_ids);

static struct platform_driver ideapad_acpi_driver = {
	.probe = ideapad_acpi_add,
	.remove = ideapad_acpi_remove,
	.driver = {
		.name   = "ideapad_acpi",
		.pm     = &ideapad_pm,
		.acpi_match_table = ACPI_PTR(ideapad_device_ids),
	},
};

module_platform_driver(ideapad_acpi_driver);

MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org>");
MODULE_DESCRIPTION("IdeaPad ACPI Extras");
MODULE_LICENSE("GPL");
