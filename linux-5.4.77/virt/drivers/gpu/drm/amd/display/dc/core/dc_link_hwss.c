/* Copyright 2015 Advanced Micro Devices, Inc. */


#include "dm_services.h"
#include "dc.h"
#include "inc/core_types.h"
#include "include/ddc_service_types.h"
#include "include/i2caux_interface.h"
#include "link_hwss.h"
#include "hw_sequencer.h"
#include "dc_link_dp.h"
#include "dc_link_ddc.h"
#include "dm_helpers.h"
#include "dpcd_defs.h"
#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
#include "dsc.h"
#endif
#if defined(CONFIG_DRM_AMD_DC_DCN2_0)
#include "resource.h"
#endif

enum dc_status core_link_read_dpcd(
	struct dc_link *link,
	uint32_t address,
	uint8_t *data,
	uint32_t size)
{
	if (!link->aux_access_disabled &&
			!dm_helpers_dp_read_dpcd(link->ctx,
			link, address, data, size)) {
		return DC_ERROR_UNEXPECTED;
	}

	return DC_OK;
}

enum dc_status core_link_write_dpcd(
	struct dc_link *link,
	uint32_t address,
	const uint8_t *data,
	uint32_t size)
{
	if (!link->aux_access_disabled &&
			!dm_helpers_dp_write_dpcd(link->ctx,
			link, address, data, size)) {
		return DC_ERROR_UNEXPECTED;
	}

	return DC_OK;
}

void dp_receiver_power_ctrl(struct dc_link *link, bool on)
{
	uint8_t state;

	state = on ? DP_POWER_STATE_D0 : DP_POWER_STATE_D3;

	if (link->sync_lt_in_progress)
		return;

	core_link_write_dpcd(link, DP_SET_POWER, &state,
			sizeof(state));
}

void dp_enable_link_phy(
	struct dc_link *link,
	enum signal_type signal,
	enum clock_source_id clock_source,
	const struct dc_link_settings *link_settings)
{
	struct link_encoder *link_enc = link->link_enc;
	struct dc  *core_dc = link->ctx->dc;
	struct dmcu *dmcu = core_dc->res_pool->dmcu;

	struct pipe_ctx *pipes =
			link->dc->current_state->res_ctx.pipe_ctx;
	struct clock_source *dp_cs =
			link->dc->res_pool->dp_clock_source;
	unsigned int i;
	/* If the current pixel clock source is not DTO(happens after
	 * switching from HDMI passive dongle to DP on the same connector),
	 * switch the pixel clock source to DTO.
	 */
	for (i = 0; i < MAX_PIPES; i++) {
		if (pipes[i].stream != NULL &&
			pipes[i].stream->link == link) {
			if (pipes[i].clock_source != NULL &&
					pipes[i].clock_source->id != CLOCK_SOURCE_ID_DP_DTO) {
				pipes[i].clock_source = dp_cs;
				pipes[i].stream_res.pix_clk_params.requested_pix_clk_100hz =
						pipes[i].stream->timing.pix_clk_100hz;
				pipes[i].clock_source->funcs->program_pix_clk(
							pipes[i].clock_source,
							&pipes[i].stream_res.pix_clk_params,
							&pipes[i].pll_settings);
			}
		}
	}

	if (dmcu != NULL && dmcu->funcs->lock_phy)
		dmcu->funcs->lock_phy(dmcu);

	if (dc_is_dp_sst_signal(signal)) {
		link_enc->funcs->enable_dp_output(
						link_enc,
						link_settings,
						clock_source);
	} else {
		link_enc->funcs->enable_dp_mst_output(
						link_enc,
						link_settings,
						clock_source);
	}

	if (dmcu != NULL && dmcu->funcs->unlock_phy)
		dmcu->funcs->unlock_phy(dmcu);

	link->cur_link_settings = *link_settings;

	dp_receiver_power_ctrl(link, true);
}

bool edp_receiver_ready_T9(struct dc_link *link)
{
	unsigned int tries = 0;
	unsigned char sinkstatus = 0;
	unsigned char edpRev = 0;
	enum dc_status result = DC_OK;
	result = core_link_read_dpcd(link, DP_EDP_DPCD_REV, &edpRev, sizeof(edpRev));
	if (edpRev < DP_EDP_12)
		return true;
	/* start from eDP version 1.2, SINK_STAUS indicate the sink is ready.*/
	do {
		sinkstatus = 1;
		result = core_link_read_dpcd(link, DP_SINK_STATUS, &sinkstatus, sizeof(sinkstatus));
		if (sinkstatus == 0)
			break;
		if (result != DC_OK)
			break;
		udelay(100); //MAx T9
	} while (++tries < 50);

	if (link->local_sink->edid_caps.panel_patch.extra_delay_backlight_off > 0)
		udelay(link->local_sink->edid_caps.panel_patch.extra_delay_backlight_off * 1000);

	return result;
}
bool edp_receiver_ready_T7(struct dc_link *link)
{
	unsigned int tries = 0;
	unsigned char sinkstatus = 0;
	unsigned char edpRev = 0;
	enum dc_status result = DC_OK;

	result = core_link_read_dpcd(link, DP_EDP_DPCD_REV, &edpRev, sizeof(edpRev));
	if (result == DC_OK && edpRev < DP_EDP_12)
		return true;
	/* start from eDP version 1.2, SINK_STAUS indicate the sink is ready.*/
	do {
		sinkstatus = 0;
		result = core_link_read_dpcd(link, DP_SINK_STATUS, &sinkstatus, sizeof(sinkstatus));
		if (sinkstatus == 1)
			break;
		if (result != DC_OK)
			break;
		udelay(25); //MAx T7 is 50ms
	} while (++tries < 300);

	if (link->local_sink->edid_caps.panel_patch.extra_t7_ms > 0)
		udelay(link->local_sink->edid_caps.panel_patch.extra_t7_ms * 1000);

	return result;
}

void dp_disable_link_phy(struct dc_link *link, enum signal_type signal)
{
	struct dc  *core_dc = link->ctx->dc;
	struct dmcu *dmcu = core_dc->res_pool->dmcu;

	if (!link->wa_flags.dp_keep_receiver_powered)
		dp_receiver_power_ctrl(link, false);

	if (signal == SIGNAL_TYPE_EDP) {
		link->link_enc->funcs->disable_output(link->link_enc, signal);
		link->dc->hwss.edp_power_control(link, false);
	} else {
		if (dmcu != NULL && dmcu->funcs->lock_phy)
			dmcu->funcs->lock_phy(dmcu);

		link->link_enc->funcs->disable_output(link->link_enc, signal);

		if (dmcu != NULL && dmcu->funcs->unlock_phy)
			dmcu->funcs->unlock_phy(dmcu);
	}

	/* Clear current link setting.*/
	memset(&link->cur_link_settings, 0,
			sizeof(link->cur_link_settings));
}

void dp_disable_link_phy_mst(struct dc_link *link, enum signal_type signal)
{
	/* MST disable link only when no stream use the link */
	if (link->mst_stream_alloc_table.stream_count > 0)
		return;

	dp_disable_link_phy(link, signal);

	/* set the sink to SST mode after disabling the link */
	dp_enable_mst_on_sink(link, false);
}

bool dp_set_hw_training_pattern(
	struct dc_link *link,
	enum dc_dp_training_pattern pattern)
{
	enum dp_test_pattern test_pattern = DP_TEST_PATTERN_UNSUPPORTED;

	switch (pattern) {
	case DP_TRAINING_PATTERN_SEQUENCE_1:
		test_pattern = DP_TEST_PATTERN_TRAINING_PATTERN1;
		break;
	case DP_TRAINING_PATTERN_SEQUENCE_2:
		test_pattern = DP_TEST_PATTERN_TRAINING_PATTERN2;
		break;
	case DP_TRAINING_PATTERN_SEQUENCE_3:
		test_pattern = DP_TEST_PATTERN_TRAINING_PATTERN3;
		break;
	case DP_TRAINING_PATTERN_SEQUENCE_4:
		test_pattern = DP_TEST_PATTERN_TRAINING_PATTERN4;
		break;
	default:
		break;
	}

	dp_set_hw_test_pattern(link, test_pattern, NULL, 0);

	return true;
}

void dp_set_hw_lane_settings(
	struct dc_link *link,
	const struct link_training_settings *link_settings)
{
	struct link_encoder *encoder = link->link_enc;

	/* call Encoder to set lane settings */
	encoder->funcs->dp_set_lane_settings(encoder, link_settings);
}

void dp_set_hw_test_pattern(
	struct dc_link *link,
	enum dp_test_pattern test_pattern,
	uint8_t *custom_pattern,
	uint32_t custom_pattern_size)
{
	struct encoder_set_dp_phy_pattern_param pattern_param = {0};
	struct link_encoder *encoder = link->link_enc;

	pattern_param.dp_phy_pattern = test_pattern;
	pattern_param.custom_pattern = custom_pattern;
	pattern_param.custom_pattern_size = custom_pattern_size;
	pattern_param.dp_panel_mode = dp_get_panel_mode(link);

	encoder->funcs->dp_set_phy_pattern(encoder, &pattern_param);
}

void dp_retrain_link_dp_test(struct dc_link *link,
			struct dc_link_settings *link_setting,
			bool skip_video_pattern)
{
	struct pipe_ctx *pipes =
			&link->dc->current_state->res_ctx.pipe_ctx[0];
	unsigned int i;

	for (i = 0; i < MAX_PIPES; i++) {
		if (pipes[i].stream != NULL &&
			!pipes[i].top_pipe && !pipes[i].prev_odm_pipe &&
			pipes[i].stream->link != NULL &&
			pipes[i].stream_res.stream_enc != NULL &&
			pipes[i].stream->link == link) {
			udelay(100);

			pipes[i].stream_res.stream_enc->funcs->dp_blank(
					pipes[i].stream_res.stream_enc);

			/* disable any test pattern that might be active */
			dp_set_hw_test_pattern(link,
					DP_TEST_PATTERN_VIDEO_MODE, NULL, 0);

			dp_receiver_power_ctrl(link, false);

			link->dc->hwss.disable_stream(&pipes[i]);
			if ((&pipes[i])->stream_res.audio && !link->dc->debug.az_endpoint_mute_only)
				(&pipes[i])->stream_res.audio->funcs->az_disable((&pipes[i])->stream_res.audio);

			link->link_enc->funcs->disable_output(
					link->link_enc,
					SIGNAL_TYPE_DISPLAY_PORT);

			/* Clear current link setting. */
			memset(&link->cur_link_settings, 0,
				sizeof(link->cur_link_settings));

			link->link_enc->funcs->enable_dp_output(
						link->link_enc,
						link_setting,
						pipes[i].clock_source->id);
			link->cur_link_settings = *link_setting;

			dp_receiver_power_ctrl(link, true);

			perform_link_training_with_retries(
					link,
					link_setting,
					skip_video_pattern,
					LINK_TRAINING_ATTEMPTS);


			link->dc->hwss.enable_stream(&pipes[i]);

			link->dc->hwss.unblank_stream(&pipes[i],
					link_setting);

			if (pipes[i].stream_res.audio) {
				/* notify audio driver for
				 * audio modes of monitor */
				pipes[i].stream_res.audio->funcs->az_enable(
						pipes[i].stream_res.audio);

				/* un-mute audio */
				/* TODO: audio should be per stream rather than
				 * per link */
				pipes[i].stream_res.stream_enc->funcs->
				audio_mute_control(
					pipes[i].stream_res.stream_enc, false);
			}
		}
	}
}

#ifdef CONFIG_DRM_AMD_DC_DSC_SUPPORT
#define DC_LOGGER \
	dsc->ctx->logger
static void dsc_optc_config_log(struct display_stream_compressor *dsc,
		struct dsc_optc_config *config)
{
	uint32_t precision = 1 << 28;
	uint32_t bytes_per_pixel_int = config->bytes_per_pixel / precision;
	uint32_t bytes_per_pixel_mod = config->bytes_per_pixel % precision;
	uint64_t ll_bytes_per_pix_fraq = bytes_per_pixel_mod;

	/* 7 fractional digits decimal precision for bytes per pixel is enough because DSC
	 * bits per pixel precision is 1/16th of a pixel, which means bytes per pixel precision is
	 * 1/16/8 = 1/128 of a byte, or 0.0078125 decimal
	 */
	ll_bytes_per_pix_fraq *= 10000000;
	ll_bytes_per_pix_fraq /= precision;

	DC_LOG_DSC("\tbytes_per_pixel 0x%08x (%d.%07d)",
			config->bytes_per_pixel, bytes_per_pixel_int, (uint32_t)ll_bytes_per_pix_fraq);
	DC_LOG_DSC("\tis_pixel_format_444 %d", config->is_pixel_format_444);
	DC_LOG_DSC("\tslice_width %d", config->slice_width);
}

static bool dp_set_dsc_on_rx(struct pipe_ctx *pipe_ctx, bool enable)
{
	struct dc *core_dc = pipe_ctx->stream->ctx->dc;
	struct dc_stream_state *stream = pipe_ctx->stream;
	bool result = false;

	if (IS_FPGA_MAXIMUS_DC(core_dc->ctx->dce_environment))
		result = true;
	else
		result = dm_helpers_dp_write_dsc_enable(core_dc->ctx, stream, enable);
	return result;
}

/* The stream with these settings can be sent (unblanked) only after DSC was enabled on RX first,
 * i.e. after dp_enable_dsc_on_rx() had been called
 */
void dp_set_dsc_on_stream(struct pipe_ctx *pipe_ctx, bool enable)
{
	struct display_stream_compressor *dsc = pipe_ctx->stream_res.dsc;
	struct dc *core_dc = pipe_ctx->stream->ctx->dc;
	struct dc_stream_state *stream = pipe_ctx->stream;
	struct pipe_ctx *odm_pipe;
	int opp_cnt = 1;

	for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe)
		opp_cnt++;

	if (enable) {
		struct dsc_config dsc_cfg;
		struct dsc_optc_config dsc_optc_cfg;
		enum optc_dsc_mode optc_dsc_mode;

		/* Enable DSC hw block */
		dsc_cfg.pic_width = (stream->timing.h_addressable + stream->timing.h_border_left + stream->timing.h_border_right) / opp_cnt;
		dsc_cfg.pic_height = stream->timing.v_addressable + stream->timing.v_border_top + stream->timing.v_border_bottom;
		dsc_cfg.pixel_encoding = stream->timing.pixel_encoding;
		dsc_cfg.color_depth = stream->timing.display_color_depth;
		dsc_cfg.is_odm = pipe_ctx->next_odm_pipe ? true : false;
		dsc_cfg.dc_dsc_cfg = stream->timing.dsc_cfg;
		ASSERT(dsc_cfg.dc_dsc_cfg.num_slices_h % opp_cnt == 0);
		dsc_cfg.dc_dsc_cfg.num_slices_h /= opp_cnt;

		dsc->funcs->dsc_set_config(dsc, &dsc_cfg, &dsc_optc_cfg);
		dsc->funcs->dsc_enable(dsc, pipe_ctx->stream_res.opp->inst);
		for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe) {
			struct display_stream_compressor *odm_dsc = odm_pipe->stream_res.dsc;

			odm_dsc->funcs->dsc_set_config(odm_dsc, &dsc_cfg, &dsc_optc_cfg);
			odm_dsc->funcs->dsc_enable(odm_dsc, odm_pipe->stream_res.opp->inst);
		}
		dsc_cfg.dc_dsc_cfg.num_slices_h *= opp_cnt;
		dsc_cfg.pic_width *= opp_cnt;

		optc_dsc_mode = dsc_optc_cfg.is_pixel_format_444 ? OPTC_DSC_ENABLED_444 : OPTC_DSC_ENABLED_NATIVE_SUBSAMPLED;

		/* Enable DSC in encoder */
		if (dc_is_dp_signal(stream->signal) && !IS_FPGA_MAXIMUS_DC(core_dc->ctx->dce_environment)) {
			DC_LOG_DSC("Setting stream encoder DSC config for engine %d:", (int)pipe_ctx->stream_res.stream_enc->id);
			dsc_optc_config_log(dsc, &dsc_optc_cfg);
			pipe_ctx->stream_res.stream_enc->funcs->dp_set_dsc_config(pipe_ctx->stream_res.stream_enc,
									optc_dsc_mode,
									dsc_optc_cfg.bytes_per_pixel,
									dsc_optc_cfg.slice_width);

			/* PPS SDP is set elsewhere because it has to be done after DIG FE is connected to DIG BE */
		}

		/* Enable DSC in OPTC */
		DC_LOG_DSC("Setting optc DSC config for tg instance %d:", pipe_ctx->stream_res.tg->inst);
		dsc_optc_config_log(dsc, &dsc_optc_cfg);
		pipe_ctx->stream_res.tg->funcs->set_dsc_config(pipe_ctx->stream_res.tg,
							optc_dsc_mode,
							dsc_optc_cfg.bytes_per_pixel,
							dsc_optc_cfg.slice_width);
	} else {
		/* disable DSC in OPTC */
		pipe_ctx->stream_res.tg->funcs->set_dsc_config(
				pipe_ctx->stream_res.tg,
				OPTC_DSC_DISABLED, 0, 0);

		/* disable DSC in stream encoder */
		if (dc_is_dp_signal(stream->signal) && !IS_FPGA_MAXIMUS_DC(core_dc->ctx->dce_environment)) {
			pipe_ctx->stream_res.stream_enc->funcs->dp_set_dsc_config(
					pipe_ctx->stream_res.stream_enc,
					OPTC_DSC_DISABLED, 0, 0);

			pipe_ctx->stream_res.stream_enc->funcs->dp_set_dsc_pps_info_packet(
					pipe_ctx->stream_res.stream_enc, false, NULL);
		}

		/* disable DSC block */
		pipe_ctx->stream_res.dsc->funcs->dsc_disable(pipe_ctx->stream_res.dsc);
		for (odm_pipe = pipe_ctx->next_odm_pipe; odm_pipe; odm_pipe = odm_pipe->next_odm_pipe)
			odm_pipe->stream_res.dsc->funcs->dsc_disable(odm_pipe->stream_res.dsc);
	}
}

bool dp_set_dsc_enable(struct pipe_ctx *pipe_ctx, bool enable)
{
	struct display_stream_compressor *dsc = pipe_ctx->stream_res.dsc;
	bool result = false;

	if (!pipe_ctx->stream->timing.flags.DSC)
		goto out;
	if (!dsc)
		goto out;

	if (enable) {
		if (dp_set_dsc_on_rx(pipe_ctx, true)) {
			dp_set_dsc_on_stream(pipe_ctx, true);
			result = true;
		}
	} else {
		dp_set_dsc_on_rx(pipe_ctx, false);
		dp_set_dsc_on_stream(pipe_ctx, false);
		result = true;
	}
out:
	return result;
}

bool dp_set_dsc_pps_sdp(struct pipe_ctx *pipe_ctx, bool enable)
{
	struct display_stream_compressor *dsc = pipe_ctx->stream_res.dsc;
	struct dc *core_dc = pipe_ctx->stream->ctx->dc;
	struct dc_stream_state *stream = pipe_ctx->stream;

	if (!pipe_ctx->stream->timing.flags.DSC || !dsc)
		return false;

	if (enable) {
		struct dsc_config dsc_cfg;
		uint8_t dsc_packed_pps[128];

		memset(&dsc_cfg, 0, sizeof(dsc_cfg));
		memset(dsc_packed_pps, 0, 128);

		/* Enable DSC hw block */
		dsc_cfg.pic_width = stream->timing.h_addressable + stream->timing.h_border_left + stream->timing.h_border_right;
		dsc_cfg.pic_height = stream->timing.v_addressable + stream->timing.v_border_top + stream->timing.v_border_bottom;
		dsc_cfg.pixel_encoding = stream->timing.pixel_encoding;
		dsc_cfg.color_depth = stream->timing.display_color_depth;
		dsc_cfg.is_odm = pipe_ctx->next_odm_pipe ? true : false;
		dsc_cfg.dc_dsc_cfg = stream->timing.dsc_cfg;

		DC_LOG_DSC(" ");
		dsc->funcs->dsc_get_packed_pps(dsc, &dsc_cfg, &dsc_packed_pps[0]);
		if (dc_is_dp_signal(stream->signal) && !IS_FPGA_MAXIMUS_DC(core_dc->ctx->dce_environment)) {
			DC_LOG_DSC("Setting stream encoder DSC PPS SDP for engine %d\n", (int)pipe_ctx->stream_res.stream_enc->id);
			pipe_ctx->stream_res.stream_enc->funcs->dp_set_dsc_pps_info_packet(
									pipe_ctx->stream_res.stream_enc,
									true,
									&dsc_packed_pps[0]);
		}
	} else {
		/* disable DSC PPS in stream encoder */
		if (dc_is_dp_signal(stream->signal) && !IS_FPGA_MAXIMUS_DC(core_dc->ctx->dce_environment)) {
			pipe_ctx->stream_res.stream_enc->funcs->dp_set_dsc_pps_info_packet(
						pipe_ctx->stream_res.stream_enc, false, NULL);
		}
	}

	return true;
}


bool dp_update_dsc_config(struct pipe_ctx *pipe_ctx)
{
	struct display_stream_compressor *dsc = pipe_ctx->stream_res.dsc;

	if (!pipe_ctx->stream->timing.flags.DSC)
		return false;
	if (!dsc)
		return false;

	dp_set_dsc_on_stream(pipe_ctx, true);
	dp_set_dsc_pps_sdp(pipe_ctx, true);
	return true;
}
#endif

