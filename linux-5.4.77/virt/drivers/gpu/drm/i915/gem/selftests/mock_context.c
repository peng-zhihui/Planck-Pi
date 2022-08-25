/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#include "mock_context.h"
#include "selftests/mock_gtt.h"

struct i915_gem_context *
mock_context(struct drm_i915_private *i915,
	     const char *name)
{
	struct i915_gem_context *ctx;
	struct i915_gem_engines *e;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	kref_init(&ctx->ref);
	INIT_LIST_HEAD(&ctx->link);
	ctx->i915 = i915;

	mutex_init(&ctx->engines_mutex);
	e = default_engines(ctx);
	if (IS_ERR(e))
		goto err_free;
	RCU_INIT_POINTER(ctx->engines, e);

	INIT_RADIX_TREE(&ctx->handles_vma, GFP_KERNEL);
	INIT_LIST_HEAD(&ctx->hw_id_link);
	mutex_init(&ctx->mutex);

	ret = i915_gem_context_pin_hw_id(ctx);
	if (ret < 0)
		goto err_engines;

	if (name) {
		struct i915_ppgtt *ppgtt;

		ctx->name = kstrdup(name, GFP_KERNEL);
		if (!ctx->name)
			goto err_put;

		ppgtt = mock_ppgtt(i915, name);
		if (!ppgtt)
			goto err_put;

		__set_ppgtt(ctx, &ppgtt->vm);
		i915_vm_put(&ppgtt->vm);
	}

	return ctx;

err_engines:
	free_engines(rcu_access_pointer(ctx->engines));
err_free:
	kfree(ctx);
	return NULL;

err_put:
	i915_gem_context_set_closed(ctx);
	i915_gem_context_put(ctx);
	return NULL;
}

void mock_context_close(struct i915_gem_context *ctx)
{
	context_close(ctx);
}

void mock_init_contexts(struct drm_i915_private *i915)
{
	init_contexts(i915);
}

struct i915_gem_context *
live_context(struct drm_i915_private *i915, struct drm_file *file)
{
	struct i915_gem_context *ctx;
	int err;

	lockdep_assert_held(&i915->drm.struct_mutex);

	ctx = i915_gem_create_context(i915, 0);
	if (IS_ERR(ctx))
		return ctx;

	err = gem_context_register(ctx, file->driver_priv);
	if (err < 0)
		goto err_ctx;

	return ctx;

err_ctx:
	context_close(ctx);
	return ERR_PTR(err);
}

struct i915_gem_context *
kernel_context(struct drm_i915_private *i915)
{
	return i915_gem_context_create_kernel(i915, I915_PRIORITY_NORMAL);
}

void kernel_context_close(struct i915_gem_context *ctx)
{
	context_close(ctx);
}
