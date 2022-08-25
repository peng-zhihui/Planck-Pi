// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * vimc-sensor.c Virtual Media Controller Driver
 *
 * Copyright (C) 2015-2017 Helen Koike <helen.fornazier@gmail.com>
 */

#include <linux/component.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/v4l2-mediabus.h>
#include <linux/vmalloc.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-subdev.h>
#include <media/tpg/v4l2-tpg.h>

#include "vimc-common.h"

#define VIMC_SEN_DRV_NAME "vimc-sensor"

struct vimc_sen_device {
	struct vimc_ent_device ved;
	struct v4l2_subdev sd;
	struct device *dev;
	struct tpg_data tpg;
	u8 *frame;
	/* The active format */
	struct v4l2_mbus_framefmt mbus_format;
	struct v4l2_ctrl_handler hdl;
};

static const struct v4l2_mbus_framefmt fmt_default = {
	.width = 640,
	.height = 480,
	.code = MEDIA_BUS_FMT_RGB888_1X24,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_DEFAULT,
};

static int vimc_sen_init_cfg(struct v4l2_subdev *sd,
			     struct v4l2_subdev_pad_config *cfg)
{
	unsigned int i;

	for (i = 0; i < sd->entity.num_pads; i++) {
		struct v4l2_mbus_framefmt *mf;

		mf = v4l2_subdev_get_try_format(sd, cfg, i);
		*mf = fmt_default;
	}

	return 0;
}

static int vimc_sen_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	const struct vimc_pix_map *vpix = vimc_pix_map_by_index(code->index);

	if (!vpix)
		return -EINVAL;

	code->code = vpix->code;

	return 0;
}

static int vimc_sen_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	const struct vimc_pix_map *vpix;

	if (fse->index)
		return -EINVAL;

	/* Only accept code in the pix map table */
	vpix = vimc_pix_map_by_code(fse->code);
	if (!vpix)
		return -EINVAL;

	fse->min_width = VIMC_FRAME_MIN_WIDTH;
	fse->max_width = VIMC_FRAME_MAX_WIDTH;
	fse->min_height = VIMC_FRAME_MIN_HEIGHT;
	fse->max_height = VIMC_FRAME_MAX_HEIGHT;

	return 0;
}

static int vimc_sen_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *fmt)
{
	struct vimc_sen_device *vsen =
				container_of(sd, struct vimc_sen_device, sd);

	fmt->format = fmt->which == V4L2_SUBDEV_FORMAT_TRY ?
		      *v4l2_subdev_get_try_format(sd, cfg, fmt->pad) :
		      vsen->mbus_format;

	return 0;
}

static void vimc_sen_tpg_s_format(struct vimc_sen_device *vsen)
{
	const struct vimc_pix_map *vpix =
				vimc_pix_map_by_code(vsen->mbus_format.code);

	tpg_reset_source(&vsen->tpg, vsen->mbus_format.width,
			 vsen->mbus_format.height, vsen->mbus_format.field);
	tpg_s_bytesperline(&vsen->tpg, 0, vsen->mbus_format.width * vpix->bpp);
	tpg_s_buf_height(&vsen->tpg, vsen->mbus_format.height);
	tpg_s_fourcc(&vsen->tpg, vpix->pixelformat);
	/* TODO: add support for V4L2_FIELD_ALTERNATE */
	tpg_s_field(&vsen->tpg, vsen->mbus_format.field, false);
	tpg_s_colorspace(&vsen->tpg, vsen->mbus_format.colorspace);
	tpg_s_ycbcr_enc(&vsen->tpg, vsen->mbus_format.ycbcr_enc);
	tpg_s_quantization(&vsen->tpg, vsen->mbus_format.quantization);
	tpg_s_xfer_func(&vsen->tpg, vsen->mbus_format.xfer_func);
}

static void vimc_sen_adjust_fmt(struct v4l2_mbus_framefmt *fmt)
{
	const struct vimc_pix_map *vpix;

	/* Only accept code in the pix map table */
	vpix = vimc_pix_map_by_code(fmt->code);
	if (!vpix)
		fmt->code = fmt_default.code;

	fmt->width = clamp_t(u32, fmt->width, VIMC_FRAME_MIN_WIDTH,
			     VIMC_FRAME_MAX_WIDTH) & ~1;
	fmt->height = clamp_t(u32, fmt->height, VIMC_FRAME_MIN_HEIGHT,
			      VIMC_FRAME_MAX_HEIGHT) & ~1;

	/* TODO: add support for V4L2_FIELD_ALTERNATE */
	if (fmt->field == V4L2_FIELD_ANY || fmt->field == V4L2_FIELD_ALTERNATE)
		fmt->field = fmt_default.field;

	vimc_colorimetry_clamp(fmt);
}

static int vimc_sen_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_pad_config *cfg,
			    struct v4l2_subdev_format *fmt)
{
	struct vimc_sen_device *vsen = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *mf;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		/* Do not change the format while stream is on */
		if (vsen->frame)
			return -EBUSY;

		mf = &vsen->mbus_format;
	} else {
		mf = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
	}

	/* Set the new format */
	vimc_sen_adjust_fmt(&fmt->format);

	dev_dbg(vsen->dev, "%s: format update: "
		"old:%dx%d (0x%x, %d, %d, %d, %d) "
		"new:%dx%d (0x%x, %d, %d, %d, %d)\n", vsen->sd.name,
		/* old */
		mf->width, mf->height, mf->code,
		mf->colorspace,	mf->quantization,
		mf->xfer_func, mf->ycbcr_enc,
		/* new */
		fmt->format.width, fmt->format.height, fmt->format.code,
		fmt->format.colorspace, fmt->format.quantization,
		fmt->format.xfer_func, fmt->format.ycbcr_enc);

	*mf = fmt->format;

	return 0;
}

static const struct v4l2_subdev_pad_ops vimc_sen_pad_ops = {
	.init_cfg		= vimc_sen_init_cfg,
	.enum_mbus_code		= vimc_sen_enum_mbus_code,
	.enum_frame_size	= vimc_sen_enum_frame_size,
	.get_fmt		= vimc_sen_get_fmt,
	.set_fmt		= vimc_sen_set_fmt,
};

static void *vimc_sen_process_frame(struct vimc_ent_device *ved,
				    const void *sink_frame)
{
	struct vimc_sen_device *vsen = container_of(ved, struct vimc_sen_device,
						    ved);

	tpg_fill_plane_buffer(&vsen->tpg, 0, 0, vsen->frame);
	return vsen->frame;
}

static int vimc_sen_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vimc_sen_device *vsen =
				container_of(sd, struct vimc_sen_device, sd);

	if (enable) {
		const struct vimc_pix_map *vpix;
		unsigned int frame_size;

		/* Calculate the frame size */
		vpix = vimc_pix_map_by_code(vsen->mbus_format.code);
		frame_size = vsen->mbus_format.width * vpix->bpp *
			     vsen->mbus_format.height;

		/*
		 * Allocate the frame buffer. Use vmalloc to be able to
		 * allocate a large amount of memory
		 */
		vsen->frame = vmalloc(frame_size);
		if (!vsen->frame)
			return -ENOMEM;

		/* configure the test pattern generator */
		vimc_sen_tpg_s_format(vsen);

	} else {

		vfree(vsen->frame);
		vsen->frame = NULL;
	}

	return 0;
}

static const struct v4l2_subdev_core_ops vimc_sen_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops vimc_sen_video_ops = {
	.s_stream = vimc_sen_s_stream,
};

static const struct v4l2_subdev_ops vimc_sen_ops = {
	.core = &vimc_sen_core_ops,
	.pad = &vimc_sen_pad_ops,
	.video = &vimc_sen_video_ops,
};

static int vimc_sen_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vimc_sen_device *vsen =
		container_of(ctrl->handler, struct vimc_sen_device, hdl);

	switch (ctrl->id) {
	case VIMC_CID_TEST_PATTERN:
		tpg_s_pattern(&vsen->tpg, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		tpg_s_hflip(&vsen->tpg, ctrl->val);
		break;
	case V4L2_CID_VFLIP:
		tpg_s_vflip(&vsen->tpg, ctrl->val);
		break;
	case V4L2_CID_BRIGHTNESS:
		tpg_s_brightness(&vsen->tpg, ctrl->val);
		break;
	case V4L2_CID_CONTRAST:
		tpg_s_contrast(&vsen->tpg, ctrl->val);
		break;
	case V4L2_CID_HUE:
		tpg_s_hue(&vsen->tpg, ctrl->val);
		break;
	case V4L2_CID_SATURATION:
		tpg_s_saturation(&vsen->tpg, ctrl->val);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct v4l2_ctrl_ops vimc_sen_ctrl_ops = {
	.s_ctrl = vimc_sen_s_ctrl,
};

static void vimc_sen_release(struct v4l2_subdev *sd)
{
	struct vimc_sen_device *vsen =
				container_of(sd, struct vimc_sen_device, sd);

	v4l2_ctrl_handler_free(&vsen->hdl);
	tpg_free(&vsen->tpg);
	vimc_pads_cleanup(vsen->ved.pads);
	kfree(vsen);
}

static const struct v4l2_subdev_internal_ops vimc_sen_int_ops = {
	.release = vimc_sen_release,
};

static void vimc_sen_comp_unbind(struct device *comp, struct device *master,
				 void *master_data)
{
	struct vimc_ent_device *ved = dev_get_drvdata(comp);
	struct vimc_sen_device *vsen =
				container_of(ved, struct vimc_sen_device, ved);

	vimc_ent_sd_unregister(ved, &vsen->sd);
}

/* Image Processing Controls */
static const struct v4l2_ctrl_config vimc_sen_ctrl_class = {
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_WRITE_ONLY,
	.id = VIMC_CID_VIMC_CLASS,
	.name = "VIMC Controls",
	.type = V4L2_CTRL_TYPE_CTRL_CLASS,
};

static const struct v4l2_ctrl_config vimc_sen_ctrl_test_pattern = {
	.ops = &vimc_sen_ctrl_ops,
	.id = VIMC_CID_TEST_PATTERN,
	.name = "Test Pattern",
	.type = V4L2_CTRL_TYPE_MENU,
	.max = TPG_PAT_NOISE,
	.qmenu = tpg_pattern_strings,
};

static int vimc_sen_comp_bind(struct device *comp, struct device *master,
			      void *master_data)
{
	struct v4l2_device *v4l2_dev = master_data;
	struct vimc_platform_data *pdata = comp->platform_data;
	struct vimc_sen_device *vsen;
	int ret;

	/* Allocate the vsen struct */
	vsen = kzalloc(sizeof(*vsen), GFP_KERNEL);
	if (!vsen)
		return -ENOMEM;

	v4l2_ctrl_handler_init(&vsen->hdl, 4);

	v4l2_ctrl_new_custom(&vsen->hdl, &vimc_sen_ctrl_class, NULL);
	v4l2_ctrl_new_custom(&vsen->hdl, &vimc_sen_ctrl_test_pattern, NULL);
	v4l2_ctrl_new_std(&vsen->hdl, &vimc_sen_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&vsen->hdl, &vimc_sen_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&vsen->hdl, &vimc_sen_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, 0, 255, 1, 128);
	v4l2_ctrl_new_std(&vsen->hdl, &vimc_sen_ctrl_ops,
			  V4L2_CID_CONTRAST, 0, 255, 1, 128);
	v4l2_ctrl_new_std(&vsen->hdl, &vimc_sen_ctrl_ops,
			  V4L2_CID_HUE, -128, 127, 1, 0);
	v4l2_ctrl_new_std(&vsen->hdl, &vimc_sen_ctrl_ops,
			  V4L2_CID_SATURATION, 0, 255, 1, 128);
	vsen->sd.ctrl_handler = &vsen->hdl;
	if (vsen->hdl.error) {
		ret = vsen->hdl.error;
		goto err_free_vsen;
	}

	/* Initialize ved and sd */
	ret = vimc_ent_sd_register(&vsen->ved, &vsen->sd, v4l2_dev,
				   pdata->entity_name,
				   MEDIA_ENT_F_CAM_SENSOR, 1,
				   (const unsigned long[1]) {MEDIA_PAD_FL_SOURCE},
				   &vimc_sen_int_ops, &vimc_sen_ops);
	if (ret)
		goto err_free_hdl;

	vsen->ved.process_frame = vimc_sen_process_frame;
	dev_set_drvdata(comp, &vsen->ved);
	vsen->dev = comp;

	/* Initialize the frame format */
	vsen->mbus_format = fmt_default;

	/* Initialize the test pattern generator */
	tpg_init(&vsen->tpg, vsen->mbus_format.width,
		 vsen->mbus_format.height);
	ret = tpg_alloc(&vsen->tpg, VIMC_FRAME_MAX_WIDTH);
	if (ret)
		goto err_unregister_ent_sd;

	return 0;

err_unregister_ent_sd:
	vimc_ent_sd_unregister(&vsen->ved,  &vsen->sd);
err_free_hdl:
	v4l2_ctrl_handler_free(&vsen->hdl);
err_free_vsen:
	kfree(vsen);

	return ret;
}

static const struct component_ops vimc_sen_comp_ops = {
	.bind = vimc_sen_comp_bind,
	.unbind = vimc_sen_comp_unbind,
};

static int vimc_sen_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vimc_sen_comp_ops);
}

static int vimc_sen_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vimc_sen_comp_ops);

	return 0;
}

static const struct platform_device_id vimc_sen_driver_ids[] = {
	{
		.name           = VIMC_SEN_DRV_NAME,
	},
	{ }
};

static struct platform_driver vimc_sen_pdrv = {
	.probe		= vimc_sen_probe,
	.remove		= vimc_sen_remove,
	.id_table	= vimc_sen_driver_ids,
	.driver		= {
		.name	= VIMC_SEN_DRV_NAME,
	},
};

module_platform_driver(vimc_sen_pdrv);

MODULE_DEVICE_TABLE(platform, vimc_sen_driver_ids);

MODULE_DESCRIPTION("Virtual Media Controller Driver (VIMC) Sensor");
MODULE_AUTHOR("Helen Mae Koike Fornazier <helen.fornazier@gmail.com>");
MODULE_LICENSE("GPL");
