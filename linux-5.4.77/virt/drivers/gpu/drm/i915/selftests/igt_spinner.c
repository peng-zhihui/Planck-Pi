/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */
#include "gt/intel_gt.h"

#include "gem/selftests/igt_gem_utils.h"

#include "igt_spinner.h"

int igt_spinner_init(struct igt_spinner *spin, struct intel_gt *gt)
{
	unsigned int mode;
	void *vaddr;
	int err;

	GEM_BUG_ON(INTEL_GEN(gt->i915) < 8);

	memset(spin, 0, sizeof(*spin));
	spin->gt = gt;

	spin->hws = i915_gem_object_create_internal(gt->i915, PAGE_SIZE);
	if (IS_ERR(spin->hws)) {
		err = PTR_ERR(spin->hws);
		goto err;
	}

	spin->obj = i915_gem_object_create_internal(gt->i915, PAGE_SIZE);
	if (IS_ERR(spin->obj)) {
		err = PTR_ERR(spin->obj);
		goto err_hws;
	}

	i915_gem_object_set_cache_coherency(spin->hws, I915_CACHE_LLC);
	vaddr = i915_gem_object_pin_map(spin->hws, I915_MAP_WB);
	if (IS_ERR(vaddr)) {
		err = PTR_ERR(vaddr);
		goto err_obj;
	}
	spin->seqno = memset(vaddr, 0xff, PAGE_SIZE);

	mode = i915_coherent_map_type(gt->i915);
	vaddr = i915_gem_object_pin_map(spin->obj, mode);
	if (IS_ERR(vaddr)) {
		err = PTR_ERR(vaddr);
		goto err_unpin_hws;
	}
	spin->batch = vaddr;

	return 0;

err_unpin_hws:
	i915_gem_object_unpin_map(spin->hws);
err_obj:
	i915_gem_object_put(spin->obj);
err_hws:
	i915_gem_object_put(spin->hws);
err:
	return err;
}

static unsigned int seqno_offset(u64 fence)
{
	return offset_in_page(sizeof(u32) * fence);
}

static u64 hws_address(const struct i915_vma *hws,
		       const struct i915_request *rq)
{
	return hws->node.start + seqno_offset(rq->fence.context);
}

static int move_to_active(struct i915_vma *vma,
			  struct i915_request *rq,
			  unsigned int flags)
{
	int err;

	i915_vma_lock(vma);
	err = i915_request_await_object(rq, vma->obj,
					flags & EXEC_OBJECT_WRITE);
	if (err == 0)
		err = i915_vma_move_to_active(vma, rq, flags);
	i915_vma_unlock(vma);

	return err;
}

struct i915_request *
igt_spinner_create_request(struct igt_spinner *spin,
			   struct intel_context *ce,
			   u32 arbitration_command)
{
	struct intel_engine_cs *engine = ce->engine;
	struct i915_request *rq = NULL;
	struct i915_vma *hws, *vma;
	u32 *batch;
	int err;

	GEM_BUG_ON(spin->gt != ce->vm->gt);

	vma = i915_vma_instance(spin->obj, ce->vm, NULL);
	if (IS_ERR(vma))
		return ERR_CAST(vma);

	hws = i915_vma_instance(spin->hws, ce->vm, NULL);
	if (IS_ERR(hws))
		return ERR_CAST(hws);

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err)
		return ERR_PTR(err);

	err = i915_vma_pin(hws, 0, 0, PIN_USER);
	if (err)
		goto unpin_vma;

	rq = intel_context_create_request(ce);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto unpin_hws;
	}

	err = move_to_active(vma, rq, 0);
	if (err)
		goto cancel_rq;

	err = move_to_active(hws, rq, 0);
	if (err)
		goto cancel_rq;

	batch = spin->batch;

	*batch++ = MI_STORE_DWORD_IMM_GEN4;
	*batch++ = lower_32_bits(hws_address(hws, rq));
	*batch++ = upper_32_bits(hws_address(hws, rq));
	*batch++ = rq->fence.seqno;

	*batch++ = arbitration_command;

	*batch++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	*batch++ = lower_32_bits(vma->node.start);
	*batch++ = upper_32_bits(vma->node.start);
	*batch++ = MI_BATCH_BUFFER_END; /* not reached */

	intel_gt_chipset_flush(engine->gt);

	if (engine->emit_init_breadcrumb &&
	    rq->timeline->has_initial_breadcrumb) {
		err = engine->emit_init_breadcrumb(rq);
		if (err)
			goto cancel_rq;
	}

	err = engine->emit_bb_start(rq, vma->node.start, PAGE_SIZE, 0);

cancel_rq:
	if (err) {
		i915_request_skip(rq, err);
		i915_request_add(rq);
	}
unpin_hws:
	i915_vma_unpin(hws);
unpin_vma:
	i915_vma_unpin(vma);
	return err ? ERR_PTR(err) : rq;
}

static u32
hws_seqno(const struct igt_spinner *spin, const struct i915_request *rq)
{
	u32 *seqno = spin->seqno + seqno_offset(rq->fence.context);

	return READ_ONCE(*seqno);
}

void igt_spinner_end(struct igt_spinner *spin)
{
	*spin->batch = MI_BATCH_BUFFER_END;
	intel_gt_chipset_flush(spin->gt);
}

void igt_spinner_fini(struct igt_spinner *spin)
{
	igt_spinner_end(spin);

	i915_gem_object_unpin_map(spin->obj);
	i915_gem_object_put(spin->obj);

	i915_gem_object_unpin_map(spin->hws);
	i915_gem_object_put(spin->hws);
}

bool igt_wait_for_spinner(struct igt_spinner *spin, struct i915_request *rq)
{
	return !(wait_for_us(i915_seqno_passed(hws_seqno(spin, rq),
					       rq->fence.seqno),
			     10) &&
		 wait_for(i915_seqno_passed(hws_seqno(spin, rq),
					    rq->fence.seqno),
			  1000));
}
