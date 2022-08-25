/*
* Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include <linux/slab.h>

#include "dm_services.h"
#include "dc.h"

#include "resource.h"
#include "include/irq_service_interface.h"
#include "dcn20/dcn20_resource.h"

#include "clk_mgr.h"
#include "dcn10/dcn10_hubp.h"
#include "dcn10/dcn10_ipp.h"
#include "dcn20/dcn20_hubbub.h"
#include "dcn20/dcn20_mpc.h"
#include "dcn20/dcn20_hubp.h"
#include "dcn21_hubp.h"
#include "irq/dcn21/irq_service_dcn21.h"
#include "dcn20/dcn20_dpp.h"
#include "dcn20/dcn20_optc.h"
#include "dcn20/dcn20_hwseq.h"
#include "dce110/dce110_hw_sequencer.h"
#include "dcn20/dcn20_opp.h"
#include "dcn20/dcn20_dsc.h"
#include "dcn20/dcn20_link_encoder.h"
#include "dcn20/dcn20_stream_encoder.h"
#include "dce/dce_clock_source.h"
#include "dce/dce_audio.h"
#include "dce/dce_hwseq.h"
#include "virtual/virtual_stream_encoder.h"
#include "dce110/dce110_resource.h"
#include "dml/display_mode_vba.h"
#include "dcn20/dcn20_dccg.h"
#include "dcn21_hubbub.h"
#include "dcn10/dcn10_resource.h"
#include "dce110/dce110_resource.h"

#include "dcn20/dcn20_dwb.h"
#include "dcn20/dcn20_mmhubbub.h"

#include "renoir_ip_offset.h"
#include "dcn/dcn_2_1_0_offset.h"
#include "dcn/dcn_2_1_0_sh_mask.h"

#include "nbio/nbio_7_0_offset.h"

#include "mmhub/mmhub_2_0_0_offset.h"
#include "mmhub/mmhub_2_0_0_sh_mask.h"

#include "reg_helper.h"
#include "dce/dce_abm.h"
#include "dce/dce_dmcu.h"
#include "dce/dce_aux.h"
#include "dce/dce_i2c.h"
#include "dcn21_resource.h"
#include "vm_helper.h"
#include "dcn20/dcn20_vmid.h"

#define SOC_BOUNDING_BOX_VALID false
#define DC_LOGGER_INIT(logger)


struct _vcs_dpi_ip_params_st dcn2_1_ip = {
	.gpuvm_enable = 0,
	.hostvm_enable = 0,
	.gpuvm_max_page_table_levels = 1,
	.hostvm_max_page_table_levels = 4,
	.hostvm_cached_page_table_levels = 2,
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	.num_dsc = 3,
#else
	.num_dsc = 0,
#endif
	.rob_buffer_size_kbytes = 168,
	.det_buffer_size_kbytes = 164,
	.dpte_buffer_size_in_pte_reqs_luma = 44,
	.dpte_buffer_size_in_pte_reqs_chroma = 42,//todo
	.dpp_output_buffer_pixels = 2560,
	.opp_output_buffer_lines = 1,
	.pixel_chunk_size_kbytes = 8,
	.pte_enable = 1,
	.max_page_table_levels = 4,
	.pte_chunk_size_kbytes = 2,
	.meta_chunk_size_kbytes = 2,
	.writeback_chunk_size_kbytes = 2,
	.line_buffer_size_bits = 789504,
	.is_line_buffer_bpp_fixed = 0,
	.line_buffer_fixed_bpp = 0,
	.dcc_supported = true,
	.max_line_buffer_lines = 12,
	.writeback_luma_buffer_size_kbytes = 12,
	.writeback_chroma_buffer_size_kbytes = 8,
	.writeback_chroma_line_buffer_width_pixels = 4,
	.writeback_max_hscl_ratio = 1,
	.writeback_max_vscl_ratio = 1,
	.writeback_min_hscl_ratio = 1,
	.writeback_min_vscl_ratio = 1,
	.writeback_max_hscl_taps = 12,
	.writeback_max_vscl_taps = 12,
	.writeback_line_buffer_luma_buffer_size = 0,
	.writeback_line_buffer_chroma_buffer_size = 14643,
	.cursor_buffer_size = 8,
	.cursor_chunk_size = 2,
	.max_num_otg = 4,
	.max_num_dpp = 4,
	.max_num_wb = 1,
	.max_dchub_pscl_bw_pix_per_clk = 4,
	.max_pscl_lb_bw_pix_per_clk = 2,
	.max_lb_vscl_bw_pix_per_clk = 4,
	.max_vscl_hscl_bw_pix_per_clk = 4,
	.max_hscl_ratio = 4,
	.max_vscl_ratio = 4,
	.hscl_mults = 4,
	.vscl_mults = 4,
	.max_hscl_taps = 8,
	.max_vscl_taps = 8,
	.dispclk_ramp_margin_percent = 1,
	.underscan_factor = 1.10,
	.min_vblank_lines = 32, //
	.dppclk_delay_subtotal = 77, //
	.dppclk_delay_scl_lb_only = 16,
	.dppclk_delay_scl = 50,
	.dppclk_delay_cnvc_formatter = 8,
	.dppclk_delay_cnvc_cursor = 6,
	.dispclk_delay_subtotal = 87, //
	.dcfclk_cstate_latency = 10, // SRExitTime
	.max_inter_dcn_tile_repeaters = 8,

	.xfc_supported = false,
	.xfc_fill_bw_overhead_percent = 10.0,
	.xfc_fill_constant_bytes = 0,
	.ptoi_supported = 0
};

struct _vcs_dpi_soc_bounding_box_st dcn2_1_soc = {
	.clock_limits = {
			{
				.state = 0,
				.dcfclk_mhz = 304.0,
				.fabricclk_mhz = 600.0,
				.dispclk_mhz = 618.0,
				.dppclk_mhz = 440.0,
				.phyclk_mhz = 600.0,
				.socclk_mhz = 278.0,
				.dscclk_mhz = 205.67,
				.dram_speed_mts = 1600.0,
			},
			{
				.state = 1,
				.dcfclk_mhz = 304.0,
				.fabricclk_mhz = 600.0,
				.dispclk_mhz = 618.0,
				.dppclk_mhz = 618.0,
				.phyclk_mhz = 600.0,
				.socclk_mhz = 278.0,
				.dscclk_mhz = 205.67,
				.dram_speed_mts = 1600.0,
			},
			{
				.state = 2,
				.dcfclk_mhz = 608.0,
				.fabricclk_mhz = 1066.0,
				.dispclk_mhz = 888.0,
				.dppclk_mhz = 888.0,
				.phyclk_mhz = 810.0,
				.socclk_mhz = 278.0,
				.dscclk_mhz = 287.67,
				.dram_speed_mts = 2133.0,
			},
			{
				.state = 3,
				.dcfclk_mhz = 676.0,
				.fabricclk_mhz = 1600.0,
				.dispclk_mhz = 1015.0,
				.dppclk_mhz = 1015.0,
				.phyclk_mhz = 810.0,
				.socclk_mhz = 715.0,
				.dscclk_mhz = 318.334,
				.dram_speed_mts = 4266.0,
			},
			{
				.state = 4,
				.dcfclk_mhz = 810.0,
				.fabricclk_mhz = 1600.0,
				.dispclk_mhz = 1015.0,
				.dppclk_mhz = 1015.0,
				.phyclk_mhz = 810.0,
				.socclk_mhz = 953.0,
				.dscclk_mhz = 318.334,
				.dram_speed_mts = 4266.0,
			},
			/*Extra state, no dispclk ramping*/
			{
				.state = 5,
				.dcfclk_mhz = 810.0,
				.fabricclk_mhz = 1600.0,
				.dispclk_mhz = 1015.0,
				.dppclk_mhz = 1015.0,
				.phyclk_mhz = 810.0,
				.socclk_mhz = 953.0,
				.dscclk_mhz = 318.334,
				.dram_speed_mts = 4266.0,
			},

		},

	.sr_exit_time_us = 9.0,
	.sr_enter_plus_exit_time_us = 11.0,
	.urgent_latency_us = 4.0,
	.urgent_latency_pixel_data_only_us = 4.0,
	.urgent_latency_pixel_mixed_with_vm_data_us = 4.0,
	.urgent_latency_vm_data_only_us = 4.0,
	.urgent_out_of_order_return_per_channel_pixel_only_bytes = 4096,
	.urgent_out_of_order_return_per_channel_pixel_and_vm_bytes = 4096,
	.urgent_out_of_order_return_per_channel_vm_only_bytes = 4096,
	.pct_ideal_dram_sdp_bw_after_urgent_pixel_only = 80.0,
	.pct_ideal_dram_sdp_bw_after_urgent_pixel_and_vm = 75.0,
	.pct_ideal_dram_sdp_bw_after_urgent_vm_only = 40.0,
	.max_avg_sdp_bw_use_normal_percent = 60.0,
	.max_avg_dram_bw_use_normal_percent = 100.0,
	.writeback_latency_us = 12.0,
	.max_request_size_bytes = 256,
	.dram_channel_width_bytes = 4,
	.fabric_datapath_to_dcn_data_return_bytes = 32,
	.dcn_downspread_percent = 0.5,
	.downspread_percent = 0.38,
	.dram_page_open_time_ns = 50.0,
	.dram_rw_turnaround_time_ns = 17.5,
	.dram_return_buffer_per_channel_bytes = 8192,
	.round_trip_ping_latency_dcfclk_cycles = 128,
	.urgent_out_of_order_return_per_channel_bytes = 4096,
	.channel_interleave_bytes = 256,
	.num_banks = 8,
	.num_chans = 4,
	.vmm_page_size_bytes = 4096,
	.dram_clock_change_latency_us = 23.84,
	.return_bus_width_bytes = 64,
	.dispclk_dppclk_vco_speed_mhz = 3600,
	.xfc_bus_transport_time_us = 4,
	.xfc_xbuf_latency_tolerance_us = 4,
	.use_urgent_burst_bw = 1,
	.num_states = 5
};

#ifndef MAX
#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))
#endif
#ifndef MIN
#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#endif

/* begin *********************
 * macros to expend register list macro defined in HW object header file */

/* DCN */
/* TODO awful hack. fixup dcn20_dwb.h */
#undef BASE_INNER
#define BASE_INNER(seg) DMU_BASE__INST0_SEG ## seg

#define BASE(seg) BASE_INNER(seg)

#define SR(reg_name)\
		.reg_name = BASE(mm ## reg_name ## _BASE_IDX) +  \
					mm ## reg_name

#define SRI(reg_name, block, id)\
	.reg_name = BASE(mm ## block ## id ## _ ## reg_name ## _BASE_IDX) + \
					mm ## block ## id ## _ ## reg_name

#define SRIR(var_name, reg_name, block, id)\
	.var_name = BASE(mm ## block ## id ## _ ## reg_name ## _BASE_IDX) + \
					mm ## block ## id ## _ ## reg_name

#define SRII(reg_name, block, id)\
	.reg_name[id] = BASE(mm ## block ## id ## _ ## reg_name ## _BASE_IDX) + \
					mm ## block ## id ## _ ## reg_name

#define DCCG_SRII(reg_name, block, id)\
	.block ## _ ## reg_name[id] = BASE(mm ## block ## id ## _ ## reg_name ## _BASE_IDX) + \
					mm ## block ## id ## _ ## reg_name

/* NBIO */
#define NBIO_BASE_INNER(seg) \
	NBIF0_BASE__INST0_SEG ## seg

#define NBIO_BASE(seg) \
	NBIO_BASE_INNER(seg)

#define NBIO_SR(reg_name)\
		.reg_name = NBIO_BASE(mm ## reg_name ## _BASE_IDX) + \
					mm ## reg_name

/* MMHUB */
#define MMHUB_BASE_INNER(seg) \
	MMHUB_BASE__INST0_SEG ## seg

#define MMHUB_BASE(seg) \
	MMHUB_BASE_INNER(seg)

#define MMHUB_SR(reg_name)\
		.reg_name = MMHUB_BASE(mmMM ## reg_name ## _BASE_IDX) + \
					mmMM ## reg_name

#define clk_src_regs(index, pllid)\
[index] = {\
	CS_COMMON_REG_LIST_DCN2_1(index, pllid),\
}

static const struct dce110_clk_src_regs clk_src_regs[] = {
	clk_src_regs(0, A),
	clk_src_regs(1, B),
	clk_src_regs(2, C),
	clk_src_regs(3, D),
	clk_src_regs(4, E),
};

static const struct dce110_clk_src_shift cs_shift = {
		CS_COMMON_MASK_SH_LIST_DCN2_0(__SHIFT)
};

static const struct dce110_clk_src_mask cs_mask = {
		CS_COMMON_MASK_SH_LIST_DCN2_0(_MASK)
};

static const struct bios_registers bios_regs = {
		NBIO_SR(BIOS_SCRATCH_3),
		NBIO_SR(BIOS_SCRATCH_6)
};

#ifdef CONFIG_DRM_AMD_DC_DMUB
static const struct dcn21_dmcub_registers dmcub_regs = {
		DMCUB_REG_LIST_DCN()
};

static const struct dcn21_dmcub_shift dmcub_shift = {
		DMCUB_COMMON_MASK_SH_LIST_BASE(__SHIFT)
};

static const struct dcn21_dmcub_mask dmcub_mask = {
		DMCUB_COMMON_MASK_SH_LIST_BASE(_MASK)
};
#endif

#define audio_regs(id)\
[id] = {\
		AUD_COMMON_REG_LIST(id)\
}

static const struct dce_audio_registers audio_regs[] = {
	audio_regs(0),
	audio_regs(1),
	audio_regs(2),
	audio_regs(3),
	audio_regs(4),
	audio_regs(5),
};

#define DCE120_AUD_COMMON_MASK_SH_LIST(mask_sh)\
		SF(AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX, AZALIA_ENDPOINT_REG_INDEX, mask_sh),\
		SF(AZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA, AZALIA_ENDPOINT_REG_DATA, mask_sh),\
		AUD_COMMON_MASK_SH_LIST_BASE(mask_sh)

static const struct dce_audio_shift audio_shift = {
		DCE120_AUD_COMMON_MASK_SH_LIST(__SHIFT)
};

static const struct dce_audio_mask audio_mask = {
		DCE120_AUD_COMMON_MASK_SH_LIST(_MASK)
};

static const struct dccg_registers dccg_regs = {
		DCCG_COMMON_REG_LIST_DCN_BASE()
};

static const struct dccg_shift dccg_shift = {
		DCCG_MASK_SH_LIST_DCN2(__SHIFT)
};

static const struct dccg_mask dccg_mask = {
		DCCG_MASK_SH_LIST_DCN2(_MASK)
};

#define opp_regs(id)\
[id] = {\
	OPP_REG_LIST_DCN20(id),\
}

static const struct dcn20_opp_registers opp_regs[] = {
	opp_regs(0),
	opp_regs(1),
	opp_regs(2),
	opp_regs(3),
	opp_regs(4),
	opp_regs(5),
};

static const struct dcn20_opp_shift opp_shift = {
		OPP_MASK_SH_LIST_DCN20(__SHIFT)
};

static const struct dcn20_opp_mask opp_mask = {
		OPP_MASK_SH_LIST_DCN20(_MASK)
};

#define tg_regs(id)\
[id] = {TG_COMMON_REG_LIST_DCN2_0(id)}

static const struct dcn_optc_registers tg_regs[] = {
	tg_regs(0),
	tg_regs(1),
	tg_regs(2),
	tg_regs(3)
};

static const struct dcn_optc_shift tg_shift = {
	TG_COMMON_MASK_SH_LIST_DCN2_0(__SHIFT)
};

static const struct dcn_optc_mask tg_mask = {
	TG_COMMON_MASK_SH_LIST_DCN2_0(_MASK)
};

static const struct dcn20_mpc_registers mpc_regs = {
		MPC_REG_LIST_DCN2_0(0),
		MPC_REG_LIST_DCN2_0(1),
		MPC_REG_LIST_DCN2_0(2),
		MPC_REG_LIST_DCN2_0(3),
		MPC_REG_LIST_DCN2_0(4),
		MPC_REG_LIST_DCN2_0(5),
		MPC_OUT_MUX_REG_LIST_DCN2_0(0),
		MPC_OUT_MUX_REG_LIST_DCN2_0(1),
		MPC_OUT_MUX_REG_LIST_DCN2_0(2),
		MPC_OUT_MUX_REG_LIST_DCN2_0(3)
};

static const struct dcn20_mpc_shift mpc_shift = {
	MPC_COMMON_MASK_SH_LIST_DCN2_0(__SHIFT)
};

static const struct dcn20_mpc_mask mpc_mask = {
	MPC_COMMON_MASK_SH_LIST_DCN2_0(_MASK)
};

#define hubp_regs(id)\
[id] = {\
	HUBP_REG_LIST_DCN21(id)\
}

static const struct dcn_hubp2_registers hubp_regs[] = {
		hubp_regs(0),
		hubp_regs(1),
		hubp_regs(2),
		hubp_regs(3)
};

static const struct dcn_hubp2_shift hubp_shift = {
		HUBP_MASK_SH_LIST_DCN21(__SHIFT)
};

static const struct dcn_hubp2_mask hubp_mask = {
		HUBP_MASK_SH_LIST_DCN21(_MASK)
};

static const struct dcn_hubbub_registers hubbub_reg = {
		HUBBUB_REG_LIST_DCN21()
};

static const struct dcn_hubbub_shift hubbub_shift = {
		HUBBUB_MASK_SH_LIST_DCN21(__SHIFT)
};

static const struct dcn_hubbub_mask hubbub_mask = {
		HUBBUB_MASK_SH_LIST_DCN21(_MASK)
};


#define vmid_regs(id)\
[id] = {\
		DCN20_VMID_REG_LIST(id)\
}

static const struct dcn_vmid_registers vmid_regs[] = {
	vmid_regs(0),
	vmid_regs(1),
	vmid_regs(2),
	vmid_regs(3),
	vmid_regs(4),
	vmid_regs(5),
	vmid_regs(6),
	vmid_regs(7),
	vmid_regs(8),
	vmid_regs(9),
	vmid_regs(10),
	vmid_regs(11),
	vmid_regs(12),
	vmid_regs(13),
	vmid_regs(14),
	vmid_regs(15)
};

static const struct dcn20_vmid_shift vmid_shifts = {
		DCN20_VMID_MASK_SH_LIST(__SHIFT)
};

static const struct dcn20_vmid_mask vmid_masks = {
		DCN20_VMID_MASK_SH_LIST(_MASK)
};

#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
#define dsc_regsDCN20(id)\
[id] = {\
	DSC_REG_LIST_DCN20(id)\
}

static const struct dcn20_dsc_registers dsc_regs[] = {
	dsc_regsDCN20(0),
	dsc_regsDCN20(1),
	dsc_regsDCN20(2),
	dsc_regsDCN20(3),
	dsc_regsDCN20(4),
	dsc_regsDCN20(5)
};

static const struct dcn20_dsc_shift dsc_shift = {
	DSC_REG_LIST_SH_MASK_DCN20(__SHIFT)
};

static const struct dcn20_dsc_mask dsc_mask = {
	DSC_REG_LIST_SH_MASK_DCN20(_MASK)
};
#endif

#define ipp_regs(id)\
[id] = {\
	IPP_REG_LIST_DCN20(id),\
}

static const struct dcn10_ipp_registers ipp_regs[] = {
	ipp_regs(0),
	ipp_regs(1),
	ipp_regs(2),
	ipp_regs(3),
};

static const struct dcn10_ipp_shift ipp_shift = {
		IPP_MASK_SH_LIST_DCN20(__SHIFT)
};

static const struct dcn10_ipp_mask ipp_mask = {
		IPP_MASK_SH_LIST_DCN20(_MASK),
};

#define opp_regs(id)\
[id] = {\
	OPP_REG_LIST_DCN20(id),\
}


#define aux_engine_regs(id)\
[id] = {\
	AUX_COMMON_REG_LIST0(id), \
	.AUXN_IMPCAL = 0, \
	.AUXP_IMPCAL = 0, \
	.AUX_RESET_MASK = DP_AUX0_AUX_CONTROL__AUX_RESET_MASK, \
}

static const struct dce110_aux_registers aux_engine_regs[] = {
		aux_engine_regs(0),
		aux_engine_regs(1),
		aux_engine_regs(2),
		aux_engine_regs(3),
		aux_engine_regs(4),
};

#define tf_regs(id)\
[id] = {\
	TF_REG_LIST_DCN20(id),\
}

static const struct dcn2_dpp_registers tf_regs[] = {
	tf_regs(0),
	tf_regs(1),
	tf_regs(2),
	tf_regs(3),
};

static const struct dcn2_dpp_shift tf_shift = {
		TF_REG_LIST_SH_MASK_DCN20(__SHIFT)
};

static const struct dcn2_dpp_mask tf_mask = {
		TF_REG_LIST_SH_MASK_DCN20(_MASK)
};

#define stream_enc_regs(id)\
[id] = {\
	SE_DCN2_REG_LIST(id)\
}

static const struct dcn10_stream_enc_registers stream_enc_regs[] = {
	stream_enc_regs(0),
	stream_enc_regs(1),
	stream_enc_regs(2),
	stream_enc_regs(3),
	stream_enc_regs(4),
};

static const struct dcn10_stream_encoder_shift se_shift = {
		SE_COMMON_MASK_SH_LIST_DCN20(__SHIFT)
};

static const struct dcn10_stream_encoder_mask se_mask = {
		SE_COMMON_MASK_SH_LIST_DCN20(_MASK)
};

static struct input_pixel_processor *dcn21_ipp_create(
	struct dc_context *ctx, uint32_t inst)
{
	struct dcn10_ipp *ipp =
		kzalloc(sizeof(struct dcn10_ipp), GFP_KERNEL);

	if (!ipp) {
		BREAK_TO_DEBUGGER();
		return NULL;
	}

	dcn20_ipp_construct(ipp, ctx, inst,
			&ipp_regs[inst], &ipp_shift, &ipp_mask);
	return &ipp->base;
}

static struct dpp *dcn21_dpp_create(
	struct dc_context *ctx,
	uint32_t inst)
{
	struct dcn20_dpp *dpp =
		kzalloc(sizeof(struct dcn20_dpp), GFP_KERNEL);

	if (!dpp)
		return NULL;

	if (dpp2_construct(dpp, ctx, inst,
			&tf_regs[inst], &tf_shift, &tf_mask))
		return &dpp->base;

	BREAK_TO_DEBUGGER();
	kfree(dpp);
	return NULL;
}

static struct dce_aux *dcn21_aux_engine_create(
	struct dc_context *ctx,
	uint32_t inst)
{
	struct aux_engine_dce110 *aux_engine =
		kzalloc(sizeof(struct aux_engine_dce110), GFP_KERNEL);

	if (!aux_engine)
		return NULL;

	dce110_aux_engine_construct(aux_engine, ctx, inst,
				    SW_AUX_TIMEOUT_PERIOD_MULTIPLIER * AUX_TIMEOUT_PERIOD,
				    &aux_engine_regs[inst]);

	return &aux_engine->base;
}

#define i2c_inst_regs(id) { I2C_HW_ENGINE_COMMON_REG_LIST(id) }

static const struct dce_i2c_registers i2c_hw_regs[] = {
		i2c_inst_regs(1),
		i2c_inst_regs(2),
		i2c_inst_regs(3),
		i2c_inst_regs(4),
		i2c_inst_regs(5),
};

static const struct dce_i2c_shift i2c_shifts = {
		I2C_COMMON_MASK_SH_LIST_DCN2(__SHIFT)
};

static const struct dce_i2c_mask i2c_masks = {
		I2C_COMMON_MASK_SH_LIST_DCN2(_MASK)
};

struct dce_i2c_hw *dcn21_i2c_hw_create(
	struct dc_context *ctx,
	uint32_t inst)
{
	struct dce_i2c_hw *dce_i2c_hw =
		kzalloc(sizeof(struct dce_i2c_hw), GFP_KERNEL);

	if (!dce_i2c_hw)
		return NULL;

	dcn2_i2c_hw_construct(dce_i2c_hw, ctx, inst,
				    &i2c_hw_regs[inst], &i2c_shifts, &i2c_masks);

	return dce_i2c_hw;
}

static const struct resource_caps res_cap_rn = {
		.num_timing_generator = 4,
		.num_opp = 4,
		.num_video_plane = 4,
		.num_audio = 6, // 6 audio endpoints.  4 audio streams
		.num_stream_encoder = 5,
		.num_pll = 5,  // maybe 3 because the last two used for USB-c
		.num_dwb = 1,
		.num_ddc = 5,
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
		.num_dsc = 3,
#endif
};

#ifdef DIAGS_BUILD
static const struct resource_caps res_cap_rn_FPGA_4pipe = {
		.num_timing_generator = 4,
		.num_opp = 4,
		.num_video_plane = 4,
		.num_audio = 7,
		.num_stream_encoder = 4,
		.num_pll = 4,
		.num_dwb = 1,
		.num_ddc = 4,
		.num_dsc = 0,
};

static const struct resource_caps res_cap_rn_FPGA_2pipe_dsc = {
		.num_timing_generator = 2,
		.num_opp = 2,
		.num_video_plane = 2,
		.num_audio = 7,
		.num_stream_encoder = 2,
		.num_pll = 4,
		.num_dwb = 1,
		.num_ddc = 4,
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
		.num_dsc = 2,
#endif
};
#endif

static const struct dc_plane_cap plane_cap = {
	.type = DC_PLANE_TYPE_DCN_UNIVERSAL,
	.blends_with_above = true,
	.blends_with_below = true,
	.per_pixel_alpha = true,

	.pixel_format_support = {
			.argb8888 = true,
			.nv12 = true,
			.fp16 = true
	},

	.max_upscale_factor = {
			.argb8888 = 16000,
			.nv12 = 16000,
			.fp16 = 16000
	},

	.max_downscale_factor = {
			.argb8888 = 250,
			.nv12 = 250,
			.fp16 = 250
	}
};

static const struct dc_debug_options debug_defaults_drv = {
		.disable_dmcu = true,
		.force_abm_enable = false,
		.timing_trace = false,
		.clock_trace = true,
		.disable_pplib_clock_request = true,
		.pipe_split_policy = MPC_SPLIT_AVOID_MULT_DISP,
		.force_single_disp_pipe_split = true,
		.disable_dcc = DCC_ENABLE,
		.vsr_support = true,
		.performance_trace = false,
		.max_downscale_src_width = 5120,/*upto 5K*/
		.disable_pplib_wm_range = false,
		.scl_reset_length10 = true,
		.sanity_checks = true,
		.disable_48mhz_pwrdwn = true,
};

static const struct dc_debug_options debug_defaults_diags = {
		.disable_dmcu = true,
		.force_abm_enable = false,
		.timing_trace = true,
		.clock_trace = true,
		.disable_dpp_power_gate = true,
		.disable_hubp_power_gate = true,
		.disable_clock_gate = true,
		.disable_pplib_clock_request = true,
		.disable_pplib_wm_range = true,
		.disable_stutter = true,
		.disable_48mhz_pwrdwn = true,
};

enum dcn20_clk_src_array_id {
	DCN20_CLK_SRC_PLL0,
	DCN20_CLK_SRC_PLL1,
	DCN20_CLK_SRC_PLL2,
	DCN20_CLK_SRC_TOTAL_DCN21
};

static void destruct(struct dcn21_resource_pool *pool)
{
	unsigned int i;

	for (i = 0; i < pool->base.stream_enc_count; i++) {
		if (pool->base.stream_enc[i] != NULL) {
			kfree(DCN10STRENC_FROM_STRENC(pool->base.stream_enc[i]));
			pool->base.stream_enc[i] = NULL;
		}
	}

#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	for (i = 0; i < pool->base.res_cap->num_dsc; i++) {
		if (pool->base.dscs[i] != NULL)
			dcn20_dsc_destroy(&pool->base.dscs[i]);
	}
#endif

	if (pool->base.mpc != NULL) {
		kfree(TO_DCN20_MPC(pool->base.mpc));
		pool->base.mpc = NULL;
	}
	if (pool->base.hubbub != NULL) {
		kfree(pool->base.hubbub);
		pool->base.hubbub = NULL;
	}
	for (i = 0; i < pool->base.pipe_count; i++) {
		if (pool->base.dpps[i] != NULL)
			dcn20_dpp_destroy(&pool->base.dpps[i]);

		if (pool->base.ipps[i] != NULL)
			pool->base.ipps[i]->funcs->ipp_destroy(&pool->base.ipps[i]);

		if (pool->base.hubps[i] != NULL) {
			kfree(TO_DCN20_HUBP(pool->base.hubps[i]));
			pool->base.hubps[i] = NULL;
		}

		if (pool->base.irqs != NULL) {
			dal_irq_service_destroy(&pool->base.irqs);
		}
	}

	for (i = 0; i < pool->base.res_cap->num_ddc; i++) {
		if (pool->base.engines[i] != NULL)
			dce110_engine_destroy(&pool->base.engines[i]);
		if (pool->base.hw_i2cs[i] != NULL) {
			kfree(pool->base.hw_i2cs[i]);
			pool->base.hw_i2cs[i] = NULL;
		}
		if (pool->base.sw_i2cs[i] != NULL) {
			kfree(pool->base.sw_i2cs[i]);
			pool->base.sw_i2cs[i] = NULL;
		}
	}

	for (i = 0; i < pool->base.res_cap->num_opp; i++) {
		if (pool->base.opps[i] != NULL)
			pool->base.opps[i]->funcs->opp_destroy(&pool->base.opps[i]);
	}

	for (i = 0; i < pool->base.res_cap->num_timing_generator; i++) {
		if (pool->base.timing_generators[i] != NULL)	{
			kfree(DCN10TG_FROM_TG(pool->base.timing_generators[i]));
			pool->base.timing_generators[i] = NULL;
		}
	}

	for (i = 0; i < pool->base.res_cap->num_dwb; i++) {
		if (pool->base.dwbc[i] != NULL) {
			kfree(TO_DCN20_DWBC(pool->base.dwbc[i]));
			pool->base.dwbc[i] = NULL;
		}
		if (pool->base.mcif_wb[i] != NULL) {
			kfree(TO_DCN20_MMHUBBUB(pool->base.mcif_wb[i]));
			pool->base.mcif_wb[i] = NULL;
		}
	}

	for (i = 0; i < pool->base.audio_count; i++) {
		if (pool->base.audios[i])
			dce_aud_destroy(&pool->base.audios[i]);
	}

	for (i = 0; i < pool->base.clk_src_count; i++) {
		if (pool->base.clock_sources[i] != NULL) {
			dcn20_clock_source_destroy(&pool->base.clock_sources[i]);
			pool->base.clock_sources[i] = NULL;
		}
	}

	if (pool->base.dp_clock_source != NULL) {
		dcn20_clock_source_destroy(&pool->base.dp_clock_source);
		pool->base.dp_clock_source = NULL;
	}


	if (pool->base.abm != NULL)
		dce_abm_destroy(&pool->base.abm);

	if (pool->base.dmcu != NULL)
		dce_dmcu_destroy(&pool->base.dmcu);

#ifdef CONFIG_DRM_AMD_DC_DMUB
	if (pool->base.dmcub != NULL)
		dcn21_dmcub_destroy(&pool->base.dmcub);
#endif

	if (pool->base.dccg != NULL)
		dcn_dccg_destroy(&pool->base.dccg);

	if (pool->base.pp_smu != NULL)
		dcn20_pp_smu_destroy(&pool->base.pp_smu);
}


static void calculate_wm_set_for_vlevel(
		int vlevel,
		struct wm_range_table_entry *table_entry,
		struct dcn_watermarks *wm_set,
		struct display_mode_lib *dml,
		display_e2e_pipe_params_st *pipes,
		int pipe_cnt)
{
	double dram_clock_change_latency_cached = dml->soc.dram_clock_change_latency_us;

	ASSERT(vlevel < dml->soc.num_states);
	/* only pipe 0 is read for voltage and dcf/soc clocks */
	pipes[0].clks_cfg.voltage = vlevel;
	pipes[0].clks_cfg.dcfclk_mhz = dml->soc.clock_limits[vlevel].dcfclk_mhz;
	pipes[0].clks_cfg.socclk_mhz = dml->soc.clock_limits[vlevel].socclk_mhz;

	dml->soc.dram_clock_change_latency_us = table_entry->pstate_latency_us;

	wm_set->urgent_ns = get_wm_urgent(dml, pipes, pipe_cnt) * 1000;
	wm_set->cstate_pstate.cstate_enter_plus_exit_ns = get_wm_stutter_enter_exit(dml, pipes, pipe_cnt) * 1000;
	wm_set->cstate_pstate.cstate_exit_ns = get_wm_stutter_exit(dml, pipes, pipe_cnt) * 1000;
	wm_set->cstate_pstate.pstate_change_ns = get_wm_dram_clock_change(dml, pipes, pipe_cnt) * 1000;
	wm_set->pte_meta_urgent_ns = get_wm_memory_trip(dml, pipes, pipe_cnt) * 1000;
#if defined(CONFIG_DRM_AMD_DC_DCN2_1)
	wm_set->frac_urg_bw_nom = get_fraction_of_urgent_bandwidth(dml, pipes, pipe_cnt) * 1000;
	wm_set->frac_urg_bw_flip = get_fraction_of_urgent_bandwidth_imm_flip(dml, pipes, pipe_cnt) * 1000;
#endif
	dml->soc.dram_clock_change_latency_us = dram_clock_change_latency_cached;

}

void dcn21_calculate_wm(
		struct dc *dc, struct dc_state *context,
		display_e2e_pipe_params_st *pipes,
		int *out_pipe_cnt,
		int *pipe_split_from,
		int vlevel_req)
{
	int pipe_cnt, i, pipe_idx;
	int vlevel, vlevel_max;
	struct wm_range_table_entry *table_entry;
	struct clk_bw_params *bw_params = dc->clk_mgr->bw_params;

	ASSERT(bw_params);

	for (i = 0, pipe_idx = 0, pipe_cnt = 0; i < dc->res_pool->pipe_count; i++) {
			if (!context->res_ctx.pipe_ctx[i].stream)
				continue;

			pipes[pipe_cnt].clks_cfg.refclk_mhz = dc->res_pool->ref_clocks.dchub_ref_clock_inKhz / 1000.0;
			pipes[pipe_cnt].clks_cfg.dispclk_mhz = context->bw_ctx.dml.vba.RequiredDISPCLK[vlevel_req][context->bw_ctx.dml.vba.maxMpcComb];

			if (pipe_split_from[i] < 0) {
				pipes[pipe_cnt].clks_cfg.dppclk_mhz =
						context->bw_ctx.dml.vba.RequiredDPPCLK[vlevel_req][context->bw_ctx.dml.vba.maxMpcComb][pipe_idx];
				if (context->bw_ctx.dml.vba.BlendingAndTiming[pipe_idx] == pipe_idx)
					pipes[pipe_cnt].pipe.dest.odm_combine =
							context->bw_ctx.dml.vba.ODMCombineEnablePerState[vlevel_req][pipe_idx];
				else
					pipes[pipe_cnt].pipe.dest.odm_combine = 0;
				pipe_idx++;
			} else {
				pipes[pipe_cnt].clks_cfg.dppclk_mhz =
						context->bw_ctx.dml.vba.RequiredDPPCLK[vlevel_req][context->bw_ctx.dml.vba.maxMpcComb][pipe_split_from[i]];
				if (context->bw_ctx.dml.vba.BlendingAndTiming[pipe_split_from[i]] == pipe_split_from[i])
					pipes[pipe_cnt].pipe.dest.odm_combine =
							context->bw_ctx.dml.vba.ODMCombineEnablePerState[vlevel_req][pipe_split_from[i]];
				else
					pipes[pipe_cnt].pipe.dest.odm_combine = 0;
			}
			pipe_cnt++;
	}

	if (pipe_cnt != pipe_idx) {
		if (dc->res_pool->funcs->populate_dml_pipes)
			pipe_cnt = dc->res_pool->funcs->populate_dml_pipes(dc,
				&context->res_ctx, pipes);
		else
			pipe_cnt = dcn20_populate_dml_pipes_from_context(dc,
				&context->res_ctx, pipes);
	}

	*out_pipe_cnt = pipe_cnt;

	vlevel_max = bw_params->clk_table.num_entries - 1;


	/* WM Set D */
	table_entry = &bw_params->wm_table.entries[WM_D];
	if (table_entry->wm_type == WM_TYPE_RETRAINING)
		vlevel = 0;
	else
		vlevel = vlevel_max;
	calculate_wm_set_for_vlevel(vlevel, table_entry, &context->bw_ctx.bw.dcn.watermarks.d,
						&context->bw_ctx.dml, pipes, pipe_cnt);
	/* WM Set C */
	table_entry = &bw_params->wm_table.entries[WM_C];
	vlevel = MIN(MAX(vlevel_req, 2), vlevel_max);
	calculate_wm_set_for_vlevel(vlevel, table_entry, &context->bw_ctx.bw.dcn.watermarks.c,
						&context->bw_ctx.dml, pipes, pipe_cnt);
	/* WM Set B */
	table_entry = &bw_params->wm_table.entries[WM_B];
	vlevel = MIN(MAX(vlevel_req, 1), vlevel_max);
	calculate_wm_set_for_vlevel(vlevel, table_entry, &context->bw_ctx.bw.dcn.watermarks.b,
						&context->bw_ctx.dml, pipes, pipe_cnt);

	/* WM Set A */
	table_entry = &bw_params->wm_table.entries[WM_A];
	vlevel = MIN(vlevel_req, vlevel_max);
	calculate_wm_set_for_vlevel(vlevel, table_entry, &context->bw_ctx.bw.dcn.watermarks.a,
						&context->bw_ctx.dml, pipes, pipe_cnt);
}


bool dcn21_validate_bandwidth(struct dc *dc, struct dc_state *context,
		bool fast_validate)
{
	bool out = false;

	BW_VAL_TRACE_SETUP();

	int vlevel = 0;
	int pipe_split_from[MAX_PIPES];
	int pipe_cnt = 0;
	display_e2e_pipe_params_st *pipes = kzalloc(dc->res_pool->pipe_count * sizeof(display_e2e_pipe_params_st), GFP_KERNEL);
	DC_LOGGER_INIT(dc->ctx->logger);

	BW_VAL_TRACE_COUNT();

	out = dcn20_fast_validate_bw(dc, context, pipes, &pipe_cnt, pipe_split_from, &vlevel);

	if (pipe_cnt == 0)
		goto validate_out;

	if (!out)
		goto validate_fail;

	BW_VAL_TRACE_END_VOLTAGE_LEVEL();

	if (fast_validate) {
		BW_VAL_TRACE_SKIP(fast);
		goto validate_out;
	}

	dcn21_calculate_wm(dc, context, pipes, &pipe_cnt, pipe_split_from, vlevel);
	dcn20_calculate_dlg_params(dc, context, pipes, pipe_cnt, vlevel);

	BW_VAL_TRACE_END_WATERMARKS();

	goto validate_out;

validate_fail:
	DC_LOG_WARNING("Mode Validation Warning: %s failed validation.\n",
		dml_get_status_message(context->bw_ctx.dml.vba.ValidationStatus[context->bw_ctx.dml.vba.soc.num_states]));

	BW_VAL_TRACE_SKIP(fail);
	out = false;

validate_out:
	kfree(pipes);

	BW_VAL_TRACE_FINISH();

	return out;
}
static void dcn21_destroy_resource_pool(struct resource_pool **pool)
{
	struct dcn21_resource_pool *dcn21_pool = TO_DCN21_RES_POOL(*pool);

	destruct(dcn21_pool);
	kfree(dcn21_pool);
	*pool = NULL;
}

static struct clock_source *dcn21_clock_source_create(
		struct dc_context *ctx,
		struct dc_bios *bios,
		enum clock_source_id id,
		const struct dce110_clk_src_regs *regs,
		bool dp_clk_src)
{
	struct dce110_clk_src *clk_src =
		kzalloc(sizeof(struct dce110_clk_src), GFP_KERNEL);

	if (!clk_src)
		return NULL;

	if (dcn20_clk_src_construct(clk_src, ctx, bios, id,
			regs, &cs_shift, &cs_mask)) {
		clk_src->base.dp_clk_src = dp_clk_src;
		return &clk_src->base;
	}

	BREAK_TO_DEBUGGER();
	return NULL;
}

static struct hubp *dcn21_hubp_create(
	struct dc_context *ctx,
	uint32_t inst)
{
	struct dcn21_hubp *hubp21 =
		kzalloc(sizeof(struct dcn21_hubp), GFP_KERNEL);

	if (!hubp21)
		return NULL;

	if (hubp21_construct(hubp21, ctx, inst,
			&hubp_regs[inst], &hubp_shift, &hubp_mask))
		return &hubp21->base;

	BREAK_TO_DEBUGGER();
	kfree(hubp21);
	return NULL;
}

static struct hubbub *dcn21_hubbub_create(struct dc_context *ctx)
{
	int i;

	struct dcn20_hubbub *hubbub = kzalloc(sizeof(struct dcn20_hubbub),
					  GFP_KERNEL);

	if (!hubbub)
		return NULL;

	hubbub21_construct(hubbub, ctx,
			&hubbub_reg,
			&hubbub_shift,
			&hubbub_mask);

	for (i = 0; i < res_cap_rn.num_vmid; i++) {
		struct dcn20_vmid *vmid = &hubbub->vmid[i];

		vmid->ctx = ctx;

		vmid->regs = &vmid_regs[i];
		vmid->shifts = &vmid_shifts;
		vmid->masks = &vmid_masks;
	}

	return &hubbub->base;
}

struct output_pixel_processor *dcn21_opp_create(
	struct dc_context *ctx, uint32_t inst)
{
	struct dcn20_opp *opp =
		kzalloc(sizeof(struct dcn20_opp), GFP_KERNEL);

	if (!opp) {
		BREAK_TO_DEBUGGER();
		return NULL;
	}

	dcn20_opp_construct(opp, ctx, inst,
			&opp_regs[inst], &opp_shift, &opp_mask);
	return &opp->base;
}

struct timing_generator *dcn21_timing_generator_create(
		struct dc_context *ctx,
		uint32_t instance)
{
	struct optc *tgn10 =
		kzalloc(sizeof(struct optc), GFP_KERNEL);

	if (!tgn10)
		return NULL;

	tgn10->base.inst = instance;
	tgn10->base.ctx = ctx;

	tgn10->tg_regs = &tg_regs[instance];
	tgn10->tg_shift = &tg_shift;
	tgn10->tg_mask = &tg_mask;

	dcn20_timing_generator_init(tgn10);

	return &tgn10->base;
}

struct mpc *dcn21_mpc_create(struct dc_context *ctx)
{
	struct dcn20_mpc *mpc20 = kzalloc(sizeof(struct dcn20_mpc),
					  GFP_KERNEL);

	if (!mpc20)
		return NULL;

	dcn20_mpc_construct(mpc20, ctx,
			&mpc_regs,
			&mpc_shift,
			&mpc_mask,
			6);

	return &mpc20->base;
}

static void read_dce_straps(
	struct dc_context *ctx,
	struct resource_straps *straps)
{
	generic_reg_get(ctx, mmDC_PINSTRAPS + BASE(mmDC_PINSTRAPS_BASE_IDX),
		FN(DC_PINSTRAPS, DC_PINSTRAPS_AUDIO), &straps->dc_pinstraps_audio);

}

#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT

struct display_stream_compressor *dcn21_dsc_create(
	struct dc_context *ctx, uint32_t inst)
{
	struct dcn20_dsc *dsc =
		kzalloc(sizeof(struct dcn20_dsc), GFP_KERNEL);

	if (!dsc) {
		BREAK_TO_DEBUGGER();
		return NULL;
	}

	dsc2_construct(dsc, ctx, inst, &dsc_regs[inst], &dsc_shift, &dsc_mask);
	return &dsc->base;
}
#endif

static void update_bw_bounding_box(struct dc *dc, struct clk_bw_params *bw_params)
{
	struct dcn21_resource_pool *pool = TO_DCN21_RES_POOL(dc->res_pool);
	struct clk_limit_table *clk_table = &bw_params->clk_table;
	int i;

	dcn2_1_ip.max_num_otg = pool->base.res_cap->num_timing_generator;
	dcn2_1_ip.max_num_dpp = pool->base.pipe_count;
	dcn2_1_soc.num_chans = bw_params->num_channels;
	dcn2_1_soc.num_states = 0;

	for (i = 0; i < clk_table->num_entries; i++) {

		dcn2_1_soc.clock_limits[i].state = i;
		dcn2_1_soc.clock_limits[i].dcfclk_mhz = clk_table->entries[i].dcfclk_mhz;
		dcn2_1_soc.clock_limits[i].fabricclk_mhz = clk_table->entries[i].fclk_mhz;
		dcn2_1_soc.clock_limits[i].socclk_mhz = clk_table->entries[i].socclk_mhz;
		/* This is probably wrong, TODO: find correct calculation */
		dcn2_1_soc.clock_limits[i].dram_speed_mts = clk_table->entries[i].memclk_mhz * 16 / 1000;
		dcn2_1_soc.num_states++;
	}
}

/* Temporary Place holder until we can get them from fuse */
static struct dpm_clocks dummy_clocks = {
		.DcfClocks = {
				{.Freq = 400, .Vol = 1},
				{.Freq = 483, .Vol = 1},
				{.Freq = 602, .Vol = 1},
				{.Freq = 738, .Vol = 1} },
		.SocClocks = {
				{.Freq = 300, .Vol = 1},
				{.Freq = 400, .Vol = 1},
				{.Freq = 400, .Vol = 1},
				{.Freq = 400, .Vol = 1} },
		.FClocks = {
				{.Freq = 400, .Vol = 1},
				{.Freq = 800, .Vol = 1},
				{.Freq = 1067, .Vol = 1},
				{.Freq = 1600, .Vol = 1} },
		.MemClocks = {
				{.Freq = 800, .Vol = 1},
				{.Freq = 1600, .Vol = 1},
				{.Freq = 1067, .Vol = 1},
				{.Freq = 1600, .Vol = 1} },

};

enum pp_smu_status dummy_set_wm_ranges(struct pp_smu *pp,
		struct pp_smu_wm_range_sets *ranges)
{
	return PP_SMU_RESULT_OK;
}

enum pp_smu_status dummy_get_dpm_clock_table(struct pp_smu *pp,
		struct dpm_clocks *clock_table)
{
	*clock_table = dummy_clocks;
	return PP_SMU_RESULT_OK;
}

struct pp_smu_funcs *dcn21_pp_smu_create(struct dc_context *ctx)
{
	struct pp_smu_funcs *pp_smu = kzalloc(sizeof(*pp_smu), GFP_KERNEL);

	pp_smu->ctx.ver = PP_SMU_VER_RN;

	pp_smu->rn_funcs.get_dpm_clock_table = dummy_get_dpm_clock_table;
	pp_smu->rn_funcs.set_wm_ranges = dummy_set_wm_ranges;

	return pp_smu;
}

void dcn21_pp_smu_destroy(struct pp_smu_funcs **pp_smu)
{
	if (pp_smu && *pp_smu) {
		kfree(*pp_smu);
		*pp_smu = NULL;
	}
}

static struct audio *dcn21_create_audio(
		struct dc_context *ctx, unsigned int inst)
{
	return dce_audio_create(ctx, inst,
			&audio_regs[inst], &audio_shift, &audio_mask);
}

static struct dc_cap_funcs cap_funcs = {
	.get_dcc_compression_cap = dcn20_get_dcc_compression_cap
};

struct stream_encoder *dcn21_stream_encoder_create(
	enum engine_id eng_id,
	struct dc_context *ctx)
{
	struct dcn10_stream_encoder *enc1 =
		kzalloc(sizeof(struct dcn10_stream_encoder), GFP_KERNEL);

	if (!enc1)
		return NULL;

	dcn20_stream_encoder_construct(enc1, ctx, ctx->dc_bios, eng_id,
					&stream_enc_regs[eng_id],
					&se_shift, &se_mask);

	return &enc1->base;
}

static const struct dce_hwseq_registers hwseq_reg = {
		HWSEQ_DCN21_REG_LIST()
};

static const struct dce_hwseq_shift hwseq_shift = {
		HWSEQ_DCN21_MASK_SH_LIST(__SHIFT)
};

static const struct dce_hwseq_mask hwseq_mask = {
		HWSEQ_DCN21_MASK_SH_LIST(_MASK)
};

static struct dce_hwseq *dcn21_hwseq_create(
	struct dc_context *ctx)
{
	struct dce_hwseq *hws = kzalloc(sizeof(struct dce_hwseq), GFP_KERNEL);

	if (hws) {
		hws->ctx = ctx;
		hws->regs = &hwseq_reg;
		hws->shifts = &hwseq_shift;
		hws->masks = &hwseq_mask;
	}
	return hws;
}

static const struct resource_create_funcs res_create_funcs = {
	.read_dce_straps = read_dce_straps,
	.create_audio = dcn21_create_audio,
	.create_stream_encoder = dcn21_stream_encoder_create,
	.create_hwseq = dcn21_hwseq_create,
};

static const struct resource_create_funcs res_create_maximus_funcs = {
	.read_dce_straps = NULL,
	.create_audio = NULL,
	.create_stream_encoder = NULL,
	.create_hwseq = dcn21_hwseq_create,
};

static struct resource_funcs dcn21_res_pool_funcs = {
	.destroy = dcn21_destroy_resource_pool,
	.link_enc_create = dcn20_link_encoder_create,
	.validate_bandwidth = dcn21_validate_bandwidth,
	.add_stream_to_ctx = dcn20_add_stream_to_ctx,
	.remove_stream_from_ctx = dcn20_remove_stream_from_ctx,
	.acquire_idle_pipe_for_layer = dcn20_acquire_idle_pipe_for_layer,
	.populate_dml_writeback_from_context = dcn20_populate_dml_writeback_from_context,
	.get_default_swizzle_mode = dcn20_get_default_swizzle_mode,
	.set_mcif_arb_params = dcn20_set_mcif_arb_params,
	.find_first_free_match_stream_enc_for_link = dcn10_find_first_free_match_stream_enc_for_link,
	.update_bw_bounding_box = update_bw_bounding_box
};

static bool construct(
	uint8_t num_virtual_links,
	struct dc *dc,
	struct dcn21_resource_pool *pool)
{
	int i;
	struct dc_context *ctx = dc->ctx;
	struct irq_service_init_data init_data;

	ctx->dc_bios->regs = &bios_regs;

	pool->base.res_cap = &res_cap_rn;
#ifdef DIAGS_BUILD
	if (IS_FPGA_MAXIMUS_DC(dc->ctx->dce_environment))
		//pool->base.res_cap = &res_cap_nv10_FPGA_2pipe_dsc;
		pool->base.res_cap = &res_cap_rn_FPGA_4pipe;
#endif

	pool->base.funcs = &dcn21_res_pool_funcs;

	/*************************************************
	 *  Resource + asic cap harcoding                *
	 *************************************************/
	pool->base.underlay_pipe_index = NO_UNDERLAY_PIPE;

	pool->base.pipe_count = 4;
	dc->caps.max_downscale_ratio = 200;
	dc->caps.i2c_speed_in_khz = 100;
	dc->caps.max_cursor_size = 256;
	dc->caps.dmdata_alloc_size = 2048;
	dc->caps.hw_3d_lut = true;

	dc->caps.max_slave_planes = 1;
	dc->caps.post_blend_color_processing = true;
	dc->caps.force_dp_tps4_for_cp2520 = true;

	if (dc->ctx->dce_environment == DCE_ENV_PRODUCTION_DRV)
		dc->debug = debug_defaults_drv;
	else if (dc->ctx->dce_environment == DCE_ENV_FPGA_MAXIMUS) {
		pool->base.pipe_count = 4;
		dc->debug = debug_defaults_diags;
	} else
		dc->debug = debug_defaults_diags;

	// Init the vm_helper
	if (dc->vm_helper)
		vm_helper_init(dc->vm_helper, 16);

	/*************************************************
	 *  Create resources                             *
	 *************************************************/

	pool->base.clock_sources[DCN20_CLK_SRC_PLL0] =
			dcn21_clock_source_create(ctx, ctx->dc_bios,
				CLOCK_SOURCE_COMBO_PHY_PLL0,
				&clk_src_regs[0], false);
	pool->base.clock_sources[DCN20_CLK_SRC_PLL1] =
			dcn21_clock_source_create(ctx, ctx->dc_bios,
				CLOCK_SOURCE_COMBO_PHY_PLL1,
				&clk_src_regs[1], false);
	pool->base.clock_sources[DCN20_CLK_SRC_PLL2] =
			dcn21_clock_source_create(ctx, ctx->dc_bios,
				CLOCK_SOURCE_COMBO_PHY_PLL2,
				&clk_src_regs[2], false);

	pool->base.clk_src_count = DCN20_CLK_SRC_TOTAL_DCN21;

	/* todo: not reuse phy_pll registers */
	pool->base.dp_clock_source =
			dcn21_clock_source_create(ctx, ctx->dc_bios,
				CLOCK_SOURCE_ID_DP_DTO,
				&clk_src_regs[0], true);

	for (i = 0; i < pool->base.clk_src_count; i++) {
		if (pool->base.clock_sources[i] == NULL) {
			dm_error("DC: failed to create clock sources!\n");
			BREAK_TO_DEBUGGER();
			goto create_fail;
		}
	}

	pool->base.dccg = dccg2_create(ctx, &dccg_regs, &dccg_shift, &dccg_mask);
	if (pool->base.dccg == NULL) {
		dm_error("DC: failed to create dccg!\n");
		BREAK_TO_DEBUGGER();
		goto create_fail;
	}

#ifdef CONFIG_DRM_AMD_DC_DMUB
	pool->base.dmcub = dcn21_dmcub_create(ctx,
			&dmcub_regs,
			&dmcub_shift,
			&dmcub_mask);
	if (pool->base.dmcub == NULL) {
		dm_error("DC: failed to create dmcub!\n");
		BREAK_TO_DEBUGGER();
		goto create_fail;
	}
#endif

	pool->base.pp_smu = dcn21_pp_smu_create(ctx);

	dml_init_instance(&dc->dml, &dcn2_1_soc, &dcn2_1_ip, DML_PROJECT_DCN21);

	init_data.ctx = dc->ctx;
	pool->base.irqs = dal_irq_service_dcn21_create(&init_data);
	if (!pool->base.irqs)
		goto create_fail;

	/* mem input -> ipp -> dpp -> opp -> TG */
	for (i = 0; i < pool->base.pipe_count; i++) {
		pool->base.hubps[i] = dcn21_hubp_create(ctx, i);
		if (pool->base.hubps[i] == NULL) {
			BREAK_TO_DEBUGGER();
			dm_error(
				"DC: failed to create memory input!\n");
			goto create_fail;
		}

		pool->base.ipps[i] = dcn21_ipp_create(ctx, i);
		if (pool->base.ipps[i] == NULL) {
			BREAK_TO_DEBUGGER();
			dm_error(
				"DC: failed to create input pixel processor!\n");
			goto create_fail;
		}

		pool->base.dpps[i] = dcn21_dpp_create(ctx, i);
		if (pool->base.dpps[i] == NULL) {
			BREAK_TO_DEBUGGER();
			dm_error(
				"DC: failed to create dpps!\n");
			goto create_fail;
		}
	}

	for (i = 0; i < pool->base.res_cap->num_ddc; i++) {
		pool->base.engines[i] = dcn21_aux_engine_create(ctx, i);
		if (pool->base.engines[i] == NULL) {
			BREAK_TO_DEBUGGER();
			dm_error(
				"DC:failed to create aux engine!!\n");
			goto create_fail;
		}
		pool->base.hw_i2cs[i] = dcn21_i2c_hw_create(ctx, i);
		if (pool->base.hw_i2cs[i] == NULL) {
			BREAK_TO_DEBUGGER();
			dm_error(
				"DC:failed to create hw i2c!!\n");
			goto create_fail;
		}
		pool->base.sw_i2cs[i] = NULL;
	}

	for (i = 0; i < pool->base.res_cap->num_opp; i++) {
		pool->base.opps[i] = dcn21_opp_create(ctx, i);
		if (pool->base.opps[i] == NULL) {
			BREAK_TO_DEBUGGER();
			dm_error(
				"DC: failed to create output pixel processor!\n");
			goto create_fail;
		}
	}

	for (i = 0; i < pool->base.res_cap->num_timing_generator; i++) {
		pool->base.timing_generators[i] = dcn21_timing_generator_create(
				ctx, i);
		if (pool->base.timing_generators[i] == NULL) {
			BREAK_TO_DEBUGGER();
			dm_error("DC: failed to create tg!\n");
			goto create_fail;
		}
	}

	pool->base.timing_generator_count = i;

	pool->base.mpc = dcn21_mpc_create(ctx);
	if (pool->base.mpc == NULL) {
		BREAK_TO_DEBUGGER();
		dm_error("DC: failed to create mpc!\n");
		goto create_fail;
	}

	pool->base.hubbub = dcn21_hubbub_create(ctx);
	if (pool->base.hubbub == NULL) {
		BREAK_TO_DEBUGGER();
		dm_error("DC: failed to create hubbub!\n");
		goto create_fail;
	}

#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	for (i = 0; i < pool->base.res_cap->num_dsc; i++) {
		pool->base.dscs[i] = dcn21_dsc_create(ctx, i);
		if (pool->base.dscs[i] == NULL) {
			BREAK_TO_DEBUGGER();
			dm_error("DC: failed to create display stream compressor %d!\n", i);
			goto create_fail;
		}
	}
#endif

	if (!dcn20_dwbc_create(ctx, &pool->base)) {
		BREAK_TO_DEBUGGER();
		dm_error("DC: failed to create dwbc!\n");
		goto create_fail;
	}
	if (!dcn20_mmhubbub_create(ctx, &pool->base)) {
		BREAK_TO_DEBUGGER();
		dm_error("DC: failed to create mcif_wb!\n");
		goto create_fail;
	}

	if (!resource_construct(num_virtual_links, dc, &pool->base,
			(!IS_FPGA_MAXIMUS_DC(dc->ctx->dce_environment) ?
			&res_create_funcs : &res_create_maximus_funcs)))
			goto create_fail;

	dcn20_hw_sequencer_construct(dc);

	dc->caps.max_planes =  pool->base.pipe_count;

	for (i = 0; i < dc->caps.max_planes; ++i)
		dc->caps.planes[i] = plane_cap;

	dc->cap_funcs = cap_funcs;

	return true;

create_fail:

	destruct(pool);

	return false;
}

struct resource_pool *dcn21_create_resource_pool(
		const struct dc_init_data *init_data,
		struct dc *dc)
{
	struct dcn21_resource_pool *pool =
		kzalloc(sizeof(struct dcn21_resource_pool), GFP_KERNEL);

	if (!pool)
		return NULL;

	if (construct(init_data->num_virtual_links, dc, pool))
		return &pool->base;

	BREAK_TO_DEBUGGER();
	kfree(pool);
	return NULL;
}
