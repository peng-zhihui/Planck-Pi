// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/file.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>

#include <drm/drm_file.h>

#include "msm_drv.h"
#include "msm_gpu.h"
#include "msm_gem.h"
#include "msm_gpu_trace.h"

/*
 * Cmdstream submission:
 */

/* make sure these don't conflict w/ MSM_SUBMIT_BO_x */
#define BO_VALID    0x8000   /* is current addr in cmdstream correct/valid? */
#define BO_LOCKED   0x4000
#define BO_PINNED   0x2000

static struct msm_gem_submit *submit_create(struct drm_device *dev,
		struct msm_gpu *gpu, struct msm_gem_address_space *aspace,
		struct msm_gpu_submitqueue *queue, uint32_t nr_bos,
		uint32_t nr_cmds)
{
	struct msm_gem_submit *submit;
	uint64_t sz = struct_size(submit, bos, nr_bos) +
				  ((u64)nr_cmds * sizeof(submit->cmd[0]));

	if (sz > SIZE_MAX)
		return NULL;

	submit = kmalloc(sz, GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);
	if (!submit)
		return NULL;

	submit->dev = dev;
	submit->aspace = aspace;
	submit->gpu = gpu;
	submit->fence = NULL;
	submit->cmd = (void *)&submit->bos[nr_bos];
	submit->queue = queue;
	submit->ring = gpu->rb[queue->prio];

	/* initially, until copy_from_user() and bo lookup succeeds: */
	submit->nr_bos = 0;
	submit->nr_cmds = 0;

	INIT_LIST_HEAD(&submit->node);
	INIT_LIST_HEAD(&submit->bo_list);
	ww_acquire_init(&submit->ticket, &reservation_ww_class);

	return submit;
}

void msm_gem_submit_free(struct msm_gem_submit *submit)
{
	dma_fence_put(submit->fence);
	list_del(&submit->node);
	put_pid(submit->pid);
	msm_submitqueue_put(submit->queue);

	kfree(submit);
}

static int submit_lookup_objects(struct msm_gem_submit *submit,
		struct drm_msm_gem_submit *args, struct drm_file *file)
{
	unsigned i;
	int ret = 0;

	for (i = 0; i < args->nr_bos; i++) {
		struct drm_msm_gem_submit_bo submit_bo;
		void __user *userptr =
			u64_to_user_ptr(args->bos + (i * sizeof(submit_bo)));

		/* make sure we don't have garbage flags, in case we hit
		 * error path before flags is initialized:
		 */
		submit->bos[i].flags = 0;

		if (copy_from_user(&submit_bo, userptr, sizeof(submit_bo))) {
			ret = -EFAULT;
			i = 0;
			goto out;
		}

/* at least one of READ and/or WRITE flags should be set: */
#define MANDATORY_FLAGS (MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE)

		if ((submit_bo.flags & ~MSM_SUBMIT_BO_FLAGS) ||
			!(submit_bo.flags & MANDATORY_FLAGS)) {
			DRM_ERROR("invalid flags: %x\n", submit_bo.flags);
			ret = -EINVAL;
			i = 0;
			goto out;
		}

		submit->bos[i].handle = submit_bo.handle;
		submit->bos[i].flags = submit_bo.flags;
		/* in validate_objects() we figure out if this is true: */
		submit->bos[i].iova  = submit_bo.presumed;
	}

	spin_lock(&file->table_lock);

	for (i = 0; i < args->nr_bos; i++) {
		struct drm_gem_object *obj;
		struct msm_gem_object *msm_obj;

		/* normally use drm_gem_object_lookup(), but for bulk lookup
		 * all under single table_lock just hit object_idr directly:
		 */
		obj = idr_find(&file->object_idr, submit->bos[i].handle);
		if (!obj) {
			DRM_ERROR("invalid handle %u at index %u\n", submit->bos[i].handle, i);
			ret = -EINVAL;
			goto out_unlock;
		}

		msm_obj = to_msm_bo(obj);

		if (!list_empty(&msm_obj->submit_entry)) {
			DRM_ERROR("handle %u at index %u already on submit list\n",
					submit->bos[i].handle, i);
			ret = -EINVAL;
			goto out_unlock;
		}

		drm_gem_object_get(obj);

		submit->bos[i].obj = msm_obj;

		list_add_tail(&msm_obj->submit_entry, &submit->bo_list);
	}

out_unlock:
	spin_unlock(&file->table_lock);

out:
	submit->nr_bos = i;

	return ret;
}

static void submit_unlock_unpin_bo(struct msm_gem_submit *submit,
		int i, bool backoff)
{
	struct msm_gem_object *msm_obj = submit->bos[i].obj;

	if (submit->bos[i].flags & BO_PINNED)
		msm_gem_unpin_iova(&msm_obj->base, submit->aspace);

	if (submit->bos[i].flags & BO_LOCKED)
		ww_mutex_unlock(&msm_obj->base.resv->lock);

	if (backoff && !(submit->bos[i].flags & BO_VALID))
		submit->bos[i].iova = 0;

	submit->bos[i].flags &= ~(BO_LOCKED | BO_PINNED);
}

/* This is where we make sure all the bo's are reserved and pin'd: */
static int submit_lock_objects(struct msm_gem_submit *submit)
{
	int contended, slow_locked = -1, i, ret = 0;

retry:
	for (i = 0; i < submit->nr_bos; i++) {
		struct msm_gem_object *msm_obj = submit->bos[i].obj;

		if (slow_locked == i)
			slow_locked = -1;

		contended = i;

		if (!(submit->bos[i].flags & BO_LOCKED)) {
			ret = ww_mutex_lock_interruptible(&msm_obj->base.resv->lock,
					&submit->ticket);
			if (ret)
				goto fail;
			submit->bos[i].flags |= BO_LOCKED;
		}
	}

	ww_acquire_done(&submit->ticket);

	return 0;

fail:
	for (; i >= 0; i--)
		submit_unlock_unpin_bo(submit, i, true);

	if (slow_locked > 0)
		submit_unlock_unpin_bo(submit, slow_locked, true);

	if (ret == -EDEADLK) {
		struct msm_gem_object *msm_obj = submit->bos[contended].obj;
		/* we lost out in a seqno race, lock and retry.. */
		ret = ww_mutex_lock_slow_interruptible(&msm_obj->base.resv->lock,
				&submit->ticket);
		if (!ret) {
			submit->bos[contended].flags |= BO_LOCKED;
			slow_locked = contended;
			goto retry;
		}
	}

	return ret;
}

static int submit_fence_sync(struct msm_gem_submit *submit, bool no_implicit)
{
	int i, ret = 0;

	for (i = 0; i < submit->nr_bos; i++) {
		struct msm_gem_object *msm_obj = submit->bos[i].obj;
		bool write = submit->bos[i].flags & MSM_SUBMIT_BO_WRITE;

		if (!write) {
			/* NOTE: _reserve_shared() must happen before
			 * _add_shared_fence(), which makes this a slightly
			 * strange place to call it.  OTOH this is a
			 * convenient can-fail point to hook it in.
			 */
			ret = dma_resv_reserve_shared(msm_obj->base.resv,
								1);
			if (ret)
				return ret;
		}

		if (no_implicit)
			continue;

		ret = msm_gem_sync_object(&msm_obj->base, submit->ring->fctx,
			write);
		if (ret)
			break;
	}

	return ret;
}

static int submit_pin_objects(struct msm_gem_submit *submit)
{
	int i, ret = 0;

	submit->valid = true;

	for (i = 0; i < submit->nr_bos; i++) {
		struct msm_gem_object *msm_obj = submit->bos[i].obj;
		uint64_t iova;

		/* if locking succeeded, pin bo: */
		ret = msm_gem_get_and_pin_iova(&msm_obj->base,
				submit->aspace, &iova);

		if (ret)
			break;

		submit->bos[i].flags |= BO_PINNED;

		if (iova == submit->bos[i].iova) {
			submit->bos[i].flags |= BO_VALID;
		} else {
			submit->bos[i].iova = iova;
			/* iova changed, so address in cmdstream is not valid: */
			submit->bos[i].flags &= ~BO_VALID;
			submit->valid = false;
		}
	}

	return ret;
}

static int submit_bo(struct msm_gem_submit *submit, uint32_t idx,
		struct msm_gem_object **obj, uint64_t *iova, bool *valid)
{
	if (idx >= submit->nr_bos) {
		DRM_ERROR("invalid buffer index: %u (out of %u)\n",
				idx, submit->nr_bos);
		return -EINVAL;
	}

	if (obj)
		*obj = submit->bos[idx].obj;
	if (iova)
		*iova = submit->bos[idx].iova;
	if (valid)
		*valid = !!(submit->bos[idx].flags & BO_VALID);

	return 0;
}

/* process the reloc's and patch up the cmdstream as needed: */
static int submit_reloc(struct msm_gem_submit *submit, struct msm_gem_object *obj,
		uint32_t offset, uint32_t nr_relocs, uint64_t relocs)
{
	uint32_t i, last_offset = 0;
	uint32_t *ptr;
	int ret = 0;

	if (!nr_relocs)
		return 0;

	if (offset % 4) {
		DRM_ERROR("non-aligned cmdstream buffer: %u\n", offset);
		return -EINVAL;
	}

	/* For now, just map the entire thing.  Eventually we probably
	 * to do it page-by-page, w/ kmap() if not vmap()d..
	 */
	ptr = msm_gem_get_vaddr(&obj->base);

	if (IS_ERR(ptr)) {
		ret = PTR_ERR(ptr);
		DBG("failed to map: %d", ret);
		return ret;
	}

	for (i = 0; i < nr_relocs; i++) {
		struct drm_msm_gem_submit_reloc submit_reloc;
		void __user *userptr =
			u64_to_user_ptr(relocs + (i * sizeof(submit_reloc)));
		uint32_t off;
		uint64_t iova;
		bool valid;

		if (copy_from_user(&submit_reloc, userptr, sizeof(submit_reloc))) {
			ret = -EFAULT;
			goto out;
		}

		if (submit_reloc.submit_offset % 4) {
			DRM_ERROR("non-aligned reloc offset: %u\n",
					submit_reloc.submit_offset);
			ret = -EINVAL;
			goto out;
		}

		/* offset in dwords: */
		off = submit_reloc.submit_offset / 4;

		if ((off >= (obj->base.size / 4)) ||
				(off < last_offset)) {
			DRM_ERROR("invalid offset %u at reloc %u\n", off, i);
			ret = -EINVAL;
			goto out;
		}

		ret = submit_bo(submit, submit_reloc.reloc_idx, NULL, &iova, &valid);
		if (ret)
			goto out;

		if (valid)
			continue;

		iova += submit_reloc.reloc_offset;

		if (submit_reloc.shift < 0)
			iova >>= -submit_reloc.shift;
		else
			iova <<= submit_reloc.shift;

		ptr[off] = iova | submit_reloc.or;

		last_offset = off;
	}

out:
	msm_gem_put_vaddr(&obj->base);

	return ret;
}

static void submit_cleanup(struct msm_gem_submit *submit)
{
	unsigned i;

	for (i = 0; i < submit->nr_bos; i++) {
		struct msm_gem_object *msm_obj = submit->bos[i].obj;
		submit_unlock_unpin_bo(submit, i, false);
		list_del_init(&msm_obj->submit_entry);
		drm_gem_object_put(&msm_obj->base);
	}

	ww_acquire_fini(&submit->ticket);
}

int msm_ioctl_gem_submit(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	static atomic_t ident = ATOMIC_INIT(0);
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_gem_submit *args = data;
	struct msm_file_private *ctx = file->driver_priv;
	struct msm_gem_submit *submit;
	struct msm_gpu *gpu = priv->gpu;
	struct sync_file *sync_file = NULL;
	struct msm_gpu_submitqueue *queue;
	struct msm_ringbuffer *ring;
	int out_fence_fd = -1;
	struct pid *pid = get_pid(task_pid(current));
	unsigned i;
	int ret, submitid;
	if (!gpu)
		return -ENXIO;

	/* for now, we just have 3d pipe.. eventually this would need to
	 * be more clever to dispatch to appropriate gpu module:
	 */
	if (MSM_PIPE_ID(args->flags) != MSM_PIPE_3D0)
		return -EINVAL;

	if (MSM_PIPE_FLAGS(args->flags) & ~MSM_SUBMIT_FLAGS)
		return -EINVAL;

	if (args->flags & MSM_SUBMIT_SUDO) {
		if (!IS_ENABLED(CONFIG_DRM_MSM_GPU_SUDO) ||
		    !capable(CAP_SYS_RAWIO))
			return -EINVAL;
	}

	queue = msm_submitqueue_get(ctx, args->queueid);
	if (!queue)
		return -ENOENT;

	/* Get a unique identifier for the submission for logging purposes */
	submitid = atomic_inc_return(&ident) - 1;

	ring = gpu->rb[queue->prio];
	trace_msm_gpu_submit(pid_nr(pid), ring->id, submitid,
		args->nr_bos, args->nr_cmds);

	if (args->flags & MSM_SUBMIT_FENCE_FD_IN) {
		struct dma_fence *in_fence;

		in_fence = sync_file_get_fence(args->fence_fd);

		if (!in_fence)
			return -EINVAL;

		/*
		 * Wait if the fence is from a foreign context, or if the fence
		 * array contains any fence from a foreign context.
		 */
		ret = 0;
		if (!dma_fence_match_context(in_fence, ring->fctx->context))
			ret = dma_fence_wait(in_fence, true);

		dma_fence_put(in_fence);
		if (ret)
			return ret;
	}

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		return ret;

	if (args->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		out_fence_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fence_fd < 0) {
			ret = out_fence_fd;
			goto out_unlock;
		}
	}

	submit = submit_create(dev, gpu, ctx->aspace, queue, args->nr_bos,
		args->nr_cmds);
	if (!submit) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	submit->pid = pid;
	submit->ident = submitid;

	if (args->flags & MSM_SUBMIT_SUDO)
		submit->in_rb = true;

	ret = submit_lookup_objects(submit, args, file);
	if (ret)
		goto out;

	ret = submit_lock_objects(submit);
	if (ret)
		goto out;

	ret = submit_fence_sync(submit, !!(args->flags & MSM_SUBMIT_NO_IMPLICIT));
	if (ret)
		goto out;

	ret = submit_pin_objects(submit);
	if (ret)
		goto out;

	for (i = 0; i < args->nr_cmds; i++) {
		struct drm_msm_gem_submit_cmd submit_cmd;
		void __user *userptr =
			u64_to_user_ptr(args->cmds + (i * sizeof(submit_cmd)));
		struct msm_gem_object *msm_obj;
		uint64_t iova;

		ret = copy_from_user(&submit_cmd, userptr, sizeof(submit_cmd));
		if (ret) {
			ret = -EFAULT;
			goto out;
		}

		/* validate input from userspace: */
		switch (submit_cmd.type) {
		case MSM_SUBMIT_CMD_BUF:
		case MSM_SUBMIT_CMD_IB_TARGET_BUF:
		case MSM_SUBMIT_CMD_CTX_RESTORE_BUF:
			break;
		default:
			DRM_ERROR("invalid type: %08x\n", submit_cmd.type);
			ret = -EINVAL;
			goto out;
		}

		ret = submit_bo(submit, submit_cmd.submit_idx,
				&msm_obj, &iova, NULL);
		if (ret)
			goto out;

		if (submit_cmd.size % 4) {
			DRM_ERROR("non-aligned cmdstream buffer size: %u\n",
					submit_cmd.size);
			ret = -EINVAL;
			goto out;
		}

		if (!submit_cmd.size ||
			((submit_cmd.size + submit_cmd.submit_offset) >
				msm_obj->base.size)) {
			DRM_ERROR("invalid cmdstream size: %u\n", submit_cmd.size);
			ret = -EINVAL;
			goto out;
		}

		submit->cmd[i].type = submit_cmd.type;
		submit->cmd[i].size = submit_cmd.size / 4;
		submit->cmd[i].iova = iova + submit_cmd.submit_offset;
		submit->cmd[i].idx  = submit_cmd.submit_idx;

		if (submit->valid)
			continue;

		ret = submit_reloc(submit, msm_obj, submit_cmd.submit_offset,
				submit_cmd.nr_relocs, submit_cmd.relocs);
		if (ret)
			goto out;
	}

	submit->nr_cmds = i;

	submit->fence = msm_fence_alloc(ring->fctx);
	if (IS_ERR(submit->fence)) {
		ret = PTR_ERR(submit->fence);
		submit->fence = NULL;
		goto out;
	}

	if (args->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		sync_file = sync_file_create(submit->fence);
		if (!sync_file) {
			ret = -ENOMEM;
			goto out;
		}
	}

	msm_gpu_submit(gpu, submit, ctx);

	args->fence = submit->fence->seqno;

	if (args->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		fd_install(out_fence_fd, sync_file->file);
		args->fence_fd = out_fence_fd;
	}

out:
	submit_cleanup(submit);
	if (ret)
		msm_gem_submit_free(submit);
out_unlock:
	if (ret && (out_fence_fd >= 0))
		put_unused_fd(out_fence_fd);
	mutex_unlock(&dev->struct_mutex);
	return ret;
}
