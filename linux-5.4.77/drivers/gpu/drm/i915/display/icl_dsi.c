/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Madhav Chauhan <madhav.chauhan@intel.com>
 *   Jani Nikula <jani.nikula@intel.com>
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_mipi_dsi.h>

#include "intel_atomic.h"
#include "intel_combo_phy.h"
#include "intel_connector.h"
#include "intel_ddi.h"
#include "intel_dsi.h"
#include "intel_panel.h"

static inline int header_credits_available(struct drm_i915_private *dev_priv,
					   enum transcoder dsi_trans)
{
	return (I915_READ(DSI_CMD_TXCTL(dsi_trans)) & FREE_HEADER_CREDIT_MASK)
		>> FREE_HEADER_CREDIT_SHIFT;
}

static inline int payload_credits_available(struct drm_i915_private *dev_priv,
					    enum transcoder dsi_trans)
{
	return (I915_READ(DSI_CMD_TXCTL(dsi_trans)) & FREE_PLOAD_CREDIT_MASK)
		>> FREE_PLOAD_CREDIT_SHIFT;
}

static void wait_for_header_credits(struct drm_i915_private *dev_priv,
				    enum transcoder dsi_trans)
{
	if (wait_for_us(header_credits_available(dev_priv, dsi_trans) >=
			MAX_HEADER_CREDIT, 100))
		DRM_ERROR("DSI header credits not released\n");
}

static void wait_for_payload_credits(struct drm_i915_private *dev_priv,
				     enum transcoder dsi_trans)
{
	if (wait_for_us(payload_credits_available(dev_priv, dsi_trans) >=
			MAX_PLOAD_CREDIT, 100))
		DRM_ERROR("DSI payload credits not released\n");
}

static enum transcoder dsi_port_to_transcoder(enum port port)
{
	if (port == PORT_A)
		return TRANSCODER_DSI_0;
	else
		return TRANSCODER_DSI_1;
}

static void wait_for_cmds_dispatched_to_panel(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct mipi_dsi_device *dsi;
	enum port port;
	enum transcoder dsi_trans;
	int ret;

	/* wait for header/payload credits to be released */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		wait_for_header_credits(dev_priv, dsi_trans);
		wait_for_payload_credits(dev_priv, dsi_trans);
	}

	/* send nop DCS command */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi = intel_dsi->dsi_hosts[port]->device;
		dsi->mode_flags |= MIPI_DSI_MODE_LPM;
		dsi->channel = 0;
		ret = mipi_dsi_dcs_nop(dsi);
		if (ret < 0)
			DRM_ERROR("error sending DCS NOP command\n");
	}

	/* wait for header credits to be released */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		wait_for_header_credits(dev_priv, dsi_trans);
	}

	/* wait for LP TX in progress bit to be cleared */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		if (wait_for_us(!(I915_READ(DSI_LP_MSG(dsi_trans)) &
				  LPTX_IN_PROGRESS), 20))
			DRM_ERROR("LPTX bit not cleared\n");
	}
}

static bool add_payld_to_queue(struct intel_dsi_host *host, const u8 *data,
			       u32 len)
{
	struct intel_dsi *intel_dsi = host->intel_dsi;
	struct drm_i915_private *dev_priv = to_i915(intel_dsi->base.base.dev);
	enum transcoder dsi_trans = dsi_port_to_transcoder(host->port);
	int free_credits;
	int i, j;

	for (i = 0; i < len; i += 4) {
		u32 tmp = 0;

		free_credits = payload_credits_available(dev_priv, dsi_trans);
		if (free_credits < 1) {
			DRM_ERROR("Payload credit not available\n");
			return false;
		}

		for (j = 0; j < min_t(u32, len - i, 4); j++)
			tmp |= *data++ << 8 * j;

		I915_WRITE(DSI_CMD_TXPYLD(dsi_trans), tmp);
	}

	return true;
}

static int dsi_send_pkt_hdr(struct intel_dsi_host *host,
			    struct mipi_dsi_packet pkt, bool enable_lpdt)
{
	struct intel_dsi *intel_dsi = host->intel_dsi;
	struct drm_i915_private *dev_priv = to_i915(intel_dsi->base.base.dev);
	enum transcoder dsi_trans = dsi_port_to_transcoder(host->port);
	u32 tmp;
	int free_credits;

	/* check if header credit available */
	free_credits = header_credits_available(dev_priv, dsi_trans);
	if (free_credits < 1) {
		DRM_ERROR("send pkt header failed, not enough hdr credits\n");
		return -1;
	}

	tmp = I915_READ(DSI_CMD_TXHDR(dsi_trans));

	if (pkt.payload)
		tmp |= PAYLOAD_PRESENT;
	else
		tmp &= ~PAYLOAD_PRESENT;

	tmp &= ~VBLANK_FENCE;

	if (enable_lpdt)
		tmp |= LP_DATA_TRANSFER;

	tmp &= ~(PARAM_WC_MASK | VC_MASK | DT_MASK);
	tmp |= ((pkt.header[0] & VC_MASK) << VC_SHIFT);
	tmp |= ((pkt.header[0] & DT_MASK) << DT_SHIFT);
	tmp |= (pkt.header[1] << PARAM_WC_LOWER_SHIFT);
	tmp |= (pkt.header[2] << PARAM_WC_UPPER_SHIFT);
	I915_WRITE(DSI_CMD_TXHDR(dsi_trans), tmp);

	return 0;
}

static int dsi_send_pkt_payld(struct intel_dsi_host *host,
			      struct mipi_dsi_packet pkt)
{
	/* payload queue can accept *256 bytes*, check limit */
	if (pkt.payload_length > MAX_PLOAD_CREDIT * 4) {
		DRM_ERROR("payload size exceeds max queue limit\n");
		return -1;
	}

	/* load data into command payload queue */
	if (!add_payld_to_queue(host, pkt.payload,
				pkt.payload_length)) {
		DRM_ERROR("adding payload to queue failed\n");
		return -1;
	}

	return 0;
}

static void dsi_program_swing_and_deemphasis(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum phy phy;
	u32 tmp;
	int lane;

	for_each_dsi_phy(phy, intel_dsi->phys) {
		/*
		 * Program voltage swing and pre-emphasis level values as per
		 * table in BSPEC under DDI buffer programing
		 */
		tmp = I915_READ(ICL_PORT_TX_DW5_LN0(phy));
		tmp &= ~(SCALING_MODE_SEL_MASK | RTERM_SELECT_MASK);
		tmp |= SCALING_MODE_SEL(0x2);
		tmp |= TAP2_DISABLE | TAP3_DISABLE;
		tmp |= RTERM_SELECT(0x6);
		I915_WRITE(ICL_PORT_TX_DW5_GRP(phy), tmp);

		tmp = I915_READ(ICL_PORT_TX_DW5_AUX(phy));
		tmp &= ~(SCALING_MODE_SEL_MASK | RTERM_SELECT_MASK);
		tmp |= SCALING_MODE_SEL(0x2);
		tmp |= TAP2_DISABLE | TAP3_DISABLE;
		tmp |= RTERM_SELECT(0x6);
		I915_WRITE(ICL_PORT_TX_DW5_AUX(phy), tmp);

		tmp = I915_READ(ICL_PORT_TX_DW2_LN0(phy));
		tmp &= ~(SWING_SEL_LOWER_MASK | SWING_SEL_UPPER_MASK |
			 RCOMP_SCALAR_MASK);
		tmp |= SWING_SEL_UPPER(0x2);
		tmp |= SWING_SEL_LOWER(0x2);
		tmp |= RCOMP_SCALAR(0x98);
		I915_WRITE(ICL_PORT_TX_DW2_GRP(phy), tmp);

		tmp = I915_READ(ICL_PORT_TX_DW2_AUX(phy));
		tmp &= ~(SWING_SEL_LOWER_MASK | SWING_SEL_UPPER_MASK |
			 RCOMP_SCALAR_MASK);
		tmp |= SWING_SEL_UPPER(0x2);
		tmp |= SWING_SEL_LOWER(0x2);
		tmp |= RCOMP_SCALAR(0x98);
		I915_WRITE(ICL_PORT_TX_DW2_AUX(phy), tmp);

		tmp = I915_READ(ICL_PORT_TX_DW4_AUX(phy));
		tmp &= ~(POST_CURSOR_1_MASK | POST_CURSOR_2_MASK |
			 CURSOR_COEFF_MASK);
		tmp |= POST_CURSOR_1(0x0);
		tmp |= POST_CURSOR_2(0x0);
		tmp |= CURSOR_COEFF(0x3f);
		I915_WRITE(ICL_PORT_TX_DW4_AUX(phy), tmp);

		for (lane = 0; lane <= 3; lane++) {
			/* Bspec: must not use GRP register for write */
			tmp = I915_READ(ICL_PORT_TX_DW4_LN(lane, phy));
			tmp &= ~(POST_CURSOR_1_MASK | POST_CURSOR_2_MASK |
				 CURSOR_COEFF_MASK);
			tmp |= POST_CURSOR_1(0x0);
			tmp |= POST_CURSOR_2(0x0);
			tmp |= CURSOR_COEFF(0x3f);
			I915_WRITE(ICL_PORT_TX_DW4_LN(lane, phy), tmp);
		}
	}
}

static void configure_dual_link_mode(struct intel_encoder *encoder,
				     const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	u32 dss_ctl1;

	dss_ctl1 = I915_READ(DSS_CTL1);
	dss_ctl1 |= SPLITTER_ENABLE;
	dss_ctl1 &= ~OVERLAP_PIXELS_MASK;
	dss_ctl1 |= OVERLAP_PIXELS(intel_dsi->pixel_overlap);

	if (intel_dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK) {
		const struct drm_display_mode *adjusted_mode =
					&pipe_config->base.adjusted_mode;
		u32 dss_ctl2;
		u16 hactive = adjusted_mode->crtc_hdisplay;
		u16 dl_buffer_depth;

		dss_ctl1 &= ~DUAL_LINK_MODE_INTERLEAVE;
		dl_buffer_depth = hactive / 2 + intel_dsi->pixel_overlap;

		if (dl_buffer_depth > MAX_DL_BUFFER_TARGET_DEPTH)
			DRM_ERROR("DL buffer depth exceed max value\n");

		dss_ctl1 &= ~LEFT_DL_BUF_TARGET_DEPTH_MASK;
		dss_ctl1 |= LEFT_DL_BUF_TARGET_DEPTH(dl_buffer_depth);
		dss_ctl2 = I915_READ(DSS_CTL2);
		dss_ctl2 &= ~RIGHT_DL_BUF_TARGET_DEPTH_MASK;
		dss_ctl2 |= RIGHT_DL_BUF_TARGET_DEPTH(dl_buffer_depth);
		I915_WRITE(DSS_CTL2, dss_ctl2);
	} else {
		/* Interleave */
		dss_ctl1 |= DUAL_LINK_MODE_INTERLEAVE;
	}

	I915_WRITE(DSS_CTL1, dss_ctl1);
}

static void gen11_dsi_program_esc_clk_div(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum port port;
	u32 bpp = mipi_dsi_pixel_format_to_bpp(intel_dsi->pixel_format);
	u32 afe_clk_khz; /* 8X Clock */
	u32 esc_clk_div_m;

	afe_clk_khz = DIV_ROUND_CLOSEST(intel_dsi->pclk * bpp,
					intel_dsi->lane_count);

	esc_clk_div_m = DIV_ROUND_UP(afe_clk_khz, DSI_MAX_ESC_CLK);

	for_each_dsi_port(port, intel_dsi->ports) {
		I915_WRITE(ICL_DSI_ESC_CLK_DIV(port),
			   esc_clk_div_m & ICL_ESC_CLK_DIV_MASK);
		POSTING_READ(ICL_DSI_ESC_CLK_DIV(port));
	}

	for_each_dsi_port(port, intel_dsi->ports) {
		I915_WRITE(ICL_DPHY_ESC_CLK_DIV(port),
			   esc_clk_div_m & ICL_ESC_CLK_DIV_MASK);
		POSTING_READ(ICL_DPHY_ESC_CLK_DIV(port));
	}
}

static void get_dsi_io_power_domains(struct drm_i915_private *dev_priv,
				     struct intel_dsi *intel_dsi)
{
	enum port port;

	for_each_dsi_port(port, intel_dsi->ports) {
		WARN_ON(intel_dsi->io_wakeref[port]);
		intel_dsi->io_wakeref[port] =
			intel_display_power_get(dev_priv,
						port == PORT_A ?
						POWER_DOMAIN_PORT_DDI_A_IO :
						POWER_DOMAIN_PORT_DDI_B_IO);
	}
}

static void gen11_dsi_enable_io_power(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum port port;
	u32 tmp;

	for_each_dsi_port(port, intel_dsi->ports) {
		tmp = I915_READ(ICL_DSI_IO_MODECTL(port));
		tmp |= COMBO_PHY_MODE_DSI;
		I915_WRITE(ICL_DSI_IO_MODECTL(port), tmp);
	}

	get_dsi_io_power_domains(dev_priv, intel_dsi);
}

static void gen11_dsi_power_up_lanes(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum phy phy;

	for_each_dsi_phy(phy, intel_dsi->phys)
		intel_combo_phy_power_up_lanes(dev_priv, phy, true,
					       intel_dsi->lane_count, false);
}

static void gen11_dsi_config_phy_lanes_sequence(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum phy phy;
	u32 tmp;
	int lane;

	/* Step 4b(i) set loadgen select for transmit and aux lanes */
	for_each_dsi_phy(phy, intel_dsi->phys) {
		tmp = I915_READ(ICL_PORT_TX_DW4_AUX(phy));
		tmp &= ~LOADGEN_SELECT;
		I915_WRITE(ICL_PORT_TX_DW4_AUX(phy), tmp);
		for (lane = 0; lane <= 3; lane++) {
			tmp = I915_READ(ICL_PORT_TX_DW4_LN(lane, phy));
			tmp &= ~LOADGEN_SELECT;
			if (lane != 2)
				tmp |= LOADGEN_SELECT;
			I915_WRITE(ICL_PORT_TX_DW4_LN(lane, phy), tmp);
		}
	}

	/* Step 4b(ii) set latency optimization for transmit and aux lanes */
	for_each_dsi_phy(phy, intel_dsi->phys) {
		tmp = I915_READ(ICL_PORT_TX_DW2_AUX(phy));
		tmp &= ~FRC_LATENCY_OPTIM_MASK;
		tmp |= FRC_LATENCY_OPTIM_VAL(0x5);
		I915_WRITE(ICL_PORT_TX_DW2_AUX(phy), tmp);
		tmp = I915_READ(ICL_PORT_TX_DW2_LN0(phy));
		tmp &= ~FRC_LATENCY_OPTIM_MASK;
		tmp |= FRC_LATENCY_OPTIM_VAL(0x5);
		I915_WRITE(ICL_PORT_TX_DW2_GRP(phy), tmp);

		/* For EHL, TGL, set latency optimization for PCS_DW1 lanes */
		if (IS_ELKHARTLAKE(dev_priv) || (INTEL_GEN(dev_priv) >= 12)) {
			tmp = I915_READ(ICL_PORT_PCS_DW1_AUX(phy));
			tmp &= ~LATENCY_OPTIM_MASK;
			tmp |= LATENCY_OPTIM_VAL(0);
			I915_WRITE(ICL_PORT_PCS_DW1_AUX(phy), tmp);

			tmp = I915_READ(ICL_PORT_PCS_DW1_LN0(phy));
			tmp &= ~LATENCY_OPTIM_MASK;
			tmp |= LATENCY_OPTIM_VAL(0x1);
			I915_WRITE(ICL_PORT_PCS_DW1_GRP(phy), tmp);
		}
	}

}

static void gen11_dsi_voltage_swing_program_seq(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	u32 tmp;
	enum phy phy;

	/* clear common keeper enable bit */
	for_each_dsi_phy(phy, intel_dsi->phys) {
		tmp = I915_READ(ICL_PORT_PCS_DW1_LN0(phy));
		tmp &= ~COMMON_KEEPER_EN;
		I915_WRITE(ICL_PORT_PCS_DW1_GRP(phy), tmp);
		tmp = I915_READ(ICL_PORT_PCS_DW1_AUX(phy));
		tmp &= ~COMMON_KEEPER_EN;
		I915_WRITE(ICL_PORT_PCS_DW1_AUX(phy), tmp);
	}

	/*
	 * Set SUS Clock Config bitfield to 11b
	 * Note: loadgen select program is done
	 * as part of lane phy sequence configuration
	 */
	for_each_dsi_phy(phy, intel_dsi->phys) {
		tmp = I915_READ(ICL_PORT_CL_DW5(phy));
		tmp |= SUS_CLOCK_CONFIG;
		I915_WRITE(ICL_PORT_CL_DW5(phy), tmp);
	}

	/* Clear training enable to change swing values */
	for_each_dsi_phy(phy, intel_dsi->phys) {
		tmp = I915_READ(ICL_PORT_TX_DW5_LN0(phy));
		tmp &= ~TX_TRAINING_EN;
		I915_WRITE(ICL_PORT_TX_DW5_GRP(phy), tmp);
		tmp = I915_READ(ICL_PORT_TX_DW5_AUX(phy));
		tmp &= ~TX_TRAINING_EN;
		I915_WRITE(ICL_PORT_TX_DW5_AUX(phy), tmp);
	}

	/* Program swing and de-emphasis */
	dsi_program_swing_and_deemphasis(encoder);

	/* Set training enable to trigger update */
	for_each_dsi_phy(phy, intel_dsi->phys) {
		tmp = I915_READ(ICL_PORT_TX_DW5_LN0(phy));
		tmp |= TX_TRAINING_EN;
		I915_WRITE(ICL_PORT_TX_DW5_GRP(phy), tmp);
		tmp = I915_READ(ICL_PORT_TX_DW5_AUX(phy));
		tmp |= TX_TRAINING_EN;
		I915_WRITE(ICL_PORT_TX_DW5_AUX(phy), tmp);
	}
}

static void gen11_dsi_enable_ddi_buffer(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	u32 tmp;
	enum port port;

	for_each_dsi_port(port, intel_dsi->ports) {
		tmp = I915_READ(DDI_BUF_CTL(port));
		tmp |= DDI_BUF_CTL_ENABLE;
		I915_WRITE(DDI_BUF_CTL(port), tmp);

		if (wait_for_us(!(I915_READ(DDI_BUF_CTL(port)) &
				  DDI_BUF_IS_IDLE),
				  500))
			DRM_ERROR("DDI port:%c buffer idle\n", port_name(port));
	}
}

static void gen11_dsi_setup_dphy_timings(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	u32 tmp;
	enum port port;
	enum phy phy;

	/* Program T-INIT master registers */
	for_each_dsi_port(port, intel_dsi->ports) {
		tmp = I915_READ(ICL_DSI_T_INIT_MASTER(port));
		tmp &= ~MASTER_INIT_TIMER_MASK;
		tmp |= intel_dsi->init_count;
		I915_WRITE(ICL_DSI_T_INIT_MASTER(port), tmp);
	}

	/* Program DPHY clock lanes timings */
	for_each_dsi_port(port, intel_dsi->ports) {
		I915_WRITE(DPHY_CLK_TIMING_PARAM(port), intel_dsi->dphy_reg);

		/* shadow register inside display core */
		I915_WRITE(DSI_CLK_TIMING_PARAM(port), intel_dsi->dphy_reg);
	}

	/* Program DPHY data lanes timings */
	for_each_dsi_port(port, intel_dsi->ports) {
		I915_WRITE(DPHY_DATA_TIMING_PARAM(port),
			   intel_dsi->dphy_data_lane_reg);

		/* shadow register inside display core */
		I915_WRITE(DSI_DATA_TIMING_PARAM(port),
			   intel_dsi->dphy_data_lane_reg);
	}

	/*
	 * If DSI link operating at or below an 800 MHz,
	 * TA_SURE should be override and programmed to
	 * a value '0' inside TA_PARAM_REGISTERS otherwise
	 * leave all fields at HW default values.
	 */
	if (IS_GEN(dev_priv, 11)) {
		if (intel_dsi_bitrate(intel_dsi) <= 800000) {
			for_each_dsi_port(port, intel_dsi->ports) {
				tmp = I915_READ(DPHY_TA_TIMING_PARAM(port));
				tmp &= ~TA_SURE_MASK;
				tmp |= TA_SURE_OVERRIDE | TA_SURE(0);
				I915_WRITE(DPHY_TA_TIMING_PARAM(port), tmp);

				/* shadow register inside display core */
				tmp = I915_READ(DSI_TA_TIMING_PARAM(port));
				tmp &= ~TA_SURE_MASK;
				tmp |= TA_SURE_OVERRIDE | TA_SURE(0);
				I915_WRITE(DSI_TA_TIMING_PARAM(port), tmp);
			}
		}
	}

	if (IS_ELKHARTLAKE(dev_priv)) {
		for_each_dsi_phy(phy, intel_dsi->phys) {
			tmp = I915_READ(ICL_DPHY_CHKN(phy));
			tmp |= ICL_DPHY_CHKN_AFE_OVER_PPI_STRAP;
			I915_WRITE(ICL_DPHY_CHKN(phy), tmp);
		}
	}
}

static void gen11_dsi_gate_clocks(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	u32 tmp;
	enum phy phy;

	mutex_lock(&dev_priv->dpll_lock);
	tmp = I915_READ(ICL_DPCLKA_CFGCR0);
	for_each_dsi_phy(phy, intel_dsi->phys)
		tmp |= ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy);

	I915_WRITE(ICL_DPCLKA_CFGCR0, tmp);
	mutex_unlock(&dev_priv->dpll_lock);
}

static void gen11_dsi_ungate_clocks(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	u32 tmp;
	enum phy phy;

	mutex_lock(&dev_priv->dpll_lock);
	tmp = I915_READ(ICL_DPCLKA_CFGCR0);
	for_each_dsi_phy(phy, intel_dsi->phys)
		tmp &= ~ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy);

	I915_WRITE(ICL_DPCLKA_CFGCR0, tmp);
	mutex_unlock(&dev_priv->dpll_lock);
}

static void gen11_dsi_map_pll(struct intel_encoder *encoder,
			      const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct intel_shared_dpll *pll = crtc_state->shared_dpll;
	enum phy phy;
	u32 val;

	mutex_lock(&dev_priv->dpll_lock);

	val = I915_READ(ICL_DPCLKA_CFGCR0);
	for_each_dsi_phy(phy, intel_dsi->phys) {
		val &= ~ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_MASK(phy);
		val |= ICL_DPCLKA_CFGCR0_DDI_CLK_SEL(pll->info->id, phy);
	}
	I915_WRITE(ICL_DPCLKA_CFGCR0, val);

	for_each_dsi_phy(phy, intel_dsi->phys) {
		if (INTEL_GEN(dev_priv) >= 12)
			val |= ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy);
		else
			val &= ~ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy);
	}
	I915_WRITE(ICL_DPCLKA_CFGCR0, val);

	POSTING_READ(ICL_DPCLKA_CFGCR0);

	mutex_unlock(&dev_priv->dpll_lock);
}

static void
gen11_dsi_configure_transcoder(struct intel_encoder *encoder,
			       const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct intel_crtc *intel_crtc = to_intel_crtc(pipe_config->base.crtc);
	enum pipe pipe = intel_crtc->pipe;
	u32 tmp;
	enum port port;
	enum transcoder dsi_trans;

	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		tmp = I915_READ(DSI_TRANS_FUNC_CONF(dsi_trans));

		if (intel_dsi->eotp_pkt)
			tmp &= ~EOTP_DISABLED;
		else
			tmp |= EOTP_DISABLED;

		/* enable link calibration if freq > 1.5Gbps */
		if (intel_dsi_bitrate(intel_dsi) >= 1500 * 1000) {
			tmp &= ~LINK_CALIBRATION_MASK;
			tmp |= CALIBRATION_ENABLED_INITIAL_ONLY;
		}

		/* configure continuous clock */
		tmp &= ~CONTINUOUS_CLK_MASK;
		if (intel_dsi->clock_stop)
			tmp |= CLK_ENTER_LP_AFTER_DATA;
		else
			tmp |= CLK_HS_CONTINUOUS;

		/* configure buffer threshold limit to minimum */
		tmp &= ~PIX_BUF_THRESHOLD_MASK;
		tmp |= PIX_BUF_THRESHOLD_1_4;

		/* set virtual channel to '0' */
		tmp &= ~PIX_VIRT_CHAN_MASK;
		tmp |= PIX_VIRT_CHAN(0);

		/* program BGR transmission */
		if (intel_dsi->bgr_enabled)
			tmp |= BGR_TRANSMISSION;

		/* select pixel format */
		tmp &= ~PIX_FMT_MASK;
		switch (intel_dsi->pixel_format) {
		default:
			MISSING_CASE(intel_dsi->pixel_format);
			/* fallthrough */
		case MIPI_DSI_FMT_RGB565:
			tmp |= PIX_FMT_RGB565;
			break;
		case MIPI_DSI_FMT_RGB666_PACKED:
			tmp |= PIX_FMT_RGB666_PACKED;
			break;
		case MIPI_DSI_FMT_RGB666:
			tmp |= PIX_FMT_RGB666_LOOSE;
			break;
		case MIPI_DSI_FMT_RGB888:
			tmp |= PIX_FMT_RGB888;
			break;
		}

		if (INTEL_GEN(dev_priv) >= 12) {
			if (is_vid_mode(intel_dsi))
				tmp |= BLANKING_PACKET_ENABLE;
		}

		/* program DSI operation mode */
		if (is_vid_mode(intel_dsi)) {
			tmp &= ~OP_MODE_MASK;
			switch (intel_dsi->video_mode_format) {
			default:
				MISSING_CASE(intel_dsi->video_mode_format);
				/* fallthrough */
			case VIDEO_MODE_NON_BURST_WITH_SYNC_EVENTS:
				tmp |= VIDEO_MODE_SYNC_EVENT;
				break;
			case VIDEO_MODE_NON_BURST_WITH_SYNC_PULSE:
				tmp |= VIDEO_MODE_SYNC_PULSE;
				break;
			}
		}

		I915_WRITE(DSI_TRANS_FUNC_CONF(dsi_trans), tmp);
	}

	/* enable port sync mode if dual link */
	if (intel_dsi->dual_link) {
		for_each_dsi_port(port, intel_dsi->ports) {
			dsi_trans = dsi_port_to_transcoder(port);
			tmp = I915_READ(TRANS_DDI_FUNC_CTL2(dsi_trans));
			tmp |= PORT_SYNC_MODE_ENABLE;
			I915_WRITE(TRANS_DDI_FUNC_CTL2(dsi_trans), tmp);
		}

		/* configure stream splitting */
		configure_dual_link_mode(encoder, pipe_config);
	}

	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);

		/* select data lane width */
		tmp = I915_READ(TRANS_DDI_FUNC_CTL(dsi_trans));
		tmp &= ~DDI_PORT_WIDTH_MASK;
		tmp |= DDI_PORT_WIDTH(intel_dsi->lane_count);

		/* select input pipe */
		tmp &= ~TRANS_DDI_EDP_INPUT_MASK;
		switch (pipe) {
		default:
			MISSING_CASE(pipe);
			/* fallthrough */
		case PIPE_A:
			tmp |= TRANS_DDI_EDP_INPUT_A_ON;
			break;
		case PIPE_B:
			tmp |= TRANS_DDI_EDP_INPUT_B_ONOFF;
			break;
		case PIPE_C:
			tmp |= TRANS_DDI_EDP_INPUT_C_ONOFF;
			break;
		}

		/* enable DDI buffer */
		tmp |= TRANS_DDI_FUNC_ENABLE;
		I915_WRITE(TRANS_DDI_FUNC_CTL(dsi_trans), tmp);
	}

	/* wait for link ready */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		if (wait_for_us((I915_READ(DSI_TRANS_FUNC_CONF(dsi_trans)) &
				LINK_READY), 2500))
			DRM_ERROR("DSI link not ready\n");
	}
}

static void
gen11_dsi_set_transcoder_timings(struct intel_encoder *encoder,
				 const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	const struct drm_display_mode *adjusted_mode =
					&pipe_config->base.adjusted_mode;
	enum port port;
	enum transcoder dsi_trans;
	/* horizontal timings */
	u16 htotal, hactive, hsync_start, hsync_end, hsync_size;
	u16 hback_porch;
	/* vertical timings */
	u16 vtotal, vactive, vsync_start, vsync_end, vsync_shift;

	hactive = adjusted_mode->crtc_hdisplay;
	htotal = adjusted_mode->crtc_htotal;
	hsync_start = adjusted_mode->crtc_hsync_start;
	hsync_end = adjusted_mode->crtc_hsync_end;
	hsync_size  = hsync_end - hsync_start;
	hback_porch = (adjusted_mode->crtc_htotal -
		       adjusted_mode->crtc_hsync_end);
	vactive = adjusted_mode->crtc_vdisplay;
	vtotal = adjusted_mode->crtc_vtotal;
	vsync_start = adjusted_mode->crtc_vsync_start;
	vsync_end = adjusted_mode->crtc_vsync_end;
	vsync_shift = hsync_start - htotal / 2;

	if (intel_dsi->dual_link) {
		hactive /= 2;
		if (intel_dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK)
			hactive += intel_dsi->pixel_overlap;
		htotal /= 2;
	}

	/* minimum hactive as per bspec: 256 pixels */
	if (adjusted_mode->crtc_hdisplay < 256)
		DRM_ERROR("hactive is less then 256 pixels\n");

	/* if RGB666 format, then hactive must be multiple of 4 pixels */
	if (intel_dsi->pixel_format == MIPI_DSI_FMT_RGB666 && hactive % 4 != 0)
		DRM_ERROR("hactive pixels are not multiple of 4\n");

	/* program TRANS_HTOTAL register */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		I915_WRITE(HTOTAL(dsi_trans),
			   (hactive - 1) | ((htotal - 1) << 16));
	}

	/* TRANS_HSYNC register to be programmed only for video mode */
	if (intel_dsi->operation_mode == INTEL_DSI_VIDEO_MODE) {
		if (intel_dsi->video_mode_format ==
		    VIDEO_MODE_NON_BURST_WITH_SYNC_PULSE) {
			/* BSPEC: hsync size should be atleast 16 pixels */
			if (hsync_size < 16)
				DRM_ERROR("hsync size < 16 pixels\n");
		}

		if (hback_porch < 16)
			DRM_ERROR("hback porch < 16 pixels\n");

		if (intel_dsi->dual_link) {
			hsync_start /= 2;
			hsync_end /= 2;
		}

		for_each_dsi_port(port, intel_dsi->ports) {
			dsi_trans = dsi_port_to_transcoder(port);
			I915_WRITE(HSYNC(dsi_trans),
				   (hsync_start - 1) | ((hsync_end - 1) << 16));
		}
	}

	/* program TRANS_VTOTAL register */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		/*
		 * FIXME: Programing this by assuming progressive mode, since
		 * non-interlaced info from VBT is not saved inside
		 * struct drm_display_mode.
		 * For interlace mode: program required pixel minus 2
		 */
		I915_WRITE(VTOTAL(dsi_trans),
			   (vactive - 1) | ((vtotal - 1) << 16));
	}

	if (vsync_end < vsync_start || vsync_end > vtotal)
		DRM_ERROR("Invalid vsync_end value\n");

	if (vsync_start < vactive)
		DRM_ERROR("vsync_start less than vactive\n");

	/* program TRANS_VSYNC register */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		I915_WRITE(VSYNC(dsi_trans),
			   (vsync_start - 1) | ((vsync_end - 1) << 16));
	}

	/*
	 * FIXME: It has to be programmed only for interlaced
	 * modes. Put the check condition here once interlaced
	 * info available as described above.
	 * program TRANS_VSYNCSHIFT register
	 */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		I915_WRITE(VSYNCSHIFT(dsi_trans), vsync_shift);
	}

	/* program TRANS_VBLANK register, should be same as vtotal programmed */
	if (INTEL_GEN(dev_priv) >= 12) {
		for_each_dsi_port(port, intel_dsi->ports) {
			dsi_trans = dsi_port_to_transcoder(port);
			I915_WRITE(VBLANK(dsi_trans),
				   (vactive - 1) | ((vtotal - 1) << 16));
		}
	}
}

static void gen11_dsi_enable_transcoder(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum port port;
	enum transcoder dsi_trans;
	u32 tmp;

	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		tmp = I915_READ(PIPECONF(dsi_trans));
		tmp |= PIPECONF_ENABLE;
		I915_WRITE(PIPECONF(dsi_trans), tmp);

		/* wait for transcoder to be enabled */
		if (intel_de_wait_for_set(dev_priv, PIPECONF(dsi_trans),
					  I965_PIPECONF_ACTIVE, 10))
			DRM_ERROR("DSI transcoder not enabled\n");
	}
}

static void gen11_dsi_setup_timeouts(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum port port;
	enum transcoder dsi_trans;
	u32 tmp, hs_tx_timeout, lp_rx_timeout, ta_timeout, divisor, mul;

	/*
	 * escape clock count calculation:
	 * BYTE_CLK_COUNT = TIME_NS/(8 * UI)
	 * UI (nsec) = (10^6)/Bitrate
	 * TIME_NS = (BYTE_CLK_COUNT * 8 * 10^6)/ Bitrate
	 * ESCAPE_CLK_COUNT  = TIME_NS/ESC_CLK_NS
	 */
	divisor = intel_dsi_tlpx_ns(intel_dsi) * intel_dsi_bitrate(intel_dsi) * 1000;
	mul = 8 * 1000000;
	hs_tx_timeout = DIV_ROUND_UP(intel_dsi->hs_tx_timeout * mul,
				     divisor);
	lp_rx_timeout = DIV_ROUND_UP(intel_dsi->lp_rx_timeout * mul, divisor);
	ta_timeout = DIV_ROUND_UP(intel_dsi->turn_arnd_val * mul, divisor);

	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);

		/* program hst_tx_timeout */
		tmp = I915_READ(DSI_HSTX_TO(dsi_trans));
		tmp &= ~HSTX_TIMEOUT_VALUE_MASK;
		tmp |= HSTX_TIMEOUT_VALUE(hs_tx_timeout);
		I915_WRITE(DSI_HSTX_TO(dsi_trans), tmp);

		/* FIXME: DSI_CALIB_TO */

		/* program lp_rx_host timeout */
		tmp = I915_READ(DSI_LPRX_HOST_TO(dsi_trans));
		tmp &= ~LPRX_TIMEOUT_VALUE_MASK;
		tmp |= LPRX_TIMEOUT_VALUE(lp_rx_timeout);
		I915_WRITE(DSI_LPRX_HOST_TO(dsi_trans), tmp);

		/* FIXME: DSI_PWAIT_TO */

		/* program turn around timeout */
		tmp = I915_READ(DSI_TA_TO(dsi_trans));
		tmp &= ~TA_TIMEOUT_VALUE_MASK;
		tmp |= TA_TIMEOUT_VALUE(ta_timeout);
		I915_WRITE(DSI_TA_TO(dsi_trans), tmp);
	}
}

static void
gen11_dsi_enable_port_and_phy(struct intel_encoder *encoder,
			      const struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);

	/* step 4a: power up all lanes of the DDI used by DSI */
	gen11_dsi_power_up_lanes(encoder);

	/* step 4b: configure lane sequencing of the Combo-PHY transmitters */
	gen11_dsi_config_phy_lanes_sequence(encoder);

	/* step 4c: configure voltage swing and skew */
	gen11_dsi_voltage_swing_program_seq(encoder);

	/* enable DDI buffer */
	gen11_dsi_enable_ddi_buffer(encoder);

	/* setup D-PHY timings */
	gen11_dsi_setup_dphy_timings(encoder);

	/* step 4h: setup DSI protocol timeouts */
	gen11_dsi_setup_timeouts(encoder);

	/* Step (4h, 4i, 4j, 4k): Configure transcoder */
	gen11_dsi_configure_transcoder(encoder, pipe_config);

	/* Step 4l: Gate DDI clocks */
	if (IS_GEN(dev_priv, 11))
		gen11_dsi_gate_clocks(encoder);
}

static void gen11_dsi_powerup_panel(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct mipi_dsi_device *dsi;
	enum port port;
	enum transcoder dsi_trans;
	u32 tmp;
	int ret;

	/* set maximum return packet size */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);

		/*
		 * FIXME: This uses the number of DW's currently in the payload
		 * receive queue. This is probably not what we want here.
		 */
		tmp = I915_READ(DSI_CMD_RXCTL(dsi_trans));
		tmp &= NUMBER_RX_PLOAD_DW_MASK;
		/* multiply "Number Rx Payload DW" by 4 to get max value */
		tmp = tmp * 4;
		dsi = intel_dsi->dsi_hosts[port]->device;
		ret = mipi_dsi_set_maximum_return_packet_size(dsi, tmp);
		if (ret < 0)
			DRM_ERROR("error setting max return pkt size%d\n", tmp);
	}

	/* panel power on related mipi dsi vbt sequences */
	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_POWER_ON);
	intel_dsi_msleep(intel_dsi, intel_dsi->panel_on_delay);
	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_DEASSERT_RESET);
	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_INIT_OTP);
	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_DISPLAY_ON);

	/* ensure all panel commands dispatched before enabling transcoder */
	wait_for_cmds_dispatched_to_panel(encoder);
}

static void gen11_dsi_pre_pll_enable(struct intel_encoder *encoder,
				     const struct intel_crtc_state *pipe_config,
				     const struct drm_connector_state *conn_state)
{
	/* step2: enable IO power */
	gen11_dsi_enable_io_power(encoder);

	/* step3: enable DSI PLL */
	gen11_dsi_program_esc_clk_div(encoder);
}

static void gen11_dsi_pre_enable(struct intel_encoder *encoder,
				 const struct intel_crtc_state *pipe_config,
				 const struct drm_connector_state *conn_state)
{
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);

	/* step3b */
	gen11_dsi_map_pll(encoder, pipe_config);

	/* step4: enable DSI port and DPHY */
	gen11_dsi_enable_port_and_phy(encoder, pipe_config);

	/* step5: program and powerup panel */
	gen11_dsi_powerup_panel(encoder);

	/* step6c: configure transcoder timings */
	gen11_dsi_set_transcoder_timings(encoder, pipe_config);

	/* step6d: enable dsi transcoder */
	gen11_dsi_enable_transcoder(encoder);

	/* step7: enable backlight */
	intel_panel_enable_backlight(pipe_config, conn_state);
	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_BACKLIGHT_ON);
}

static void gen11_dsi_disable_transcoder(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum port port;
	enum transcoder dsi_trans;
	u32 tmp;

	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);

		/* disable transcoder */
		tmp = I915_READ(PIPECONF(dsi_trans));
		tmp &= ~PIPECONF_ENABLE;
		I915_WRITE(PIPECONF(dsi_trans), tmp);

		/* wait for transcoder to be disabled */
		if (intel_de_wait_for_clear(dev_priv, PIPECONF(dsi_trans),
					    I965_PIPECONF_ACTIVE, 50))
			DRM_ERROR("DSI trancoder not disabled\n");
	}
}

static void gen11_dsi_powerdown_panel(struct intel_encoder *encoder)
{
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);

	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_DISPLAY_OFF);
	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_ASSERT_RESET);
	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_POWER_OFF);

	/* ensure cmds dispatched to panel */
	wait_for_cmds_dispatched_to_panel(encoder);
}

static void gen11_dsi_deconfigure_trancoder(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum port port;
	enum transcoder dsi_trans;
	u32 tmp;

	/* put dsi link in ULPS */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		tmp = I915_READ(DSI_LP_MSG(dsi_trans));
		tmp |= LINK_ENTER_ULPS;
		tmp &= ~LINK_ULPS_TYPE_LP11;
		I915_WRITE(DSI_LP_MSG(dsi_trans), tmp);

		if (wait_for_us((I915_READ(DSI_LP_MSG(dsi_trans)) &
				LINK_IN_ULPS),
				10))
			DRM_ERROR("DSI link not in ULPS\n");
	}

	/* disable ddi function */
	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		tmp = I915_READ(TRANS_DDI_FUNC_CTL(dsi_trans));
		tmp &= ~TRANS_DDI_FUNC_ENABLE;
		I915_WRITE(TRANS_DDI_FUNC_CTL(dsi_trans), tmp);
	}

	/* disable port sync mode if dual link */
	if (intel_dsi->dual_link) {
		for_each_dsi_port(port, intel_dsi->ports) {
			dsi_trans = dsi_port_to_transcoder(port);
			tmp = I915_READ(TRANS_DDI_FUNC_CTL2(dsi_trans));
			tmp &= ~PORT_SYNC_MODE_ENABLE;
			I915_WRITE(TRANS_DDI_FUNC_CTL2(dsi_trans), tmp);
		}
	}
}

static void gen11_dsi_disable_port(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	u32 tmp;
	enum port port;

	gen11_dsi_ungate_clocks(encoder);
	for_each_dsi_port(port, intel_dsi->ports) {
		tmp = I915_READ(DDI_BUF_CTL(port));
		tmp &= ~DDI_BUF_CTL_ENABLE;
		I915_WRITE(DDI_BUF_CTL(port), tmp);

		if (wait_for_us((I915_READ(DDI_BUF_CTL(port)) &
				 DDI_BUF_IS_IDLE),
				 8))
			DRM_ERROR("DDI port:%c buffer not idle\n",
				  port_name(port));
	}
	gen11_dsi_gate_clocks(encoder);
}

static void gen11_dsi_disable_io_power(struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum port port;
	u32 tmp;

	for_each_dsi_port(port, intel_dsi->ports) {
		intel_wakeref_t wakeref;

		wakeref = fetch_and_zero(&intel_dsi->io_wakeref[port]);
		intel_display_power_put(dev_priv,
					port == PORT_A ?
					POWER_DOMAIN_PORT_DDI_A_IO :
					POWER_DOMAIN_PORT_DDI_B_IO,
					wakeref);
	}

	/* set mode to DDI */
	for_each_dsi_port(port, intel_dsi->ports) {
		tmp = I915_READ(ICL_DSI_IO_MODECTL(port));
		tmp &= ~COMBO_PHY_MODE_DSI;
		I915_WRITE(ICL_DSI_IO_MODECTL(port), tmp);
	}
}

static void gen11_dsi_disable(struct intel_encoder *encoder,
			      const struct intel_crtc_state *old_crtc_state,
			      const struct drm_connector_state *old_conn_state)
{
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);

	/* step1: turn off backlight */
	intel_dsi_vbt_exec_sequence(intel_dsi, MIPI_SEQ_BACKLIGHT_OFF);
	intel_panel_disable_backlight(old_conn_state);

	/* step2d,e: disable transcoder and wait */
	gen11_dsi_disable_transcoder(encoder);

	/* step2f,g: powerdown panel */
	gen11_dsi_powerdown_panel(encoder);

	/* step2h,i,j: deconfig trancoder */
	gen11_dsi_deconfigure_trancoder(encoder);

	/* step3: disable port */
	gen11_dsi_disable_port(encoder);

	/* step4: disable IO power */
	gen11_dsi_disable_io_power(encoder);
}

static void gen11_dsi_get_timings(struct intel_encoder *encoder,
				  struct intel_crtc_state *pipe_config)
{
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct drm_display_mode *adjusted_mode =
					&pipe_config->base.adjusted_mode;

	if (intel_dsi->dual_link) {
		adjusted_mode->crtc_hdisplay *= 2;
		if (intel_dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK)
			adjusted_mode->crtc_hdisplay -=
						intel_dsi->pixel_overlap;
		adjusted_mode->crtc_htotal *= 2;
	}
	adjusted_mode->crtc_hblank_start = adjusted_mode->crtc_hdisplay;
	adjusted_mode->crtc_hblank_end = adjusted_mode->crtc_htotal;

	if (intel_dsi->operation_mode == INTEL_DSI_VIDEO_MODE) {
		if (intel_dsi->dual_link) {
			adjusted_mode->crtc_hsync_start *= 2;
			adjusted_mode->crtc_hsync_end *= 2;
		}
	}
	adjusted_mode->crtc_vblank_start = adjusted_mode->crtc_vdisplay;
	adjusted_mode->crtc_vblank_end = adjusted_mode->crtc_vtotal;
}

static void gen11_dsi_get_config(struct intel_encoder *encoder,
				 struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_crtc *crtc = to_intel_crtc(pipe_config->base.crtc);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);

	/* FIXME: adapt icl_ddi_clock_get() for DSI and use that? */
	pipe_config->port_clock =
		cnl_calc_wrpll_link(dev_priv, &pipe_config->dpll_hw_state);

	pipe_config->base.adjusted_mode.crtc_clock = intel_dsi->pclk;
	if (intel_dsi->dual_link)
		pipe_config->base.adjusted_mode.crtc_clock *= 2;

	gen11_dsi_get_timings(encoder, pipe_config);
	pipe_config->output_types |= BIT(INTEL_OUTPUT_DSI);
	pipe_config->pipe_bpp = bdw_get_pipemisc_bpp(crtc);
}

static int gen11_dsi_compute_config(struct intel_encoder *encoder,
				    struct intel_crtc_state *pipe_config,
				    struct drm_connector_state *conn_state)
{
	struct intel_dsi *intel_dsi = container_of(encoder, struct intel_dsi,
						   base);
	struct intel_connector *intel_connector = intel_dsi->attached_connector;
	struct intel_crtc *crtc = to_intel_crtc(pipe_config->base.crtc);
	const struct drm_display_mode *fixed_mode =
					intel_connector->panel.fixed_mode;
	struct drm_display_mode *adjusted_mode =
					&pipe_config->base.adjusted_mode;

	pipe_config->output_format = INTEL_OUTPUT_FORMAT_RGB;
	intel_fixed_panel_mode(fixed_mode, adjusted_mode);
	intel_pch_panel_fitting(crtc, pipe_config, conn_state->scaling_mode);

	adjusted_mode->flags = 0;

	/* Dual link goes to trancoder DSI'0' */
	if (intel_dsi->ports == BIT(PORT_B))
		pipe_config->cpu_transcoder = TRANSCODER_DSI_1;
	else
		pipe_config->cpu_transcoder = TRANSCODER_DSI_0;

	pipe_config->clock_set = true;
	pipe_config->port_clock = intel_dsi_bitrate(intel_dsi) / 5;

	return 0;
}

static void gen11_dsi_get_power_domains(struct intel_encoder *encoder,
					struct intel_crtc_state *crtc_state)
{
	get_dsi_io_power_domains(to_i915(encoder->base.dev),
				 enc_to_intel_dsi(&encoder->base));
}

static bool gen11_dsi_get_hw_state(struct intel_encoder *encoder,
				   enum pipe *pipe)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	enum transcoder dsi_trans;
	intel_wakeref_t wakeref;
	enum port port;
	bool ret = false;
	u32 tmp;

	wakeref = intel_display_power_get_if_enabled(dev_priv,
						     encoder->power_domain);
	if (!wakeref)
		return false;

	for_each_dsi_port(port, intel_dsi->ports) {
		dsi_trans = dsi_port_to_transcoder(port);
		tmp = I915_READ(TRANS_DDI_FUNC_CTL(dsi_trans));
		switch (tmp & TRANS_DDI_EDP_INPUT_MASK) {
		case TRANS_DDI_EDP_INPUT_A_ON:
			*pipe = PIPE_A;
			break;
		case TRANS_DDI_EDP_INPUT_B_ONOFF:
			*pipe = PIPE_B;
			break;
		case TRANS_DDI_EDP_INPUT_C_ONOFF:
			*pipe = PIPE_C;
			break;
		default:
			DRM_ERROR("Invalid PIPE input\n");
			goto out;
		}

		tmp = I915_READ(PIPECONF(dsi_trans));
		ret = tmp & PIPECONF_ENABLE;
	}
out:
	intel_display_power_put(dev_priv, encoder->power_domain, wakeref);
	return ret;
}

static void gen11_dsi_encoder_destroy(struct drm_encoder *encoder)
{
	intel_encoder_destroy(encoder);
}

static const struct drm_encoder_funcs gen11_dsi_encoder_funcs = {
	.destroy = gen11_dsi_encoder_destroy,
};

static const struct drm_connector_funcs gen11_dsi_connector_funcs = {
	.late_register = intel_connector_register,
	.early_unregister = intel_connector_unregister,
	.destroy = intel_connector_destroy,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_get_property = intel_digital_connector_atomic_get_property,
	.atomic_set_property = intel_digital_connector_atomic_set_property,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state = intel_digital_connector_duplicate_state,
};

static const struct drm_connector_helper_funcs gen11_dsi_connector_helper_funcs = {
	.get_modes = intel_dsi_get_modes,
	.mode_valid = intel_dsi_mode_valid,
	.atomic_check = intel_digital_connector_atomic_check,
};

static int gen11_dsi_host_attach(struct mipi_dsi_host *host,
				 struct mipi_dsi_device *dsi)
{
	return 0;
}

static int gen11_dsi_host_detach(struct mipi_dsi_host *host,
				 struct mipi_dsi_device *dsi)
{
	return 0;
}

static ssize_t gen11_dsi_host_transfer(struct mipi_dsi_host *host,
				       const struct mipi_dsi_msg *msg)
{
	struct intel_dsi_host *intel_dsi_host = to_intel_dsi_host(host);
	struct mipi_dsi_packet dsi_pkt;
	ssize_t ret;
	bool enable_lpdt = false;

	ret = mipi_dsi_create_packet(&dsi_pkt, msg);
	if (ret < 0)
		return ret;

	if (msg->flags & MIPI_DSI_MSG_USE_LPM)
		enable_lpdt = true;

	/* send packet header */
	ret  = dsi_send_pkt_hdr(intel_dsi_host, dsi_pkt, enable_lpdt);
	if (ret < 0)
		return ret;

	/* only long packet contains payload */
	if (mipi_dsi_packet_format_is_long(msg->type)) {
		ret = dsi_send_pkt_payld(intel_dsi_host, dsi_pkt);
		if (ret < 0)
			return ret;
	}

	//TODO: add payload receive code if needed

	ret = sizeof(dsi_pkt.header) + dsi_pkt.payload_length;

	return ret;
}

static const struct mipi_dsi_host_ops gen11_dsi_host_ops = {
	.attach = gen11_dsi_host_attach,
	.detach = gen11_dsi_host_detach,
	.transfer = gen11_dsi_host_transfer,
};

#define ICL_PREPARE_CNT_MAX	0x7
#define ICL_CLK_ZERO_CNT_MAX	0xf
#define ICL_TRAIL_CNT_MAX	0x7
#define ICL_TCLK_PRE_CNT_MAX	0x3
#define ICL_TCLK_POST_CNT_MAX	0x7
#define ICL_HS_ZERO_CNT_MAX	0xf
#define ICL_EXIT_ZERO_CNT_MAX	0x7

static void icl_dphy_param_init(struct intel_dsi *intel_dsi)
{
	struct drm_device *dev = intel_dsi->base.base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct mipi_config *mipi_config = dev_priv->vbt.dsi.config;
	u32 tlpx_ns;
	u32 prepare_cnt, exit_zero_cnt, clk_zero_cnt, trail_cnt;
	u32 ths_prepare_ns, tclk_trail_ns;
	u32 hs_zero_cnt;
	u32 tclk_pre_cnt, tclk_post_cnt;

	tlpx_ns = intel_dsi_tlpx_ns(intel_dsi);

	tclk_trail_ns = max(mipi_config->tclk_trail, mipi_config->ths_trail);
	ths_prepare_ns = max(mipi_config->ths_prepare,
			     mipi_config->tclk_prepare);

	/*
	 * prepare cnt in escape clocks
	 * this field represents a hexadecimal value with a precision
	 * of 1.2 – i.e. the most significant bit is the integer
	 * and the least significant 2 bits are fraction bits.
	 * so, the field can represent a range of 0.25 to 1.75
	 */
	prepare_cnt = DIV_ROUND_UP(ths_prepare_ns * 4, tlpx_ns);
	if (prepare_cnt > ICL_PREPARE_CNT_MAX) {
		DRM_DEBUG_KMS("prepare_cnt out of range (%d)\n", prepare_cnt);
		prepare_cnt = ICL_PREPARE_CNT_MAX;
	}

	/* clk zero count in escape clocks */
	clk_zero_cnt = DIV_ROUND_UP(mipi_config->tclk_prepare_clkzero -
				    ths_prepare_ns, tlpx_ns);
	if (clk_zero_cnt > ICL_CLK_ZERO_CNT_MAX) {
		DRM_DEBUG_KMS("clk_zero_cnt out of range (%d)\n", clk_zero_cnt);
		clk_zero_cnt = ICL_CLK_ZERO_CNT_MAX;
	}

	/* trail cnt in escape clocks*/
	trail_cnt = DIV_ROUND_UP(tclk_trail_ns, tlpx_ns);
	if (trail_cnt > ICL_TRAIL_CNT_MAX) {
		DRM_DEBUG_KMS("trail_cnt out of range (%d)\n", trail_cnt);
		trail_cnt = ICL_TRAIL_CNT_MAX;
	}

	/* tclk pre count in escape clocks */
	tclk_pre_cnt = DIV_ROUND_UP(mipi_config->tclk_pre, tlpx_ns);
	if (tclk_pre_cnt > ICL_TCLK_PRE_CNT_MAX) {
		DRM_DEBUG_KMS("tclk_pre_cnt out of range (%d)\n", tclk_pre_cnt);
		tclk_pre_cnt = ICL_TCLK_PRE_CNT_MAX;
	}

	/* tclk post count in escape clocks */
	tclk_post_cnt = DIV_ROUND_UP(mipi_config->tclk_post, tlpx_ns);
	if (tclk_post_cnt > ICL_TCLK_POST_CNT_MAX) {
		DRM_DEBUG_KMS("tclk_post_cnt out of range (%d)\n", tclk_post_cnt);
		tclk_post_cnt = ICL_TCLK_POST_CNT_MAX;
	}

	/* hs zero cnt in escape clocks */
	hs_zero_cnt = DIV_ROUND_UP(mipi_config->ths_prepare_hszero -
				   ths_prepare_ns, tlpx_ns);
	if (hs_zero_cnt > ICL_HS_ZERO_CNT_MAX) {
		DRM_DEBUG_KMS("hs_zero_cnt out of range (%d)\n", hs_zero_cnt);
		hs_zero_cnt = ICL_HS_ZERO_CNT_MAX;
	}

	/* hs exit zero cnt in escape clocks */
	exit_zero_cnt = DIV_ROUND_UP(mipi_config->ths_exit, tlpx_ns);
	if (exit_zero_cnt > ICL_EXIT_ZERO_CNT_MAX) {
		DRM_DEBUG_KMS("exit_zero_cnt out of range (%d)\n", exit_zero_cnt);
		exit_zero_cnt = ICL_EXIT_ZERO_CNT_MAX;
	}

	/* clock lane dphy timings */
	intel_dsi->dphy_reg = (CLK_PREPARE_OVERRIDE |
			       CLK_PREPARE(prepare_cnt) |
			       CLK_ZERO_OVERRIDE |
			       CLK_ZERO(clk_zero_cnt) |
			       CLK_PRE_OVERRIDE |
			       CLK_PRE(tclk_pre_cnt) |
			       CLK_POST_OVERRIDE |
			       CLK_POST(tclk_post_cnt) |
			       CLK_TRAIL_OVERRIDE |
			       CLK_TRAIL(trail_cnt));

	/* data lanes dphy timings */
	intel_dsi->dphy_data_lane_reg = (HS_PREPARE_OVERRIDE |
					 HS_PREPARE(prepare_cnt) |
					 HS_ZERO_OVERRIDE |
					 HS_ZERO(hs_zero_cnt) |
					 HS_TRAIL_OVERRIDE |
					 HS_TRAIL(trail_cnt) |
					 HS_EXIT_OVERRIDE |
					 HS_EXIT(exit_zero_cnt));

	intel_dsi_log_params(intel_dsi);
}

static void icl_dsi_add_properties(struct intel_connector *connector)
{
	u32 allowed_scalers;

	allowed_scalers = BIT(DRM_MODE_SCALE_ASPECT) |
			   BIT(DRM_MODE_SCALE_FULLSCREEN) |
			   BIT(DRM_MODE_SCALE_CENTER);

	drm_connector_attach_scaling_mode_property(&connector->base,
						   allowed_scalers);

	connector->base.state->scaling_mode = DRM_MODE_SCALE_ASPECT;

	connector->base.display_info.panel_orientation =
			intel_dsi_get_panel_orientation(connector);
	drm_connector_init_panel_orientation_property(&connector->base,
				connector->panel.fixed_mode->hdisplay,
				connector->panel.fixed_mode->vdisplay);
}

void icl_dsi_init(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;
	struct intel_dsi *intel_dsi;
	struct intel_encoder *encoder;
	struct intel_connector *intel_connector;
	struct drm_connector *connector;
	struct drm_display_mode *fixed_mode;
	enum port port;

	if (!intel_bios_is_dsi_present(dev_priv, &port))
		return;

	intel_dsi = kzalloc(sizeof(*intel_dsi), GFP_KERNEL);
	if (!intel_dsi)
		return;

	intel_connector = intel_connector_alloc();
	if (!intel_connector) {
		kfree(intel_dsi);
		return;
	}

	encoder = &intel_dsi->base;
	intel_dsi->attached_connector = intel_connector;
	connector = &intel_connector->base;

	/* register DSI encoder with DRM subsystem */
	drm_encoder_init(dev, &encoder->base, &gen11_dsi_encoder_funcs,
			 DRM_MODE_ENCODER_DSI, "DSI %c", port_name(port));

	encoder->pre_pll_enable = gen11_dsi_pre_pll_enable;
	encoder->pre_enable = gen11_dsi_pre_enable;
	encoder->disable = gen11_dsi_disable;
	encoder->port = port;
	encoder->get_config = gen11_dsi_get_config;
	encoder->update_pipe = intel_panel_update_backlight;
	encoder->compute_config = gen11_dsi_compute_config;
	encoder->get_hw_state = gen11_dsi_get_hw_state;
	encoder->type = INTEL_OUTPUT_DSI;
	encoder->cloneable = 0;
	encoder->crtc_mask = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C);
	encoder->power_domain = POWER_DOMAIN_PORT_DSI;
	encoder->get_power_domains = gen11_dsi_get_power_domains;

	/* register DSI connector with DRM subsystem */
	drm_connector_init(dev, connector, &gen11_dsi_connector_funcs,
			   DRM_MODE_CONNECTOR_DSI);
	drm_connector_helper_add(connector, &gen11_dsi_connector_helper_funcs);
	connector->display_info.subpixel_order = SubPixelHorizontalRGB;
	connector->interlace_allowed = false;
	connector->doublescan_allowed = false;
	intel_connector->get_hw_state = intel_connector_get_hw_state;

	/* attach connector to encoder */
	intel_connector_attach_encoder(intel_connector, encoder);

	mutex_lock(&dev->mode_config.mutex);
	fixed_mode = intel_panel_vbt_fixed_mode(intel_connector);
	mutex_unlock(&dev->mode_config.mutex);

	if (!fixed_mode) {
		DRM_ERROR("DSI fixed mode info missing\n");
		goto err;
	}

	intel_panel_init(&intel_connector->panel, fixed_mode, NULL);
	intel_panel_setup_backlight(connector, INVALID_PIPE);

	if (dev_priv->vbt.dsi.config->dual_link)
		intel_dsi->ports = BIT(PORT_A) | BIT(PORT_B);
	else
		intel_dsi->ports = BIT(port);

	intel_dsi->dcs_backlight_ports = dev_priv->vbt.dsi.bl_ports;
	intel_dsi->dcs_cabc_ports = dev_priv->vbt.dsi.cabc_ports;

	for_each_dsi_port(port, intel_dsi->ports) {
		struct intel_dsi_host *host;

		host = intel_dsi_host_init(intel_dsi, &gen11_dsi_host_ops, port);
		if (!host)
			goto err;

		intel_dsi->dsi_hosts[port] = host;
	}

	if (!intel_dsi_vbt_init(intel_dsi, MIPI_DSI_GENERIC_PANEL_ID)) {
		DRM_DEBUG_KMS("no device found\n");
		goto err;
	}

	icl_dphy_param_init(intel_dsi);

	icl_dsi_add_properties(intel_connector);
	return;

err:
	drm_encoder_cleanup(&encoder->base);
	kfree(intel_dsi);
	kfree(intel_connector);
}
