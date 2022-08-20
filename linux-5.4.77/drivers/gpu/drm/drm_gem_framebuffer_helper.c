// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * drm gem framebuffer helper functions
 *
 * Copyright (C) 2017 Noralf Trønnes
 */

#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_simple_kms_helper.h>

/**
 * DOC: overview
 *
 * This library provides helpers for drivers that don't subclass
 * &drm_framebuffer and use &drm_gem_object for their backing storage.
 *
 * Drivers without additional needs to validate framebuffers can simply use
 * drm_gem_fb_create() and everything is wired up automatically. Other drivers
 * can use all parts independently.
 */

/**
 * drm_gem_fb_get_obj() - Get GEM object backing the framebuffer
 * @fb: Framebuffer
 * @plane: Plane index
 *
 * No additional reference is taken beyond the one that the &drm_frambuffer
 * already holds.
 *
 * Returns:
 * Pointer to &drm_gem_object for the given framebuffer and plane index or NULL
 * if it does not exist.
 */
struct drm_gem_object *drm_gem_fb_get_obj(struct drm_framebuffer *fb,
					  unsigned int plane)
{
	if (plane >= 4)
		return NULL;

	return fb->obj[plane];
}
EXPORT_SYMBOL_GPL(drm_gem_fb_get_obj);

static struct drm_framebuffer *
drm_gem_fb_alloc(struct drm_device *dev,
		 const struct drm_mode_fb_cmd2 *mode_cmd,
		 struct drm_gem_object **obj, unsigned int num_planes,
		 const struct drm_framebuffer_funcs *funcs)
{
	struct drm_framebuffer *fb;
	int ret, i;

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return ERR_PTR(-ENOMEM);

	drm_helper_mode_fill_fb_struct(dev, fb, mode_cmd);

	for (i = 0; i < num_planes; i++)
		fb->obj[i] = obj[i];

	ret = drm_framebuffer_init(dev, fb, funcs);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "Failed to init framebuffer: %d\n",
			      ret);
		kfree(fb);
		return ERR_PTR(ret);
	}

	return fb;
}

/**
 * drm_gem_fb_destroy - Free GEM backed framebuffer
 * @fb: Framebuffer
 *
 * Frees a GEM backed framebuffer with its backing buffer(s) and the structure
 * itself. Drivers can use this as their &drm_framebuffer_funcs->destroy
 * callback.
 */
void drm_gem_fb_destroy(struct drm_framebuffer *fb)
{
	int i;

	for (i = 0; i < 4; i++)
		drm_gem_object_put_unlocked(fb->obj[i]);

	drm_framebuffer_cleanup(fb);
	kfree(fb);
}
EXPORT_SYMBOL(drm_gem_fb_destroy);

/**
 * drm_gem_fb_create_handle - Create handle for GEM backed framebuffer
 * @fb: Framebuffer
 * @file: DRM file to register the handle for
 * @handle: Pointer to return the created handle
 *
 * This function creates a handle for the GEM object backing the framebuffer.
 * Drivers can use this as their &drm_framebuffer_funcs->create_handle
 * callback. The GETFB IOCTL calls into this callback.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_fb_create_handle(struct drm_framebuffer *fb, struct drm_file *file,
			     unsigned int *handle)
{
	return drm_gem_handle_create(file, fb->obj[0], handle);
}
EXPORT_SYMBOL(drm_gem_fb_create_handle);

/**
 * drm_gem_fb_create_with_funcs() - Helper function for the
 *                                  &drm_mode_config_funcs.fb_create
 *                                  callback
 * @dev: DRM device
 * @file: DRM file that holds the GEM handle(s) backing the framebuffer
 * @mode_cmd: Metadata from the userspace framebuffer creation request
 * @funcs: vtable to be used for the new framebuffer object
 *
 * This function can be used to set &drm_framebuffer_funcs for drivers that need
 * custom framebuffer callbacks. Use drm_gem_fb_create() if you don't need to
 * change &drm_framebuffer_funcs. The function does buffer size validation.
 *
 * Returns:
 * Pointer to a &drm_framebuffer on success or an error pointer on failure.
 */
struct drm_framebuffer *
drm_gem_fb_create_with_funcs(struct drm_device *dev, struct drm_file *file,
			     const struct drm_mode_fb_cmd2 *mode_cmd,
			     const struct drm_framebuffer_funcs *funcs)
{
	const struct drm_format_info *info;
	struct drm_gem_object *objs[4];
	struct drm_framebuffer *fb;
	int ret, i;

	info = drm_get_format_info(dev, mode_cmd);
	if (!info)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < info->num_planes; i++) {
		unsigned int width = mode_cmd->width / (i ? info->hsub : 1);
		unsigned int height = mode_cmd->height / (i ? info->vsub : 1);
		unsigned int min_size;

		objs[i] = drm_gem_object_lookup(file, mode_cmd->handles[i]);
		if (!objs[i]) {
			DRM_DEBUG_KMS("Failed to lookup GEM object\n");
			ret = -ENOENT;
			goto err_gem_object_put;
		}

		min_size = (height - 1) * mode_cmd->pitches[i]
			 + drm_format_info_min_pitch(info, i, width)
			 + mode_cmd->offsets[i];

		if (objs[i]->size < min_size) {
			drm_gem_object_put_unlocked(objs[i]);
			ret = -EINVAL;
			goto err_gem_object_put;
		}
	}

	fb = drm_gem_fb_alloc(dev, mode_cmd, objs, i, funcs);
	if (IS_ERR(fb)) {
		ret = PTR_ERR(fb);
		goto err_gem_object_put;
	}

	return fb;

err_gem_object_put:
	for (i--; i >= 0; i--)
		drm_gem_object_put_unlocked(objs[i]);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gem_fb_create_with_funcs);

static const struct drm_framebuffer_funcs drm_gem_fb_funcs = {
	.destroy	= drm_gem_fb_destroy,
	.create_handle	= drm_gem_fb_create_handle,
};

/**
 * drm_gem_fb_create() - Helper function for the
 *                       &drm_mode_config_funcs.fb_create callback
 * @dev: DRM device
 * @file: DRM file that holds the GEM handle(s) backing the framebuffer
 * @mode_cmd: Metadata from the userspace framebuffer creation request
 *
 * This function creates a new framebuffer object described by
 * &drm_mode_fb_cmd2. This description includes handles for the buffer(s)
 * backing the framebuffer.
 *
 * If your hardware has special alignment or pitch requirements these should be
 * checked before calling this function. The function does buffer size
 * validation. Use drm_gem_fb_create_with_dirty() if you need framebuffer
 * flushing.
 *
 * Drivers can use this as their &drm_mode_config_funcs.fb_create callback.
 * The ADDFB2 IOCTL calls into this callback.
 *
 * Returns:
 * Pointer to a &drm_framebuffer on success or an error pointer on failure.
 */
struct drm_framebuffer *
drm_gem_fb_create(struct drm_device *dev, struct drm_file *file,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return drm_gem_fb_create_with_funcs(dev, file, mode_cmd,
					    &drm_gem_fb_funcs);
}
EXPORT_SYMBOL_GPL(drm_gem_fb_create);

static const struct drm_framebuffer_funcs drm_gem_fb_funcs_dirtyfb = {
	.destroy	= drm_gem_fb_destroy,
	.create_handle	= drm_gem_fb_create_handle,
	.dirty		= drm_atomic_helper_dirtyfb,
};

/**
 * drm_gem_fb_create_with_dirty() - Helper function for the
 *                       &drm_mode_config_funcs.fb_create callback
 * @dev: DRM device
 * @file: DRM file that holds the GEM handle(s) backing the framebuffer
 * @mode_cmd: Metadata from the userspace framebuffer creation request
 *
 * This function creates a new framebuffer object described by
 * &drm_mode_fb_cmd2. This description includes handles for the buffer(s)
 * backing the framebuffer. drm_atomic_helper_dirtyfb() is used for the dirty
 * callback giving framebuffer flushing through the atomic machinery. Use
 * drm_gem_fb_create() if you don't need the dirty callback.
 * The function does buffer size validation.
 *
 * Drivers should also call drm_plane_enable_fb_damage_clips() on all planes
 * to enable userspace to use damage clips also with the ATOMIC IOCTL.
 *
 * Drivers can use this as their &drm_mode_config_funcs.fb_create callback.
 * The ADDFB2 IOCTL calls into this callback.
 *
 * Returns:
 * Pointer to a &drm_framebuffer on success or an error pointer on failure.
 */
struct drm_framebuffer *
drm_gem_fb_create_with_dirty(struct drm_device *dev, struct drm_file *file,
			     const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return drm_gem_fb_create_with_funcs(dev, file, mode_cmd,
					    &drm_gem_fb_funcs_dirtyfb);
}
EXPORT_SYMBOL_GPL(drm_gem_fb_create_with_dirty);

/**
 * drm_gem_fb_prepare_fb() - Prepare a GEM backed framebuffer
 * @plane: Plane
 * @state: Plane state the fence will be attached to
 *
 * This function extracts the exclusive fence from &drm_gem_object.resv and
 * attaches it to plane state for the atomic helper to wait on. This is
 * necessary to correctly implement implicit synchronization for any buffers
 * shared as a struct &dma_buf. This function can be used as the
 * &drm_plane_helper_funcs.prepare_fb callback.
 *
 * There is no need for &drm_plane_helper_funcs.cleanup_fb hook for simple
 * gem based framebuffer drivers which have their buffers always pinned in
 * memory.
 *
 * See drm_atomic_set_fence_for_plane() for a discussion of implicit and
 * explicit fencing in atomic modeset updates.
 */
int drm_gem_fb_prepare_fb(struct drm_plane *plane,
			  struct drm_plane_state *state)
{
	struct drm_gem_object *obj;
	struct dma_fence *fence;

	if (!state->fb)
		return 0;

	obj = drm_gem_fb_get_obj(state->fb, 0);
	fence = dma_resv_get_excl_rcu(obj->resv);
	drm_atomic_set_fence_for_plane(state, fence);

	return 0;
}
EXPORT_SYMBOL_GPL(drm_gem_fb_prepare_fb);

/**
 * drm_gem_fb_simple_display_pipe_prepare_fb - prepare_fb helper for
 *     &drm_simple_display_pipe
 * @pipe: Simple display pipe
 * @plane_state: Plane state
 *
 * This function uses drm_gem_fb_prepare_fb() to extract the exclusive fence
 * from &drm_gem_object.resv and attaches it to plane state for the atomic
 * helper to wait on. This is necessary to correctly implement implicit
 * synchronization for any buffers shared as a struct &dma_buf. Drivers can use
 * this as their &drm_simple_display_pipe_funcs.prepare_fb callback.
 *
 * See drm_atomic_set_fence_for_plane() for a discussion of implicit and
 * explicit fencing in atomic modeset updates.
 */
int drm_gem_fb_simple_display_pipe_prepare_fb(struct drm_simple_display_pipe *pipe,
					      struct drm_plane_state *plane_state)
{
	return drm_gem_fb_prepare_fb(&pipe->plane, plane_state);
}
EXPORT_SYMBOL(drm_gem_fb_simple_display_pipe_prepare_fb);
