/*
 * Copyright 2016 Advanced Micro Devices, Inc.
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
#include <linux/delay.h>

#include "dm_services.h"
#include "dm_helpers.h"
#include "core_types.h"
#include "resource.h"
#include "dcn20/dcn20_resource.h"
#include "dce110/dce110_hw_sequencer.h"
#include "dcn10/dcn10_hw_sequencer.h"
#include "dcn20_hwseq.h"
#include "dce/dce_hwseq.h"
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
#include "dcn20/dcn20_dsc.h"
#endif
#include "abm.h"
#include "clk_mgr.h"
#include "dmcu.h"
#include "hubp.h"
#include "timing_generator.h"
#include "opp.h"
#include "ipp.h"
#include "mpc.h"
#include "mcif_wb.h"
#include "reg_helper.h"
#include "dcn10/dcn10_cm_common.h"
#include "dcn10/dcn10_hubbub.h"
#include "dcn10/dcn10_optc.h"
#include "dc_link_dp.h"
#include "vm_helper.h"
#include "dccg.h"

#define DC_LOGGER_INIT(logger)

#define CTX \
	hws->ctx
#define REG(reg)\
	hws->regs->reg

#undef FN
#define FN(reg_name, field_name) \
	hws->shifts->field_name, hws->masks->field_name

static void dcn20_enable_power_gating_plane(
	struct dce_hwseq *hws,
	bool enable)
{
	bool force_on = 1; /* disable power gating */

	if (enable)
		force_on = 0;

	/* DCHUBP0/1/2/3/4/5 */
	REG_UPDATE(DOMAIN0_PG_CONFIG, DOMAIN0_POWER_FORCEON, force_on);
	REG_UPDATE(DOMAIN2_PG_CONFIG, DOMAIN2_POWER_FORCEON, force_on);
	REG_UPDATE(DOMAIN4_PG_CONFIG, DOMAIN4_POWER_FORCEON, force_on);
	REG_UPDATE(DOMAIN6_PG_CONFIG, DOMAIN6_POWER_FORCEON, force_on);
	if (REG(DOMAIN8_PG_CONFIG))
		REG_UPDATE(DOMAIN8_PG_CONFIG, DOMAIN8_POWER_FORCEON, force_on);
	if (REG(DOMAIN10_PG_CONFIG))
		REG_UPDATE(DOMAIN10_PG_CONFIG, DOMAIN8_POWER_FORCEON, force_on);

	/* DPP0/1/2/3/4/5 */
	REG_UPDATE(DOMAIN1_PG_CONFIG, DOMAIN1_POWER_FORCEON, force_on);
	REG_UPDATE(DOMAIN3_PG_CONFIG, DOMAIN3_POWER_FORCEON, force_on);
	REG_UPDATE(DOMAIN5_PG_CONFIG, DOMAIN5_POWER_FORCEON, force_on);
	REG_UPDATE(DOMAIN7_PG_CONFIG, DOMAIN7_POWER_FORCEON, force_on);
	if (REG(DOMAIN9_PG_CONFIG))
		REG_UPDATE(DOMAIN9_PG_CONFIG, DOMAIN9_POWER_FORCEON, force_on);
	if (REG(DOMAIN11_PG_CONFIG))
		REG_UPDATE(DOMAIN11_PG_CONFIG, DOMAIN9_POWER_FORCEON, force_on);

	/* DCS0/1/2/3/4/5 */
	REG_UPDATE(DOMAIN16_PG_CONFIG, DOMAIN16_POWER_FORCEON, force_on);
	REG_UPDATE(DOMAIN17_PG_CONFIG, DOMAIN17_POWER_FORCEON, force_on);
	REG_UPDATE(DOMAIN18_PG_CONFIG, DOMAIN18_POWER_FORCEON, force_on);
	if (REG(DOMAIN19_PG_CONFIG))
		REG_UPDATE(DOMAIN19_PG_CONFIG, DOMAIN19_POWER_FORCEON, force_on);
	if (REG(DOMAIN20_PG_CONFIG))
		REG_UPDATE(DOMAIN20_PG_CONFIG, DOMAIN20_POWER_FORCEON, force_on);
	if (REG(DOMAIN21_PG_CONFIG))
		REG_UPDATE(DOMAIN21_PG_CONFIG, DOMAIN21_POWER_FORCEON, force_on);
}

void dcn20_dccg_init(struct dce_hwseq *hws)
{
	/*
	 * set MICROSECOND_TIME_BASE_DIV
	 * 100Mhz refclk -> 0x120264
	 * 27Mhz refclk -> 0x12021b
	 * 48Mhz refclk -> 0x120230
	 *
	 */
	REG_WRITE(MICROSECOND_TIME_BASE_DIV, 0x120264);

	/*
	 * set MILLISECOND_TIME_BASE_DIV
	 * 100Mhz refclk -> 0x1186a0
	 * 27Mhz refclk -> 0x106978
	 * 48Mhz refclk -> 0x10bb80
	 *
	 */
	REG_WRITE(MILLISECOND_TIME_BASE_DIV, 0x1186a0);

	/* This value is dependent on the hardware pipeline delay so set once per SOC */
	REG_WRITE(DISPCLK_FREQ_CHANGE_CNTL, 0x801003c);
}
void dcn20_display_init(struct dc *dc)
{
	struct dce_hwseq *hws = dc->hwseq;

	/* RBBMIF
	 * disable RBBMIF timeout detection for all clients
	 * Ensure RBBMIF does not drop register accesses due to the per-client timeout
	 */
	REG_WRITE(RBBMIF_TIMEOUT_DIS, 0xFFFFFFFF);
	REG_WRITE(RBBMIF_TIMEOUT_DIS_2, 0xFFFFFFFF);

	/* DCCG */
	dcn20_dccg_init(hws);

	REG_UPDATE(DC_MEM_GLOBAL_PWR_REQ_CNTL, DC_MEM_GLOBAL_PWR_REQ_DIS, 0);

	/* DCHUB/MMHUBBUB
	 * set global timer refclk divider
	 * 100Mhz refclk -> 2
	 * 27Mhz refclk ->  1
	 * 48Mhz refclk ->  1
	 */
	REG_UPDATE(DCHUBBUB_GLOBAL_TIMER_CNTL, DCHUBBUB_GLOBAL_TIMER_REFDIV, 2);
	REG_UPDATE(DCHUBBUB_GLOBAL_TIMER_CNTL, DCHUBBUB_GLOBAL_TIMER_ENABLE, 1);
	REG_WRITE(REFCLK_CNTL, 0);

	/* OPTC
	 * OTG_CONTROL.OTG_DISABLE_POINT_CNTL = 0x3; will be set during optc2_enable_crtc
	 */

	/* AZ
	 * default value is 0x64 for 100Mhz ref clock, if the ref clock is 100Mhz, no need to program this regiser,
	 * if not, it should be programmed according to the ref clock
	 */
	REG_UPDATE(AZALIA_AUDIO_DTO, AZALIA_AUDIO_DTO_MODULE, 0x64);
	/* Enable controller clock gating */
	REG_WRITE(AZALIA_CONTROLLER_CLOCK_GATING, 0x1);
}

void dcn20_disable_vga(
	struct dce_hwseq *hws)
{
	REG_WRITE(D1VGA_CONTROL, 0);
	REG_WRITE(D2VGA_CONTROL, 0);
	REG_WRITE(D3VGA_CONTROL, 0);
	REG_WRITE(D4VGA_CONTROL, 0);
	REG_WRITE(D5VGA_CONTROL, 0);
	REG_WRITE(D6VGA_CONTROL, 0);
}

void dcn20_program_tripleBuffer(
	const struct dc *dc,
	struct pipe_ctx *pipe_ctx,
	bool enableTripleBuffer)
{
	if (pipe_ctx->plane_res.hubp && pipe_ctx->plane_res.hubp->funcs) {
		pipe_ctx->plane_res.hubp->funcs->hubp_enable_tripleBuffer(
			pipe_ctx->plane_res.hubp,
			enableTripleBuffer);
	}
}

/* Blank pixel data during initialization */
void dcn20_init_blank(
		struct dc *dc,
		struct timing_generator *tg)
{
	enum dc_color_space color_space;
	struct tg_color black_color = {0};
	struct output_pixel_processor *opp = NULL;
	struct output_pixel_processor *bottom_opp = NULL;
	uint32_t num_opps, opp_id_src0, opp_id_src1;
	uint32_t otg_active_width, otg_active_height;

	/* program opp dpg blank color */
	color_space = COLOR_SPACE_SRGB;
	color_space_to_black_color(dc, color_space, &black_color);

	/* get the OTG active size */
	tg->funcs->get_otg_active_size(tg,
			&otg_active_width,
			&otg_active_height);

	/* get the OPTC source */
	tg->funcs->get_optc_source(tg, &num_opps, &opp_id_src0, &opp_id_src1);
	ASSERT(opp_id_src0 < dc->res_pool->res_cap->num_opp);
	opp = dc->res_pool->opps[opp_id_src0];

	if (num_opps == 2) {
		otg_active_width = otg_active_width / 2;
		ASSERT(opp_id_src1 < dc->res_pool->res_cap->num_opp);
		bottom_opp = dc->res_pool->opps[opp_id_src1];
	}

	opp->funcs->opp_set_disp_pattern_generator(
			opp,
			CONTROLLER_DP_TEST_PATTERN_SOLID_COLOR,
			COLOR_DEPTH_UNDEFINED,
			&black_color,
			otg_active_width,
			otg_active_height);

	if (num_opps == 2) {
		bottom_opp->funcs->opp_set_disp_pattern_generator(
				bottom_opp,
				CONTROLLER_DP_TEST_PATTERN_SOLID_COLOR,
				COLOR_DEPTH_UNDEFINED,
				&black_color,
				otg_active_width,
				otg_active_height);
	}

	dcn20_hwss_wait_for_blank_complete(opp);
}

#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
static void dcn20_dsc_pg_control(
		struct dce_hwseq *hws,
		unsigned int dsc_inst,
		bool power_on)
{
	uint32_t power_gate = power_on ? 0 : 1;
	uint32_t pwr_status = power_on ? 0 : 2;
	uint32_t org_ip_request_cntl = 0;

	if (hws->ctx->dc->debug.disable_dsc_power_gate)
		return;

	if (REG(DOMAIN16_PG_CONFIG) == 0)
		return;

	REG_GET(DC_IP_REQUEST_CNTL, IP_REQUEST_EN, &org_ip_request_cntl);
	if (org_ip_request_cntl == 0)
		REG_SET(DC_IP_REQUEST_CNTL, 0, IP_REQUEST_EN, 1);

	switch (dsc_inst) {
	case 0: /* DSC0 */
		REG_UPDATE(DOMAIN16_PG_CONFIG,
				DOMAIN16_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN16_PG_STATUS,
				DOMAIN16_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 1: /* DSC1 */
		REG_UPDATE(DOMAIN17_PG_CONFIG,
				DOMAIN17_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN17_PG_STATUS,
				DOMAIN17_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 2: /* DSC2 */
		REG_UPDATE(DOMAIN18_PG_CONFIG,
				DOMAIN18_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN18_PG_STATUS,
				DOMAIN18_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 3: /* DSC3 */
		REG_UPDATE(DOMAIN19_PG_CONFIG,
				DOMAIN19_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN19_PG_STATUS,
				DOMAIN19_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 4: /* DSC4 */
		REG_UPDATE(DOMAIN20_PG_CONFIG,
				DOMAIN20_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN20_PG_STATUS,
				DOMAIN20_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 5: /* DSC5 */
		REG_UPDATE(DOMAIN21_PG_CONFIG,
				DOMAIN21_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN21_PG_STATUS,
				DOMAIN21_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	default:
		BREAK_TO_DEBUGGER();
		break;
	}

	if (org_ip_request_cntl == 0)
		REG_SET(DC_IP_REQUEST_CNTL, 0, IP_REQUEST_EN, 0);
}
#endif

static void dcn20_dpp_pg_control(
		struct dce_hwseq *hws,
		unsigned int dpp_inst,
		bool power_on)
{
	uint32_t power_gate = power_on ? 0 : 1;
	uint32_t pwr_status = power_on ? 0 : 2;

	if (hws->ctx->dc->debug.disable_dpp_power_gate)
		return;
	if (REG(DOMAIN1_PG_CONFIG) == 0)
		return;

	switch (dpp_inst) {
	case 0: /* DPP0 */
		REG_UPDATE(DOMAIN1_PG_CONFIG,
				DOMAIN1_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN1_PG_STATUS,
				DOMAIN1_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 1: /* DPP1 */
		REG_UPDATE(DOMAIN3_PG_CONFIG,
				DOMAIN3_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN3_PG_STATUS,
				DOMAIN3_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 2: /* DPP2 */
		REG_UPDATE(DOMAIN5_PG_CONFIG,
				DOMAIN5_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN5_PG_STATUS,
				DOMAIN5_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 3: /* DPP3 */
		REG_UPDATE(DOMAIN7_PG_CONFIG,
				DOMAIN7_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN7_PG_STATUS,
				DOMAIN7_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 4: /* DPP4 */
		REG_UPDATE(DOMAIN9_PG_CONFIG,
				DOMAIN9_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN9_PG_STATUS,
				DOMAIN9_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 5: /* DPP5 */
		/*
		 * Do not power gate DPP5, should be left at HW default, power on permanently.
		 * PG on Pipe5 is De-featured, attempting to put it to PG state may result in hard
		 * reset.
		 * REG_UPDATE(DOMAIN11_PG_CONFIG,
		 *		DOMAIN11_POWER_GATE, power_gate);
		 *
		 * REG_WAIT(DOMAIN11_PG_STATUS,
		 *		DOMAIN11_PGFSM_PWR_STATUS, pwr_status,
		 * 		1, 1000);
		 */
		break;
	default:
		BREAK_TO_DEBUGGER();
		break;
	}
}


static void dcn20_hubp_pg_control(
		struct dce_hwseq *hws,
		unsigned int hubp_inst,
		bool power_on)
{
	uint32_t power_gate = power_on ? 0 : 1;
	uint32_t pwr_status = power_on ? 0 : 2;

	if (hws->ctx->dc->debug.disable_hubp_power_gate)
		return;
	if (REG(DOMAIN0_PG_CONFIG) == 0)
		return;

	switch (hubp_inst) {
	case 0: /* DCHUBP0 */
		REG_UPDATE(DOMAIN0_PG_CONFIG,
				DOMAIN0_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN0_PG_STATUS,
				DOMAIN0_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 1: /* DCHUBP1 */
		REG_UPDATE(DOMAIN2_PG_CONFIG,
				DOMAIN2_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN2_PG_STATUS,
				DOMAIN2_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 2: /* DCHUBP2 */
		REG_UPDATE(DOMAIN4_PG_CONFIG,
				DOMAIN4_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN4_PG_STATUS,
				DOMAIN4_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 3: /* DCHUBP3 */
		REG_UPDATE(DOMAIN6_PG_CONFIG,
				DOMAIN6_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN6_PG_STATUS,
				DOMAIN6_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 4: /* DCHUBP4 */
		REG_UPDATE(DOMAIN8_PG_CONFIG,
				DOMAIN8_POWER_GATE, power_gate);

		REG_WAIT(DOMAIN8_PG_STATUS,
				DOMAIN8_PGFSM_PWR_STATUS, pwr_status,
				1, 1000);
		break;
	case 5: /* DCHUBP5 */
		/*
		 * Do not power gate DCHUB5, should be left at HW default, power on permanently.
		 * PG on Pipe5 is De-featured, attempting to put it to PG state may result in hard
		 * reset.
		 * REG_UPDATE(DOMAIN10_PG_CONFIG,
		 *		DOMAIN10_POWER_GATE, power_gate);
		 *
		 * REG_WAIT(DOMAIN10_PG_STATUS,
		 *		DOMAIN10_PGFSM_PWR_STATUS, pwr_status,
		 *		1, 1000);
		 */
		break;
	default:
		BREAK_TO_DEBUGGER();
		break;
	}
}


/* disable HW used by plane.
 * note:  cannot disable until disconnect is complete
 */
static void dcn20_plane_atomic_disable(struct dc *dc, struct pipe_ctx *pipe_ctx)
{
	struct hubp *hubp = pipe_ctx->plane_res.hubp;
	struct dpp *dpp = pipe_ctx->plane_res.dpp;

	dc->hwss.wait_for_mpcc_disconnect(dc, dc->res_pool, pipe_ctx);

	/* In flip immediate with pipe splitting case GSL is used for
	 * synchronization so we must disable it when the plane is disabled.
	 */
	if (pipe_ctx->stream_res.gsl_group != 0)
		dcn20_setup_gsl_group_as_lock(dc, pipe_ctx, false);

	dc->hwss.set_flip_control_gsl(pipe_ctx, false);

	hubp->funcs->hubp_clk_cntl(hubp, false);

	dpp->funcs->dpp_dppclk_control(dpp, false, false);

	hubp->power_gated = true;

	dc->hwss.plane_atomic_power_down(dc,
			pipe_ctx->plane_res.dpp,
			pipe_ctx->plane_res.hubp);

	pipe_ctx->stream = NULL;
	memset(&pipe_ctx->stream_res, 0, sizeof(pipe_ctx->stream_res));
	memset(&pipe_ctx->plane_res, 0, sizeof(pipe_ctx->plane_res));
	pipe_ctx->top_pipe = NULL;
	pipe_ctx->bottom_pipe = NULL;
	pipe_ctx->plane_state = NULL;
}


void dcn20_disable_plane(struct dc *dc, struct pipe_ctx *pipe_ctx)
{
	DC_LOGGER_INIT(dc->ctx->logger);

	if (!pipe_ctx->plane_res.hubp || pipe_ctx->plane_res.hubp->power_gated)
		return;

	dcn20_plane_atomic_disable(dc, pipe_ctx);

	DC_LOG_DC("Power down front end %d\n",
					pipe_ctx->pipe_idx);
}

enum dc_status dcn20_enable_stream_timing(
		struct pipe_ctx *pipe_ctx,
		struct dc_state *context,
		struct dc *dc)
{
	struct dc_stream_state *stream = pipe_ctx->stream;
	struct drr_params params = {0};
	unsigned int event_triggers = 0;
	struct pipe_ctx *odm_pipe;
	int opp_cnt = 1;
	int opp_inst[MAX_PIPES] = { pipe_ctx->stream_res.opp->inst };

	/* by upper caller loop, pipe0 is parent pipe and be called first.
	 * back end is set up by for pipe0. Other children pipe share back end
	 * with pipe 0. No program is needed.
	 */
	if (pipe_ctx->top_pipe != NULL)
		return DC_OK;

	/* TODO check if timing_changed, disable stream if timing changed */

	for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe) {
		opp_inst[opp_cnt] = odm_pipe->stream_res.opp->inst;
		opp_cnt++;
	}

	if (opp_cnt > 1)
		pipe_ctx->stream_res.tg->funcs->set_odm_combine(
				pipe_ctx->stream_res.tg,
				opp_inst, opp_cnt,
				&pipe_ctx->stream->timing);

	/* HW program guide assume display already disable
	 * by unplug sequence. OTG assume stop.
	 */
	pipe_ctx->stream_res.tg->funcs->enable_optc_clock(pipe_ctx->stream_res.tg, true);

	if (false == pipe_ctx->clock_source->funcs->program_pix_clk(
			pipe_ctx->clock_source,
			&pipe_ctx->stream_res.pix_clk_params,
			&pipe_ctx->pll_settings)) {
		BREAK_TO_DEBUGGER();
		return DC_ERROR_UNEXPECTED;
	}

	pipe_ctx->stream_res.tg->funcs->program_timing(
			pipe_ctx->stream_res.tg,
			&stream->timing,
			pipe_ctx->pipe_dlg_param.vready_offset,
			pipe_ctx->pipe_dlg_param.vstartup_start,
			pipe_ctx->pipe_dlg_param.vupdate_offset,
			pipe_ctx->pipe_dlg_param.vupdate_width,
			pipe_ctx->stream->signal,
			true);

	for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe)
		odm_pipe->stream_res.opp->funcs->opp_pipe_clock_control(
				odm_pipe->stream_res.opp,
				true);

	pipe_ctx->stream_res.opp->funcs->opp_pipe_clock_control(
			pipe_ctx->stream_res.opp,
			true);

	dc->hwss.blank_pixel_data(dc, pipe_ctx, true);

	/* VTG is  within DCHUB command block. DCFCLK is always on */
	if (false == pipe_ctx->stream_res.tg->funcs->enable_crtc(pipe_ctx->stream_res.tg)) {
		BREAK_TO_DEBUGGER();
		return DC_ERROR_UNEXPECTED;
	}

	dcn20_hwss_wait_for_blank_complete(pipe_ctx->stream_res.opp);

	params.vertical_total_min = stream->adjust.v_total_min;
	params.vertical_total_max = stream->adjust.v_total_max;
	params.vertical_total_mid = stream->adjust.v_total_mid;
	params.vertical_total_mid_frame_num = stream->adjust.v_total_mid_frame_num;
	if (pipe_ctx->stream_res.tg->funcs->set_drr)
		pipe_ctx->stream_res.tg->funcs->set_drr(
			pipe_ctx->stream_res.tg, &params);

	// DRR should set trigger event to monitor surface update event
	if (stream->adjust.v_total_min != 0 && stream->adjust.v_total_max != 0)
		event_triggers = 0x80;
	if (pipe_ctx->stream_res.tg->funcs->set_static_screen_control)
		pipe_ctx->stream_res.tg->funcs->set_static_screen_control(
				pipe_ctx->stream_res.tg, event_triggers);

	/* TODO program crtc source select for non-virtual signal*/
	/* TODO program FMT */
	/* TODO setup link_enc */
	/* TODO set stream attributes */
	/* TODO program audio */
	/* TODO enable stream if timing changed */
	/* TODO unblank stream if DP */

	return DC_OK;
}

void dcn20_program_output_csc(struct dc *dc,
		struct pipe_ctx *pipe_ctx,
		enum dc_color_space colorspace,
		uint16_t *matrix,
		int opp_id)
{
	struct mpc *mpc = dc->res_pool->mpc;
	enum mpc_output_csc_mode ocsc_mode = MPC_OUTPUT_CSC_COEF_A;
	int mpcc_id = pipe_ctx->plane_res.hubp->inst;

	if (mpc->funcs->power_on_mpc_mem_pwr)
		mpc->funcs->power_on_mpc_mem_pwr(mpc, mpcc_id, true);

	if (pipe_ctx->stream->csc_color_matrix.enable_adjustment == true) {
		if (mpc->funcs->set_output_csc != NULL)
			mpc->funcs->set_output_csc(mpc,
					opp_id,
					matrix,
					ocsc_mode);
	} else {
		if (mpc->funcs->set_ocsc_default != NULL)
			mpc->funcs->set_ocsc_default(mpc,
					opp_id,
					colorspace,
					ocsc_mode);
	}
}

bool dcn20_set_output_transfer_func(struct pipe_ctx *pipe_ctx,
				const struct dc_stream_state *stream)
{
	int mpcc_id = pipe_ctx->plane_res.hubp->inst;
	struct mpc *mpc = pipe_ctx->stream_res.opp->ctx->dc->res_pool->mpc;
	struct pwl_params *params = NULL;
	/*
	 * program OGAM only for the top pipe
	 * if there is a pipe split then fix diagnostic is required:
	 * how to pass OGAM parameter for stream.
	 * if programming for all pipes is required then remove condition
	 * pipe_ctx->top_pipe == NULL ,but then fix the diagnostic.
	 */
	if (mpc->funcs->power_on_mpc_mem_pwr)
		mpc->funcs->power_on_mpc_mem_pwr(mpc, mpcc_id, true);
	if (pipe_ctx->top_pipe == NULL
			&& mpc->funcs->set_output_gamma && stream->out_transfer_func) {
		if (stream->out_transfer_func->type == TF_TYPE_HWPWL)
			params = &stream->out_transfer_func->pwl;
		else if (pipe_ctx->stream->out_transfer_func->type ==
			TF_TYPE_DISTRIBUTED_POINTS &&
			cm_helper_translate_curve_to_hw_format(
			stream->out_transfer_func,
			&mpc->blender_params, false))
			params = &mpc->blender_params;
		/*
		 * there is no ROM
		 */
		if (stream->out_transfer_func->type == TF_TYPE_PREDEFINED)
			BREAK_TO_DEBUGGER();
	}
	/*
	 * if above if is not executed then 'params' equal to 0 and set in bypass
	 */
	mpc->funcs->set_output_gamma(mpc, mpcc_id, params);

	return true;
}

static bool dcn20_set_blend_lut(
	struct pipe_ctx *pipe_ctx, const struct dc_plane_state *plane_state)
{
	struct dpp *dpp_base = pipe_ctx->plane_res.dpp;
	bool result = true;
	struct pwl_params *blend_lut = NULL;

	if (plane_state->blend_tf) {
		if (plane_state->blend_tf->type == TF_TYPE_HWPWL)
			blend_lut = &plane_state->blend_tf->pwl;
		else if (plane_state->blend_tf->type == TF_TYPE_DISTRIBUTED_POINTS) {
			cm_helper_translate_curve_to_hw_format(
					plane_state->blend_tf,
					&dpp_base->regamma_params, false);
			blend_lut = &dpp_base->regamma_params;
		}
	}
	result = dpp_base->funcs->dpp_program_blnd_lut(dpp_base, blend_lut);

	return result;
}

static bool dcn20_set_shaper_3dlut(
	struct pipe_ctx *pipe_ctx, const struct dc_plane_state *plane_state)
{
	struct dpp *dpp_base = pipe_ctx->plane_res.dpp;
	bool result = true;
	struct pwl_params *shaper_lut = NULL;

	if (plane_state->in_shaper_func) {
		if (plane_state->in_shaper_func->type == TF_TYPE_HWPWL)
			shaper_lut = &plane_state->in_shaper_func->pwl;
		else if (plane_state->in_shaper_func->type == TF_TYPE_DISTRIBUTED_POINTS) {
			cm_helper_translate_curve_to_hw_format(
					plane_state->in_shaper_func,
					&dpp_base->shaper_params, true);
			shaper_lut = &dpp_base->shaper_params;
		}
	}

	result = dpp_base->funcs->dpp_program_shaper_lut(dpp_base, shaper_lut);
	if (plane_state->lut3d_func &&
		plane_state->lut3d_func->state.bits.initialized == 1)
		result = dpp_base->funcs->dpp_program_3dlut(dpp_base,
								&plane_state->lut3d_func->lut_3d);
	else
		result = dpp_base->funcs->dpp_program_3dlut(dpp_base, NULL);

	if (plane_state->lut3d_func &&
		plane_state->lut3d_func->state.bits.initialized == 1 &&
		plane_state->lut3d_func->hdr_multiplier != 0)
		dpp_base->funcs->dpp_set_hdr_multiplier(dpp_base,
				plane_state->lut3d_func->hdr_multiplier);
	else
		dpp_base->funcs->dpp_set_hdr_multiplier(dpp_base, 0x1f000);

	return result;
}

bool dcn20_set_input_transfer_func(struct pipe_ctx *pipe_ctx,
					  const struct dc_plane_state *plane_state)
{
	struct dpp *dpp_base = pipe_ctx->plane_res.dpp;
	const struct dc_transfer_func *tf = NULL;
	bool result = true;
	bool use_degamma_ram = false;

	if (dpp_base == NULL || plane_state == NULL)
		return false;

	dcn20_set_shaper_3dlut(pipe_ctx, plane_state);
	dcn20_set_blend_lut(pipe_ctx, plane_state);

	if (plane_state->in_transfer_func)
		tf = plane_state->in_transfer_func;


	if (tf == NULL) {
		dpp_base->funcs->dpp_set_degamma(dpp_base,
				IPP_DEGAMMA_MODE_BYPASS);
		return true;
	}

	if (tf->type == TF_TYPE_HWPWL || tf->type == TF_TYPE_DISTRIBUTED_POINTS)
		use_degamma_ram = true;

	if (use_degamma_ram == true) {
		if (tf->type == TF_TYPE_HWPWL)
			dpp_base->funcs->dpp_program_degamma_pwl(dpp_base,
					&tf->pwl);
		else if (tf->type == TF_TYPE_DISTRIBUTED_POINTS) {
			cm_helper_translate_curve_to_degamma_hw_format(tf,
					&dpp_base->degamma_params);
			dpp_base->funcs->dpp_program_degamma_pwl(dpp_base,
				&dpp_base->degamma_params);
		}
		return true;
	}
	/* handle here the optimized cases when de-gamma ROM could be used.
	 *
	 */
	if (tf->type == TF_TYPE_PREDEFINED) {
		switch (tf->tf) {
		case TRANSFER_FUNCTION_SRGB:
			dpp_base->funcs->dpp_set_degamma(dpp_base,
					IPP_DEGAMMA_MODE_HW_sRGB);
			break;
		case TRANSFER_FUNCTION_BT709:
			dpp_base->funcs->dpp_set_degamma(dpp_base,
					IPP_DEGAMMA_MODE_HW_xvYCC);
			break;
		case TRANSFER_FUNCTION_LINEAR:
			dpp_base->funcs->dpp_set_degamma(dpp_base,
					IPP_DEGAMMA_MODE_BYPASS);
			break;
		case TRANSFER_FUNCTION_PQ:
		default:
			result = false;
			break;
		}
	} else if (tf->type == TF_TYPE_BYPASS)
		dpp_base->funcs->dpp_set_degamma(dpp_base,
				IPP_DEGAMMA_MODE_BYPASS);
	else {
		/*
		 * if we are here, we did not handle correctly.
		 * fix is required for this use case
		 */
		BREAK_TO_DEBUGGER();
		dpp_base->funcs->dpp_set_degamma(dpp_base,
				IPP_DEGAMMA_MODE_BYPASS);
	}

	return result;
}

static void dcn20_update_odm(struct dc *dc, struct dc_state *context, struct pipe_ctx *pipe_ctx)
{
	struct pipe_ctx *odm_pipe;
	int opp_cnt = 1;
	int opp_inst[MAX_PIPES] = { pipe_ctx->stream_res.opp->inst };

	for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe) {
		opp_inst[opp_cnt] = odm_pipe->stream_res.opp->inst;
		opp_cnt++;
	}

	if (opp_cnt > 1)
		pipe_ctx->stream_res.tg->funcs->set_odm_combine(
				pipe_ctx->stream_res.tg,
				opp_inst, opp_cnt,
				&pipe_ctx->stream->timing);
	else
		pipe_ctx->stream_res.tg->funcs->set_odm_bypass(
				pipe_ctx->stream_res.tg, &pipe_ctx->stream->timing);
}

void dcn20_blank_pixel_data(
		struct dc *dc,
		struct pipe_ctx *pipe_ctx,
		bool blank)
{
	struct tg_color black_color = {0};
	struct stream_resource *stream_res = &pipe_ctx->stream_res;
	struct dc_stream_state *stream = pipe_ctx->stream;
	enum dc_color_space color_space = stream->output_color_space;
	enum controller_dp_test_pattern test_pattern = CONTROLLER_DP_TEST_PATTERN_SOLID_COLOR;
	struct pipe_ctx *odm_pipe;
	int odm_cnt = 1;

	int width = stream->timing.h_addressable + stream->timing.h_border_left + stream->timing.h_border_right;
	int height = stream->timing.v_addressable + stream->timing.v_border_bottom + stream->timing.v_border_top;

	/* get opp dpg blank color */
	color_space_to_black_color(dc, color_space, &black_color);

	for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe)
		odm_cnt++;

	width = width / odm_cnt;

	if (blank) {
		if (stream_res->abm)
			stream_res->abm->funcs->set_abm_immediate_disable(stream_res->abm);

		if (dc->debug.visual_confirm != VISUAL_CONFIRM_DISABLE)
			test_pattern = CONTROLLER_DP_TEST_PATTERN_COLORSQUARES;
	} else {
		test_pattern = CONTROLLER_DP_TEST_PATTERN_VIDEOMODE;
	}

	stream_res->opp->funcs->opp_set_disp_pattern_generator(
			stream_res->opp,
			test_pattern,
			stream->timing.display_color_depth,
			&black_color,
			width,
			height);

	for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe) {
		odm_pipe->stream_res.opp->funcs->opp_set_disp_pattern_generator(
				odm_pipe->stream_res.opp,
				dc->debug.visual_confirm != VISUAL_CONFIRM_DISABLE && blank ?
						CONTROLLER_DP_TEST_PATTERN_COLORRAMP : test_pattern,
				stream->timing.display_color_depth,
				&black_color,
				width,
				height);
	}

	if (!blank)
		if (stream_res->abm) {
			stream_res->abm->funcs->set_pipe(stream_res->abm, stream_res->tg->inst + 1);
			stream_res->abm->funcs->set_abm_level(stream_res->abm, stream->abm_level);
		}
}


static void dcn20_power_on_plane(
	struct dce_hwseq *hws,
	struct pipe_ctx *pipe_ctx)
{
	DC_LOGGER_INIT(hws->ctx->logger);
	if (REG(DC_IP_REQUEST_CNTL)) {
		REG_SET(DC_IP_REQUEST_CNTL, 0,
				IP_REQUEST_EN, 1);
		dcn20_dpp_pg_control(hws, pipe_ctx->plane_res.dpp->inst, true);
		dcn20_hubp_pg_control(hws, pipe_ctx->plane_res.hubp->inst, true);
		REG_SET(DC_IP_REQUEST_CNTL, 0,
				IP_REQUEST_EN, 0);
		DC_LOG_DEBUG(
				"Un-gated front end for pipe %d\n", pipe_ctx->plane_res.hubp->inst);
	}
}

void dcn20_enable_plane(
	struct dc *dc,
	struct pipe_ctx *pipe_ctx,
	struct dc_state *context)
{
	//if (dc->debug.sanity_checks) {
	//	dcn10_verify_allow_pstate_change_high(dc);
	//}
	dcn20_power_on_plane(dc->hwseq, pipe_ctx);

	/* enable DCFCLK current DCHUB */
	pipe_ctx->plane_res.hubp->funcs->hubp_clk_cntl(pipe_ctx->plane_res.hubp, true);

	/* initialize HUBP on power up */
	pipe_ctx->plane_res.hubp->funcs->hubp_init(pipe_ctx->plane_res.hubp);

	/* make sure OPP_PIPE_CLOCK_EN = 1 */
	pipe_ctx->stream_res.opp->funcs->opp_pipe_clock_control(
			pipe_ctx->stream_res.opp,
			true);

/* TODO: enable/disable in dm as per update type.
	if (plane_state) {
		DC_LOG_DC(dc->ctx->logger,
				"Pipe:%d 0x%x: addr hi:0x%x, "
				"addr low:0x%x, "
				"src: %d, %d, %d,"
				" %d; dst: %d, %d, %d, %d;\n",
				pipe_ctx->pipe_idx,
				plane_state,
				plane_state->address.grph.addr.high_part,
				plane_state->address.grph.addr.low_part,
				plane_state->src_rect.x,
				plane_state->src_rect.y,
				plane_state->src_rect.width,
				plane_state->src_rect.height,
				plane_state->dst_rect.x,
				plane_state->dst_rect.y,
				plane_state->dst_rect.width,
				plane_state->dst_rect.height);

		DC_LOG_DC(dc->ctx->logger,
				"Pipe %d: width, height, x, y         format:%d\n"
				"viewport:%d, %d, %d, %d\n"
				"recout:  %d, %d, %d, %d\n",
				pipe_ctx->pipe_idx,
				plane_state->format,
				pipe_ctx->plane_res.scl_data.viewport.width,
				pipe_ctx->plane_res.scl_data.viewport.height,
				pipe_ctx->plane_res.scl_data.viewport.x,
				pipe_ctx->plane_res.scl_data.viewport.y,
				pipe_ctx->plane_res.scl_data.recout.width,
				pipe_ctx->plane_res.scl_data.recout.height,
				pipe_ctx->plane_res.scl_data.recout.x,
				pipe_ctx->plane_res.scl_data.recout.y);
		print_rq_dlg_ttu(dc, pipe_ctx);
	}
*/
	if (dc->vm_pa_config.valid) {
		struct vm_system_aperture_param apt;

		apt.sys_default.quad_part = 0;

		apt.sys_low.quad_part = dc->vm_pa_config.system_aperture.start_addr;
		apt.sys_high.quad_part = dc->vm_pa_config.system_aperture.end_addr;

		// Program system aperture settings
		pipe_ctx->plane_res.hubp->funcs->hubp_set_vm_system_aperture_settings(pipe_ctx->plane_res.hubp, &apt);
	}

//	if (dc->debug.sanity_checks) {
//		dcn10_verify_allow_pstate_change_high(dc);
//	}
}


static void dcn20_program_pipe(
		struct dc *dc,
		struct pipe_ctx *pipe_ctx,
		struct dc_state *context)
{
	pipe_ctx->plane_state->update_flags.bits.full_update =
			context->commit_hints.full_update_needed ? 1 : pipe_ctx->plane_state->update_flags.bits.full_update;

	if (pipe_ctx->plane_state->update_flags.bits.full_update)
		dcn20_enable_plane(dc, pipe_ctx, context);

	update_dchubp_dpp(dc, pipe_ctx, context);

	set_hdr_multiplier(pipe_ctx);

	if (pipe_ctx->plane_state->update_flags.bits.full_update ||
			pipe_ctx->plane_state->update_flags.bits.in_transfer_func_change ||
			pipe_ctx->plane_state->update_flags.bits.gamma_change)
		dc->hwss.set_input_transfer_func(pipe_ctx, pipe_ctx->plane_state);

	/* dcn10_translate_regamma_to_hw_format takes 750us to finish
	 * only do gamma programming for full update.
	 * TODO: This can be further optimized/cleaned up
	 * Always call this for now since it does memcmp inside before
	 * doing heavy calculation and programming
	 */
	if (pipe_ctx->plane_state->update_flags.bits.full_update)
		dc->hwss.set_output_transfer_func(pipe_ctx, pipe_ctx->stream);
}

static void dcn20_program_all_pipe_in_tree(
		struct dc *dc,
		struct pipe_ctx *pipe_ctx,
		struct dc_state *context)
{
	if (pipe_ctx->top_pipe == NULL && !pipe_ctx->prev_odm_pipe) {
		bool blank = !is_pipe_tree_visible(pipe_ctx);

		pipe_ctx->stream_res.tg->funcs->program_global_sync(
				pipe_ctx->stream_res.tg,
				pipe_ctx->pipe_dlg_param.vready_offset,
				pipe_ctx->pipe_dlg_param.vstartup_start,
				pipe_ctx->pipe_dlg_param.vupdate_offset,
				pipe_ctx->pipe_dlg_param.vupdate_width);

		pipe_ctx->stream_res.tg->funcs->set_vtg_params(
				pipe_ctx->stream_res.tg, &pipe_ctx->stream->timing);

		dc->hwss.blank_pixel_data(dc, pipe_ctx, blank);

		if (dc->hwss.update_odm)
			dc->hwss.update_odm(dc, context, pipe_ctx);
	}

	if (pipe_ctx->plane_state != NULL)
		dcn20_program_pipe(dc, pipe_ctx, context);

	if (pipe_ctx->bottom_pipe != NULL) {
		ASSERT(pipe_ctx->bottom_pipe != pipe_ctx);
		dcn20_program_all_pipe_in_tree(dc, pipe_ctx->bottom_pipe, context);
	} else if (pipe_ctx->next_odm_pipe != NULL) {
		ASSERT(pipe_ctx->next_odm_pipe != pipe_ctx);
		dcn20_program_all_pipe_in_tree(dc, pipe_ctx->next_odm_pipe, context);
	}
}

void dcn20_pipe_control_lock_global(
		struct dc *dc,
		struct pipe_ctx *pipe,
		bool lock)
{
	if (lock) {
		pipe->stream_res.tg->funcs->lock_doublebuffer_enable(
				pipe->stream_res.tg);
		pipe->stream_res.tg->funcs->lock(pipe->stream_res.tg);
	} else {
		pipe->stream_res.tg->funcs->unlock(pipe->stream_res.tg);
		pipe->stream_res.tg->funcs->wait_for_state(pipe->stream_res.tg,
				CRTC_STATE_VACTIVE);
		pipe->stream_res.tg->funcs->wait_for_state(pipe->stream_res.tg,
				CRTC_STATE_VBLANK);
		pipe->stream_res.tg->funcs->wait_for_state(pipe->stream_res.tg,
				CRTC_STATE_VACTIVE);
		pipe->stream_res.tg->funcs->lock_doublebuffer_disable(
				pipe->stream_res.tg);
	}
}

void dcn20_pipe_control_lock(
	struct dc *dc,
	struct pipe_ctx *pipe,
	bool lock)
{
	bool flip_immediate = false;

	/* use TG master update lock to lock everything on the TG
	 * therefore only top pipe need to lock
	 */
	if (pipe->top_pipe)
		return;

	if (pipe->plane_state != NULL)
		flip_immediate = pipe->plane_state->flip_immediate;

	if (flip_immediate && lock) {
		const int TIMEOUT_FOR_FLIP_PENDING = 100000;
		int i;

		for (i = 0; i < TIMEOUT_FOR_FLIP_PENDING; ++i) {
			if (!pipe->plane_res.hubp->funcs->hubp_is_flip_pending(pipe->plane_res.hubp))
				break;
			udelay(1);
		}

		if (pipe->bottom_pipe != NULL) {
			for (i = 0; i < TIMEOUT_FOR_FLIP_PENDING; ++i) {
				if (!pipe->bottom_pipe->plane_res.hubp->funcs->hubp_is_flip_pending(pipe->bottom_pipe->plane_res.hubp))
					break;
				udelay(1);
			}
		}
	}

	/* In flip immediate and pipe splitting case, we need to use GSL
	 * for synchronization. Only do setup on locking and on flip type change.
	 */
	if (lock && pipe->bottom_pipe != NULL)
		if ((flip_immediate && pipe->stream_res.gsl_group == 0) ||
		    (!flip_immediate && pipe->stream_res.gsl_group > 0))
			dcn20_setup_gsl_group_as_lock(dc, pipe, flip_immediate);

	if (pipe->plane_state != NULL && pipe->plane_state->triplebuffer_flips) {
		if (lock)
			pipe->stream_res.tg->funcs->triplebuffer_lock(pipe->stream_res.tg);
		else
			pipe->stream_res.tg->funcs->triplebuffer_unlock(pipe->stream_res.tg);
	} else {
		if (lock)
			pipe->stream_res.tg->funcs->lock(pipe->stream_res.tg);
		else
			pipe->stream_res.tg->funcs->unlock(pipe->stream_res.tg);
	}
}

static void dcn20_apply_ctx_for_surface(
		struct dc *dc,
		const struct dc_stream_state *stream,
		int num_planes,
		struct dc_state *context)
{
	const unsigned int TIMEOUT_FOR_PIPE_ENABLE_MS = 100;
	int i;
	struct timing_generator *tg;
	bool removed_pipe[6] = { false };
	bool interdependent_update = false;
	struct pipe_ctx *top_pipe_to_program =
			find_top_pipe_for_stream(dc, context, stream);
	struct pipe_ctx *prev_top_pipe_to_program =
			find_top_pipe_for_stream(dc, dc->current_state, stream);
	DC_LOGGER_INIT(dc->ctx->logger);

	if (!top_pipe_to_program)
		return;

	/* Carry over GSL groups in case the context is changing. */
	for (i = 0; i < dc->res_pool->pipe_count; i++) {
		struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[i];
		struct pipe_ctx *old_pipe_ctx =
			&dc->current_state->res_ctx.pipe_ctx[i];

		if (pipe_ctx->stream == stream &&
		    pipe_ctx->stream == old_pipe_ctx->stream)
			pipe_ctx->stream_res.gsl_group =
				old_pipe_ctx->stream_res.gsl_group;
	}

	tg = top_pipe_to_program->stream_res.tg;

	interdependent_update = top_pipe_to_program->plane_state &&
		top_pipe_to_program->plane_state->update_flags.bits.full_update;

	if (interdependent_update)
		lock_all_pipes(dc, context, true);
	else
		dcn20_pipe_control_lock(dc, top_pipe_to_program, true);

	if (num_planes == 0) {
		/* OTG blank before remove all front end */
		dc->hwss.blank_pixel_data(dc, top_pipe_to_program, true);
	}

	/* Disconnect unused mpcc */
	for (i = 0; i < dc->res_pool->pipe_count; i++) {
		struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[i];
		struct pipe_ctx *old_pipe_ctx =
				&dc->current_state->res_ctx.pipe_ctx[i];
		/*
		 * Powergate reused pipes that are not powergated
		 * fairly hacky right now, using opp_id as indicator
		 * TODO: After move dc_post to dc_update, this will
		 * be removed.
		 */
		if (pipe_ctx->plane_state && !old_pipe_ctx->plane_state) {
			if (old_pipe_ctx->stream_res.tg == tg &&
			    old_pipe_ctx->plane_res.hubp &&
			    old_pipe_ctx->plane_res.hubp->opp_id != OPP_ID_INVALID)
				dc->hwss.disable_plane(dc, old_pipe_ctx);
		}

		if ((!pipe_ctx->plane_state ||
		     pipe_ctx->stream_res.tg != old_pipe_ctx->stream_res.tg) &&
		     old_pipe_ctx->plane_state &&
		     old_pipe_ctx->stream_res.tg == tg) {

			dc->hwss.plane_atomic_disconnect(dc, old_pipe_ctx);
			removed_pipe[i] = true;

			DC_LOG_DC("Reset mpcc for pipe %d\n",
					old_pipe_ctx->pipe_idx);
		}
	}

	if (num_planes > 0)
		dcn20_program_all_pipe_in_tree(dc, top_pipe_to_program, context);

	/* Program secondary blending tree and writeback pipes */
	if ((stream->num_wb_info > 0) && (dc->hwss.program_all_writeback_pipes_in_tree))
		dc->hwss.program_all_writeback_pipes_in_tree(dc, stream, context);

	if (interdependent_update)
		for (i = 0; i < dc->res_pool->pipe_count; i++) {
			struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[i];

			/* Skip inactive pipes and ones already updated */
			if (!pipe_ctx->stream || pipe_ctx->stream == stream ||
			    !pipe_ctx->plane_state || !tg->funcs->is_tg_enabled(tg))
				continue;

			pipe_ctx->plane_res.hubp->funcs->hubp_setup_interdependent(
				pipe_ctx->plane_res.hubp,
				&pipe_ctx->dlg_regs,
				&pipe_ctx->ttu_regs);
		}

	if (interdependent_update)
		lock_all_pipes(dc, context, false);
	else
		dcn20_pipe_control_lock(dc, top_pipe_to_program, false);

	for (i = 0; i < dc->res_pool->pipe_count; i++)
		if (removed_pipe[i])
			dcn20_disable_plane(dc, &dc->current_state->res_ctx.pipe_ctx[i]);

	/*
	 * If we are enabling a pipe, we need to wait for pending clear as this is a critical
	 * part of the enable operation otherwise, DM may request an immediate flip which
	 * will cause HW to perform an "immediate enable" (as opposed to "vsync enable") which
	 * is unsupported on DCN.
	 */
	i = 0;
	if (num_planes > 0 && top_pipe_to_program &&
			(prev_top_pipe_to_program == NULL || prev_top_pipe_to_program->plane_state == NULL)) {
		while (i < TIMEOUT_FOR_PIPE_ENABLE_MS &&
				top_pipe_to_program->plane_res.hubp->funcs->hubp_is_flip_pending(top_pipe_to_program->plane_res.hubp)) {
			i += 1;
			msleep(1);
		}
	}
}


void dcn20_prepare_bandwidth(
		struct dc *dc,
		struct dc_state *context)
{
	struct hubbub *hubbub = dc->res_pool->hubbub;

	dc->clk_mgr->funcs->update_clocks(
			dc->clk_mgr,
			context,
			false);

	/* program dchubbub watermarks */
	hubbub->funcs->program_watermarks(hubbub,
					&context->bw_ctx.bw.dcn.watermarks,
					dc->res_pool->ref_clocks.dchub_ref_clock_inKhz / 1000,
					false);
}

void dcn20_optimize_bandwidth(
		struct dc *dc,
		struct dc_state *context)
{
	struct hubbub *hubbub = dc->res_pool->hubbub;

	/* program dchubbub watermarks */
	hubbub->funcs->program_watermarks(hubbub,
					&context->bw_ctx.bw.dcn.watermarks,
					dc->res_pool->ref_clocks.dchub_ref_clock_inKhz / 1000,
					true);

	dc->clk_mgr->funcs->update_clocks(
			dc->clk_mgr,
			context,
			true);
}

bool dcn20_update_bandwidth(
		struct dc *dc,
		struct dc_state *context)
{
	int i;

	/* recalculate DML parameters */
	if (!dc->res_pool->funcs->validate_bandwidth(dc, context, false))
		return false;

	/* apply updated bandwidth parameters */
	dc->hwss.prepare_bandwidth(dc, context);

	/* update hubp configs for all pipes */
	for (i = 0; i < dc->res_pool->pipe_count; i++) {
		struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[i];

		if (pipe_ctx->plane_state == NULL)
			continue;

		if (pipe_ctx->top_pipe == NULL) {
			bool blank = !is_pipe_tree_visible(pipe_ctx);

			pipe_ctx->stream_res.tg->funcs->program_global_sync(
					pipe_ctx->stream_res.tg,
					pipe_ctx->pipe_dlg_param.vready_offset,
					pipe_ctx->pipe_dlg_param.vstartup_start,
					pipe_ctx->pipe_dlg_param.vupdate_offset,
					pipe_ctx->pipe_dlg_param.vupdate_width);

			pipe_ctx->stream_res.tg->funcs->set_vtg_params(
					pipe_ctx->stream_res.tg, &pipe_ctx->stream->timing);
			if (pipe_ctx->prev_odm_pipe == NULL)
				dc->hwss.blank_pixel_data(dc, pipe_ctx, blank);
		}

		pipe_ctx->plane_res.hubp->funcs->hubp_setup(
				pipe_ctx->plane_res.hubp,
					&pipe_ctx->dlg_regs,
					&pipe_ctx->ttu_regs,
					&pipe_ctx->rq_regs,
					&pipe_ctx->pipe_dlg_param);
	}

	return true;
}

static void dcn20_enable_writeback(
		struct dc *dc,
		const struct dc_stream_status *stream_status,
		struct dc_writeback_info *wb_info,
		struct dc_state *context)
{
	struct dwbc *dwb;
	struct mcif_wb *mcif_wb;
	struct timing_generator *optc;

	ASSERT(wb_info->dwb_pipe_inst < MAX_DWB_PIPES);
	ASSERT(wb_info->wb_enabled);
	dwb = dc->res_pool->dwbc[wb_info->dwb_pipe_inst];
	mcif_wb = dc->res_pool->mcif_wb[wb_info->dwb_pipe_inst];

	/* set the OPTC source mux */
	ASSERT(stream_status->primary_otg_inst < MAX_PIPES);
	optc = dc->res_pool->timing_generators[stream_status->primary_otg_inst];
	optc->funcs->set_dwb_source(optc, wb_info->dwb_pipe_inst);
	/* set MCIF_WB buffer and arbitration configuration */
	mcif_wb->funcs->config_mcif_buf(mcif_wb, &wb_info->mcif_buf_params, wb_info->dwb_params.dest_height);
	mcif_wb->funcs->config_mcif_arb(mcif_wb, &context->bw_ctx.bw.dcn.bw_writeback.mcif_wb_arb[wb_info->dwb_pipe_inst]);
	/* Enable MCIF_WB */
	mcif_wb->funcs->enable_mcif(mcif_wb);
	/* Enable DWB */
	dwb->funcs->enable(dwb, &wb_info->dwb_params);
	/* TODO: add sequence to enable/disable warmup */
}

void dcn20_disable_writeback(
		struct dc *dc,
		unsigned int dwb_pipe_inst)
{
	struct dwbc *dwb;
	struct mcif_wb *mcif_wb;

	ASSERT(dwb_pipe_inst < MAX_DWB_PIPES);
	dwb = dc->res_pool->dwbc[dwb_pipe_inst];
	mcif_wb = dc->res_pool->mcif_wb[dwb_pipe_inst];

	dwb->funcs->disable(dwb);
	mcif_wb->funcs->disable_mcif(mcif_wb);
}

bool dcn20_hwss_wait_for_blank_complete(
		struct output_pixel_processor *opp)
{
	int counter;

	for (counter = 0; counter < 1000; counter++) {
		if (opp->funcs->dpg_is_blanked(opp))
			break;

		udelay(100);
	}

	if (counter == 1000) {
		dm_error("DC: failed to blank crtc!\n");
		return false;
	}

	return true;
}

bool dcn20_dmdata_status_done(struct pipe_ctx *pipe_ctx)
{
	struct hubp *hubp = pipe_ctx->plane_res.hubp;

	if (!hubp)
		return false;
	return hubp->funcs->dmdata_status_done(hubp);
}

static void dcn20_disable_stream_gating(struct dc *dc, struct pipe_ctx *pipe_ctx)
{
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	struct dce_hwseq *hws = dc->hwseq;

	if (pipe_ctx->stream_res.dsc) {
		struct pipe_ctx *odm_pipe = pipe_ctx->next_odm_pipe;

		dcn20_dsc_pg_control(hws, pipe_ctx->stream_res.dsc->inst, true);
		while (odm_pipe) {
			dcn20_dsc_pg_control(hws, odm_pipe->stream_res.dsc->inst, true);
			odm_pipe = odm_pipe->next_odm_pipe;
		}
	}
#endif
}

static void dcn20_enable_stream_gating(struct dc *dc, struct pipe_ctx *pipe_ctx)
{
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	struct dce_hwseq *hws = dc->hwseq;

	if (pipe_ctx->stream_res.dsc) {
		struct pipe_ctx *odm_pipe = pipe_ctx->next_odm_pipe;

		dcn20_dsc_pg_control(hws, pipe_ctx->stream_res.dsc->inst, false);
		while (odm_pipe) {
			dcn20_dsc_pg_control(hws, odm_pipe->stream_res.dsc->inst, false);
			odm_pipe = odm_pipe->next_odm_pipe;
		}
	}
#endif
}

void dcn20_set_dmdata_attributes(struct pipe_ctx *pipe_ctx)
{
	struct dc_dmdata_attributes attr = { 0 };
	struct hubp *hubp = pipe_ctx->plane_res.hubp;

	attr.dmdata_mode = DMDATA_HW_MODE;
	attr.dmdata_size =
		dc_is_hdmi_signal(pipe_ctx->stream->signal) ? 32 : 36;
	attr.address.quad_part =
			pipe_ctx->stream->dmdata_address.quad_part;
	attr.dmdata_dl_delta = 0;
	attr.dmdata_qos_mode = 0;
	attr.dmdata_qos_level = 0;
	attr.dmdata_repeat = 1; /* always repeat */
	attr.dmdata_updated = 1;
	attr.dmdata_sw_data = NULL;

	hubp->funcs->dmdata_set_attributes(hubp, &attr);
}

void dcn20_disable_stream(struct pipe_ctx *pipe_ctx)
{
	dce110_disable_stream(pipe_ctx);
}

static void dcn20_init_vm_ctx(
		struct dce_hwseq *hws,
		struct dc *dc,
		struct dc_virtual_addr_space_config *va_config,
		int vmid)
{
	struct dcn_hubbub_virt_addr_config config;

	if (vmid == 0) {
		ASSERT(0); /* VMID cannot be 0 for vm context */
		return;
	}

	config.page_table_start_addr = va_config->page_table_start_addr;
	config.page_table_end_addr = va_config->page_table_end_addr;
	config.page_table_block_size = va_config->page_table_block_size_in_bytes;
	config.page_table_depth = va_config->page_table_depth;
	config.page_table_base_addr = va_config->page_table_base_addr;

	dc->res_pool->hubbub->funcs->init_vm_ctx(dc->res_pool->hubbub, &config, vmid);
}

static int dcn20_init_sys_ctx(struct dce_hwseq *hws, struct dc *dc, struct dc_phy_addr_space_config *pa_config)
{
	struct dcn_hubbub_phys_addr_config config;

	config.system_aperture.fb_top = pa_config->system_aperture.fb_top;
	config.system_aperture.fb_offset = pa_config->system_aperture.fb_offset;
	config.system_aperture.fb_base = pa_config->system_aperture.fb_base;
	config.system_aperture.agp_top = pa_config->system_aperture.agp_top;
	config.system_aperture.agp_bot = pa_config->system_aperture.agp_bot;
	config.system_aperture.agp_base = pa_config->system_aperture.agp_base;
	config.gart_config.page_table_start_addr = pa_config->gart_config.page_table_start_addr;
	config.gart_config.page_table_end_addr = pa_config->gart_config.page_table_end_addr;
	config.gart_config.page_table_base_addr = pa_config->gart_config.page_table_base_addr;
	config.page_table_default_page_addr = pa_config->page_table_default_page_addr;

	return dc->res_pool->hubbub->funcs->init_dchub_sys_ctx(dc->res_pool->hubbub, &config);
}

static bool patch_address_for_sbs_tb_stereo(
		struct pipe_ctx *pipe_ctx, PHYSICAL_ADDRESS_LOC *addr)
{
	struct dc_plane_state *plane_state = pipe_ctx->plane_state;
	bool sec_split = pipe_ctx->top_pipe &&
			pipe_ctx->top_pipe->plane_state == pipe_ctx->plane_state;
	if (sec_split && plane_state->address.type == PLN_ADDR_TYPE_GRPH_STEREO &&
			(pipe_ctx->stream->timing.timing_3d_format ==
			TIMING_3D_FORMAT_SIDE_BY_SIDE ||
			pipe_ctx->stream->timing.timing_3d_format ==
			TIMING_3D_FORMAT_TOP_AND_BOTTOM)) {
		*addr = plane_state->address.grph_stereo.left_addr;
		plane_state->address.grph_stereo.left_addr =
				plane_state->address.grph_stereo.right_addr;
		return true;
	}

	if (pipe_ctx->stream->view_format != VIEW_3D_FORMAT_NONE &&
			plane_state->address.type != PLN_ADDR_TYPE_GRPH_STEREO) {
		plane_state->address.type = PLN_ADDR_TYPE_GRPH_STEREO;
		plane_state->address.grph_stereo.right_addr =
				plane_state->address.grph_stereo.left_addr;
	}
	return false;
}


static void dcn20_update_plane_addr(const struct dc *dc, struct pipe_ctx *pipe_ctx)
{
	bool addr_patched = false;
	PHYSICAL_ADDRESS_LOC addr;
	struct dc_plane_state *plane_state = pipe_ctx->plane_state;

	if (plane_state == NULL)
		return;

	addr_patched = patch_address_for_sbs_tb_stereo(pipe_ctx, &addr);

	// Call Helper to track VMID use
	vm_helper_mark_vmid_used(dc->vm_helper, plane_state->address.vmid, pipe_ctx->plane_res.hubp->inst);

	pipe_ctx->plane_res.hubp->funcs->hubp_program_surface_flip_and_addr(
			pipe_ctx->plane_res.hubp,
			&plane_state->address,
			plane_state->flip_immediate);

	plane_state->status.requested_address = plane_state->address;

	if (plane_state->flip_immediate)
		plane_state->status.current_address = plane_state->address;

	if (addr_patched)
		pipe_ctx->plane_state->address.grph_stereo.left_addr = addr;
}

void dcn20_unblank_stream(struct pipe_ctx *pipe_ctx,
		struct dc_link_settings *link_settings)
{
	struct encoder_unblank_param params = { { 0 } };
	struct dc_stream_state *stream = pipe_ctx->stream;
	struct dc_link *link = stream->link;
	struct pipe_ctx *odm_pipe;

	params.opp_cnt = 1;
	for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe) {
		params.opp_cnt++;
	}
	/* only 3 items below are used by unblank */
	params.timing = pipe_ctx->stream->timing;

	params.link_settings.link_rate = link_settings->link_rate;

	if (dc_is_dp_signal(pipe_ctx->stream->signal)) {
		if (optc1_is_two_pixels_per_containter(&stream->timing) || params.opp_cnt > 1)
			params.timing.pix_clk_100hz /= 2;
		pipe_ctx->stream_res.stream_enc->funcs->dp_set_odm_combine(
				pipe_ctx->stream_res.stream_enc, params.opp_cnt > 1);
		pipe_ctx->stream_res.stream_enc->funcs->dp_unblank(pipe_ctx->stream_res.stream_enc, &params);
	}

	if (link->local_sink && link->local_sink->sink_signal == SIGNAL_TYPE_EDP) {
		link->dc->hwss.edp_backlight_control(link, true);
	}
}

void dcn20_setup_vupdate_interrupt(struct pipe_ctx *pipe_ctx)
{
	struct timing_generator *tg = pipe_ctx->stream_res.tg;
	int start_line = get_vupdate_offset_from_vsync(pipe_ctx);

	if (start_line < 0)
		start_line = 0;

	if (tg->funcs->setup_vertical_interrupt2)
		tg->funcs->setup_vertical_interrupt2(tg, start_line);
}

static void dcn20_reset_back_end_for_pipe(
		struct dc *dc,
		struct pipe_ctx *pipe_ctx,
		struct dc_state *context)
{
	int i;
	DC_LOGGER_INIT(dc->ctx->logger);
	if (pipe_ctx->stream_res.stream_enc == NULL) {
		pipe_ctx->stream = NULL;
		return;
	}

	if (!IS_FPGA_MAXIMUS_DC(dc->ctx->dce_environment)) {
		/* DPMS may already disable */
		if (!pipe_ctx->stream->dpms_off)
			core_link_disable_stream(pipe_ctx);
		else if (pipe_ctx->stream_res.audio)
			dc->hwss.disable_audio_stream(pipe_ctx);

		/* free acquired resources */
		if (pipe_ctx->stream_res.audio) {
			/*disable az_endpoint*/
			pipe_ctx->stream_res.audio->funcs->az_disable(pipe_ctx->stream_res.audio);

			/*free audio*/
			if (dc->caps.dynamic_audio == true) {
				/*we have to dynamic arbitrate the audio endpoints*/
				/*we free the resource, need reset is_audio_acquired*/
				update_audio_usage(&dc->current_state->res_ctx, dc->res_pool,
						pipe_ctx->stream_res.audio, false);
				pipe_ctx->stream_res.audio = NULL;
			}
		}
	}
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	else if (pipe_ctx->stream_res.dsc) {
		dp_set_dsc_enable(pipe_ctx, false);
	}
#endif

	/* by upper caller loop, parent pipe: pipe0, will be reset last.
	 * back end share by all pipes and will be disable only when disable
	 * parent pipe.
	 */
	if (pipe_ctx->top_pipe == NULL) {
		pipe_ctx->stream_res.tg->funcs->disable_crtc(pipe_ctx->stream_res.tg);

		pipe_ctx->stream_res.tg->funcs->enable_optc_clock(pipe_ctx->stream_res.tg, false);
		if (pipe_ctx->stream_res.tg->funcs->set_odm_bypass)
			pipe_ctx->stream_res.tg->funcs->set_odm_bypass(
					pipe_ctx->stream_res.tg, &pipe_ctx->stream->timing);

		if (pipe_ctx->stream_res.tg->funcs->set_drr)
			pipe_ctx->stream_res.tg->funcs->set_drr(
					pipe_ctx->stream_res.tg, NULL);
	}

	for (i = 0; i < dc->res_pool->pipe_count; i++)
		if (&dc->current_state->res_ctx.pipe_ctx[i] == pipe_ctx)
			break;

	if (i == dc->res_pool->pipe_count)
		return;

	pipe_ctx->stream = NULL;
	DC_LOG_DEBUG("Reset back end for pipe %d, tg:%d\n",
					pipe_ctx->pipe_idx, pipe_ctx->stream_res.tg->inst);
}

static void dcn20_reset_hw_ctx_wrap(
		struct dc *dc,
		struct dc_state *context)
{
	int i;

	/* Reset Back End*/
	for (i = dc->res_pool->pipe_count - 1; i >= 0 ; i--) {
		struct pipe_ctx *pipe_ctx_old =
			&dc->current_state->res_ctx.pipe_ctx[i];
		struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[i];

		if (!pipe_ctx_old->stream)
			continue;

		if (pipe_ctx_old->top_pipe || pipe_ctx_old->prev_odm_pipe)
			continue;

		if (!pipe_ctx->stream ||
				pipe_need_reprogram(pipe_ctx_old, pipe_ctx)) {
			struct clock_source *old_clk = pipe_ctx_old->clock_source;

			dcn20_reset_back_end_for_pipe(dc, pipe_ctx_old, dc->current_state);
			if (dc->hwss.enable_stream_gating)
				dc->hwss.enable_stream_gating(dc, pipe_ctx);
			if (old_clk)
				old_clk->funcs->cs_power_down(old_clk);
		}
	}
}

static void dcn20_update_mpcc(struct dc *dc, struct pipe_ctx *pipe_ctx)
{
	struct hubp *hubp = pipe_ctx->plane_res.hubp;
	struct mpcc_blnd_cfg blnd_cfg = { {0} };
	bool per_pixel_alpha = pipe_ctx->plane_state->per_pixel_alpha;
	int mpcc_id;
	struct mpcc *new_mpcc;
	struct mpc *mpc = dc->res_pool->mpc;
	struct mpc_tree *mpc_tree_params = &(pipe_ctx->stream_res.opp->mpc_tree_params);

	// input to MPCC is always RGB, by default leave black_color at 0
	if (dc->debug.visual_confirm == VISUAL_CONFIRM_HDR) {
		dcn10_get_hdr_visual_confirm_color(
				pipe_ctx, &blnd_cfg.black_color);
	} else if (dc->debug.visual_confirm == VISUAL_CONFIRM_SURFACE) {
		dcn10_get_surface_visual_confirm_color(
				pipe_ctx, &blnd_cfg.black_color);
	}

	if (per_pixel_alpha)
		blnd_cfg.alpha_mode = MPCC_ALPHA_BLEND_MODE_PER_PIXEL_ALPHA;
	else
		blnd_cfg.alpha_mode = MPCC_ALPHA_BLEND_MODE_GLOBAL_ALPHA;

	blnd_cfg.overlap_only = false;
	blnd_cfg.global_gain = 0xff;

	if (pipe_ctx->plane_state->global_alpha)
		blnd_cfg.global_alpha = pipe_ctx->plane_state->global_alpha_value;
	else
		blnd_cfg.global_alpha = 0xff;

	blnd_cfg.background_color_bpc = 4;
	blnd_cfg.bottom_gain_mode = 0;
	blnd_cfg.top_gain = 0x1f000;
	blnd_cfg.bottom_inside_gain = 0x1f000;
	blnd_cfg.bottom_outside_gain = 0x1f000;
	blnd_cfg.pre_multiplied_alpha = per_pixel_alpha;

	/*
	 * TODO: remove hack
	 * Note: currently there is a bug in init_hw such that
	 * on resume from hibernate, BIOS sets up MPCC0, and
	 * we do mpcc_remove but the mpcc cannot go to idle
	 * after remove. This cause us to pick mpcc1 here,
	 * which causes a pstate hang for yet unknown reason.
	 */
	mpcc_id = hubp->inst;

	/* If there is no full update, don't need to touch MPC tree*/
	if (!pipe_ctx->plane_state->update_flags.bits.full_update) {
		mpc->funcs->update_blending(mpc, &blnd_cfg, mpcc_id);
		return;
	}

	/* check if this MPCC is already being used */
	new_mpcc = mpc->funcs->get_mpcc_for_dpp(mpc_tree_params, mpcc_id);
	/* remove MPCC if being used */
	if (new_mpcc != NULL)
		mpc->funcs->remove_mpcc(mpc, mpc_tree_params, new_mpcc);
	else
		if (dc->debug.sanity_checks)
			mpc->funcs->assert_mpcc_idle_before_connect(
					dc->res_pool->mpc, mpcc_id);

	/* Call MPC to insert new plane */
	new_mpcc = mpc->funcs->insert_plane(dc->res_pool->mpc,
			mpc_tree_params,
			&blnd_cfg,
			NULL,
			NULL,
			hubp->inst,
			mpcc_id);

	ASSERT(new_mpcc != NULL);
	hubp->opp_id = pipe_ctx->stream_res.opp->inst;
	hubp->mpcc_id = mpcc_id;
}

static int find_free_gsl_group(const struct dc *dc)
{
	if (dc->res_pool->gsl_groups.gsl_0 == 0)
		return 1;
	if (dc->res_pool->gsl_groups.gsl_1 == 0)
		return 2;
	if (dc->res_pool->gsl_groups.gsl_2 == 0)
		return 3;

	return 0;
}

/* NOTE: This is not a generic setup_gsl function (hence the suffix as_lock)
 * This is only used to lock pipes in pipe splitting case with immediate flip
 * Ordinary MPC/OTG locks suppress VUPDATE which doesn't help with immediate,
 * so we get tearing with freesync since we cannot flip multiple pipes
 * atomically.
 * We use GSL for this:
 * - immediate flip: find first available GSL group if not already assigned
 *                   program gsl with that group, set current OTG as master
 *                   and always us 0x4 = AND of flip_ready from all pipes
 * - vsync flip: disable GSL if used
 *
 * Groups in stream_res are stored as +1 from HW registers, i.e.
 * gsl_0 <=> pipe_ctx->stream_res.gsl_group == 1
 * Using a magic value like -1 would require tracking all inits/resets
 */
void dcn20_setup_gsl_group_as_lock(
		const struct dc *dc,
		struct pipe_ctx *pipe_ctx,
		bool enable)
{
	struct gsl_params gsl;
	int group_idx;

	memset(&gsl, 0, sizeof(struct gsl_params));

	if (enable) {
		/* return if group already assigned since GSL was set up
		 * for vsync flip, we would unassign so it can't be "left over"
		 */
		if (pipe_ctx->stream_res.gsl_group > 0)
			return;

		group_idx = find_free_gsl_group(dc);
		ASSERT(group_idx != 0);
		pipe_ctx->stream_res.gsl_group = group_idx;

		/* set gsl group reg field and mark resource used */
		switch (group_idx) {
		case 1:
			gsl.gsl0_en = 1;
			dc->res_pool->gsl_groups.gsl_0 = 1;
			break;
		case 2:
			gsl.gsl1_en = 1;
			dc->res_pool->gsl_groups.gsl_1 = 1;
			break;
		case 3:
			gsl.gsl2_en = 1;
			dc->res_pool->gsl_groups.gsl_2 = 1;
			break;
		default:
			BREAK_TO_DEBUGGER();
			return; // invalid case
		}
		gsl.gsl_master_en = 1;
	} else {
		group_idx = pipe_ctx->stream_res.gsl_group;
		if (group_idx == 0)
			return; // if not in use, just return

		pipe_ctx->stream_res.gsl_group = 0;

		/* unset gsl group reg field and mark resource free */
		switch (group_idx) {
		case 1:
			gsl.gsl0_en = 0;
			dc->res_pool->gsl_groups.gsl_0 = 0;
			break;
		case 2:
			gsl.gsl1_en = 0;
			dc->res_pool->gsl_groups.gsl_1 = 0;
			break;
		case 3:
			gsl.gsl2_en = 0;
			dc->res_pool->gsl_groups.gsl_2 = 0;
			break;
		default:
			BREAK_TO_DEBUGGER();
			return;
		}
		gsl.gsl_master_en = 0;
	}

	/* at this point we want to program whether it's to enable or disable */
	if (pipe_ctx->stream_res.tg->funcs->set_gsl != NULL &&
		pipe_ctx->stream_res.tg->funcs->set_gsl_source_select != NULL) {
		pipe_ctx->stream_res.tg->funcs->set_gsl(
			pipe_ctx->stream_res.tg,
			&gsl);

		pipe_ctx->stream_res.tg->funcs->set_gsl_source_select(
			pipe_ctx->stream_res.tg, group_idx,	enable ? 4 : 0);
	} else
		BREAK_TO_DEBUGGER();
}

static void dcn20_set_flip_control_gsl(
		struct pipe_ctx *pipe_ctx,
		bool flip_immediate)
{
	if (pipe_ctx && pipe_ctx->plane_res.hubp->funcs->hubp_set_flip_control_surface_gsl)
		pipe_ctx->plane_res.hubp->funcs->hubp_set_flip_control_surface_gsl(
				pipe_ctx->plane_res.hubp, flip_immediate);

}

static void dcn20_enable_stream(struct pipe_ctx *pipe_ctx)
{
	enum dc_lane_count lane_count =
		pipe_ctx->stream->link->cur_link_settings.lane_count;

	struct dc_crtc_timing *timing = &pipe_ctx->stream->timing;
	struct dc_link *link = pipe_ctx->stream->link;

	uint32_t active_total_with_borders;
	uint32_t early_control = 0;
	struct timing_generator *tg = pipe_ctx->stream_res.tg;

	/* For MST, there are multiply stream go to only one link.
	 * connect DIG back_end to front_end while enable_stream and
	 * disconnect them during disable_stream
	 * BY this, it is logic clean to separate stream and link
	 */
	link->link_enc->funcs->connect_dig_be_to_fe(link->link_enc,
						    pipe_ctx->stream_res.stream_enc->id, true);

	if (link->dc->hwss.program_dmdata_engine)
		link->dc->hwss.program_dmdata_engine(pipe_ctx);

	link->dc->hwss.update_info_frame(pipe_ctx);

	/* enable early control to avoid corruption on DP monitor*/
	active_total_with_borders =
			timing->h_addressable
				+ timing->h_border_left
				+ timing->h_border_right;

	if (lane_count != 0)
		early_control = active_total_with_borders % lane_count;

	if (early_control == 0)
		early_control = lane_count;

	tg->funcs->set_early_control(tg, early_control);

	/* enable audio only within mode set */
	if (pipe_ctx->stream_res.audio != NULL) {
		if (dc_is_dp_signal(pipe_ctx->stream->signal))
			pipe_ctx->stream_res.stream_enc->funcs->dp_audio_enable(pipe_ctx->stream_res.stream_enc);
	}
}

static void dcn20_program_dmdata_engine(struct pipe_ctx *pipe_ctx)
{
	struct dc_stream_state    *stream     = pipe_ctx->stream;
	struct hubp               *hubp       = pipe_ctx->plane_res.hubp;
	bool                       enable     = false;
	struct stream_encoder     *stream_enc = pipe_ctx->stream_res.stream_enc;
	enum dynamic_metadata_mode mode       = dc_is_dp_signal(stream->signal)
							? dmdata_dp
							: dmdata_hdmi;

	/* if using dynamic meta, don't set up generic infopackets */
	if (pipe_ctx->stream->dmdata_address.quad_part != 0) {
		pipe_ctx->stream_res.encoder_info_frame.hdrsmd.valid = false;
		enable = true;
	}

	if (!hubp)
		return;

	if (!stream_enc || !stream_enc->funcs->set_dynamic_metadata)
		return;

	stream_enc->funcs->set_dynamic_metadata(stream_enc, enable,
						hubp->inst, mode);
}

static void dcn20_fpga_init_hw(struct dc *dc)
{
	int i, j;
	struct dce_hwseq *hws = dc->hwseq;
	struct resource_pool *res_pool = dc->res_pool;
	struct dc_state  *context = dc->current_state;

	if (dc->clk_mgr && dc->clk_mgr->funcs->init_clocks)
		dc->clk_mgr->funcs->init_clocks(dc->clk_mgr);

	// Initialize the dccg
	if (res_pool->dccg->funcs->dccg_init)
		res_pool->dccg->funcs->dccg_init(res_pool->dccg);

	//Enable ability to power gate / don't force power on permanently
	dc->hwss.enable_power_gating_plane(hws, true);

	// Specific to FPGA dccg and registers
	REG_WRITE(RBBMIF_TIMEOUT_DIS, 0xFFFFFFFF);
	REG_WRITE(RBBMIF_TIMEOUT_DIS_2, 0xFFFFFFFF);

	dcn20_dccg_init(hws);

	REG_UPDATE(DCHUBBUB_GLOBAL_TIMER_CNTL, DCHUBBUB_GLOBAL_TIMER_REFDIV, 2);
	REG_UPDATE(DCHUBBUB_GLOBAL_TIMER_CNTL, DCHUBBUB_GLOBAL_TIMER_ENABLE, 1);
	if (REG(REFCLK_CNTL))
		REG_WRITE(REFCLK_CNTL, 0);
	//


	/* Blank pixel data with OPP DPG */
	for (i = 0; i < dc->res_pool->timing_generator_count; i++) {
		struct timing_generator *tg = dc->res_pool->timing_generators[i];

		if (tg->funcs->is_tg_enabled(tg))
			dcn20_init_blank(dc, tg);
	}

	for (i = 0; i < res_pool->timing_generator_count; i++) {
		struct timing_generator *tg = dc->res_pool->timing_generators[i];

		if (tg->funcs->is_tg_enabled(tg))
			tg->funcs->lock(tg);
	}

	for (i = 0; i < dc->res_pool->pipe_count; i++) {
		struct dpp *dpp = res_pool->dpps[i];

		dpp->funcs->dpp_reset(dpp);
	}

	/* Reset all MPCC muxes */
	res_pool->mpc->funcs->mpc_init(res_pool->mpc);

	/* initialize OPP mpc_tree parameter */
	for (i = 0; i < dc->res_pool->res_cap->num_opp; i++) {
		res_pool->opps[i]->mpc_tree_params.opp_id = res_pool->opps[i]->inst;
		res_pool->opps[i]->mpc_tree_params.opp_list = NULL;
		for (j = 0; j < MAX_PIPES; j++)
			res_pool->opps[i]->mpcc_disconnect_pending[j] = false;
	}

	for (i = 0; i < dc->res_pool->pipe_count; i++) {
		struct timing_generator *tg = dc->res_pool->timing_generators[i];
		struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[i];
		struct hubp *hubp = dc->res_pool->hubps[i];
		struct dpp *dpp = dc->res_pool->dpps[i];

		pipe_ctx->stream_res.tg = tg;
		pipe_ctx->pipe_idx = i;

		pipe_ctx->plane_res.hubp = hubp;
		pipe_ctx->plane_res.dpp = dpp;
		pipe_ctx->plane_res.mpcc_inst = dpp->inst;
		hubp->mpcc_id = dpp->inst;
		hubp->opp_id = OPP_ID_INVALID;
		hubp->power_gated = false;
		pipe_ctx->stream_res.opp = NULL;

		hubp->funcs->hubp_init(hubp);

		//dc->res_pool->opps[i]->mpc_tree_params.opp_id = dc->res_pool->opps[i]->inst;
		//dc->res_pool->opps[i]->mpc_tree_params.opp_list = NULL;
		dc->res_pool->opps[i]->mpcc_disconnect_pending[pipe_ctx->plane_res.mpcc_inst] = true;
		pipe_ctx->stream_res.opp = dc->res_pool->opps[i];
		/*to do*/
		hwss1_plane_atomic_disconnect(dc, pipe_ctx);
	}

	/* initialize DWB pointer to MCIF_WB */
	for (i = 0; i < res_pool->res_cap->num_dwb; i++)
		res_pool->dwbc[i]->mcif = res_pool->mcif_wb[i];

	for (i = 0; i < dc->res_pool->timing_generator_count; i++) {
		struct timing_generator *tg = dc->res_pool->timing_generators[i];

		if (tg->funcs->is_tg_enabled(tg))
			tg->funcs->unlock(tg);
	}

	for (i = 0; i < dc->res_pool->pipe_count; i++) {
		struct pipe_ctx *pipe_ctx = &context->res_ctx.pipe_ctx[i];

		dc->hwss.disable_plane(dc, pipe_ctx);

		pipe_ctx->stream_res.tg = NULL;
		pipe_ctx->plane_res.hubp = NULL;
	}

	for (i = 0; i < dc->res_pool->timing_generator_count; i++) {
		struct timing_generator *tg = dc->res_pool->timing_generators[i];

		tg->funcs->tg_init(tg);
	}
}

void dcn20_hw_sequencer_construct(struct dc *dc)
{
	dcn10_hw_sequencer_construct(dc);
	dc->hwss.unblank_stream = dcn20_unblank_stream;
	dc->hwss.update_plane_addr = dcn20_update_plane_addr;
	dc->hwss.enable_stream_timing = dcn20_enable_stream_timing;
	dc->hwss.program_triplebuffer = dcn20_program_tripleBuffer;
	dc->hwss.set_input_transfer_func = dcn20_set_input_transfer_func;
	dc->hwss.set_output_transfer_func = dcn20_set_output_transfer_func;
	dc->hwss.apply_ctx_for_surface = dcn20_apply_ctx_for_surface;
	dc->hwss.pipe_control_lock = dcn20_pipe_control_lock;
	dc->hwss.pipe_control_lock_global = dcn20_pipe_control_lock_global;
	dc->hwss.optimize_bandwidth = dcn20_optimize_bandwidth;
	dc->hwss.prepare_bandwidth = dcn20_prepare_bandwidth;
	dc->hwss.update_bandwidth = dcn20_update_bandwidth;
	dc->hwss.enable_writeback = dcn20_enable_writeback;
	dc->hwss.disable_writeback = dcn20_disable_writeback;
	dc->hwss.program_output_csc = dcn20_program_output_csc;
	dc->hwss.update_odm = dcn20_update_odm;
	dc->hwss.blank_pixel_data = dcn20_blank_pixel_data;
	dc->hwss.dmdata_status_done = dcn20_dmdata_status_done;
	dc->hwss.program_dmdata_engine = dcn20_program_dmdata_engine;
	dc->hwss.enable_stream = dcn20_enable_stream;
	dc->hwss.disable_stream = dcn20_disable_stream;
	dc->hwss.init_sys_ctx = dcn20_init_sys_ctx;
	dc->hwss.init_vm_ctx = dcn20_init_vm_ctx;
	dc->hwss.disable_stream_gating = dcn20_disable_stream_gating;
	dc->hwss.enable_stream_gating = dcn20_enable_stream_gating;
	dc->hwss.setup_vupdate_interrupt = dcn20_setup_vupdate_interrupt;
	dc->hwss.reset_hw_ctx_wrap = dcn20_reset_hw_ctx_wrap;
	dc->hwss.update_mpcc = dcn20_update_mpcc;
	dc->hwss.set_flip_control_gsl = dcn20_set_flip_control_gsl;
	dc->hwss.init_blank = dcn20_init_blank;
	dc->hwss.disable_plane = dcn20_disable_plane;
	dc->hwss.plane_atomic_disable = dcn20_plane_atomic_disable;
	dc->hwss.enable_power_gating_plane = dcn20_enable_power_gating_plane;
	dc->hwss.dpp_pg_control = dcn20_dpp_pg_control;
	dc->hwss.hubp_pg_control = dcn20_hubp_pg_control;
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
	dc->hwss.dsc_pg_control = dcn20_dsc_pg_control;
#else
	dc->hwss.dsc_pg_control = NULL;
#endif
	dc->hwss.disable_vga = dcn20_disable_vga;

	if (IS_FPGA_MAXIMUS_DC(dc->ctx->dce_environment)) {
		dc->hwss.init_hw = dcn20_fpga_init_hw;
		dc->hwss.init_pipes = NULL;
	}


}
