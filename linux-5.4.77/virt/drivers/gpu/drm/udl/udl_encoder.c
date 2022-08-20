// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * based in parts on udlfb.c:
 * Copyright (C) 2009 Roberto De Ioris <roberto@unbit.it>
 * Copyright (C) 2009 Jaya Kumar <jayakumar.lkml@gmail.com>
 * Copyright (C) 2009 Bernie Thompson <bernie@plugable.com>
 */

#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper_vtables.h>

#include "udl_drv.h"

/* dummy encoder */
static void udl_enc_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static void udl_encoder_disable(struct drm_encoder *encoder)
{
}

static void udl_encoder_prepare(struct drm_encoder *encoder)
{
}

static void udl_encoder_commit(struct drm_encoder *encoder)
{
}

static void udl_encoder_mode_set(struct drm_encoder *encoder,
				 struct drm_display_mode *mode,
				 struct drm_display_mode *adjusted_mode)
{
}

static void
udl_encoder_dpms(struct drm_encoder *encoder, int mode)
{
}

static const struct drm_encoder_helper_funcs udl_helper_funcs = {
	.dpms = udl_encoder_dpms,
	.prepare = udl_encoder_prepare,
	.mode_set = udl_encoder_mode_set,
	.commit = udl_encoder_commit,
	.disable = udl_encoder_disable,
};

static const struct drm_encoder_funcs udl_enc_funcs = {
	.destroy = udl_enc_destroy,
};

struct drm_encoder *udl_encoder_init(struct drm_device *dev)
{
	struct drm_encoder *encoder;

	encoder = kzalloc(sizeof(struct drm_encoder), GFP_KERNEL);
	if (!encoder)
		return NULL;

	drm_encoder_init(dev, encoder, &udl_enc_funcs, DRM_MODE_ENCODER_TMDS,
			 NULL);
	drm_encoder_helper_add(encoder, &udl_helper_funcs);
	encoder->possible_crtcs = 1;
	return encoder;
}
