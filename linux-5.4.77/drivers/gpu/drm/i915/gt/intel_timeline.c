/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016-2018 Intel Corporation
 */

#include "gt/intel_gt_types.h"

#include "i915_drv.h"

#include "i915_active.h"
#include "i915_syncmap.h"
#include "gt/intel_timeline.h"

#define ptr_set_bit(ptr, bit) ((typeof(ptr))((unsigned long)(ptr) | BIT(bit)))
#define ptr_test_bit(ptr, bit) ((unsigned long)(ptr) & BIT(bit))

struct intel_timeline_hwsp {
	struct intel_gt *gt;
	struct intel_gt_timelines *gt_timelines;
	struct list_head free_link;
	struct i915_vma *vma;
	u64 free_bitmap;
};

struct intel_timeline_cacheline {
	struct i915_active active;
	struct intel_timeline_hwsp *hwsp;
	void *vaddr;
#define CACHELINE_BITS 6
#define CACHELINE_FREE CACHELINE_BITS
};

static struct i915_vma *__hwsp_alloc(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;

	obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	i915_gem_object_set_cache_coherency(obj, I915_CACHE_LLC);

	vma = i915_vma_instance(obj, &gt->ggtt->vm, NULL);
	if (IS_ERR(vma))
		i915_gem_object_put(obj);

	return vma;
}

static struct i915_vma *
hwsp_alloc(struct intel_timeline *timeline, unsigned int *cacheline)
{
	struct intel_gt_timelines *gt = &timeline->gt->timelines;
	struct intel_timeline_hwsp *hwsp;

	BUILD_BUG_ON(BITS_PER_TYPE(u64) * CACHELINE_BYTES > PAGE_SIZE);

	spin_lock_irq(&gt->hwsp_lock);

	/* hwsp_free_list only contains HWSP that have available cachelines */
	hwsp = list_first_entry_or_null(&gt->hwsp_free_list,
					typeof(*hwsp), free_link);
	if (!hwsp) {
		struct i915_vma *vma;

		spin_unlock_irq(&gt->hwsp_lock);

		hwsp = kmalloc(sizeof(*hwsp), GFP_KERNEL);
		if (!hwsp)
			return ERR_PTR(-ENOMEM);

		vma = __hwsp_alloc(timeline->gt);
		if (IS_ERR(vma)) {
			kfree(hwsp);
			return vma;
		}

		vma->private = hwsp;
		hwsp->gt = timeline->gt;
		hwsp->vma = vma;
		hwsp->free_bitmap = ~0ull;
		hwsp->gt_timelines = gt;

		spin_lock_irq(&gt->hwsp_lock);
		list_add(&hwsp->free_link, &gt->hwsp_free_list);
	}

	GEM_BUG_ON(!hwsp->free_bitmap);
	*cacheline = __ffs64(hwsp->free_bitmap);
	hwsp->free_bitmap &= ~BIT_ULL(*cacheline);
	if (!hwsp->free_bitmap)
		list_del(&hwsp->free_link);

	spin_unlock_irq(&gt->hwsp_lock);

	GEM_BUG_ON(hwsp->vma->private != hwsp);
	return hwsp->vma;
}

static void __idle_hwsp_free(struct intel_timeline_hwsp *hwsp, int cacheline)
{
	struct intel_gt_timelines *gt = hwsp->gt_timelines;
	unsigned long flags;

	spin_lock_irqsave(&gt->hwsp_lock, flags);

	/* As a cacheline becomes available, publish the HWSP on the freelist */
	if (!hwsp->free_bitmap)
		list_add_tail(&hwsp->free_link, &gt->hwsp_free_list);

	GEM_BUG_ON(cacheline >= BITS_PER_TYPE(hwsp->free_bitmap));
	hwsp->free_bitmap |= BIT_ULL(cacheline);

	/* And if no one is left using it, give the page back to the system */
	if (hwsp->free_bitmap == ~0ull) {
		i915_vma_put(hwsp->vma);
		list_del(&hwsp->free_link);
		kfree(hwsp);
	}

	spin_unlock_irqrestore(&gt->hwsp_lock, flags);
}

static void __idle_cacheline_free(struct intel_timeline_cacheline *cl)
{
	GEM_BUG_ON(!i915_active_is_idle(&cl->active));

	i915_gem_object_unpin_map(cl->hwsp->vma->obj);
	i915_vma_put(cl->hwsp->vma);
	__idle_hwsp_free(cl->hwsp, ptr_unmask_bits(cl->vaddr, CACHELINE_BITS));

	i915_active_fini(&cl->active);
	kfree(cl);
}

static void __cacheline_retire(struct i915_active *active)
{
	struct intel_timeline_cacheline *cl =
		container_of(active, typeof(*cl), active);

	i915_vma_unpin(cl->hwsp->vma);
	if (ptr_test_bit(cl->vaddr, CACHELINE_FREE))
		__idle_cacheline_free(cl);
}

static int __cacheline_active(struct i915_active *active)
{
	struct intel_timeline_cacheline *cl =
		container_of(active, typeof(*cl), active);

	__i915_vma_pin(cl->hwsp->vma);
	return 0;
}

static struct intel_timeline_cacheline *
cacheline_alloc(struct intel_timeline_hwsp *hwsp, unsigned int cacheline)
{
	struct intel_timeline_cacheline *cl;
	void *vaddr;

	GEM_BUG_ON(cacheline >= BIT(CACHELINE_BITS));

	cl = kmalloc(sizeof(*cl), GFP_KERNEL);
	if (!cl)
		return ERR_PTR(-ENOMEM);

	vaddr = i915_gem_object_pin_map(hwsp->vma->obj, I915_MAP_WB);
	if (IS_ERR(vaddr)) {
		kfree(cl);
		return ERR_CAST(vaddr);
	}

	i915_vma_get(hwsp->vma);
	cl->hwsp = hwsp;
	cl->vaddr = page_pack_bits(vaddr, cacheline);

	i915_active_init(hwsp->gt->i915, &cl->active,
			 __cacheline_active, __cacheline_retire);

	return cl;
}

static void cacheline_acquire(struct intel_timeline_cacheline *cl)
{
	if (cl)
		i915_active_acquire(&cl->active);
}

static void cacheline_release(struct intel_timeline_cacheline *cl)
{
	if (cl)
		i915_active_release(&cl->active);
}

static void cacheline_free(struct intel_timeline_cacheline *cl)
{
	GEM_BUG_ON(ptr_test_bit(cl->vaddr, CACHELINE_FREE));
	cl->vaddr = ptr_set_bit(cl->vaddr, CACHELINE_FREE);

	if (i915_active_is_idle(&cl->active))
		__idle_cacheline_free(cl);
}

int intel_timeline_init(struct intel_timeline *timeline,
			struct intel_gt *gt,
			struct i915_vma *hwsp)
{
	void *vaddr;

	kref_init(&timeline->kref);
	atomic_set(&timeline->pin_count, 0);

	timeline->gt = gt;

	timeline->has_initial_breadcrumb = !hwsp;
	timeline->hwsp_cacheline = NULL;

	if (!hwsp) {
		struct intel_timeline_cacheline *cl;
		unsigned int cacheline;

		hwsp = hwsp_alloc(timeline, &cacheline);
		if (IS_ERR(hwsp))
			return PTR_ERR(hwsp);

		cl = cacheline_alloc(hwsp->private, cacheline);
		if (IS_ERR(cl)) {
			__idle_hwsp_free(hwsp->private, cacheline);
			return PTR_ERR(cl);
		}

		timeline->hwsp_cacheline = cl;
		timeline->hwsp_offset = cacheline * CACHELINE_BYTES;

		vaddr = page_mask_bits(cl->vaddr);
	} else {
		timeline->hwsp_offset = I915_GEM_HWS_SEQNO_ADDR;

		vaddr = i915_gem_object_pin_map(hwsp->obj, I915_MAP_WB);
		if (IS_ERR(vaddr))
			return PTR_ERR(vaddr);
	}

	timeline->hwsp_seqno =
		memset(vaddr + timeline->hwsp_offset, 0, CACHELINE_BYTES);

	timeline->hwsp_ggtt = i915_vma_get(hwsp);
	GEM_BUG_ON(timeline->hwsp_offset >= hwsp->size);

	timeline->fence_context = dma_fence_context_alloc(1);

	mutex_init(&timeline->mutex);

	INIT_ACTIVE_REQUEST(&timeline->last_request, &timeline->mutex);
	INIT_LIST_HEAD(&timeline->requests);

	i915_syncmap_init(&timeline->sync);

	return 0;
}

static void timelines_init(struct intel_gt *gt)
{
	struct intel_gt_timelines *timelines = &gt->timelines;

	spin_lock_init(&timelines->lock);
	INIT_LIST_HEAD(&timelines->active_list);

	spin_lock_init(&timelines->hwsp_lock);
	INIT_LIST_HEAD(&timelines->hwsp_free_list);
}

void intel_timelines_init(struct drm_i915_private *i915)
{
	timelines_init(&i915->gt);
}

void intel_timeline_fini(struct intel_timeline *timeline)
{
	GEM_BUG_ON(atomic_read(&timeline->pin_count));
	GEM_BUG_ON(!list_empty(&timeline->requests));

	if (timeline->hwsp_cacheline)
		cacheline_free(timeline->hwsp_cacheline);
	else
		i915_gem_object_unpin_map(timeline->hwsp_ggtt->obj);

	i915_vma_put(timeline->hwsp_ggtt);
}

struct intel_timeline *
intel_timeline_create(struct intel_gt *gt, struct i915_vma *global_hwsp)
{
	struct intel_timeline *timeline;
	int err;

	timeline = kzalloc(sizeof(*timeline), GFP_KERNEL);
	if (!timeline)
		return ERR_PTR(-ENOMEM);

	err = intel_timeline_init(timeline, gt, global_hwsp);
	if (err) {
		kfree(timeline);
		return ERR_PTR(err);
	}

	return timeline;
}

int intel_timeline_pin(struct intel_timeline *tl)
{
	int err;

	if (atomic_add_unless(&tl->pin_count, 1, 0))
		return 0;

	err = i915_vma_pin(tl->hwsp_ggtt, 0, 0, PIN_GLOBAL | PIN_HIGH);
	if (err)
		return err;

	tl->hwsp_offset =
		i915_ggtt_offset(tl->hwsp_ggtt) +
		offset_in_page(tl->hwsp_offset);

	cacheline_acquire(tl->hwsp_cacheline);
	if (atomic_fetch_inc(&tl->pin_count)) {
		cacheline_release(tl->hwsp_cacheline);
		__i915_vma_unpin(tl->hwsp_ggtt);
	}

	return 0;
}

void intel_timeline_enter(struct intel_timeline *tl)
{
	struct intel_gt_timelines *timelines = &tl->gt->timelines;
	unsigned long flags;

	lockdep_assert_held(&tl->mutex);

	GEM_BUG_ON(!atomic_read(&tl->pin_count));
	if (tl->active_count++)
		return;
	GEM_BUG_ON(!tl->active_count); /* overflow? */

	spin_lock_irqsave(&timelines->lock, flags);
	list_add(&tl->link, &timelines->active_list);
	spin_unlock_irqrestore(&timelines->lock, flags);
}

void intel_timeline_exit(struct intel_timeline *tl)
{
	struct intel_gt_timelines *timelines = &tl->gt->timelines;
	unsigned long flags;

	lockdep_assert_held(&tl->mutex);

	GEM_BUG_ON(!tl->active_count);
	if (--tl->active_count)
		return;

	spin_lock_irqsave(&timelines->lock, flags);
	list_del(&tl->link);
	spin_unlock_irqrestore(&timelines->lock, flags);

	/*
	 * Since this timeline is idle, all bariers upon which we were waiting
	 * must also be complete and so we can discard the last used barriers
	 * without loss of information.
	 */
	i915_syncmap_free(&tl->sync);
}

static u32 timeline_advance(struct intel_timeline *tl)
{
	GEM_BUG_ON(!atomic_read(&tl->pin_count));
	GEM_BUG_ON(tl->seqno & tl->has_initial_breadcrumb);

	return tl->seqno += 1 + tl->has_initial_breadcrumb;
}

static void timeline_rollback(struct intel_timeline *tl)
{
	tl->seqno -= 1 + tl->has_initial_breadcrumb;
}

static noinline int
__intel_timeline_get_seqno(struct intel_timeline *tl,
			   struct i915_request *rq,
			   u32 *seqno)
{
	struct intel_timeline_cacheline *cl;
	unsigned int cacheline;
	struct i915_vma *vma;
	void *vaddr;
	int err;

	/*
	 * If there is an outstanding GPU reference to this cacheline,
	 * such as it being sampled by a HW semaphore on another timeline,
	 * we cannot wraparound our seqno value (the HW semaphore does
	 * a strict greater-than-or-equals compare, not i915_seqno_passed).
	 * So if the cacheline is still busy, we must detach ourselves
	 * from it and leave it inflight alongside its users.
	 *
	 * However, if nobody is watching and we can guarantee that nobody
	 * will, we could simply reuse the same cacheline.
	 *
	 * if (i915_active_request_is_signaled(&tl->last_request) &&
	 *     i915_active_is_signaled(&tl->hwsp_cacheline->active))
	 *	return 0;
	 *
	 * That seems unlikely for a busy timeline that needed to wrap in
	 * the first place, so just replace the cacheline.
	 */

	vma = hwsp_alloc(tl, &cacheline);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err_rollback;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_GLOBAL | PIN_HIGH);
	if (err) {
		__idle_hwsp_free(vma->private, cacheline);
		goto err_rollback;
	}

	cl = cacheline_alloc(vma->private, cacheline);
	if (IS_ERR(cl)) {
		err = PTR_ERR(cl);
		__idle_hwsp_free(vma->private, cacheline);
		goto err_unpin;
	}
	GEM_BUG_ON(cl->hwsp->vma != vma);

	/*
	 * Attach the old cacheline to the current request, so that we only
	 * free it after the current request is retired, which ensures that
	 * all writes into the cacheline from previous requests are complete.
	 */
	err = i915_active_ref(&tl->hwsp_cacheline->active, tl, rq);
	if (err)
		goto err_cacheline;

	cacheline_release(tl->hwsp_cacheline); /* ownership now xfered to rq */
	cacheline_free(tl->hwsp_cacheline);

	i915_vma_unpin(tl->hwsp_ggtt); /* binding kept alive by old cacheline */
	i915_vma_put(tl->hwsp_ggtt);

	tl->hwsp_ggtt = i915_vma_get(vma);

	vaddr = page_mask_bits(cl->vaddr);
	tl->hwsp_offset = cacheline * CACHELINE_BYTES;
	tl->hwsp_seqno =
		memset(vaddr + tl->hwsp_offset, 0, CACHELINE_BYTES);

	tl->hwsp_offset += i915_ggtt_offset(vma);

	cacheline_acquire(cl);
	tl->hwsp_cacheline = cl;

	*seqno = timeline_advance(tl);
	GEM_BUG_ON(i915_seqno_passed(*tl->hwsp_seqno, *seqno));
	return 0;

err_cacheline:
	cacheline_free(cl);
err_unpin:
	i915_vma_unpin(vma);
err_rollback:
	timeline_rollback(tl);
	return err;
}

int intel_timeline_get_seqno(struct intel_timeline *tl,
			     struct i915_request *rq,
			     u32 *seqno)
{
	*seqno = timeline_advance(tl);

	/* Replace the HWSP on wraparound for HW semaphores */
	if (unlikely(!*seqno && tl->hwsp_cacheline))
		return __intel_timeline_get_seqno(tl, rq, seqno);

	return 0;
}

static int cacheline_ref(struct intel_timeline_cacheline *cl,
			 struct i915_request *rq)
{
	return i915_active_ref(&cl->active, rq->timeline, rq);
}

int intel_timeline_read_hwsp(struct i915_request *from,
			     struct i915_request *to,
			     u32 *hwsp)
{
	struct intel_timeline_cacheline *cl = from->hwsp_cacheline;
	struct intel_timeline *tl = from->timeline;
	int err;

	GEM_BUG_ON(to->timeline == tl);

	mutex_lock_nested(&tl->mutex, SINGLE_DEPTH_NESTING);
	err = i915_request_completed(from);
	if (!err)
		err = cacheline_ref(cl, to);
	if (!err) {
		if (likely(cl == tl->hwsp_cacheline)) {
			*hwsp = tl->hwsp_offset;
		} else { /* across a seqno wrap, recover the original offset */
			*hwsp = i915_ggtt_offset(cl->hwsp->vma) +
				ptr_unmask_bits(cl->vaddr, CACHELINE_BITS) *
				CACHELINE_BYTES;
		}
	}
	mutex_unlock(&tl->mutex);

	return err;
}

void intel_timeline_unpin(struct intel_timeline *tl)
{
	GEM_BUG_ON(!atomic_read(&tl->pin_count));
	if (!atomic_dec_and_test(&tl->pin_count))
		return;

	cacheline_release(tl->hwsp_cacheline);

	__i915_vma_unpin(tl->hwsp_ggtt);
}

void __intel_timeline_free(struct kref *kref)
{
	struct intel_timeline *timeline =
		container_of(kref, typeof(*timeline), kref);

	intel_timeline_fini(timeline);
	kfree(timeline);
}

static void timelines_fini(struct intel_gt *gt)
{
	struct intel_gt_timelines *timelines = &gt->timelines;

	GEM_BUG_ON(!list_empty(&timelines->active_list));
	GEM_BUG_ON(!list_empty(&timelines->hwsp_free_list));
}

void intel_timelines_fini(struct drm_i915_private *i915)
{
	timelines_fini(&i915->gt);
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "gt/selftests/mock_timeline.c"
#include "gt/selftest_timeline.c"
#endif
