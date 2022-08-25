// SPDX-License-Identifier: GPL-2.0-only
/*
 * Line 6 Pod HD
 *
 * Copyright (C) 2011 Stefan Hajnoczi <stefanha@gmail.com>
 * Copyright (C) 2015 Andrej Krutak <dev@andree.sk>
 * Copyright (C) 2017 Hans P. Moller <hmoller@uc.cl>
 */

#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <sound/core.h>
#include <sound/pcm.h>

#include "driver.h"
#include "pcm.h"

#define PODHD_STARTUP_DELAY 500

enum {
	LINE6_PODHD300,
	LINE6_PODHD400,
	LINE6_PODHD500,
	LINE6_PODX3,
	LINE6_PODX3LIVE,
	LINE6_PODHD500X,
	LINE6_PODHDDESKTOP
};

struct usb_line6_podhd {
	/* Generic Line 6 USB data */
	struct usb_line6 line6;

	/* Serial number of device */
	u32 serial_number;

	/* Firmware version */
	int firmware_version;
};

#define line6_to_podhd(x)	container_of(x, struct usb_line6_podhd, line6)

static struct snd_ratden podhd_ratden = {
	.num_min = 48000,
	.num_max = 48000,
	.num_step = 1,
	.den = 1,
};

static struct line6_pcm_properties podhd_pcm_properties = {
	.playback_hw = {
				  .info = (SNDRV_PCM_INFO_MMAP |
					   SNDRV_PCM_INFO_INTERLEAVED |
					   SNDRV_PCM_INFO_BLOCK_TRANSFER |
					   SNDRV_PCM_INFO_MMAP_VALID |
					   SNDRV_PCM_INFO_PAUSE |
					   SNDRV_PCM_INFO_SYNC_START),
				  .formats = SNDRV_PCM_FMTBIT_S24_3LE,
				  .rates = SNDRV_PCM_RATE_48000,
				  .rate_min = 48000,
				  .rate_max = 48000,
				  .channels_min = 2,
				  .channels_max = 2,
				  .buffer_bytes_max = 60000,
				  .period_bytes_min = 64,
				  .period_bytes_max = 8192,
				  .periods_min = 1,
				  .periods_max = 1024},
	.capture_hw = {
				 .info = (SNDRV_PCM_INFO_MMAP |
					  SNDRV_PCM_INFO_INTERLEAVED |
					  SNDRV_PCM_INFO_BLOCK_TRANSFER |
					  SNDRV_PCM_INFO_MMAP_VALID |
					  SNDRV_PCM_INFO_SYNC_START),
				 .formats = SNDRV_PCM_FMTBIT_S24_3LE,
				 .rates = SNDRV_PCM_RATE_48000,
				 .rate_min = 48000,
				 .rate_max = 48000,
				 .channels_min = 2,
				 .channels_max = 2,
				 .buffer_bytes_max = 60000,
				 .period_bytes_min = 64,
				 .period_bytes_max = 8192,
				 .periods_min = 1,
				 .periods_max = 1024},
	.rates = {
			    .nrats = 1,
			    .rats = &podhd_ratden},
	.bytes_per_channel = 3 /* SNDRV_PCM_FMTBIT_S24_3LE */
};

static struct line6_pcm_properties podx3_pcm_properties = {
	.playback_hw = {
				  .info = (SNDRV_PCM_INFO_MMAP |
					   SNDRV_PCM_INFO_INTERLEAVED |
					   SNDRV_PCM_INFO_BLOCK_TRANSFER |
					   SNDRV_PCM_INFO_MMAP_VALID |
					   SNDRV_PCM_INFO_PAUSE |
					   SNDRV_PCM_INFO_SYNC_START),
				  .formats = SNDRV_PCM_FMTBIT_S24_3LE,
				  .rates = SNDRV_PCM_RATE_48000,
				  .rate_min = 48000,
				  .rate_max = 48000,
				  .channels_min = 2,
				  .channels_max = 2,
				  .buffer_bytes_max = 60000,
				  .period_bytes_min = 64,
				  .period_bytes_max = 8192,
				  .periods_min = 1,
				  .periods_max = 1024},
	.capture_hw = {
				 .info = (SNDRV_PCM_INFO_MMAP |
					  SNDRV_PCM_INFO_INTERLEAVED |
					  SNDRV_PCM_INFO_BLOCK_TRANSFER |
					  SNDRV_PCM_INFO_MMAP_VALID |
					  SNDRV_PCM_INFO_SYNC_START),
				 .formats = SNDRV_PCM_FMTBIT_S24_3LE,
				 .rates = SNDRV_PCM_RATE_48000,
				 .rate_min = 48000,
				 .rate_max = 48000,
				 /* 1+2: Main signal (out), 3+4: Tone 1,
				  * 5+6: Tone 2, 7+8: raw
				  */
				 .channels_min = 8,
				 .channels_max = 8,
				 .buffer_bytes_max = 60000,
				 .period_bytes_min = 64,
				 .period_bytes_max = 8192,
				 .periods_min = 1,
				 .periods_max = 1024},
	.rates = {
			    .nrats = 1,
			    .rats = &podhd_ratden},
	.bytes_per_channel = 3 /* SNDRV_PCM_FMTBIT_S24_3LE */
};
static struct usb_driver podhd_driver;

static ssize_t serial_number_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct snd_card *card = dev_to_snd_card(dev);
	struct usb_line6_podhd *pod = card->private_data;

	return sprintf(buf, "%u\n", pod->serial_number);
}

static ssize_t firmware_version_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct snd_card *card = dev_to_snd_card(dev);
	struct usb_line6_podhd *pod = card->private_data;

	return sprintf(buf, "%06x\n", pod->firmware_version);
}

static DEVICE_ATTR_RO(firmware_version);
static DEVICE_ATTR_RO(serial_number);

static struct attribute *podhd_dev_attrs[] = {
	&dev_attr_firmware_version.attr,
	&dev_attr_serial_number.attr,
	NULL
};

static const struct attribute_group podhd_dev_attr_group = {
	.name = "podhd",
	.attrs = podhd_dev_attrs,
};

/*
 * POD X3 startup procedure.
 *
 * May be compatible with other POD HD's, since it's also similar to the
 * previous POD setup. In any case, it doesn't seem to be required for the
 * audio nor bulk interfaces to work.
 */

static int podhd_dev_start(struct usb_line6_podhd *pod)
{
	int ret;
	u8 *init_bytes;
	int i;
	struct usb_device *usbdev = pod->line6.usbdev;

	init_bytes = kmalloc(8, GFP_KERNEL);
	if (!init_bytes)
		return -ENOMEM;

	ret = usb_control_msg(usbdev, usb_sndctrlpipe(usbdev, 0),
					0x67, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_OUT,
					0x11, 0,
					NULL, 0, LINE6_TIMEOUT * HZ);
	if (ret < 0) {
		dev_err(pod->line6.ifcdev, "read request failed (error %d)\n", ret);
		goto exit;
	}

	/* NOTE: looks like some kind of ping message */
	ret = usb_control_msg(usbdev, usb_rcvctrlpipe(usbdev, 0), 0x67,
					USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
					0x11, 0x0,
					init_bytes, 3, LINE6_TIMEOUT * HZ);
	if (ret < 0) {
		dev_err(pod->line6.ifcdev,
			"receive length failed (error %d)\n", ret);
		goto exit;
	}

	pod->firmware_version =
		(init_bytes[0] << 16) | (init_bytes[1] << 8) | (init_bytes[2] << 0);

	for (i = 0; i <= 16; i++) {
		ret = line6_read_data(&pod->line6, 0xf000 + 0x08 * i, init_bytes, 8);
		if (ret < 0)
			goto exit;
	}

	ret = usb_control_msg(usbdev, usb_sndctrlpipe(usbdev, 0),
					USB_REQ_SET_FEATURE,
					USB_TYPE_STANDARD | USB_RECIP_DEVICE | USB_DIR_OUT,
					1, 0,
					NULL, 0, LINE6_TIMEOUT * HZ);
exit:
	kfree(init_bytes);
	return ret;
}

static void podhd_startup(struct usb_line6 *line6)
{
	struct usb_line6_podhd *pod = line6_to_podhd(line6);

	podhd_dev_start(pod);
	line6_read_serial_number(&pod->line6, &pod->serial_number);
	if (snd_card_register(line6->card))
		dev_err(line6->ifcdev, "Failed to register POD HD card.\n");
}

static void podhd_disconnect(struct usb_line6 *line6)
{
	struct usb_line6_podhd *pod = line6_to_podhd(line6);

	if (pod->line6.properties->capabilities & LINE6_CAP_CONTROL_INFO) {
		struct usb_interface *intf;

		intf = usb_ifnum_to_if(line6->usbdev,
					pod->line6.properties->ctrl_if);
		if (intf)
			usb_driver_release_interface(&podhd_driver, intf);
	}
}

/*
	Try to init POD HD device.
*/
static int podhd_init(struct usb_line6 *line6,
		      const struct usb_device_id *id)
{
	int err;
	struct usb_line6_podhd *pod = line6_to_podhd(line6);
	struct usb_interface *intf;

	line6->disconnect = podhd_disconnect;
	line6->startup = podhd_startup;

	if (pod->line6.properties->capabilities & LINE6_CAP_CONTROL) {
		/* claim the data interface */
		intf = usb_ifnum_to_if(line6->usbdev,
					pod->line6.properties->ctrl_if);
		if (!intf) {
			dev_err(pod->line6.ifcdev, "interface %d not found\n",
				pod->line6.properties->ctrl_if);
			return -ENODEV;
		}

		err = usb_driver_claim_interface(&podhd_driver, intf, NULL);
		if (err != 0) {
			dev_err(pod->line6.ifcdev, "can't claim interface %d, error %d\n",
				pod->line6.properties->ctrl_if, err);
			return err;
		}
	}

	if (pod->line6.properties->capabilities & LINE6_CAP_CONTROL_INFO) {
		/* create sysfs entries: */
		err = snd_card_add_dev_attr(line6->card, &podhd_dev_attr_group);
		if (err < 0)
			return err;
	}

	if (pod->line6.properties->capabilities & LINE6_CAP_PCM) {
		/* initialize PCM subsystem: */
		err = line6_init_pcm(line6,
			(id->driver_info == LINE6_PODX3 ||
			id->driver_info == LINE6_PODX3LIVE) ? &podx3_pcm_properties :
			&podhd_pcm_properties);
		if (err < 0)
			return err;
	}

	if (!(pod->line6.properties->capabilities & LINE6_CAP_CONTROL_INFO)) {
		/* register USB audio system directly */
		return snd_card_register(line6->card);
	}

	/* init device and delay registering */
	schedule_delayed_work(&line6->startup_work,
			      msecs_to_jiffies(PODHD_STARTUP_DELAY));
	return 0;
}

#define LINE6_DEVICE(prod) USB_DEVICE(0x0e41, prod)
#define LINE6_IF_NUM(prod, n) USB_DEVICE_INTERFACE_NUMBER(0x0e41, prod, n)

/* table of devices that work with this driver */
static const struct usb_device_id podhd_id_table[] = {
	/* TODO: no need to alloc data interfaces when only audio is used */
	{ LINE6_DEVICE(0x5057),    .driver_info = LINE6_PODHD300 },
	{ LINE6_DEVICE(0x5058),    .driver_info = LINE6_PODHD400 },
	{ LINE6_IF_NUM(0x414D, 0), .driver_info = LINE6_PODHD500 },
	{ LINE6_IF_NUM(0x414A, 0), .driver_info = LINE6_PODX3 },
	{ LINE6_IF_NUM(0x414B, 0), .driver_info = LINE6_PODX3LIVE },
	{ LINE6_IF_NUM(0x4159, 0), .driver_info = LINE6_PODHD500X },
	{ LINE6_IF_NUM(0x4156, 0), .driver_info = LINE6_PODHDDESKTOP },
	{}
};

MODULE_DEVICE_TABLE(usb, podhd_id_table);

static const struct line6_properties podhd_properties_table[] = {
	[LINE6_PODHD300] = {
		.id = "PODHD300",
		.name = "POD HD300",
		.capabilities	= LINE6_CAP_PCM
				| LINE6_CAP_HWMON,
		.altsetting = 5,
		.ep_ctrl_r = 0x84,
		.ep_ctrl_w = 0x03,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01,
	},
	[LINE6_PODHD400] = {
		.id = "PODHD400",
		.name = "POD HD400",
		.capabilities	= LINE6_CAP_PCM
				| LINE6_CAP_HWMON,
		.altsetting = 5,
		.ep_ctrl_r = 0x84,
		.ep_ctrl_w = 0x03,
		.ep_audio_r = 0x82,
		.ep_audio_w = 0x01,
	},
	[LINE6_PODHD500] = {
		.id = "PODHD500",
		.name = "POD HD500",
		.capabilities	= LINE6_CAP_PCM | LINE6_CAP_CONTROL
				| LINE6_CAP_HWMON,
		.altsetting = 1,
		.ctrl_if = 1,
		.ep_ctrl_r = 0x81,
		.ep_ctrl_w = 0x01,
		.ep_audio_r = 0x86,
		.ep_audio_w = 0x02,
	},
	[LINE6_PODX3] = {
		.id = "PODX3",
		.name = "POD X3",
		.capabilities	= LINE6_CAP_CONTROL | LINE6_CAP_CONTROL_INFO
				| LINE6_CAP_PCM | LINE6_CAP_HWMON | LINE6_CAP_IN_NEEDS_OUT,
		.altsetting = 1,
		.ep_ctrl_r = 0x81,
		.ep_ctrl_w = 0x01,
		.ctrl_if = 1,
		.ep_audio_r = 0x86,
		.ep_audio_w = 0x02,
	},
	[LINE6_PODX3LIVE] = {
		.id = "PODX3LIVE",
		.name = "POD X3 LIVE",
		.capabilities	= LINE6_CAP_CONTROL | LINE6_CAP_CONTROL_INFO
				| LINE6_CAP_PCM | LINE6_CAP_HWMON | LINE6_CAP_IN_NEEDS_OUT,
		.altsetting = 1,
		.ep_ctrl_r = 0x81,
		.ep_ctrl_w = 0x01,
		.ctrl_if = 1,
		.ep_audio_r = 0x86,
		.ep_audio_w = 0x02,
	},
	[LINE6_PODHD500X] = {
		.id = "PODHD500X",
		.name = "POD HD500X",
		.capabilities	= LINE6_CAP_CONTROL
				| LINE6_CAP_PCM | LINE6_CAP_HWMON,
		.altsetting = 1,
		.ep_ctrl_r = 0x81,
		.ep_ctrl_w = 0x01,
		.ctrl_if = 1,
		.ep_audio_r = 0x86,
		.ep_audio_w = 0x02,
	},
	[LINE6_PODHDDESKTOP] = {
		.id = "PODHDDESKTOP",
		.name = "POD HDDESKTOP",
		.capabilities    = LINE6_CAP_CONTROL
			| LINE6_CAP_PCM | LINE6_CAP_HWMON,
		.altsetting = 1,
		.ep_ctrl_r = 0x81,
		.ep_ctrl_w = 0x01,
		.ctrl_if = 1,
		.ep_audio_r = 0x86,
		.ep_audio_w = 0x02,
	},
};

/*
	Probe USB device.
*/
static int podhd_probe(struct usb_interface *interface,
		       const struct usb_device_id *id)
{
	return line6_probe(interface, id, "Line6-PODHD",
			   &podhd_properties_table[id->driver_info],
			   podhd_init, sizeof(struct usb_line6_podhd));
}

static struct usb_driver podhd_driver = {
	.name = KBUILD_MODNAME,
	.probe = podhd_probe,
	.disconnect = line6_disconnect,
#ifdef CONFIG_PM
	.suspend = line6_suspend,
	.resume = line6_resume,
	.reset_resume = line6_resume,
#endif
	.id_table = podhd_id_table,
};

module_usb_driver(podhd_driver);

MODULE_DESCRIPTION("Line 6 PODHD USB driver");
MODULE_LICENSE("GPL");
