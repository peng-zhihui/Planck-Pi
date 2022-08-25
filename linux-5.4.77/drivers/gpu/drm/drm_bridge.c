/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include <drm/drm_bridge.h>
#include <drm/drm_encoder.h>

#include "drm_crtc_internal.h"

/**
 * DOC: overview
 *
 * &struct drm_bridge represents a device that hangs on to an encoder. These are
 * handy when a regular &drm_encoder entity isn't enough to represent the entire
 * encoder chain.
 *
 * A bridge is always attached to a single &drm_encoder at a time, but can be
 * either connected to it directly, or through an intermediate bridge::
 *
 *     encoder ---> bridge B ---> bridge A
 *
 * Here, the output of the encoder feeds to bridge B, and that furthers feeds to
 * bridge A.
 *
 * The driver using the bridge is responsible to make the associations between
 * the encoder and bridges. Once these links are made, the bridges will
 * participate along with encoder functions to perform mode_set/enable/disable
 * through the ops provided in &drm_bridge_funcs.
 *
 * drm_bridge, like drm_panel, aren't drm_mode_object entities like planes,
 * CRTCs, encoders or connectors and hence are not visible to userspace. They
 * just provide additional hooks to get the desired output at the end of the
 * encoder chain.
 *
 * Bridges can also be chained up using the &drm_bridge.next pointer.
 *
 * Both legacy CRTC helpers and the new atomic modeset helpers support bridges.
 */

static DEFINE_MUTEX(bridge_lock);
static LIST_HEAD(bridge_list);

/**
 * drm_bridge_add - add the given bridge to the global bridge list
 *
 * @bridge: bridge control structure
 */
void drm_bridge_add(struct drm_bridge *bridge)
{
	mutex_lock(&bridge_lock);
	list_add_tail(&bridge->list, &bridge_list);
	mutex_unlock(&bridge_lock);
}
EXPORT_SYMBOL(drm_bridge_add);

/**
 * drm_bridge_remove - remove the given bridge from the global bridge list
 *
 * @bridge: bridge control structure
 */
void drm_bridge_remove(struct drm_bridge *bridge)
{
	mutex_lock(&bridge_lock);
	list_del_init(&bridge->list);
	mutex_unlock(&bridge_lock);
}
EXPORT_SYMBOL(drm_bridge_remove);

/**
 * drm_bridge_attach - attach the bridge to an encoder's chain
 *
 * @encoder: DRM encoder
 * @bridge: bridge to attach
 * @previous: previous bridge in the chain (optional)
 *
 * Called by a kms driver to link the bridge to an encoder's chain. The previous
 * argument specifies the previous bridge in the chain. If NULL, the bridge is
 * linked directly at the encoder's output. Otherwise it is linked at the
 * previous bridge's output.
 *
 * If non-NULL the previous bridge must be already attached by a call to this
 * function.
 *
 * Note that bridges attached to encoders are auto-detached during encoder
 * cleanup in drm_encoder_cleanup(), so drm_bridge_attach() should generally
 * *not* be balanced with a drm_bridge_detach() in driver code.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int drm_bridge_attach(struct drm_encoder *encoder, struct drm_bridge *bridge,
		      struct drm_bridge *previous)
{
	int ret;

	if (!encoder || !bridge)
		return -EINVAL;

	if (previous && (!previous->dev || previous->encoder != encoder))
		return -EINVAL;

	if (bridge->dev)
		return -EBUSY;

	bridge->dev = encoder->dev;
	bridge->encoder = encoder;

	if (bridge->funcs->attach) {
		ret = bridge->funcs->attach(bridge);
		if (ret < 0) {
			bridge->dev = NULL;
			bridge->encoder = NULL;
			return ret;
		}
	}

	if (previous)
		previous->next = bridge;
	else
		encoder->bridge = bridge;

	return 0;
}
EXPORT_SYMBOL(drm_bridge_attach);

void drm_bridge_detach(struct drm_bridge *bridge)
{
	if (WARN_ON(!bridge))
		return;

	if (WARN_ON(!bridge->dev))
		return;

	if (bridge->funcs->detach)
		bridge->funcs->detach(bridge);

	bridge->dev = NULL;
}

/**
 * DOC: bridge callbacks
 *
 * The &drm_bridge_funcs ops are populated by the bridge driver. The DRM
 * internals (atomic and CRTC helpers) use the helpers defined in drm_bridge.c
 * These helpers call a specific &drm_bridge_funcs op for all the bridges
 * during encoder configuration.
 *
 * For detailed specification of the bridge callbacks see &drm_bridge_funcs.
 */

/**
 * drm_bridge_mode_fixup - fixup proposed mode for all bridges in the
 *			   encoder chain
 * @bridge: bridge control structure
 * @mode: desired mode to be set for the bridge
 * @adjusted_mode: updated mode that works for this bridge
 *
 * Calls &drm_bridge_funcs.mode_fixup for all the bridges in the
 * encoder chain, starting from the first bridge to the last.
 *
 * Note: the bridge passed should be the one closest to the encoder
 *
 * RETURNS:
 * true on success, false on failure
 */
bool drm_bridge_mode_fixup(struct drm_bridge *bridge,
			const struct drm_display_mode *mode,
			struct drm_display_mode *adjusted_mode)
{
	bool ret = true;

	if (!bridge)
		return true;

	if (bridge->funcs->mode_fixup)
		ret = bridge->funcs->mode_fixup(bridge, mode, adjusted_mode);

	ret = ret && drm_bridge_mode_fixup(bridge->next, mode, adjusted_mode);

	return ret;
}
EXPORT_SYMBOL(drm_bridge_mode_fixup);

/**
 * drm_bridge_mode_valid - validate the mode against all bridges in the
 * 			   encoder chain.
 * @bridge: bridge control structure
 * @mode: desired mode to be validated
 *
 * Calls &drm_bridge_funcs.mode_valid for all the bridges in the encoder
 * chain, starting from the first bridge to the last. If at least one bridge
 * does not accept the mode the function returns the error code.
 *
 * Note: the bridge passed should be the one closest to the encoder.
 *
 * RETURNS:
 * MODE_OK on success, drm_mode_status Enum error code on failure
 */
enum drm_mode_status drm_bridge_mode_valid(struct drm_bridge *bridge,
					   const struct drm_display_mode *mode)
{
	enum drm_mode_status ret = MODE_OK;

	if (!bridge)
		return ret;

	if (bridge->funcs->mode_valid)
		ret = bridge->funcs->mode_valid(bridge, mode);

	if (ret != MODE_OK)
		return ret;

	return drm_bridge_mode_valid(bridge->next, mode);
}
EXPORT_SYMBOL(drm_bridge_mode_valid);

/**
 * drm_bridge_disable - disables all bridges in the encoder chain
 * @bridge: bridge control structure
 *
 * Calls &drm_bridge_funcs.disable op for all the bridges in the encoder
 * chain, starting from the last bridge to the first. These are called before
 * calling the encoder's prepare op.
 *
 * Note: the bridge passed should be the one closest to the encoder
 */
void drm_bridge_disable(struct drm_bridge *bridge)
{
	if (!bridge)
		return;

	drm_bridge_disable(bridge->next);

	if (bridge->funcs->disable)
		bridge->funcs->disable(bridge);
}
EXPORT_SYMBOL(drm_bridge_disable);

/**
 * drm_bridge_post_disable - cleans up after disabling all bridges in the encoder chain
 * @bridge: bridge control structure
 *
 * Calls &drm_bridge_funcs.post_disable op for all the bridges in the
 * encoder chain, starting from the first bridge to the last. These are called
 * after completing the encoder's prepare op.
 *
 * Note: the bridge passed should be the one closest to the encoder
 */
void drm_bridge_post_disable(struct drm_bridge *bridge)
{
	if (!bridge)
		return;

	if (bridge->funcs->post_disable)
		bridge->funcs->post_disable(bridge);

	drm_bridge_post_disable(bridge->next);
}
EXPORT_SYMBOL(drm_bridge_post_disable);

/**
 * drm_bridge_mode_set - set proposed mode for all bridges in the
 *			 encoder chain
 * @bridge: bridge control structure
 * @mode: desired mode to be set for the bridge
 * @adjusted_mode: updated mode that works for this bridge
 *
 * Calls &drm_bridge_funcs.mode_set op for all the bridges in the
 * encoder chain, starting from the first bridge to the last.
 *
 * Note: the bridge passed should be the one closest to the encoder
 */
void drm_bridge_mode_set(struct drm_bridge *bridge,
			 const struct drm_display_mode *mode,
			 const struct drm_display_mode *adjusted_mode)
{
	if (!bridge)
		return;

	if (bridge->funcs->mode_set)
		bridge->funcs->mode_set(bridge, mode, adjusted_mode);

	drm_bridge_mode_set(bridge->next, mode, adjusted_mode);
}
EXPORT_SYMBOL(drm_bridge_mode_set);

/**
 * drm_bridge_pre_enable - prepares for enabling all
 *			   bridges in the encoder chain
 * @bridge: bridge control structure
 *
 * Calls &drm_bridge_funcs.pre_enable op for all the bridges in the encoder
 * chain, starting from the last bridge to the first. These are called
 * before calling the encoder's commit op.
 *
 * Note: the bridge passed should be the one closest to the encoder
 */
void drm_bridge_pre_enable(struct drm_bridge *bridge)
{
	if (!bridge)
		return;

	drm_bridge_pre_enable(bridge->next);

	if (bridge->funcs->pre_enable)
		bridge->funcs->pre_enable(bridge);
}
EXPORT_SYMBOL(drm_bridge_pre_enable);

/**
 * drm_bridge_enable - enables all bridges in the encoder chain
 * @bridge: bridge control structure
 *
 * Calls &drm_bridge_funcs.enable op for all the bridges in the encoder
 * chain, starting from the first bridge to the last. These are called
 * after completing the encoder's commit op.
 *
 * Note that the bridge passed should be the one closest to the encoder
 */
void drm_bridge_enable(struct drm_bridge *bridge)
{
	if (!bridge)
		return;

	if (bridge->funcs->enable)
		bridge->funcs->enable(bridge);

	drm_bridge_enable(bridge->next);
}
EXPORT_SYMBOL(drm_bridge_enable);

/**
 * drm_atomic_bridge_disable - disables all bridges in the encoder chain
 * @bridge: bridge control structure
 * @state: atomic state being committed
 *
 * Calls &drm_bridge_funcs.atomic_disable (falls back on
 * &drm_bridge_funcs.disable) op for all the bridges in the encoder chain,
 * starting from the last bridge to the first. These are called before calling
 * &drm_encoder_helper_funcs.atomic_disable
 *
 * Note: the bridge passed should be the one closest to the encoder
 */
void drm_atomic_bridge_disable(struct drm_bridge *bridge,
			       struct drm_atomic_state *state)
{
	if (!bridge)
		return;

	drm_atomic_bridge_disable(bridge->next, state);

	if (bridge->funcs->atomic_disable)
		bridge->funcs->atomic_disable(bridge, state);
	else if (bridge->funcs->disable)
		bridge->funcs->disable(bridge);
}
EXPORT_SYMBOL(drm_atomic_bridge_disable);

/**
 * drm_atomic_bridge_post_disable - cleans up after disabling all bridges in the
 *				    encoder chain
 * @bridge: bridge control structure
 * @state: atomic state being committed
 *
 * Calls &drm_bridge_funcs.atomic_post_disable (falls back on
 * &drm_bridge_funcs.post_disable) op for all the bridges in the encoder chain,
 * starting from the first bridge to the last. These are called after completing
 * &drm_encoder_helper_funcs.atomic_disable
 *
 * Note: the bridge passed should be the one closest to the encoder
 */
void drm_atomic_bridge_post_disable(struct drm_bridge *bridge,
				    struct drm_atomic_state *state)
{
	if (!bridge)
		return;

	if (bridge->funcs->atomic_post_disable)
		bridge->funcs->atomic_post_disable(bridge, state);
	else if (bridge->funcs->post_disable)
		bridge->funcs->post_disable(bridge);

	drm_atomic_bridge_post_disable(bridge->next, state);
}
EXPORT_SYMBOL(drm_atomic_bridge_post_disable);

/**
 * drm_atomic_bridge_pre_enable - prepares for enabling all bridges in the
 *				  encoder chain
 * @bridge: bridge control structure
 * @state: atomic state being committed
 *
 * Calls &drm_bridge_funcs.atomic_pre_enable (falls back on
 * &drm_bridge_funcs.pre_enable) op for all the bridges in the encoder chain,
 * starting from the last bridge to the first. These are called before calling
 * &drm_encoder_helper_funcs.atomic_enable
 *
 * Note: the bridge passed should be the one closest to the encoder
 */
void drm_atomic_bridge_pre_enable(struct drm_bridge *bridge,
				  struct drm_atomic_state *state)
{
	if (!bridge)
		return;

	drm_atomic_bridge_pre_enable(bridge->next, state);

	if (bridge->funcs->atomic_pre_enable)
		bridge->funcs->atomic_pre_enable(bridge, state);
	else if (bridge->funcs->pre_enable)
		bridge->funcs->pre_enable(bridge);
}
EXPORT_SYMBOL(drm_atomic_bridge_pre_enable);

/**
 * drm_atomic_bridge_enable - enables all bridges in the encoder chain
 * @bridge: bridge control structure
 * @state: atomic state being committed
 *
 * Calls &drm_bridge_funcs.atomic_enable (falls back on
 * &drm_bridge_funcs.enable) op for all the bridges in the encoder chain,
 * starting from the first bridge to the last. These are called after completing
 * &drm_encoder_helper_funcs.atomic_enable
 *
 * Note: the bridge passed should be the one closest to the encoder
 */
void drm_atomic_bridge_enable(struct drm_bridge *bridge,
			      struct drm_atomic_state *state)
{
	if (!bridge)
		return;

	if (bridge->funcs->atomic_enable)
		bridge->funcs->atomic_enable(bridge, state);
	else if (bridge->funcs->enable)
		bridge->funcs->enable(bridge);

	drm_atomic_bridge_enable(bridge->next, state);
}
EXPORT_SYMBOL(drm_atomic_bridge_enable);

#ifdef CONFIG_OF
/**
 * of_drm_find_bridge - find the bridge corresponding to the device node in
 *			the global bridge list
 *
 * @np: device node
 *
 * RETURNS:
 * drm_bridge control struct on success, NULL on failure
 */
struct drm_bridge *of_drm_find_bridge(struct device_node *np)
{
	struct drm_bridge *bridge;

	mutex_lock(&bridge_lock);

	list_for_each_entry(bridge, &bridge_list, list) {
		if (bridge->of_node == np) {
			mutex_unlock(&bridge_lock);
			return bridge;
		}
	}

	mutex_unlock(&bridge_lock);
	return NULL;
}
EXPORT_SYMBOL(of_drm_find_bridge);
#endif

MODULE_AUTHOR("Ajay Kumar <ajaykumar.rs@samsung.com>");
MODULE_DESCRIPTION("DRM bridge infrastructure");
MODULE_LICENSE("GPL and additional rights");
