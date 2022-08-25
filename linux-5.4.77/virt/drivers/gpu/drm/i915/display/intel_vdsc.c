// SPDX-License-Identifier: MIT
/*
 * Copyright © 2018 Intel Corporation
 *
 * Author: Gaurav K Singh <gaurav.k.singh@intel.com>
 *         Manasi Navare <manasi.d.navare@intel.com>
 */

#include <drm/i915_drm.h>

#include "i915_drv.h"
#include "intel_display_types.h"
#include "intel_vdsc.h"

enum ROW_INDEX_BPP {
	ROW_INDEX_6BPP = 0,
	ROW_INDEX_8BPP,
	ROW_INDEX_10BPP,
	ROW_INDEX_12BPP,
	ROW_INDEX_15BPP,
	MAX_ROW_INDEX
};

enum COLUMN_INDEX_BPC {
	COLUMN_INDEX_8BPC = 0,
	COLUMN_INDEX_10BPC,
	COLUMN_INDEX_12BPC,
	COLUMN_INDEX_14BPC,
	COLUMN_INDEX_16BPC,
	MAX_COLUMN_INDEX
};

#define DSC_SUPPORTED_VERSION_MIN		1

/* From DSC_v1.11 spec, rc_parameter_Set syntax element typically constant */
static u16 rc_buf_thresh[] = {
	896, 1792, 2688, 3584, 4480, 5376, 6272, 6720, 7168, 7616,
	7744, 7872, 8000, 8064
};

struct rc_parameters {
	u16 initial_xmit_delay;
	u8 first_line_bpg_offset;
	u16 initial_offset;
	u8 flatness_min_qp;
	u8 flatness_max_qp;
	u8 rc_quant_incr_limit0;
	u8 rc_quant_incr_limit1;
	struct drm_dsc_rc_range_parameters rc_range_params[DSC_NUM_BUF_RANGES];
};

/*
 * Selected Rate Control Related Parameter Recommended Values
 * from DSC_v1.11 spec & C Model release: DSC_model_20161212
 */
static struct rc_parameters rc_params[][MAX_COLUMN_INDEX] = {
{
	/* 6BPP/8BPC */
	{ 768, 15, 6144, 3, 13, 11, 11, {
		{ 0, 4, 0 }, { 1, 6, -2 }, { 3, 8, -2 }, { 4, 8, -4 },
		{ 5, 9, -6 }, { 5, 9, -6 }, { 6, 9, -6 }, { 6, 10, -8 },
		{ 7, 11, -8 }, { 8, 12, -10 }, { 9, 12, -10 }, { 10, 12, -12 },
		{ 10, 12, -12 }, { 11, 12, -12 }, { 13, 14, -12 }
		}
	},
	/* 6BPP/10BPC */
	{ 768, 15, 6144, 7, 17, 15, 15, {
		{ 0, 8, 0 }, { 3, 10, -2 }, { 7, 12, -2 }, { 8, 12, -4 },
		{ 9, 13, -6 }, { 9, 13, -6 }, { 10, 13, -6 }, { 10, 14, -8 },
		{ 11, 15, -8 }, { 12, 16, -10 }, { 13, 16, -10 },
		{ 14, 16, -12 }, { 14, 16, -12 }, { 15, 16, -12 },
		{ 17, 18, -12 }
		}
	},
	/* 6BPP/12BPC */
	{ 768, 15, 6144, 11, 21, 19, 19, {
		{ 0, 12, 0 }, { 5, 14, -2 }, { 11, 16, -2 }, { 12, 16, -4 },
		{ 13, 17, -6 }, { 13, 17, -6 }, { 14, 17, -6 }, { 14, 18, -8 },
		{ 15, 19, -8 }, { 16, 20, -10 }, { 17, 20, -10 },
		{ 18, 20, -12 }, { 18, 20, -12 }, { 19, 20, -12 },
		{ 21, 22, -12 }
		}
	},
	/* 6BPP/14BPC */
	{ 768, 15, 6144, 15, 25, 23, 27, {
		{ 0, 16, 0 }, { 7, 18, -2 }, { 15, 20, -2 }, { 16, 20, -4 },
		{ 17, 21, -6 }, { 17, 21, -6 }, { 18, 21, -6 }, { 18, 22, -8 },
		{ 19, 23, -8 }, { 20, 24, -10 }, { 21, 24, -10 },
		{ 22, 24, -12 }, { 22, 24, -12 }, { 23, 24, -12 },
		{ 25, 26, -12 }
		}
	},
	/* 6BPP/16BPC */
	{ 768, 15, 6144, 19, 29, 27, 27, {
		{ 0, 20, 0 }, { 9, 22, -2 }, { 19, 24, -2 }, { 20, 24, -4 },
		{ 21, 25, -6 }, { 21, 25, -6 }, { 22, 25, -6 }, { 22, 26, -8 },
		{ 23, 27, -8 }, { 24, 28, -10 }, { 25, 28, -10 },
		{ 26, 28, -12 }, { 26, 28, -12 }, { 27, 28, -12 },
		{ 29, 30, -12 }
		}
	},
},
{
	/* 8BPP/8BPC */
	{ 512, 12, 6144, 3, 12, 11, 11, {
		{ 0, 4, 2 }, { 0, 4, 0 }, { 1, 5, 0 }, { 1, 6, -2 },
		{ 3, 7, -4 }, { 3, 7, -6 }, { 3, 7, -8 }, { 3, 8, -8 },
		{ 3, 9, -8 }, { 3, 10, -10 }, { 5, 11, -10 }, { 5, 12, -12 },
		{ 5, 13, -12 }, { 7, 13, -12 }, { 13, 15, -12 }
		}
	},
	/* 8BPP/10BPC */
	{ 512, 12, 6144, 7, 16, 15, 15, {
		{ 0, 4, 2 }, { 4, 8, 0 }, { 5, 9, 0 }, { 5, 10, -2 },
		{ 7, 11, -4 }, { 7, 11, -6 }, { 7, 11, -8 }, { 7, 12, -8 },
		{ 7, 13, -8 }, { 7, 14, -10 }, { 9, 15, -10 }, { 9, 16, -12 },
		{ 9, 17, -12 }, { 11, 17, -12 }, { 17, 19, -12 }
		}
	},
	/* 8BPP/12BPC */
	{ 512, 12, 6144, 11, 20, 19, 19, {
		{ 0, 12, 2 }, { 4, 12, 0 }, { 9, 13, 0 }, { 9, 14, -2 },
		{ 11, 15, -4 }, { 11, 15, -6 }, { 11, 15, -8 }, { 11, 16, -8 },
		{ 11, 17, -8 }, { 11, 18, -10 }, { 13, 19, -10 },
		{ 13, 20, -12 }, { 13, 21, -12 }, { 15, 21, -12 },
		{ 21, 23, -12 }
		}
	},
	/* 8BPP/14BPC */
	{ 512, 12, 6144, 15, 24, 23, 23, {
		{ 0, 12, 0 }, { 5, 13, 0 }, { 11, 15, 0 }, { 12, 17, -2 },
		{ 15, 19, -4 }, { 15, 19, -6 }, { 15, 19, -8 }, { 15, 20, -8 },
		{ 15, 21, -8 }, { 15, 22, -10 }, { 17, 22, -10 },
		{ 17, 23, -12 }, { 17, 23, -12 }, { 21, 24, -12 },
		{ 24, 25, -12 }
		}
	},
	/* 8BPP/16BPC */
	{ 512, 12, 6144, 19, 28, 27, 27, {
		{ 0, 12, 2 }, { 6, 14, 0 }, { 13, 17, 0 }, { 15, 20, -2 },
		{ 19, 23, -4 }, { 19, 23, -6 }, { 19, 23, -8 }, { 19, 24, -8 },
		{ 19, 25, -8 }, { 19, 26, -10 }, { 21, 26, -10 },
		{ 21, 27, -12 }, { 21, 27, -12 }, { 25, 28, -12 },
		{ 28, 29, -12 }
		}
	},
},
{
	/* 10BPP/8BPC */
	{ 410, 15, 5632, 3, 12, 11, 11, {
		{ 0, 3, 2 }, { 0, 4, 0 }, { 1, 5, 0 }, { 2, 6, -2 },
		{ 3, 7, -4 }, { 3, 7, -6 }, { 3, 7, -8 }, { 3, 8, -8 },
		{ 3, 9, -8 }, { 3, 9, -10 }, { 5, 10, -10 }, { 5, 10, -10 },
		{ 5, 11, -12 }, { 7, 11, -12 }, { 11, 12, -12 }
		}
	},
	/* 10BPP/10BPC */
	{ 410, 15, 5632, 7, 16, 15, 15, {
		{ 0, 7, 2 }, { 4, 8, 0 }, { 5, 9, 0 }, { 6, 10, -2 },
		{ 7, 11, -4 }, { 7, 11, -6 }, { 7, 11, -8 }, { 7, 12, -8 },
		{ 7, 13, -8 }, { 7, 13, -10 }, { 9, 14, -10 }, { 9, 14, -10 },
		{ 9, 15, -12 }, { 11, 15, -12 }, { 15, 16, -12 }
		}
	},
	/* 10BPP/12BPC */
	{ 410, 15, 5632, 11, 20, 19, 19, {
		{ 0, 11, 2 }, { 4, 12, 0 }, { 9, 13, 0 }, { 10, 14, -2 },
		{ 11, 15, -4 }, { 11, 15, -6 }, { 11, 15, -8 }, { 11, 16, -8 },
		{ 11, 17, -8 }, { 11, 17, -10 }, { 13, 18, -10 },
		{ 13, 18, -10 }, { 13, 19, -12 }, { 15, 19, -12 },
		{ 19, 20, -12 }
		}
	},
	/* 10BPP/14BPC */
	{ 410, 15, 5632, 15, 24, 23, 23, {
		{ 0, 11, 2 }, { 5, 13, 0 }, { 11, 15, 0 }, { 13, 18, -2 },
		{ 15, 19, -4 }, { 15, 19, -6 }, { 15, 19, -8 }, { 15, 20, -8 },
		{ 15, 21, -8 }, { 15, 21, -10 }, { 17, 22, -10 },
		{ 17, 22, -10 }, { 17, 23, -12 }, { 19, 23, -12 },
		{ 23, 24, -12 }
		}
	},
	/* 10BPP/16BPC */
	{ 410, 15, 5632, 19, 28, 27, 27, {
		{ 0, 11, 2 }, { 6, 14, 0 }, { 13, 17, 0 }, { 16, 20, -2 },
		{ 19, 23, -4 }, { 19, 23, -6 }, { 19, 23, -8 }, { 19, 24, -8 },
		{ 19, 25, -8 }, { 19, 25, -10 }, { 21, 26, -10 },
		{ 21, 26, -10 }, { 21, 27, -12 }, { 23, 27, -12 },
		{ 27, 28, -12 }
		}
	},
},
{
	/* 12BPP/8BPC */
	{ 341, 15, 2048, 3, 12, 11, 11, {
		{ 0, 2, 2 }, { 0, 4, 0 }, { 1, 5, 0 }, { 1, 6, -2 },
		{ 3, 7, -4 }, { 3, 7, -6 }, { 3, 7, -8 }, { 3, 8, -8 },
		{ 3, 9, -8 }, { 3, 10, -10 }, { 5, 11, -10 },
		{ 5, 12, -12 }, { 5, 13, -12 }, { 7, 13, -12 }, { 13, 15, -12 }
		}
	},
	/* 12BPP/10BPC */
	{ 341, 15, 2048, 7, 16, 15, 15, {
		{ 0, 2, 2 }, { 2, 5, 0 }, { 3, 7, 0 }, { 4, 8, -2 },
		{ 6, 9, -4 }, { 7, 10, -6 }, { 7, 11, -8 }, { 7, 12, -8 },
		{ 7, 13, -8 }, { 7, 14, -10 }, { 9, 15, -10 }, { 9, 16, -12 },
		{ 9, 17, -12 }, { 11, 17, -12 }, { 17, 19, -12 }
		}
	},
	/* 12BPP/12BPC */
	{ 341, 15, 2048, 11, 20, 19, 19, {
		{ 0, 6, 2 }, { 4, 9, 0 }, { 7, 11, 0 }, { 8, 12, -2 },
		{ 10, 13, -4 }, { 11, 14, -6 }, { 11, 15, -8 }, { 11, 16, -8 },
		{ 11, 17, -8 }, { 11, 18, -10 }, { 13, 19, -10 },
		{ 13, 20, -12 }, { 13, 21, -12 }, { 15, 21, -12 },
		{ 21, 23, -12 }
		}
	},
	/* 12BPP/14BPC */
	{ 341, 15, 2048, 15, 24, 23, 23, {
		{ 0, 6, 2 }, { 7, 10, 0 }, { 9, 13, 0 }, { 11, 16, -2 },
		{ 14, 17, -4 }, { 15, 18, -6 }, { 15, 19, -8 }, { 15, 20, -8 },
		{ 15, 20, -8 }, { 15, 21, -10 }, { 17, 21, -10 },
		{ 17, 21, -12 }, { 17, 21, -12 }, { 19, 22, -12 },
		{ 22, 23, -12 }
		}
	},
	/* 12BPP/16BPC */
	{ 341, 15, 2048, 19, 28, 27, 27, {
		{ 0, 6, 2 }, { 6, 11, 0 }, { 11, 15, 0 }, { 14, 18, -2 },
		{ 18, 21, -4 }, { 19, 22, -6 }, { 19, 23, -8 }, { 19, 24, -8 },
		{ 19, 24, -8 }, { 19, 25, -10 }, { 21, 25, -10 },
		{ 21, 25, -12 }, { 21, 25, -12 }, { 23, 26, -12 },
		{ 26, 27, -12 }
		}
	},
},
{
	/* 15BPP/8BPC */
	{ 273, 15, 2048, 3, 12, 11, 11, {
		{ 0, 0, 10 }, { 0, 1, 8 }, { 0, 1, 6 }, { 0, 2, 4 },
		{ 1, 2, 2 }, { 1, 3, 0 }, { 1, 3, -2 }, { 2, 4, -4 },
		{ 2, 5, -6 }, { 3, 5, -8 }, { 4, 6, -10 }, { 4, 7, -10 },
		{ 5, 7, -12 }, { 7, 8, -12 }, { 8, 9, -12 }
		}
	},
	/* 15BPP/10BPC */
	{ 273, 15, 2048, 7, 16, 15, 15, {
		{ 0, 2, 10 }, { 2, 5, 8 }, { 3, 5, 6 }, { 4, 6, 4 },
		{ 5, 6, 2 }, { 5, 7, 0 }, { 5, 7, -2 }, { 6, 8, -4 },
		{ 6, 9, -6 }, { 7, 9, -8 }, { 8, 10, -10 }, { 8, 11, -10 },
		{ 9, 11, -12 }, { 11, 12, -12 }, { 12, 13, -12 }
		}
	},
	/* 15BPP/12BPC */
	{ 273, 15, 2048, 11, 20, 19, 19, {
		{ 0, 4, 10 }, { 2, 7, 8 }, { 4, 9, 6 }, { 6, 11, 4 },
		{ 9, 11, 2 }, { 9, 11, 0 }, { 9, 12, -2 }, { 10, 12, -4 },
		{ 11, 13, -6 }, { 11, 13, -8 }, { 12, 14, -10 },
		{ 13, 15, -10 }, { 13, 15, -12 }, { 15, 16, -12 },
		{ 16, 17, -12 }
		}
	},
	/* 15BPP/14BPC */
	{ 273, 15, 2048, 15, 24, 23, 23, {
		{ 0, 4, 10 }, { 3, 8, 8 }, { 6, 11, 6 }, { 9, 14, 4 },
		{ 13, 15, 2 }, { 13, 15, 0 }, { 13, 16, -2 }, { 14, 16, -4 },
		{ 15, 17, -6 }, { 15, 17, -8 }, { 16, 18, -10 },
		{ 17, 19, -10 }, { 17, 19, -12 }, { 19, 20, -12 },
		{ 20, 21, -12 }
		}
	},
	/* 15BPP/16BPC */
	{ 273, 15, 2048, 19, 28, 27, 27, {
		{ 0, 4, 10 }, { 4, 9, 8 }, { 8, 13, 6 }, { 12, 17, 4 },
		{ 17, 19, 2 }, { 17, 20, 0 }, { 17, 20, -2 }, { 18, 20, -4 },
		{ 19, 21, -6 }, { 19, 21, -8 }, { 20, 22, -10 },
		{ 21, 23, -10 }, { 21, 23, -12 }, { 23, 24, -12 },
		{ 24, 25, -12 }
		}
	}
}

};

static int get_row_index_for_rc_params(u16 compressed_bpp)
{
	switch (compressed_bpp) {
	case 6:
		return ROW_INDEX_6BPP;
	case 8:
		return ROW_INDEX_8BPP;
	case 10:
		return ROW_INDEX_10BPP;
	case 12:
		return ROW_INDEX_12BPP;
	case 15:
		return ROW_INDEX_15BPP;
	default:
		return -EINVAL;
	}
}

static int get_column_index_for_rc_params(u8 bits_per_component)
{
	switch (bits_per_component) {
	case 8:
		return COLUMN_INDEX_8BPC;
	case 10:
		return COLUMN_INDEX_10BPC;
	case 12:
		return COLUMN_INDEX_12BPC;
	case 14:
		return COLUMN_INDEX_14BPC;
	case 16:
		return COLUMN_INDEX_16BPC;
	default:
		return -EINVAL;
	}
}

int intel_dp_compute_dsc_params(struct intel_dp *intel_dp,
				struct intel_crtc_state *pipe_config)
{
	struct drm_dsc_config *vdsc_cfg = &pipe_config->dp_dsc_cfg;
	u16 compressed_bpp = pipe_config->dsc_params.compressed_bpp;
	u8 i = 0;
	int row_index = 0;
	int column_index = 0;
	u8 line_buf_depth = 0;

	vdsc_cfg->pic_width = pipe_config->base.adjusted_mode.crtc_hdisplay;
	vdsc_cfg->pic_height = pipe_config->base.adjusted_mode.crtc_vdisplay;
	vdsc_cfg->slice_width = DIV_ROUND_UP(vdsc_cfg->pic_width,
					     pipe_config->dsc_params.slice_count);
	/*
	 * Slice Height of 8 works for all currently available panels. So start
	 * with that if pic_height is an integral multiple of 8.
	 * Eventually add logic to try multiple slice heights.
	 */
	if (vdsc_cfg->pic_height % 8 == 0)
		vdsc_cfg->slice_height = 8;
	else if (vdsc_cfg->pic_height % 4 == 0)
		vdsc_cfg->slice_height = 4;
	else
		vdsc_cfg->slice_height = 2;

	/* Values filled from DSC Sink DPCD */
	vdsc_cfg->dsc_version_major =
		(intel_dp->dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT] &
		 DP_DSC_MAJOR_MASK) >> DP_DSC_MAJOR_SHIFT;
	vdsc_cfg->dsc_version_minor =
		min(DSC_SUPPORTED_VERSION_MIN,
		    (intel_dp->dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT] &
		     DP_DSC_MINOR_MASK) >> DP_DSC_MINOR_SHIFT);

	vdsc_cfg->convert_rgb = intel_dp->dsc_dpcd[DP_DSC_DEC_COLOR_FORMAT_CAP - DP_DSC_SUPPORT] &
		DP_DSC_RGB;

	line_buf_depth = drm_dp_dsc_sink_line_buf_depth(intel_dp->dsc_dpcd);
	if (!line_buf_depth) {
		DRM_DEBUG_KMS("DSC Sink Line Buffer Depth invalid\n");
		return -EINVAL;
	}
	if (vdsc_cfg->dsc_version_minor == 2)
		vdsc_cfg->line_buf_depth = (line_buf_depth == DSC_1_2_MAX_LINEBUF_DEPTH_BITS) ?
			DSC_1_2_MAX_LINEBUF_DEPTH_VAL : line_buf_depth;
	else
		vdsc_cfg->line_buf_depth = (line_buf_depth > DSC_1_1_MAX_LINEBUF_DEPTH_BITS) ?
			DSC_1_1_MAX_LINEBUF_DEPTH_BITS : line_buf_depth;

	/* Gen 11 does not support YCbCr */
	vdsc_cfg->simple_422 = false;
	/* Gen 11 does not support VBR */
	vdsc_cfg->vbr_enable = false;
	vdsc_cfg->block_pred_enable =
			intel_dp->dsc_dpcd[DP_DSC_BLK_PREDICTION_SUPPORT - DP_DSC_SUPPORT] &
		DP_DSC_BLK_PREDICTION_IS_SUPPORTED;

	/* Gen 11 only supports integral values of bpp */
	vdsc_cfg->bits_per_pixel = compressed_bpp << 4;
	vdsc_cfg->bits_per_component = pipe_config->pipe_bpp / 3;

	for (i = 0; i < DSC_NUM_BUF_RANGES - 1; i++) {
		/*
		 * six 0s are appended to the lsb of each threshold value
		 * internally in h/w.
		 * Only 8 bits are allowed for programming RcBufThreshold
		 */
		vdsc_cfg->rc_buf_thresh[i] = rc_buf_thresh[i] >> 6;
	}

	/*
	 * For 6bpp, RC Buffer threshold 12 and 13 need a different value
	 * as per C Model
	 */
	if (compressed_bpp == 6) {
		vdsc_cfg->rc_buf_thresh[12] = 0x7C;
		vdsc_cfg->rc_buf_thresh[13] = 0x7D;
	}

	row_index = get_row_index_for_rc_params(compressed_bpp);
	column_index =
		get_column_index_for_rc_params(vdsc_cfg->bits_per_component);

	if (row_index < 0 || column_index < 0)
		return -EINVAL;

	vdsc_cfg->first_line_bpg_offset =
		rc_params[row_index][column_index].first_line_bpg_offset;
	vdsc_cfg->initial_xmit_delay =
		rc_params[row_index][column_index].initial_xmit_delay;
	vdsc_cfg->initial_offset =
		rc_params[row_index][column_index].initial_offset;
	vdsc_cfg->flatness_min_qp =
		rc_params[row_index][column_index].flatness_min_qp;
	vdsc_cfg->flatness_max_qp =
		rc_params[row_index][column_index].flatness_max_qp;
	vdsc_cfg->rc_quant_incr_limit0 =
		rc_params[row_index][column_index].rc_quant_incr_limit0;
	vdsc_cfg->rc_quant_incr_limit1 =
		rc_params[row_index][column_index].rc_quant_incr_limit1;

	for (i = 0; i < DSC_NUM_BUF_RANGES; i++) {
		vdsc_cfg->rc_range_params[i].range_min_qp =
			rc_params[row_index][column_index].rc_range_params[i].range_min_qp;
		vdsc_cfg->rc_range_params[i].range_max_qp =
			rc_params[row_index][column_index].rc_range_params[i].range_max_qp;
		/*
		 * Range BPG Offset uses 2's complement and is only a 6 bits. So
		 * mask it to get only 6 bits.
		 */
		vdsc_cfg->rc_range_params[i].range_bpg_offset =
			rc_params[row_index][column_index].rc_range_params[i].range_bpg_offset &
			DSC_RANGE_BPG_OFFSET_MASK;
	}

	/*
	 * BitsPerComponent value determines mux_word_size:
	 * When BitsPerComponent is 12bpc, muxWordSize will be equal to 64 bits
	 * When BitsPerComponent is 8 or 10bpc, muxWordSize will be equal to
	 * 48 bits
	 */
	if (vdsc_cfg->bits_per_component == 8 ||
	    vdsc_cfg->bits_per_component == 10)
		vdsc_cfg->mux_word_size = DSC_MUX_WORD_SIZE_8_10_BPC;
	else if (vdsc_cfg->bits_per_component == 12)
		vdsc_cfg->mux_word_size = DSC_MUX_WORD_SIZE_12_BPC;

	/* RC_MODEL_SIZE is a constant across all configurations */
	vdsc_cfg->rc_model_size = DSC_RC_MODEL_SIZE_CONST;
	/* InitialScaleValue is a 6 bit value with 3 fractional bits (U3.3) */
	vdsc_cfg->initial_scale_value = (vdsc_cfg->rc_model_size << 3) /
		(vdsc_cfg->rc_model_size - vdsc_cfg->initial_offset);

	return drm_dsc_compute_rc_parameters(vdsc_cfg);
}

enum intel_display_power_domain
intel_dsc_power_domain(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(crtc_state->base.crtc->dev);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;

	/*
	 * On ICL VDSC/joining for eDP transcoder uses a separate power well,
	 * PW2. This requires POWER_DOMAIN_TRANSCODER_VDSC_PW2 power domain.
	 * For any other transcoder, VDSC/joining uses the power well associated
	 * with the pipe/transcoder in use. Hence another reference on the
	 * transcoder power domain will suffice.
	 *
	 * On TGL we have the same mapping, but for transcoder A (the special
	 * TRANSCODER_EDP is gone).
	 */
	if (INTEL_GEN(i915) >= 12 && cpu_transcoder == TRANSCODER_A)
		return POWER_DOMAIN_TRANSCODER_VDSC_PW2;
	else if (cpu_transcoder == TRANSCODER_EDP)
		return POWER_DOMAIN_TRANSCODER_VDSC_PW2;
	else
		return POWER_DOMAIN_TRANSCODER(cpu_transcoder);
}

static void intel_configure_pps_for_dsc_encoder(struct intel_encoder *encoder,
						const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	const struct drm_dsc_config *vdsc_cfg = &crtc_state->dp_dsc_cfg;
	enum pipe pipe = crtc->pipe;
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u32 pps_val = 0;
	u32 rc_buf_thresh_dword[4];
	u32 rc_range_params_dword[8];
	u8 num_vdsc_instances = (crtc_state->dsc_params.dsc_split) ? 2 : 1;
	int i = 0;

	/* Populate PICTURE_PARAMETER_SET_0 registers */
	pps_val = DSC_VER_MAJ | vdsc_cfg->dsc_version_minor <<
		DSC_VER_MIN_SHIFT |
		vdsc_cfg->bits_per_component << DSC_BPC_SHIFT |
		vdsc_cfg->line_buf_depth << DSC_LINE_BUF_DEPTH_SHIFT;
	if (vdsc_cfg->block_pred_enable)
		pps_val |= DSC_BLOCK_PREDICTION;
	if (vdsc_cfg->convert_rgb)
		pps_val |= DSC_COLOR_SPACE_CONVERSION;
	if (vdsc_cfg->simple_422)
		pps_val |= DSC_422_ENABLE;
	if (vdsc_cfg->vbr_enable)
		pps_val |= DSC_VBR_ENABLE;
	DRM_INFO("PPS0 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_0, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_0, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_0(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_0(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_1 registers */
	pps_val = 0;
	pps_val |= DSC_BPP(vdsc_cfg->bits_per_pixel);
	DRM_INFO("PPS1 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_1, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_1, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_1(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_1(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_2 registers */
	pps_val = 0;
	pps_val |= DSC_PIC_HEIGHT(vdsc_cfg->pic_height) |
		DSC_PIC_WIDTH(vdsc_cfg->pic_width / num_vdsc_instances);
	DRM_INFO("PPS2 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_2, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_2, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_2(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_2(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_3 registers */
	pps_val = 0;
	pps_val |= DSC_SLICE_HEIGHT(vdsc_cfg->slice_height) |
		DSC_SLICE_WIDTH(vdsc_cfg->slice_width);
	DRM_INFO("PPS3 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_3, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_3, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_3(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_3(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_4 registers */
	pps_val = 0;
	pps_val |= DSC_INITIAL_XMIT_DELAY(vdsc_cfg->initial_xmit_delay) |
		DSC_INITIAL_DEC_DELAY(vdsc_cfg->initial_dec_delay);
	DRM_INFO("PPS4 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_4, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_4, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_4(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_4(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_5 registers */
	pps_val = 0;
	pps_val |= DSC_SCALE_INC_INT(vdsc_cfg->scale_increment_interval) |
		DSC_SCALE_DEC_INT(vdsc_cfg->scale_decrement_interval);
	DRM_INFO("PPS5 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_5, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_5, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_5(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_5(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_6 registers */
	pps_val = 0;
	pps_val |= DSC_INITIAL_SCALE_VALUE(vdsc_cfg->initial_scale_value) |
		DSC_FIRST_LINE_BPG_OFFSET(vdsc_cfg->first_line_bpg_offset) |
		DSC_FLATNESS_MIN_QP(vdsc_cfg->flatness_min_qp) |
		DSC_FLATNESS_MAX_QP(vdsc_cfg->flatness_max_qp);
	DRM_INFO("PPS6 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_6, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_6, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_6(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_6(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_7 registers */
	pps_val = 0;
	pps_val |= DSC_SLICE_BPG_OFFSET(vdsc_cfg->slice_bpg_offset) |
		DSC_NFL_BPG_OFFSET(vdsc_cfg->nfl_bpg_offset);
	DRM_INFO("PPS7 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_7, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_7, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_7(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_7(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_8 registers */
	pps_val = 0;
	pps_val |= DSC_FINAL_OFFSET(vdsc_cfg->final_offset) |
		DSC_INITIAL_OFFSET(vdsc_cfg->initial_offset);
	DRM_INFO("PPS8 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_8, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_8, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_8(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_8(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_9 registers */
	pps_val = 0;
	pps_val |= DSC_RC_MODEL_SIZE(DSC_RC_MODEL_SIZE_CONST) |
		DSC_RC_EDGE_FACTOR(DSC_RC_EDGE_FACTOR_CONST);
	DRM_INFO("PPS9 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_9, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_9, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_9(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_9(pipe),
				   pps_val);
	}

	/* Populate PICTURE_PARAMETER_SET_10 registers */
	pps_val = 0;
	pps_val |= DSC_RC_QUANT_INC_LIMIT0(vdsc_cfg->rc_quant_incr_limit0) |
		DSC_RC_QUANT_INC_LIMIT1(vdsc_cfg->rc_quant_incr_limit1) |
		DSC_RC_TARGET_OFF_HIGH(DSC_RC_TGT_OFFSET_HI_CONST) |
		DSC_RC_TARGET_OFF_LOW(DSC_RC_TGT_OFFSET_LO_CONST);
	DRM_INFO("PPS10 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_10, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_10, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_10(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_10(pipe),
				   pps_val);
	}

	/* Populate Picture parameter set 16 */
	pps_val = 0;
	pps_val |= DSC_SLICE_CHUNK_SIZE(vdsc_cfg->slice_chunk_size) |
		DSC_SLICE_PER_LINE((vdsc_cfg->pic_width / num_vdsc_instances) /
				   vdsc_cfg->slice_width) |
		DSC_SLICE_ROW_PER_FRAME(vdsc_cfg->pic_height /
					vdsc_cfg->slice_height);
	DRM_INFO("PPS16 = 0x%08x\n", pps_val);
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_PICTURE_PARAMETER_SET_16, pps_val);
		/*
		 * If 2 VDSC instances are needed, configure PPS for second
		 * VDSC
		 */
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(DSCC_PICTURE_PARAMETER_SET_16, pps_val);
	} else {
		I915_WRITE(ICL_DSC0_PICTURE_PARAMETER_SET_16(pipe), pps_val);
		if (crtc_state->dsc_params.dsc_split)
			I915_WRITE(ICL_DSC1_PICTURE_PARAMETER_SET_16(pipe),
				   pps_val);
	}

	/* Populate the RC_BUF_THRESH registers */
	memset(rc_buf_thresh_dword, 0, sizeof(rc_buf_thresh_dword));
	for (i = 0; i < DSC_NUM_BUF_RANGES - 1; i++) {
		rc_buf_thresh_dword[i / 4] |=
			(u32)(vdsc_cfg->rc_buf_thresh[i] <<
			      BITS_PER_BYTE * (i % 4));
		DRM_INFO(" RC_BUF_THRESH%d = 0x%08x\n", i,
			 rc_buf_thresh_dword[i / 4]);
	}
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_RC_BUF_THRESH_0, rc_buf_thresh_dword[0]);
		I915_WRITE(DSCA_RC_BUF_THRESH_0_UDW, rc_buf_thresh_dword[1]);
		I915_WRITE(DSCA_RC_BUF_THRESH_1, rc_buf_thresh_dword[2]);
		I915_WRITE(DSCA_RC_BUF_THRESH_1_UDW, rc_buf_thresh_dword[3]);
		if (crtc_state->dsc_params.dsc_split) {
			I915_WRITE(DSCC_RC_BUF_THRESH_0,
				   rc_buf_thresh_dword[0]);
			I915_WRITE(DSCC_RC_BUF_THRESH_0_UDW,
				   rc_buf_thresh_dword[1]);
			I915_WRITE(DSCC_RC_BUF_THRESH_1,
				   rc_buf_thresh_dword[2]);
			I915_WRITE(DSCC_RC_BUF_THRESH_1_UDW,
				   rc_buf_thresh_dword[3]);
		}
	} else {
		I915_WRITE(ICL_DSC0_RC_BUF_THRESH_0(pipe),
			   rc_buf_thresh_dword[0]);
		I915_WRITE(ICL_DSC0_RC_BUF_THRESH_0_UDW(pipe),
			   rc_buf_thresh_dword[1]);
		I915_WRITE(ICL_DSC0_RC_BUF_THRESH_1(pipe),
			   rc_buf_thresh_dword[2]);
		I915_WRITE(ICL_DSC0_RC_BUF_THRESH_1_UDW(pipe),
			   rc_buf_thresh_dword[3]);
		if (crtc_state->dsc_params.dsc_split) {
			I915_WRITE(ICL_DSC1_RC_BUF_THRESH_0(pipe),
				   rc_buf_thresh_dword[0]);
			I915_WRITE(ICL_DSC1_RC_BUF_THRESH_0_UDW(pipe),
				   rc_buf_thresh_dword[1]);
			I915_WRITE(ICL_DSC1_RC_BUF_THRESH_1(pipe),
				   rc_buf_thresh_dword[2]);
			I915_WRITE(ICL_DSC1_RC_BUF_THRESH_1_UDW(pipe),
				   rc_buf_thresh_dword[3]);
		}
	}

	/* Populate the RC_RANGE_PARAMETERS registers */
	memset(rc_range_params_dword, 0, sizeof(rc_range_params_dword));
	for (i = 0; i < DSC_NUM_BUF_RANGES; i++) {
		rc_range_params_dword[i / 2] |=
			(u32)(((vdsc_cfg->rc_range_params[i].range_bpg_offset <<
				RC_BPG_OFFSET_SHIFT) |
			       (vdsc_cfg->rc_range_params[i].range_max_qp <<
				RC_MAX_QP_SHIFT) |
			       (vdsc_cfg->rc_range_params[i].range_min_qp <<
				RC_MIN_QP_SHIFT)) << 16 * (i % 2));
		DRM_INFO(" RC_RANGE_PARAM_%d = 0x%08x\n", i,
			 rc_range_params_dword[i / 2]);
	}
	if (cpu_transcoder == TRANSCODER_EDP) {
		I915_WRITE(DSCA_RC_RANGE_PARAMETERS_0,
			   rc_range_params_dword[0]);
		I915_WRITE(DSCA_RC_RANGE_PARAMETERS_0_UDW,
			   rc_range_params_dword[1]);
		I915_WRITE(DSCA_RC_RANGE_PARAMETERS_1,
			   rc_range_params_dword[2]);
		I915_WRITE(DSCA_RC_RANGE_PARAMETERS_1_UDW,
			   rc_range_params_dword[3]);
		I915_WRITE(DSCA_RC_RANGE_PARAMETERS_2,
			   rc_range_params_dword[4]);
		I915_WRITE(DSCA_RC_RANGE_PARAMETERS_2_UDW,
			   rc_range_params_dword[5]);
		I915_WRITE(DSCA_RC_RANGE_PARAMETERS_3,
			   rc_range_params_dword[6]);
		I915_WRITE(DSCA_RC_RANGE_PARAMETERS_3_UDW,
			   rc_range_params_dword[7]);
		if (crtc_state->dsc_params.dsc_split) {
			I915_WRITE(DSCC_RC_RANGE_PARAMETERS_0,
				   rc_range_params_dword[0]);
			I915_WRITE(DSCC_RC_RANGE_PARAMETERS_0_UDW,
				   rc_range_params_dword[1]);
			I915_WRITE(DSCC_RC_RANGE_PARAMETERS_1,
				   rc_range_params_dword[2]);
			I915_WRITE(DSCC_RC_RANGE_PARAMETERS_1_UDW,
				   rc_range_params_dword[3]);
			I915_WRITE(DSCC_RC_RANGE_PARAMETERS_2,
				   rc_range_params_dword[4]);
			I915_WRITE(DSCC_RC_RANGE_PARAMETERS_2_UDW,
				   rc_range_params_dword[5]);
			I915_WRITE(DSCC_RC_RANGE_PARAMETERS_3,
				   rc_range_params_dword[6]);
			I915_WRITE(DSCC_RC_RANGE_PARAMETERS_3_UDW,
				   rc_range_params_dword[7]);
		}
	} else {
		I915_WRITE(ICL_DSC0_RC_RANGE_PARAMETERS_0(pipe),
			   rc_range_params_dword[0]);
		I915_WRITE(ICL_DSC0_RC_RANGE_PARAMETERS_0_UDW(pipe),
			   rc_range_params_dword[1]);
		I915_WRITE(ICL_DSC0_RC_RANGE_PARAMETERS_1(pipe),
			   rc_range_params_dword[2]);
		I915_WRITE(ICL_DSC0_RC_RANGE_PARAMETERS_1_UDW(pipe),
			   rc_range_params_dword[3]);
		I915_WRITE(ICL_DSC0_RC_RANGE_PARAMETERS_2(pipe),
			   rc_range_params_dword[4]);
		I915_WRITE(ICL_DSC0_RC_RANGE_PARAMETERS_2_UDW(pipe),
			   rc_range_params_dword[5]);
		I915_WRITE(ICL_DSC0_RC_RANGE_PARAMETERS_3(pipe),
			   rc_range_params_dword[6]);
		I915_WRITE(ICL_DSC0_RC_RANGE_PARAMETERS_3_UDW(pipe),
			   rc_range_params_dword[7]);
		if (crtc_state->dsc_params.dsc_split) {
			I915_WRITE(ICL_DSC1_RC_RANGE_PARAMETERS_0(pipe),
				   rc_range_params_dword[0]);
			I915_WRITE(ICL_DSC1_RC_RANGE_PARAMETERS_0_UDW(pipe),
				   rc_range_params_dword[1]);
			I915_WRITE(ICL_DSC1_RC_RANGE_PARAMETERS_1(pipe),
				   rc_range_params_dword[2]);
			I915_WRITE(ICL_DSC1_RC_RANGE_PARAMETERS_1_UDW(pipe),
				   rc_range_params_dword[3]);
			I915_WRITE(ICL_DSC1_RC_RANGE_PARAMETERS_2(pipe),
				   rc_range_params_dword[4]);
			I915_WRITE(ICL_DSC1_RC_RANGE_PARAMETERS_2_UDW(pipe),
				   rc_range_params_dword[5]);
			I915_WRITE(ICL_DSC1_RC_RANGE_PARAMETERS_3(pipe),
				   rc_range_params_dword[6]);
			I915_WRITE(ICL_DSC1_RC_RANGE_PARAMETERS_3_UDW(pipe),
				   rc_range_params_dword[7]);
		}
	}
}

static void intel_dp_write_dsc_pps_sdp(struct intel_encoder *encoder,
				       const struct intel_crtc_state *crtc_state)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&encoder->base);
	struct intel_digital_port *intel_dig_port = dp_to_dig_port(intel_dp);
	const struct drm_dsc_config *vdsc_cfg = &crtc_state->dp_dsc_cfg;
	struct drm_dsc_pps_infoframe dp_dsc_pps_sdp;

	/* Prepare DP SDP PPS header as per DP 1.4 spec, Table 2-123 */
	drm_dsc_dp_pps_header_init(&dp_dsc_pps_sdp.pps_header);

	/* Fill the PPS payload bytes as per DSC spec 1.2 Table 4-1 */
	drm_dsc_pps_payload_pack(&dp_dsc_pps_sdp.pps_payload, vdsc_cfg);

	intel_dig_port->write_infoframe(encoder, crtc_state,
					DP_SDP_PPS, &dp_dsc_pps_sdp,
					sizeof(dp_dsc_pps_sdp));
}

void intel_dsc_enable(struct intel_encoder *encoder,
		      const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum pipe pipe = crtc->pipe;
	i915_reg_t dss_ctl1_reg, dss_ctl2_reg;
	u32 dss_ctl1_val = 0;
	u32 dss_ctl2_val = 0;

	if (!crtc_state->dsc_params.compression_enable)
		return;

	/* Enable Power wells for VDSC/joining */
	intel_display_power_get(dev_priv,
				intel_dsc_power_domain(crtc_state));

	intel_configure_pps_for_dsc_encoder(encoder, crtc_state);

	intel_dp_write_dsc_pps_sdp(encoder, crtc_state);

	if (crtc_state->cpu_transcoder == TRANSCODER_EDP) {
		dss_ctl1_reg = DSS_CTL1;
		dss_ctl2_reg = DSS_CTL2;
	} else {
		dss_ctl1_reg = ICL_PIPE_DSS_CTL1(pipe);
		dss_ctl2_reg = ICL_PIPE_DSS_CTL2(pipe);
	}
	dss_ctl2_val |= LEFT_BRANCH_VDSC_ENABLE;
	if (crtc_state->dsc_params.dsc_split) {
		dss_ctl2_val |= RIGHT_BRANCH_VDSC_ENABLE;
		dss_ctl1_val |= JOINER_ENABLE;
	}
	I915_WRITE(dss_ctl1_reg, dss_ctl1_val);
	I915_WRITE(dss_ctl2_reg, dss_ctl2_val);
}

void intel_dsc_disable(const struct intel_crtc_state *old_crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(old_crtc_state->base.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	enum pipe pipe = crtc->pipe;
	i915_reg_t dss_ctl1_reg, dss_ctl2_reg;
	u32 dss_ctl1_val = 0, dss_ctl2_val = 0;

	if (!old_crtc_state->dsc_params.compression_enable)
		return;

	if (old_crtc_state->cpu_transcoder == TRANSCODER_EDP) {
		dss_ctl1_reg = DSS_CTL1;
		dss_ctl2_reg = DSS_CTL2;
	} else {
		dss_ctl1_reg = ICL_PIPE_DSS_CTL1(pipe);
		dss_ctl2_reg = ICL_PIPE_DSS_CTL2(pipe);
	}
	dss_ctl1_val = I915_READ(dss_ctl1_reg);
	if (dss_ctl1_val & JOINER_ENABLE)
		dss_ctl1_val &= ~JOINER_ENABLE;
	I915_WRITE(dss_ctl1_reg, dss_ctl1_val);

	dss_ctl2_val = I915_READ(dss_ctl2_reg);
	if (dss_ctl2_val & LEFT_BRANCH_VDSC_ENABLE ||
	    dss_ctl2_val & RIGHT_BRANCH_VDSC_ENABLE)
		dss_ctl2_val &= ~(LEFT_BRANCH_VDSC_ENABLE |
				  RIGHT_BRANCH_VDSC_ENABLE);
	I915_WRITE(dss_ctl2_reg, dss_ctl2_val);

	/* Disable Power wells for VDSC/joining */
	intel_display_power_put_unchecked(dev_priv,
					  intel_dsc_power_domain(old_crtc_state));
}
