/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>

#include "virtgpu_drv.h"

void virtio_gpu_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct virtio_gpu_object *obj = gem_to_virtio_gpu_obj(gem_obj);

	if (obj)
		virtio_gpu_object_unref(&obj);
}

struct virtio_gpu_object*
virtio_gpu_alloc_object(struct drm_device *dev,
			struct virtio_gpu_object_params *params,
			struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_object *obj;
	int ret;

	ret = virtio_gpu_object_create(vgdev, params, &obj, fence);
	if (ret)
		return ERR_PTR(ret);

	return obj;
}

int virtio_gpu_gem_create(struct drm_file *file,
			  struct drm_device *dev,
			  struct virtio_gpu_object_params *params,
			  struct drm_gem_object **obj_p,
			  uint32_t *handle_p)
{
	struct virtio_gpu_object *obj;
	int ret;
	u32 handle;

	obj = virtio_gpu_alloc_object(dev, params, NULL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = drm_gem_handle_create(file, &obj->gem_base, &handle);
	if (ret) {
		drm_gem_object_release(&obj->gem_base);
		return ret;
	}

	*obj_p = &obj->gem_base;

	/* drop reference from allocate - handle holds it now */
	drm_gem_object_put_unlocked(&obj->gem_base);

	*handle_p = handle;
	return 0;
}

int virtio_gpu_mode_dumb_create(struct drm_file *file_priv,
				struct drm_device *dev,
				struct drm_mode_create_dumb *args)
{
	struct drm_gem_object *gobj;
	struct virtio_gpu_object_params params = { 0 };
	int ret;
	uint32_t pitch;

	if (args->bpp != 32)
		return -EINVAL;

	pitch = args->width * 4;
	args->size = pitch * args->height;
	args->size = ALIGN(args->size, PAGE_SIZE);

	params.format = virtio_gpu_translate_format(DRM_FORMAT_HOST_XRGB8888);
	params.width = args->width;
	params.height = args->height;
	params.size = args->size;
	params.dumb = true;
	ret = virtio_gpu_gem_create(file_priv, dev, &params, &gobj,
				    &args->handle);
	if (ret)
		goto fail;

	args->pitch = pitch;
	return ret;

fail:
	return ret;
}

int virtio_gpu_mode_dumb_mmap(struct drm_file *file_priv,
			      struct drm_device *dev,
			      uint32_t handle, uint64_t *offset_p)
{
	struct drm_gem_object *gobj;
	struct virtio_gpu_object *obj;

	BUG_ON(!offset_p);
	gobj = drm_gem_object_lookup(file_priv, handle);
	if (gobj == NULL)
		return -ENOENT;
	obj = gem_to_virtio_gpu_obj(gobj);
	*offset_p = virtio_gpu_object_mmap_offset(obj);
	drm_gem_object_put_unlocked(gobj);
	return 0;
}

int virtio_gpu_gem_object_open(struct drm_gem_object *obj,
			       struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct virtio_gpu_object *qobj = gem_to_virtio_gpu_obj(obj);
	int r;

	if (!vgdev->has_virgl_3d)
		return 0;

	r = virtio_gpu_object_reserve(qobj, false);
	if (r)
		return r;

	virtio_gpu_cmd_context_attach_resource(vgdev, vfpriv->ctx_id,
					       qobj->hw_res_handle);
	virtio_gpu_object_unreserve(qobj);
	return 0;
}

void virtio_gpu_gem_object_close(struct drm_gem_object *obj,
				 struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct virtio_gpu_object *qobj = gem_to_virtio_gpu_obj(obj);
	int r;

	if (!vgdev->has_virgl_3d)
		return;

	r = virtio_gpu_object_reserve(qobj, false);
	if (r)
		return;

	virtio_gpu_cmd_context_detach_resource(vgdev, vfpriv->ctx_id,
						qobj->hw_res_handle);
	virtio_gpu_object_unreserve(qobj);
}
