// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
 * Copyright (C) 2017 Linaro Ltd.
 */
#include <linux/types.h>
#include <media/v4l2-ctrls.h>

#include "core.h"
#include "helpers.h"
#include "vdec.h"

static int vdec_op_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct venus_inst *inst = ctrl_to_inst(ctrl);
	struct vdec_controls *ctr = &inst->controls.dec;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_DECODER_MPEG4_DEBLOCK_FILTER:
		ctr->post_loop_deb_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
	case V4L2_CID_MPEG_VIDEO_MPEG4_PROFILE:
	case V4L2_CID_MPEG_VIDEO_VP8_PROFILE:
		ctr->profile = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
	case V4L2_CID_MPEG_VIDEO_MPEG4_LEVEL:
		ctr->level = ctrl->val;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int vdec_op_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct venus_inst *inst = ctrl_to_inst(ctrl);
	struct vdec_controls *ctr = &inst->controls.dec;
	struct hfi_buffer_requirements bufreq;
	union hfi_get_property hprop;
	enum hfi_version ver = inst->core->res->hfi_version;
	u32 ptype = HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
	case V4L2_CID_MPEG_VIDEO_MPEG4_PROFILE:
	case V4L2_CID_MPEG_VIDEO_VP8_PROFILE:
		ret = hfi_session_get_property(inst, ptype, &hprop);
		if (!ret)
			ctr->profile = hprop.profile_level.profile;
		ctrl->val = ctr->profile;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
	case V4L2_CID_MPEG_VIDEO_MPEG4_LEVEL:
		ret = hfi_session_get_property(inst, ptype, &hprop);
		if (!ret)
			ctr->level = hprop.profile_level.level;
		ctrl->val = ctr->level;
		break;
	case V4L2_CID_MPEG_VIDEO_DECODER_MPEG4_DEBLOCK_FILTER:
		ctrl->val = ctr->post_loop_deb_mode;
		break;
	case V4L2_CID_MIN_BUFFERS_FOR_CAPTURE:
		ret = venus_helper_get_bufreq(inst, HFI_BUFFER_OUTPUT, &bufreq);
		if (!ret)
			ctrl->val = HFI_BUFREQ_COUNT_MIN(&bufreq, ver);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops vdec_ctrl_ops = {
	.s_ctrl = vdec_op_s_ctrl,
	.g_volatile_ctrl = vdec_op_g_volatile_ctrl,
};

int vdec_ctrl_init(struct venus_inst *inst)
{
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_ctrl_handler_init(&inst->ctrl_handler, 7);
	if (ret)
		return ret;

	ctrl = v4l2_ctrl_new_std_menu(&inst->ctrl_handler, &vdec_ctrl_ops,
		V4L2_CID_MPEG_VIDEO_MPEG4_PROFILE,
		V4L2_MPEG_VIDEO_MPEG4_PROFILE_ADVANCED_CODING_EFFICIENCY,
		~((1 << V4L2_MPEG_VIDEO_MPEG4_PROFILE_SIMPLE) |
		  (1 << V4L2_MPEG_VIDEO_MPEG4_PROFILE_ADVANCED_SIMPLE)),
		V4L2_MPEG_VIDEO_MPEG4_PROFILE_SIMPLE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

	ctrl = v4l2_ctrl_new_std_menu(&inst->ctrl_handler, &vdec_ctrl_ops,
				      V4L2_CID_MPEG_VIDEO_MPEG4_LEVEL,
				      V4L2_MPEG_VIDEO_MPEG4_LEVEL_5,
				      0, V4L2_MPEG_VIDEO_MPEG4_LEVEL_0);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

	ctrl = v4l2_ctrl_new_std_menu(&inst->ctrl_handler, &vdec_ctrl_ops,
		V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		V4L2_MPEG_VIDEO_H264_PROFILE_MULTIVIEW_HIGH,
		~((1 << V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
		  (1 << V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) |
		  (1 << V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
		  (1 << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH) |
		  (1 << V4L2_MPEG_VIDEO_H264_PROFILE_STEREO_HIGH) |
		  (1 << V4L2_MPEG_VIDEO_H264_PROFILE_MULTIVIEW_HIGH)),
		V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

	ctrl = v4l2_ctrl_new_std_menu(&inst->ctrl_handler, &vdec_ctrl_ops,
				      V4L2_CID_MPEG_VIDEO_H264_LEVEL,
				      V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
				      0, V4L2_MPEG_VIDEO_H264_LEVEL_1_0);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

	ctrl = v4l2_ctrl_new_std_menu(&inst->ctrl_handler, &vdec_ctrl_ops,
				      V4L2_CID_MPEG_VIDEO_VP8_PROFILE,
				      V4L2_MPEG_VIDEO_VP8_PROFILE_3,
				      0, V4L2_MPEG_VIDEO_VP8_PROFILE_0);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

	v4l2_ctrl_new_std(&inst->ctrl_handler, &vdec_ctrl_ops,
		V4L2_CID_MPEG_VIDEO_DECODER_MPEG4_DEBLOCK_FILTER, 0, 1, 1, 0);

	ctrl = v4l2_ctrl_new_std(&inst->ctrl_handler, &vdec_ctrl_ops,
		V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 1, 32, 1, 1);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;

	ret = inst->ctrl_handler.error;
	if (ret) {
		v4l2_ctrl_handler_free(&inst->ctrl_handler);
		return ret;
	}

	return 0;
}

void vdec_ctrl_deinit(struct venus_inst *inst)
{
	v4l2_ctrl_handler_free(&inst->ctrl_handler);
}
