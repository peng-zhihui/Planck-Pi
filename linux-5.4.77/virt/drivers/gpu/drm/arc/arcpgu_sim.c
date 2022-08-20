// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARC PGU DRM driver.
 *
 * Copyright (C) 2016 Synopsys, Inc. (www.synopsys.com)
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_probe_helper.h>

#include "arcpgu.h"

#define XRES_DEF	640
#define YRES_DEF	480

#define XRES_MAX	8192
#define YRES_MAX	8192


struct arcpgu_drm_connector {
	struct drm_connector connector;
};

static int arcpgu_drm_connector_get_modes(struct drm_connector *connector)
{
	int count;

	count = drm_add_modes_noedid(connector, XRES_MAX, YRES_MAX);
	drm_set_preferred_mode(connector, XRES_DEF, YRES_DEF);
	return count;
}

static void arcpgu_drm_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_helper_funcs
arcpgu_drm_connector_helper_funcs = {
	.get_modes = arcpgu_drm_connector_get_modes,
};

static const struct drm_connector_funcs arcpgu_drm_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = arcpgu_drm_connector_destroy,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static struct drm_encoder_funcs arcpgu_drm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int arcpgu_drm_sim_init(struct drm_device *drm, struct device_node *np)
{
	struct arcpgu_drm_connector *arcpgu_connector;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	int ret;

	encoder = devm_kzalloc(drm->dev, sizeof(*encoder), GFP_KERNEL);
	if (encoder == NULL)
		return -ENOMEM;

	encoder->possible_crtcs = 1;
	encoder->possible_clones = 0;

	ret = drm_encoder_init(drm, encoder, &arcpgu_drm_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret)
		return ret;

	arcpgu_connector = devm_kzalloc(drm->dev, sizeof(*arcpgu_connector),
					GFP_KERNEL);
	if (!arcpgu_connector) {
		ret = -ENOMEM;
		goto error_encoder_cleanup;
	}

	connector = &arcpgu_connector->connector;
	drm_connector_helper_add(connector, &arcpgu_drm_connector_helper_funcs);

	ret = drm_connector_init(drm, connector, &arcpgu_drm_connector_funcs,
			DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret < 0) {
		dev_err(drm->dev, "failed to initialize drm connector\n");
		goto error_encoder_cleanup;
	}

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret < 0) {
		dev_err(drm->dev, "could not attach connector to encoder\n");
		drm_connector_unregister(connector);
		goto error_connector_cleanup;
	}

	return 0;

error_connector_cleanup:
	drm_connector_cleanup(connector);

error_encoder_cleanup:
	drm_encoder_cleanup(encoder);
	return ret;
}
