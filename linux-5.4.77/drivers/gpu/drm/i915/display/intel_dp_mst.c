/*
 * Copyright © 2008 Intel Corporation
 *             2014 Red Hat Inc.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

#include "i915_drv.h"
#include "intel_atomic.h"
#include "intel_audio.h"
#include "intel_connector.h"
#include "intel_ddi.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_mst.h"
#include "intel_dpio_phy.h"

static int intel_dp_mst_compute_link_config(struct intel_encoder *encoder,
					    struct intel_crtc_state *crtc_state,
					    struct drm_connector_state *conn_state,
					    struct link_config_limits *limits)
{
	struct drm_atomic_state *state = crtc_state->base.state;
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_dp *intel_dp = &intel_mst->primary->dp;
	struct intel_connector *connector =
		to_intel_connector(conn_state->connector);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->base.adjusted_mode;
	void *port = connector->port;
	bool constant_n = drm_dp_has_quirk(&intel_dp->desc,
					   DP_DPCD_QUIRK_CONSTANT_N);
	int bpp, slots = -EINVAL;

	crtc_state->lane_count = limits->max_lane_count;
	crtc_state->port_clock = limits->max_clock;

	for (bpp = limits->max_bpp; bpp >= limits->min_bpp; bpp -= 2 * 3) {
		crtc_state->pipe_bpp = bpp;

		crtc_state->pbn = drm_dp_calc_pbn_mode(adjusted_mode->crtc_clock,
						       crtc_state->pipe_bpp);

		slots = drm_dp_atomic_find_vcpi_slots(state, &intel_dp->mst_mgr,
						      port, crtc_state->pbn);
		if (slots == -EDEADLK)
			return slots;
		if (slots >= 0)
			break;
	}

	if (slots < 0) {
		DRM_DEBUG_KMS("failed finding vcpi slots:%d\n", slots);
		return slots;
	}

	intel_link_compute_m_n(crtc_state->pipe_bpp,
			       crtc_state->lane_count,
			       adjusted_mode->crtc_clock,
			       crtc_state->port_clock,
			       &crtc_state->dp_m_n,
			       constant_n, crtc_state->fec_enable);
	crtc_state->dp_m_n.tu = slots;

	return 0;
}

static int intel_dp_mst_compute_config(struct intel_encoder *encoder,
				       struct intel_crtc_state *pipe_config,
				       struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_dp *intel_dp = &intel_mst->primary->dp;
	struct intel_connector *connector =
		to_intel_connector(conn_state->connector);
	struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(conn_state);
	const struct drm_display_mode *adjusted_mode =
		&pipe_config->base.adjusted_mode;
	void *port = connector->port;
	struct link_config_limits limits;
	int ret;

	if (adjusted_mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	pipe_config->output_format = INTEL_OUTPUT_FORMAT_RGB;
	pipe_config->has_pch_encoder = false;

	if (intel_conn_state->force_audio == HDMI_AUDIO_AUTO)
		pipe_config->has_audio =
			drm_dp_mst_port_has_audio(&intel_dp->mst_mgr, port);
	else
		pipe_config->has_audio =
			intel_conn_state->force_audio == HDMI_AUDIO_ON;

	/*
	 * for MST we always configure max link bw - the spec doesn't
	 * seem to suggest we should do otherwise.
	 */
	limits.min_clock =
	limits.max_clock = intel_dp_max_link_rate(intel_dp);

	limits.min_lane_count =
	limits.max_lane_count = intel_dp_max_lane_count(intel_dp);

	limits.min_bpp = intel_dp_min_bpp(pipe_config);
	/*
	 * FIXME: If all the streams can't fit into the link with
	 * their current pipe_bpp we should reduce pipe_bpp across
	 * the board until things start to fit. Until then we
	 * limit to <= 8bpc since that's what was hardcoded for all
	 * MST streams previously. This hack should be removed once
	 * we have the proper retry logic in place.
	 */
	limits.max_bpp = min(pipe_config->pipe_bpp, 24);

	intel_dp_adjust_compliance_config(intel_dp, pipe_config, &limits);

	ret = intel_dp_mst_compute_link_config(encoder, pipe_config,
					       conn_state, &limits);
	if (ret)
		return ret;

	pipe_config->limited_color_range =
		intel_dp_limited_color_range(pipe_config, conn_state);

	if (IS_GEN9_LP(dev_priv))
		pipe_config->lane_lat_optim_mask =
			bxt_ddi_phy_calc_lane_lat_optim_mask(pipe_config->lane_count);

	intel_ddi_compute_min_voltage_level(dev_priv, pipe_config);

	return 0;
}

static int
intel_dp_mst_atomic_check(struct drm_connector *connector,
			  struct drm_atomic_state *state)
{
	struct drm_connector_state *new_conn_state =
		drm_atomic_get_new_connector_state(state, connector);
	struct drm_connector_state *old_conn_state =
		drm_atomic_get_old_connector_state(state, connector);
	struct intel_connector *intel_connector =
		to_intel_connector(connector);
	struct drm_crtc *new_crtc = new_conn_state->crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_dp_mst_topology_mgr *mgr;
	int ret;

	ret = intel_digital_connector_atomic_check(connector, state);
	if (ret)
		return ret;

	if (!old_conn_state->crtc)
		return 0;

	/* We only want to free VCPI if this state disables the CRTC on this
	 * connector
	 */
	if (new_crtc) {
		crtc_state = drm_atomic_get_new_crtc_state(state, new_crtc);

		if (!crtc_state ||
		    !drm_atomic_crtc_needs_modeset(crtc_state) ||
		    crtc_state->enable)
			return 0;
	}

	mgr = &enc_to_mst(old_conn_state->best_encoder)->primary->dp.mst_mgr;
	ret = drm_dp_atomic_release_vcpi_slots(state, mgr,
					       intel_connector->port);

	return ret;
}

static void intel_mst_disable_dp(struct intel_encoder *encoder,
				 const struct intel_crtc_state *old_crtc_state,
				 const struct drm_connector_state *old_conn_state)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_digital_port *intel_dig_port = intel_mst->primary;
	struct intel_dp *intel_dp = &intel_dig_port->dp;
	struct intel_connector *connector =
		to_intel_connector(old_conn_state->connector);
	int ret;

	DRM_DEBUG_KMS("active links %d\n", intel_dp->active_mst_links);

	drm_dp_mst_reset_vcpi_slots(&intel_dp->mst_mgr, connector->port);

	ret = drm_dp_update_payload_part1(&intel_dp->mst_mgr);
	if (ret) {
		DRM_ERROR("failed to update payload %d\n", ret);
	}
	if (old_crtc_state->has_audio)
		intel_audio_codec_disable(encoder,
					  old_crtc_state, old_conn_state);
}

static void intel_mst_post_disable_dp(struct intel_encoder *encoder,
				      const struct intel_crtc_state *old_crtc_state,
				      const struct drm_connector_state *old_conn_state)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_digital_port *intel_dig_port = intel_mst->primary;
	struct intel_dp *intel_dp = &intel_dig_port->dp;
	struct intel_connector *connector =
		to_intel_connector(old_conn_state->connector);

	intel_ddi_disable_pipe_clock(old_crtc_state);

	/* this can fail */
	drm_dp_check_act_status(&intel_dp->mst_mgr);
	/* and this can also fail */
	drm_dp_update_payload_part2(&intel_dp->mst_mgr);

	drm_dp_mst_deallocate_vcpi(&intel_dp->mst_mgr, connector->port);

	/*
	 * Power down mst path before disabling the port, otherwise we end
	 * up getting interrupts from the sink upon detecting link loss.
	 */
	drm_dp_send_power_updown_phy(&intel_dp->mst_mgr, connector->port,
				     false);

	intel_dp->active_mst_links--;

	intel_mst->connector = NULL;
	if (intel_dp->active_mst_links == 0) {
		intel_dp_sink_dpms(intel_dp, DRM_MODE_DPMS_OFF);
		intel_dig_port->base.post_disable(&intel_dig_port->base,
						  old_crtc_state, NULL);
	}

	DRM_DEBUG_KMS("active links %d\n", intel_dp->active_mst_links);
}

static void intel_mst_pre_pll_enable_dp(struct intel_encoder *encoder,
					const struct intel_crtc_state *pipe_config,
					const struct drm_connector_state *conn_state)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_digital_port *intel_dig_port = intel_mst->primary;
	struct intel_dp *intel_dp = &intel_dig_port->dp;

	if (intel_dp->active_mst_links == 0)
		intel_dig_port->base.pre_pll_enable(&intel_dig_port->base,
						    pipe_config, NULL);
}

static void intel_mst_post_pll_disable_dp(struct intel_encoder *encoder,
					  const struct intel_crtc_state *old_crtc_state,
					  const struct drm_connector_state *old_conn_state)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_digital_port *intel_dig_port = intel_mst->primary;
	struct intel_dp *intel_dp = &intel_dig_port->dp;

	if (intel_dp->active_mst_links == 0)
		intel_dig_port->base.post_pll_disable(&intel_dig_port->base,
						      old_crtc_state,
						      old_conn_state);
}

static void intel_mst_pre_enable_dp(struct intel_encoder *encoder,
				    const struct intel_crtc_state *pipe_config,
				    const struct drm_connector_state *conn_state)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_digital_port *intel_dig_port = intel_mst->primary;
	struct intel_dp *intel_dp = &intel_dig_port->dp;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum port port = intel_dig_port->base.port;
	struct intel_connector *connector =
		to_intel_connector(conn_state->connector);
	int ret;
	u32 temp;

	/* MST encoders are bound to a crtc, not to a connector,
	 * force the mapping here for get_hw_state.
	 */
	connector->encoder = encoder;
	intel_mst->connector = connector;

	DRM_DEBUG_KMS("active links %d\n", intel_dp->active_mst_links);

	if (intel_dp->active_mst_links == 0)
		intel_dp_sink_dpms(intel_dp, DRM_MODE_DPMS_ON);

	drm_dp_send_power_updown_phy(&intel_dp->mst_mgr, connector->port, true);

	if (intel_dp->active_mst_links == 0)
		intel_dig_port->base.pre_enable(&intel_dig_port->base,
						pipe_config, NULL);

	ret = drm_dp_mst_allocate_vcpi(&intel_dp->mst_mgr,
				       connector->port,
				       pipe_config->pbn,
				       pipe_config->dp_m_n.tu);
	if (!ret)
		DRM_ERROR("failed to allocate vcpi\n");

	intel_dp->active_mst_links++;
	temp = I915_READ(DP_TP_STATUS(port));
	I915_WRITE(DP_TP_STATUS(port), temp);

	ret = drm_dp_update_payload_part1(&intel_dp->mst_mgr);

	intel_ddi_enable_pipe_clock(pipe_config);
}

static void intel_mst_enable_dp(struct intel_encoder *encoder,
				const struct intel_crtc_state *pipe_config,
				const struct drm_connector_state *conn_state)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_digital_port *intel_dig_port = intel_mst->primary;
	struct intel_dp *intel_dp = &intel_dig_port->dp;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	enum port port = intel_dig_port->base.port;

	DRM_DEBUG_KMS("active links %d\n", intel_dp->active_mst_links);

	if (intel_de_wait_for_set(dev_priv, DP_TP_STATUS(port),
				  DP_TP_STATUS_ACT_SENT, 1))
		DRM_ERROR("Timed out waiting for ACT sent\n");

	drm_dp_check_act_status(&intel_dp->mst_mgr);

	drm_dp_update_payload_part2(&intel_dp->mst_mgr);
	if (pipe_config->has_audio)
		intel_audio_codec_enable(encoder, pipe_config, conn_state);
}

static bool intel_dp_mst_enc_get_hw_state(struct intel_encoder *encoder,
				      enum pipe *pipe)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	*pipe = intel_mst->pipe;
	if (intel_mst->connector)
		return true;
	return false;
}

static void intel_dp_mst_enc_get_config(struct intel_encoder *encoder,
					struct intel_crtc_state *pipe_config)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(&encoder->base);
	struct intel_digital_port *intel_dig_port = intel_mst->primary;

	intel_ddi_get_config(&intel_dig_port->base, pipe_config);
}

static int intel_dp_mst_get_ddc_modes(struct drm_connector *connector)
{
	struct intel_connector *intel_connector = to_intel_connector(connector);
	struct intel_dp *intel_dp = intel_connector->mst_port;
	struct edid *edid;
	int ret;

	if (drm_connector_is_unregistered(connector))
		return intel_connector_update_modes(connector, NULL);

	edid = drm_dp_mst_get_edid(connector, &intel_dp->mst_mgr, intel_connector->port);
	ret = intel_connector_update_modes(connector, edid);
	kfree(edid);

	return ret;
}

static enum drm_connector_status
intel_dp_mst_detect(struct drm_connector *connector, bool force)
{
	struct intel_connector *intel_connector = to_intel_connector(connector);
	struct intel_dp *intel_dp = intel_connector->mst_port;

	if (drm_connector_is_unregistered(connector))
		return connector_status_disconnected;
	return drm_dp_mst_detect_port(connector, &intel_dp->mst_mgr,
				      intel_connector->port);
}

static const struct drm_connector_funcs intel_dp_mst_connector_funcs = {
	.detect = intel_dp_mst_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_get_property = intel_digital_connector_atomic_get_property,
	.atomic_set_property = intel_digital_connector_atomic_set_property,
	.late_register = intel_connector_register,
	.early_unregister = intel_connector_unregister,
	.destroy = intel_connector_destroy,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state = intel_digital_connector_duplicate_state,
};

static int intel_dp_mst_get_modes(struct drm_connector *connector)
{
	return intel_dp_mst_get_ddc_modes(connector);
}

static enum drm_mode_status
intel_dp_mst_mode_valid(struct drm_connector *connector,
			struct drm_display_mode *mode)
{
	struct intel_connector *intel_connector = to_intel_connector(connector);
	struct intel_dp *intel_dp = intel_connector->mst_port;
	int max_dotclk = to_i915(connector->dev)->max_dotclk_freq;
	int max_rate, mode_rate, max_lanes, max_link_clock;

	if (drm_connector_is_unregistered(connector))
		return MODE_ERROR;

	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;

	max_link_clock = intel_dp_max_link_rate(intel_dp);
	max_lanes = intel_dp_max_lane_count(intel_dp);

	max_rate = intel_dp_max_data_rate(max_link_clock, max_lanes);
	mode_rate = intel_dp_link_required(mode->clock, 18);

	/* TODO - validate mode against available PBN for link */
	if (mode->clock < 10000)
		return MODE_CLOCK_LOW;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		return MODE_H_ILLEGAL;

	if (mode_rate > max_rate || mode->clock > max_dotclk)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static struct drm_encoder *intel_mst_atomic_best_encoder(struct drm_connector *connector,
							 struct drm_connector_state *state)
{
	struct intel_connector *intel_connector = to_intel_connector(connector);
	struct intel_dp *intel_dp = intel_connector->mst_port;
	struct intel_crtc *crtc = to_intel_crtc(state->crtc);

	return &intel_dp->mst_encoders[crtc->pipe]->base.base;
}

static const struct drm_connector_helper_funcs intel_dp_mst_connector_helper_funcs = {
	.get_modes = intel_dp_mst_get_modes,
	.mode_valid = intel_dp_mst_mode_valid,
	.atomic_best_encoder = intel_mst_atomic_best_encoder,
	.atomic_check = intel_dp_mst_atomic_check,
};

static void intel_dp_mst_encoder_destroy(struct drm_encoder *encoder)
{
	struct intel_dp_mst_encoder *intel_mst = enc_to_mst(encoder);

	drm_encoder_cleanup(encoder);
	kfree(intel_mst);
}

static const struct drm_encoder_funcs intel_dp_mst_enc_funcs = {
	.destroy = intel_dp_mst_encoder_destroy,
};

static bool intel_dp_mst_get_hw_state(struct intel_connector *connector)
{
	if (connector->encoder && connector->base.state->crtc) {
		enum pipe pipe;
		if (!connector->encoder->get_hw_state(connector->encoder, &pipe))
			return false;
		return true;
	}
	return false;
}

static struct drm_connector *intel_dp_add_mst_connector(struct drm_dp_mst_topology_mgr *mgr, struct drm_dp_mst_port *port, const char *pathprop)
{
	struct intel_dp *intel_dp = container_of(mgr, struct intel_dp, mst_mgr);
	struct intel_digital_port *intel_dig_port = dp_to_dig_port(intel_dp);
	struct drm_device *dev = intel_dig_port->base.base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_connector *intel_connector;
	struct drm_connector *connector;
	enum pipe pipe;
	int ret;

	intel_connector = intel_connector_alloc();
	if (!intel_connector)
		return NULL;

	intel_connector->get_hw_state = intel_dp_mst_get_hw_state;
	intel_connector->mst_port = intel_dp;
	intel_connector->port = port;
	drm_dp_mst_get_port_malloc(port);

	connector = &intel_connector->base;
	ret = drm_connector_init(dev, connector, &intel_dp_mst_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret) {
		intel_connector_free(intel_connector);
		return NULL;
	}

	drm_connector_helper_add(connector, &intel_dp_mst_connector_helper_funcs);

	for_each_pipe(dev_priv, pipe) {
		struct drm_encoder *enc =
			&intel_dp->mst_encoders[pipe]->base.base;

		ret = drm_connector_attach_encoder(&intel_connector->base, enc);
		if (ret)
			goto err;
	}

	drm_object_attach_property(&connector->base, dev->mode_config.path_property, 0);
	drm_object_attach_property(&connector->base, dev->mode_config.tile_property, 0);

	ret = drm_connector_set_path_property(connector, pathprop);
	if (ret)
		goto err;

	intel_attach_force_audio_property(connector);
	intel_attach_broadcast_rgb_property(connector);

	/*
	 * Reuse the prop from the SST connector because we're
	 * not allowed to create new props after device registration.
	 */
	connector->max_bpc_property =
		intel_dp->attached_connector->base.max_bpc_property;
	if (connector->max_bpc_property)
		drm_connector_attach_max_bpc_property(connector, 6, 12);

	return connector;

err:
	drm_connector_cleanup(connector);
	return NULL;
}

static void intel_dp_register_mst_connector(struct drm_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->dev);

	if (dev_priv->fbdev)
		drm_fb_helper_add_one_connector(&dev_priv->fbdev->helper,
						connector);

	drm_connector_register(connector);
}

static void intel_dp_destroy_mst_connector(struct drm_dp_mst_topology_mgr *mgr,
					   struct drm_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->dev);

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", connector->base.id, connector->name);
	drm_connector_unregister(connector);

	if (dev_priv->fbdev)
		drm_fb_helper_remove_one_connector(&dev_priv->fbdev->helper,
						   connector);

	drm_connector_put(connector);
}

static const struct drm_dp_mst_topology_cbs mst_cbs = {
	.add_connector = intel_dp_add_mst_connector,
	.register_connector = intel_dp_register_mst_connector,
	.destroy_connector = intel_dp_destroy_mst_connector,
};

static struct intel_dp_mst_encoder *
intel_dp_create_fake_mst_encoder(struct intel_digital_port *intel_dig_port, enum pipe pipe)
{
	struct intel_dp_mst_encoder *intel_mst;
	struct intel_encoder *intel_encoder;
	struct drm_device *dev = intel_dig_port->base.base.dev;

	intel_mst = kzalloc(sizeof(*intel_mst), GFP_KERNEL);

	if (!intel_mst)
		return NULL;

	intel_mst->pipe = pipe;
	intel_encoder = &intel_mst->base;
	intel_mst->primary = intel_dig_port;

	drm_encoder_init(dev, &intel_encoder->base, &intel_dp_mst_enc_funcs,
			 DRM_MODE_ENCODER_DPMST, "DP-MST %c", pipe_name(pipe));

	intel_encoder->type = INTEL_OUTPUT_DP_MST;
	intel_encoder->power_domain = intel_dig_port->base.power_domain;
	intel_encoder->port = intel_dig_port->base.port;
	intel_encoder->crtc_mask = 0x7;
	intel_encoder->cloneable = 0;

	intel_encoder->compute_config = intel_dp_mst_compute_config;
	intel_encoder->disable = intel_mst_disable_dp;
	intel_encoder->post_disable = intel_mst_post_disable_dp;
	intel_encoder->pre_pll_enable = intel_mst_pre_pll_enable_dp;
	intel_encoder->post_pll_disable = intel_mst_post_pll_disable_dp;
	intel_encoder->pre_enable = intel_mst_pre_enable_dp;
	intel_encoder->enable = intel_mst_enable_dp;
	intel_encoder->get_hw_state = intel_dp_mst_enc_get_hw_state;
	intel_encoder->get_config = intel_dp_mst_enc_get_config;

	return intel_mst;

}

static bool
intel_dp_create_fake_mst_encoders(struct intel_digital_port *intel_dig_port)
{
	struct intel_dp *intel_dp = &intel_dig_port->dp;
	struct drm_i915_private *dev_priv = to_i915(intel_dig_port->base.base.dev);
	enum pipe pipe;

	for_each_pipe(dev_priv, pipe)
		intel_dp->mst_encoders[pipe] = intel_dp_create_fake_mst_encoder(intel_dig_port, pipe);
	return true;
}

int
intel_dp_mst_encoder_active_links(struct intel_digital_port *intel_dig_port)
{
	return intel_dig_port->dp.active_mst_links;
}

int
intel_dp_mst_encoder_init(struct intel_digital_port *intel_dig_port, int conn_base_id)
{
	struct drm_i915_private *i915 = to_i915(intel_dig_port->base.base.dev);
	struct intel_dp *intel_dp = &intel_dig_port->dp;
	enum port port = intel_dig_port->base.port;
	int ret;

	if (!HAS_DP_MST(i915) || intel_dp_is_edp(intel_dp))
		return 0;

	if (INTEL_GEN(i915) < 12 && port == PORT_A)
		return 0;

	if (INTEL_GEN(i915) < 11 && port == PORT_E)
		return 0;

	intel_dp->mst_mgr.cbs = &mst_cbs;

	/* create encoders */
	intel_dp_create_fake_mst_encoders(intel_dig_port);
	ret = drm_dp_mst_topology_mgr_init(&intel_dp->mst_mgr, &i915->drm,
					   &intel_dp->aux, 16, 3, conn_base_id);
	if (ret)
		return ret;

	intel_dp->can_mst = true;

	return 0;
}

void
intel_dp_mst_encoder_cleanup(struct intel_digital_port *intel_dig_port)
{
	struct intel_dp *intel_dp = &intel_dig_port->dp;

	if (!intel_dp->can_mst)
		return;

	drm_dp_mst_topology_mgr_destroy(&intel_dp->mst_mgr);
	/* encoders will get killed by normal cleanup */
}
