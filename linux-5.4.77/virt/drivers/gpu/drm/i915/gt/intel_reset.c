/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2008-2018 Intel Corporation
 */

#include <linux/sched/mm.h>
#include <linux/stop_machine.h>

#include "display/intel_display_types.h"
#include "display/intel_overlay.h"

#include "gem/i915_gem_context.h"

#include "i915_drv.h"
#include "i915_gpu_error.h"
#include "i915_irq.h"
#include "intel_engine_pm.h"
#include "intel_gt.h"
#include "intel_gt_pm.h"
#include "intel_reset.h"

#include "uc/intel_guc.h"

#define RESET_MAX_RETRIES 3

/* XXX How to handle concurrent GGTT updates using tiling registers? */
#define RESET_UNDER_STOP_MACHINE 0

static void rmw_set_fw(struct intel_uncore *uncore, i915_reg_t reg, u32 set)
{
	intel_uncore_rmw_fw(uncore, reg, 0, set);
}

static void rmw_clear_fw(struct intel_uncore *uncore, i915_reg_t reg, u32 clr)
{
	intel_uncore_rmw_fw(uncore, reg, clr, 0);
}

static void engine_skip_context(struct i915_request *rq)
{
	struct intel_engine_cs *engine = rq->engine;
	struct i915_gem_context *hung_ctx = rq->gem_context;

	if (!i915_request_is_active(rq))
		return;

	lockdep_assert_held(&engine->active.lock);
	list_for_each_entry_continue(rq, &engine->active.requests, sched.link)
		if (rq->gem_context == hung_ctx)
			i915_request_skip(rq, -EIO);
}

static void client_mark_guilty(struct drm_i915_file_private *file_priv,
			       const struct i915_gem_context *ctx)
{
	unsigned int score;
	unsigned long prev_hang;

	if (i915_gem_context_is_banned(ctx))
		score = I915_CLIENT_SCORE_CONTEXT_BAN;
	else
		score = 0;

	prev_hang = xchg(&file_priv->hang_timestamp, jiffies);
	if (time_before(jiffies, prev_hang + I915_CLIENT_FAST_HANG_JIFFIES))
		score += I915_CLIENT_SCORE_HANG_FAST;

	if (score) {
		atomic_add(score, &file_priv->ban_score);

		DRM_DEBUG_DRIVER("client %s: gained %u ban score, now %u\n",
				 ctx->name, score,
				 atomic_read(&file_priv->ban_score));
	}
}

static bool context_mark_guilty(struct i915_gem_context *ctx)
{
	unsigned long prev_hang;
	bool banned;
	int i;

	atomic_inc(&ctx->guilty_count);

	/* Cool contexts are too cool to be banned! (Used for reset testing.) */
	if (!i915_gem_context_is_bannable(ctx))
		return false;

	/* Record the timestamp for the last N hangs */
	prev_hang = ctx->hang_timestamp[0];
	for (i = 0; i < ARRAY_SIZE(ctx->hang_timestamp) - 1; i++)
		ctx->hang_timestamp[i] = ctx->hang_timestamp[i + 1];
	ctx->hang_timestamp[i] = jiffies;

	/* If we have hung N+1 times in rapid succession, we ban the context! */
	banned = !i915_gem_context_is_recoverable(ctx);
	if (time_before(jiffies, prev_hang + CONTEXT_FAST_HANG_JIFFIES))
		banned = true;
	if (banned) {
		DRM_DEBUG_DRIVER("context %s: guilty %d, banned\n",
				 ctx->name, atomic_read(&ctx->guilty_count));
		i915_gem_context_set_banned(ctx);
	}

	if (!IS_ERR_OR_NULL(ctx->file_priv))
		client_mark_guilty(ctx->file_priv, ctx);

	return banned;
}

static void context_mark_innocent(struct i915_gem_context *ctx)
{
	atomic_inc(&ctx->active_count);
}

void __i915_request_reset(struct i915_request *rq, bool guilty)
{
	GEM_TRACE("%s rq=%llx:%lld, guilty? %s\n",
		  rq->engine->name,
		  rq->fence.context,
		  rq->fence.seqno,
		  yesno(guilty));

	GEM_BUG_ON(i915_request_completed(rq));

	if (guilty) {
		i915_request_skip(rq, -EIO);
		if (context_mark_guilty(rq->gem_context))
			engine_skip_context(rq);
	} else {
		dma_fence_set_error(&rq->fence, -EAGAIN);
		context_mark_innocent(rq->gem_context);
	}
}

static bool i915_in_reset(struct pci_dev *pdev)
{
	u8 gdrst;

	pci_read_config_byte(pdev, I915_GDRST, &gdrst);
	return gdrst & GRDOM_RESET_STATUS;
}

static int i915_do_reset(struct intel_gt *gt,
			 intel_engine_mask_t engine_mask,
			 unsigned int retry)
{
	struct pci_dev *pdev = gt->i915->drm.pdev;
	int err;

	/* Assert reset for at least 20 usec, and wait for acknowledgement. */
	pci_write_config_byte(pdev, I915_GDRST, GRDOM_RESET_ENABLE);
	udelay(50);
	err = wait_for_atomic(i915_in_reset(pdev), 50);

	/* Clear the reset request. */
	pci_write_config_byte(pdev, I915_GDRST, 0);
	udelay(50);
	if (!err)
		err = wait_for_atomic(!i915_in_reset(pdev), 50);

	return err;
}

static bool g4x_reset_complete(struct pci_dev *pdev)
{
	u8 gdrst;

	pci_read_config_byte(pdev, I915_GDRST, &gdrst);
	return (gdrst & GRDOM_RESET_ENABLE) == 0;
}

static int g33_do_reset(struct intel_gt *gt,
			intel_engine_mask_t engine_mask,
			unsigned int retry)
{
	struct pci_dev *pdev = gt->i915->drm.pdev;

	pci_write_config_byte(pdev, I915_GDRST, GRDOM_RESET_ENABLE);
	return wait_for_atomic(g4x_reset_complete(pdev), 50);
}

static int g4x_do_reset(struct intel_gt *gt,
			intel_engine_mask_t engine_mask,
			unsigned int retry)
{
	struct pci_dev *pdev = gt->i915->drm.pdev;
	struct intel_uncore *uncore = gt->uncore;
	int ret;

	/* WaVcpClkGateDisableForMediaReset:ctg,elk */
	rmw_set_fw(uncore, VDECCLK_GATE_D, VCP_UNIT_CLOCK_GATE_DISABLE);
	intel_uncore_posting_read_fw(uncore, VDECCLK_GATE_D);

	pci_write_config_byte(pdev, I915_GDRST,
			      GRDOM_MEDIA | GRDOM_RESET_ENABLE);
	ret =  wait_for_atomic(g4x_reset_complete(pdev), 50);
	if (ret) {
		DRM_DEBUG_DRIVER("Wait for media reset failed\n");
		goto out;
	}

	pci_write_config_byte(pdev, I915_GDRST,
			      GRDOM_RENDER | GRDOM_RESET_ENABLE);
	ret =  wait_for_atomic(g4x_reset_complete(pdev), 50);
	if (ret) {
		DRM_DEBUG_DRIVER("Wait for render reset failed\n");
		goto out;
	}

out:
	pci_write_config_byte(pdev, I915_GDRST, 0);

	rmw_clear_fw(uncore, VDECCLK_GATE_D, VCP_UNIT_CLOCK_GATE_DISABLE);
	intel_uncore_posting_read_fw(uncore, VDECCLK_GATE_D);

	return ret;
}

static int ironlake_do_reset(struct intel_gt *gt,
			     intel_engine_mask_t engine_mask,
			     unsigned int retry)
{
	struct intel_uncore *uncore = gt->uncore;
	int ret;

	intel_uncore_write_fw(uncore, ILK_GDSR,
			      ILK_GRDOM_RENDER | ILK_GRDOM_RESET_ENABLE);
	ret = __intel_wait_for_register_fw(uncore, ILK_GDSR,
					   ILK_GRDOM_RESET_ENABLE, 0,
					   5000, 0,
					   NULL);
	if (ret) {
		DRM_DEBUG_DRIVER("Wait for render reset failed\n");
		goto out;
	}

	intel_uncore_write_fw(uncore, ILK_GDSR,
			      ILK_GRDOM_MEDIA | ILK_GRDOM_RESET_ENABLE);
	ret = __intel_wait_for_register_fw(uncore, ILK_GDSR,
					   ILK_GRDOM_RESET_ENABLE, 0,
					   5000, 0,
					   NULL);
	if (ret) {
		DRM_DEBUG_DRIVER("Wait for media reset failed\n");
		goto out;
	}

out:
	intel_uncore_write_fw(uncore, ILK_GDSR, 0);
	intel_uncore_posting_read_fw(uncore, ILK_GDSR);
	return ret;
}

/* Reset the hardware domains (GENX_GRDOM_*) specified by mask */
static int gen6_hw_domain_reset(struct intel_gt *gt, u32 hw_domain_mask)
{
	struct intel_uncore *uncore = gt->uncore;
	int err;

	/*
	 * GEN6_GDRST is not in the gt power well, no need to check
	 * for fifo space for the write or forcewake the chip for
	 * the read
	 */
	intel_uncore_write_fw(uncore, GEN6_GDRST, hw_domain_mask);

	/* Wait for the device to ack the reset requests */
	err = __intel_wait_for_register_fw(uncore,
					   GEN6_GDRST, hw_domain_mask, 0,
					   500, 0,
					   NULL);
	if (err)
		DRM_DEBUG_DRIVER("Wait for 0x%08x engines reset failed\n",
				 hw_domain_mask);

	return err;
}

static int gen6_reset_engines(struct intel_gt *gt,
			      intel_engine_mask_t engine_mask,
			      unsigned int retry)
{
	struct intel_engine_cs *engine;
	const u32 hw_engine_mask[] = {
		[RCS0]  = GEN6_GRDOM_RENDER,
		[BCS0]  = GEN6_GRDOM_BLT,
		[VCS0]  = GEN6_GRDOM_MEDIA,
		[VCS1]  = GEN8_GRDOM_MEDIA2,
		[VECS0] = GEN6_GRDOM_VECS,
	};
	u32 hw_mask;

	if (engine_mask == ALL_ENGINES) {
		hw_mask = GEN6_GRDOM_FULL;
	} else {
		intel_engine_mask_t tmp;

		hw_mask = 0;
		for_each_engine_masked(engine, gt->i915, engine_mask, tmp) {
			GEM_BUG_ON(engine->id >= ARRAY_SIZE(hw_engine_mask));
			hw_mask |= hw_engine_mask[engine->id];
		}
	}

	return gen6_hw_domain_reset(gt, hw_mask);
}

static u32 gen11_lock_sfc(struct intel_engine_cs *engine)
{
	struct intel_uncore *uncore = engine->uncore;
	u8 vdbox_sfc_access = RUNTIME_INFO(engine->i915)->vdbox_sfc_access;
	i915_reg_t sfc_forced_lock, sfc_forced_lock_ack;
	u32 sfc_forced_lock_bit, sfc_forced_lock_ack_bit;
	i915_reg_t sfc_usage;
	u32 sfc_usage_bit;
	u32 sfc_reset_bit;

	switch (engine->class) {
	case VIDEO_DECODE_CLASS:
		if ((BIT(engine->instance) & vdbox_sfc_access) == 0)
			return 0;

		sfc_forced_lock = GEN11_VCS_SFC_FORCED_LOCK(engine);
		sfc_forced_lock_bit = GEN11_VCS_SFC_FORCED_LOCK_BIT;

		sfc_forced_lock_ack = GEN11_VCS_SFC_LOCK_STATUS(engine);
		sfc_forced_lock_ack_bit  = GEN11_VCS_SFC_LOCK_ACK_BIT;

		sfc_usage = GEN11_VCS_SFC_LOCK_STATUS(engine);
		sfc_usage_bit = GEN11_VCS_SFC_USAGE_BIT;
		sfc_reset_bit = GEN11_VCS_SFC_RESET_BIT(engine->instance);
		break;

	case VIDEO_ENHANCEMENT_CLASS:
		sfc_forced_lock = GEN11_VECS_SFC_FORCED_LOCK(engine);
		sfc_forced_lock_bit = GEN11_VECS_SFC_FORCED_LOCK_BIT;

		sfc_forced_lock_ack = GEN11_VECS_SFC_LOCK_ACK(engine);
		sfc_forced_lock_ack_bit  = GEN11_VECS_SFC_LOCK_ACK_BIT;

		sfc_usage = GEN11_VECS_SFC_USAGE(engine);
		sfc_usage_bit = GEN11_VECS_SFC_USAGE_BIT;
		sfc_reset_bit = GEN11_VECS_SFC_RESET_BIT(engine->instance);
		break;

	default:
		return 0;
	}

	/*
	 * Tell the engine that a software reset is going to happen. The engine
	 * will then try to force lock the SFC (if currently locked, it will
	 * remain so until we tell the engine it is safe to unlock; if currently
	 * unlocked, it will ignore this and all new lock requests). If SFC
	 * ends up being locked to the engine we want to reset, we have to reset
	 * it as well (we will unlock it once the reset sequence is completed).
	 */
	rmw_set_fw(uncore, sfc_forced_lock, sfc_forced_lock_bit);

	if (__intel_wait_for_register_fw(uncore,
					 sfc_forced_lock_ack,
					 sfc_forced_lock_ack_bit,
					 sfc_forced_lock_ack_bit,
					 1000, 0, NULL)) {
		DRM_DEBUG_DRIVER("Wait for SFC forced lock ack failed\n");
		return 0;
	}

	if (intel_uncore_read_fw(uncore, sfc_usage) & sfc_usage_bit)
		return sfc_reset_bit;

	return 0;
}

static void gen11_unlock_sfc(struct intel_engine_cs *engine)
{
	struct intel_uncore *uncore = engine->uncore;
	u8 vdbox_sfc_access = RUNTIME_INFO(engine->i915)->vdbox_sfc_access;
	i915_reg_t sfc_forced_lock;
	u32 sfc_forced_lock_bit;

	switch (engine->class) {
	case VIDEO_DECODE_CLASS:
		if ((BIT(engine->instance) & vdbox_sfc_access) == 0)
			return;

		sfc_forced_lock = GEN11_VCS_SFC_FORCED_LOCK(engine);
		sfc_forced_lock_bit = GEN11_VCS_SFC_FORCED_LOCK_BIT;
		break;

	case VIDEO_ENHANCEMENT_CLASS:
		sfc_forced_lock = GEN11_VECS_SFC_FORCED_LOCK(engine);
		sfc_forced_lock_bit = GEN11_VECS_SFC_FORCED_LOCK_BIT;
		break;

	default:
		return;
	}

	rmw_clear_fw(uncore, sfc_forced_lock, sfc_forced_lock_bit);
}

static int gen11_reset_engines(struct intel_gt *gt,
			       intel_engine_mask_t engine_mask,
			       unsigned int retry)
{
	const u32 hw_engine_mask[] = {
		[RCS0]  = GEN11_GRDOM_RENDER,
		[BCS0]  = GEN11_GRDOM_BLT,
		[VCS0]  = GEN11_GRDOM_MEDIA,
		[VCS1]  = GEN11_GRDOM_MEDIA2,
		[VCS2]  = GEN11_GRDOM_MEDIA3,
		[VCS3]  = GEN11_GRDOM_MEDIA4,
		[VECS0] = GEN11_GRDOM_VECS,
		[VECS1] = GEN11_GRDOM_VECS2,
	};
	struct intel_engine_cs *engine;
	intel_engine_mask_t tmp;
	u32 hw_mask;
	int ret;

	if (engine_mask == ALL_ENGINES) {
		hw_mask = GEN11_GRDOM_FULL;
	} else {
		hw_mask = 0;
		for_each_engine_masked(engine, gt->i915, engine_mask, tmp) {
			GEM_BUG_ON(engine->id >= ARRAY_SIZE(hw_engine_mask));
			hw_mask |= hw_engine_mask[engine->id];
			hw_mask |= gen11_lock_sfc(engine);
		}
	}

	ret = gen6_hw_domain_reset(gt, hw_mask);

	if (engine_mask != ALL_ENGINES)
		for_each_engine_masked(engine, gt->i915, engine_mask, tmp)
			gen11_unlock_sfc(engine);

	return ret;
}

static int gen8_engine_reset_prepare(struct intel_engine_cs *engine)
{
	struct intel_uncore *uncore = engine->uncore;
	const i915_reg_t reg = RING_RESET_CTL(engine->mmio_base);
	u32 request, mask, ack;
	int ret;

	ack = intel_uncore_read_fw(uncore, reg);
	if (ack & RESET_CTL_CAT_ERROR) {
		/*
		 * For catastrophic errors, ready-for-reset sequence
		 * needs to be bypassed: HAS#396813
		 */
		request = RESET_CTL_CAT_ERROR;
		mask = RESET_CTL_CAT_ERROR;

		/* Catastrophic errors need to be cleared by HW */
		ack = 0;
	} else if (!(ack & RESET_CTL_READY_TO_RESET)) {
		request = RESET_CTL_REQUEST_RESET;
		mask = RESET_CTL_READY_TO_RESET;
		ack = RESET_CTL_READY_TO_RESET;
	} else {
		return 0;
	}

	intel_uncore_write_fw(uncore, reg, _MASKED_BIT_ENABLE(request));
	ret = __intel_wait_for_register_fw(uncore, reg, mask, ack,
					   700, 0, NULL);
	if (ret)
		DRM_ERROR("%s reset request timed out: {request: %08x, RESET_CTL: %08x}\n",
			  engine->name, request,
			  intel_uncore_read_fw(uncore, reg));

	return ret;
}

static void gen8_engine_reset_cancel(struct intel_engine_cs *engine)
{
	intel_uncore_write_fw(engine->uncore,
			      RING_RESET_CTL(engine->mmio_base),
			      _MASKED_BIT_DISABLE(RESET_CTL_REQUEST_RESET));
}

static int gen8_reset_engines(struct intel_gt *gt,
			      intel_engine_mask_t engine_mask,
			      unsigned int retry)
{
	struct intel_engine_cs *engine;
	const bool reset_non_ready = retry >= 1;
	intel_engine_mask_t tmp;
	int ret;

	for_each_engine_masked(engine, gt->i915, engine_mask, tmp) {
		ret = gen8_engine_reset_prepare(engine);
		if (ret && !reset_non_ready)
			goto skip_reset;

		/*
		 * If this is not the first failed attempt to prepare,
		 * we decide to proceed anyway.
		 *
		 * By doing so we risk context corruption and with
		 * some gens (kbl), possible system hang if reset
		 * happens during active bb execution.
		 *
		 * We rather take context corruption instead of
		 * failed reset with a wedged driver/gpu. And
		 * active bb execution case should be covered by
		 * stop_engines() we have before the reset.
		 */
	}

	if (INTEL_GEN(gt->i915) >= 11)
		ret = gen11_reset_engines(gt, engine_mask, retry);
	else
		ret = gen6_reset_engines(gt, engine_mask, retry);

skip_reset:
	for_each_engine_masked(engine, gt->i915, engine_mask, tmp)
		gen8_engine_reset_cancel(engine);

	return ret;
}

typedef int (*reset_func)(struct intel_gt *,
			  intel_engine_mask_t engine_mask,
			  unsigned int retry);

static reset_func intel_get_gpu_reset(struct drm_i915_private *i915)
{
	if (INTEL_GEN(i915) >= 8)
		return gen8_reset_engines;
	else if (INTEL_GEN(i915) >= 6)
		return gen6_reset_engines;
	else if (INTEL_GEN(i915) >= 5)
		return ironlake_do_reset;
	else if (IS_G4X(i915))
		return g4x_do_reset;
	else if (IS_G33(i915) || IS_PINEVIEW(i915))
		return g33_do_reset;
	else if (INTEL_GEN(i915) >= 3)
		return i915_do_reset;
	else
		return NULL;
}

int __intel_gt_reset(struct intel_gt *gt, intel_engine_mask_t engine_mask)
{
	const int retries = engine_mask == ALL_ENGINES ? RESET_MAX_RETRIES : 1;
	reset_func reset;
	int ret = -ETIMEDOUT;
	int retry;

	reset = intel_get_gpu_reset(gt->i915);
	if (!reset)
		return -ENODEV;

	/*
	 * If the power well sleeps during the reset, the reset
	 * request may be dropped and never completes (causing -EIO).
	 */
	intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL);
	for (retry = 0; ret == -ETIMEDOUT && retry < retries; retry++) {
		GEM_TRACE("engine_mask=%x\n", engine_mask);
		preempt_disable();
		ret = reset(gt, engine_mask, retry);
		preempt_enable();
	}
	intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL);

	return ret;
}

bool intel_has_gpu_reset(struct drm_i915_private *i915)
{
	if (!i915_modparams.reset)
		return NULL;

	return intel_get_gpu_reset(i915);
}

bool intel_has_reset_engine(struct drm_i915_private *i915)
{
	return INTEL_INFO(i915)->has_reset_engine && i915_modparams.reset >= 2;
}

int intel_reset_guc(struct intel_gt *gt)
{
	u32 guc_domain =
		INTEL_GEN(gt->i915) >= 11 ? GEN11_GRDOM_GUC : GEN9_GRDOM_GUC;
	int ret;

	GEM_BUG_ON(!HAS_GT_UC(gt->i915));

	intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL);
	ret = gen6_hw_domain_reset(gt, guc_domain);
	intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL);

	return ret;
}

/*
 * Ensure irq handler finishes, and not run again.
 * Also return the active request so that we only search for it once.
 */
static void reset_prepare_engine(struct intel_engine_cs *engine)
{
	/*
	 * During the reset sequence, we must prevent the engine from
	 * entering RC6. As the context state is undefined until we restart
	 * the engine, if it does enter RC6 during the reset, the state
	 * written to the powercontext is undefined and so we may lose
	 * GPU state upon resume, i.e. fail to restart after a reset.
	 */
	intel_uncore_forcewake_get(engine->uncore, FORCEWAKE_ALL);
	engine->reset.prepare(engine);
}

static void revoke_mmaps(struct intel_gt *gt)
{
	int i;

	for (i = 0; i < gt->ggtt->num_fences; i++) {
		struct drm_vma_offset_node *node;
		struct i915_vma *vma;
		u64 vma_offset;

		vma = READ_ONCE(gt->ggtt->fence_regs[i].vma);
		if (!vma)
			continue;

		if (!i915_vma_has_userfault(vma))
			continue;

		GEM_BUG_ON(vma->fence != &gt->ggtt->fence_regs[i]);
		node = &vma->obj->base.vma_node;
		vma_offset = vma->ggtt_view.partial.offset << PAGE_SHIFT;
		unmap_mapping_range(gt->i915->drm.anon_inode->i_mapping,
				    drm_vma_node_offset_addr(node) + vma_offset,
				    vma->size,
				    1);
	}
}

static intel_engine_mask_t reset_prepare(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t awake = 0;
	enum intel_engine_id id;

	for_each_engine(engine, gt->i915, id) {
		if (intel_engine_pm_get_if_awake(engine))
			awake |= engine->mask;
		reset_prepare_engine(engine);
	}

	intel_uc_reset_prepare(&gt->uc);

	return awake;
}

static void gt_revoke(struct intel_gt *gt)
{
	revoke_mmaps(gt);
}

static int gt_reset(struct intel_gt *gt, intel_engine_mask_t stalled_mask)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err;

	/*
	 * Everything depends on having the GTT running, so we need to start
	 * there.
	 */
	err = i915_ggtt_enable_hw(gt->i915);
	if (err)
		return err;

	for_each_engine(engine, gt->i915, id)
		__intel_engine_reset(engine, stalled_mask & engine->mask);

	i915_gem_restore_fences(gt->i915);

	return err;
}

static void reset_finish_engine(struct intel_engine_cs *engine)
{
	engine->reset.finish(engine);
	intel_uncore_forcewake_put(engine->uncore, FORCEWAKE_ALL);

	intel_engine_signal_breadcrumbs(engine);
}

static void reset_finish(struct intel_gt *gt, intel_engine_mask_t awake)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	for_each_engine(engine, gt->i915, id) {
		reset_finish_engine(engine);
		if (awake & engine->mask)
			intel_engine_pm_put(engine);
	}
}

static void nop_submit_request(struct i915_request *request)
{
	struct intel_engine_cs *engine = request->engine;
	unsigned long flags;

	GEM_TRACE("%s fence %llx:%lld -> -EIO\n",
		  engine->name, request->fence.context, request->fence.seqno);
	dma_fence_set_error(&request->fence, -EIO);

	spin_lock_irqsave(&engine->active.lock, flags);
	__i915_request_submit(request);
	i915_request_mark_complete(request);
	spin_unlock_irqrestore(&engine->active.lock, flags);

	intel_engine_queue_breadcrumbs(engine);
}

static void __intel_gt_set_wedged(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t awake;
	enum intel_engine_id id;

	if (test_bit(I915_WEDGED, &gt->reset.flags))
		return;

	if (GEM_SHOW_DEBUG() && !intel_engines_are_idle(gt)) {
		struct drm_printer p = drm_debug_printer(__func__);

		for_each_engine(engine, gt->i915, id)
			intel_engine_dump(engine, &p, "%s\n", engine->name);
	}

	GEM_TRACE("start\n");

	/*
	 * First, stop submission to hw, but do not yet complete requests by
	 * rolling the global seqno forward (since this would complete requests
	 * for which we haven't set the fence error to EIO yet).
	 */
	awake = reset_prepare(gt);

	/* Even if the GPU reset fails, it should still stop the engines */
	if (!INTEL_INFO(gt->i915)->gpu_reset_clobbers_display)
		__intel_gt_reset(gt, ALL_ENGINES);

	for_each_engine(engine, gt->i915, id)
		engine->submit_request = nop_submit_request;

	/*
	 * Make sure no request can slip through without getting completed by
	 * either this call here to intel_engine_write_global_seqno, or the one
	 * in nop_submit_request.
	 */
	synchronize_rcu_expedited();
	set_bit(I915_WEDGED, &gt->reset.flags);

	/* Mark all executing requests as skipped */
	for_each_engine(engine, gt->i915, id)
		engine->cancel_requests(engine);

	reset_finish(gt, awake);

	GEM_TRACE("end\n");
}

void intel_gt_set_wedged(struct intel_gt *gt)
{
	intel_wakeref_t wakeref;

	mutex_lock(&gt->reset.mutex);
	with_intel_runtime_pm(&gt->i915->runtime_pm, wakeref)
		__intel_gt_set_wedged(gt);
	mutex_unlock(&gt->reset.mutex);
}

static bool __intel_gt_unset_wedged(struct intel_gt *gt)
{
	struct intel_gt_timelines *timelines = &gt->timelines;
	struct intel_timeline *tl;
	unsigned long flags;

	if (!test_bit(I915_WEDGED, &gt->reset.flags))
		return true;

	if (!gt->scratch) /* Never full initialised, recovery impossible */
		return false;

	GEM_TRACE("start\n");

	/*
	 * Before unwedging, make sure that all pending operations
	 * are flushed and errored out - we may have requests waiting upon
	 * third party fences. We marked all inflight requests as EIO, and
	 * every execbuf since returned EIO, for consistency we want all
	 * the currently pending requests to also be marked as EIO, which
	 * is done inside our nop_submit_request - and so we must wait.
	 *
	 * No more can be submitted until we reset the wedged bit.
	 */
	spin_lock_irqsave(&timelines->lock, flags);
	list_for_each_entry(tl, &timelines->active_list, link) {
		struct i915_request *rq;

		rq = i915_active_request_get_unlocked(&tl->last_request);
		if (!rq)
			continue;

		spin_unlock_irqrestore(&timelines->lock, flags);

		/*
		 * All internal dependencies (i915_requests) will have
		 * been flushed by the set-wedge, but we may be stuck waiting
		 * for external fences. These should all be capped to 10s
		 * (I915_FENCE_TIMEOUT) so this wait should not be unbounded
		 * in the worst case.
		 */
		dma_fence_default_wait(&rq->fence, false, MAX_SCHEDULE_TIMEOUT);
		i915_request_put(rq);

		/* Restart iteration after droping lock */
		spin_lock_irqsave(&timelines->lock, flags);
		tl = list_entry(&timelines->active_list, typeof(*tl), link);
	}
	spin_unlock_irqrestore(&timelines->lock, flags);

	intel_gt_sanitize(gt, false);

	/*
	 * Undo nop_submit_request. We prevent all new i915 requests from
	 * being queued (by disallowing execbuf whilst wedged) so having
	 * waited for all active requests above, we know the system is idle
	 * and do not have to worry about a thread being inside
	 * engine->submit_request() as we swap over. So unlike installing
	 * the nop_submit_request on reset, we can do this from normal
	 * context and do not require stop_machine().
	 */
	intel_engines_reset_default_submission(gt);

	GEM_TRACE("end\n");

	smp_mb__before_atomic(); /* complete takeover before enabling execbuf */
	clear_bit(I915_WEDGED, &gt->reset.flags);

	return true;
}

bool intel_gt_unset_wedged(struct intel_gt *gt)
{
	bool result;

	mutex_lock(&gt->reset.mutex);
	result = __intel_gt_unset_wedged(gt);
	mutex_unlock(&gt->reset.mutex);

	return result;
}

static int do_reset(struct intel_gt *gt, intel_engine_mask_t stalled_mask)
{
	int err, i;

	gt_revoke(gt);

	err = __intel_gt_reset(gt, ALL_ENGINES);
	for (i = 0; err && i < RESET_MAX_RETRIES; i++) {
		msleep(10 * (i + 1));
		err = __intel_gt_reset(gt, ALL_ENGINES);
	}
	if (err)
		return err;

	return gt_reset(gt, stalled_mask);
}

static int resume(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int ret;

	for_each_engine(engine, gt->i915, id) {
		ret = engine->resume(engine);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * intel_gt_reset - reset chip after a hang
 * @gt: #intel_gt to reset
 * @stalled_mask: mask of the stalled engines with the guilty requests
 * @reason: user error message for why we are resetting
 *
 * Reset the chip.  Useful if a hang is detected. Marks the device as wedged
 * on failure.
 *
 * Procedure is fairly simple:
 *   - reset the chip using the reset reg
 *   - re-init context state
 *   - re-init hardware status page
 *   - re-init ring buffer
 *   - re-init interrupt state
 *   - re-init display
 */
void intel_gt_reset(struct intel_gt *gt,
		    intel_engine_mask_t stalled_mask,
		    const char *reason)
{
	intel_engine_mask_t awake;
	int ret;

	GEM_TRACE("flags=%lx\n", gt->reset.flags);

	might_sleep();
	GEM_BUG_ON(!test_bit(I915_RESET_BACKOFF, &gt->reset.flags));
	mutex_lock(&gt->reset.mutex);

	/* Clear any previous failed attempts at recovery. Time to try again. */
	if (!__intel_gt_unset_wedged(gt))
		goto unlock;

	if (reason)
		dev_notice(gt->i915->drm.dev,
			   "Resetting chip for %s\n", reason);
	atomic_inc(&gt->i915->gpu_error.reset_count);

	awake = reset_prepare(gt);

	if (!intel_has_gpu_reset(gt->i915)) {
		if (i915_modparams.reset)
			dev_err(gt->i915->drm.dev, "GPU reset not supported\n");
		else
			DRM_DEBUG_DRIVER("GPU reset disabled\n");
		goto error;
	}

	if (INTEL_INFO(gt->i915)->gpu_reset_clobbers_display)
		intel_runtime_pm_disable_interrupts(gt->i915);

	if (do_reset(gt, stalled_mask)) {
		dev_err(gt->i915->drm.dev, "Failed to reset chip\n");
		goto taint;
	}

	if (INTEL_INFO(gt->i915)->gpu_reset_clobbers_display)
		intel_runtime_pm_enable_interrupts(gt->i915);

	intel_overlay_reset(gt->i915);

	/*
	 * Next we need to restore the context, but we don't use those
	 * yet either...
	 *
	 * Ring buffer needs to be re-initialized in the KMS case, or if X
	 * was running at the time of the reset (i.e. we weren't VT
	 * switched away).
	 */
	ret = i915_gem_init_hw(gt->i915);
	if (ret) {
		DRM_ERROR("Failed to initialise HW following reset (%d)\n",
			  ret);
		goto taint;
	}

	ret = resume(gt);
	if (ret)
		goto taint;

	intel_gt_queue_hangcheck(gt);

finish:
	reset_finish(gt, awake);
unlock:
	mutex_unlock(&gt->reset.mutex);
	return;

taint:
	/*
	 * History tells us that if we cannot reset the GPU now, we
	 * never will. This then impacts everything that is run
	 * subsequently. On failing the reset, we mark the driver
	 * as wedged, preventing further execution on the GPU.
	 * We also want to go one step further and add a taint to the
	 * kernel so that any subsequent faults can be traced back to
	 * this failure. This is important for CI, where if the
	 * GPU/driver fails we would like to reboot and restart testing
	 * rather than continue on into oblivion. For everyone else,
	 * the system should still plod along, but they have been warned!
	 */
	add_taint_for_CI(TAINT_WARN);
error:
	__intel_gt_set_wedged(gt);
	goto finish;
}

static inline int intel_gt_reset_engine(struct intel_engine_cs *engine)
{
	return __intel_gt_reset(engine->gt, engine->mask);
}

/**
 * intel_engine_reset - reset GPU engine to recover from a hang
 * @engine: engine to reset
 * @msg: reason for GPU reset; or NULL for no dev_notice()
 *
 * Reset a specific GPU engine. Useful if a hang is detected.
 * Returns zero on successful reset or otherwise an error code.
 *
 * Procedure is:
 *  - identifies the request that caused the hang and it is dropped
 *  - reset engine (which will force the engine to idle)
 *  - re-init/configure engine
 */
int intel_engine_reset(struct intel_engine_cs *engine, const char *msg)
{
	struct intel_gt *gt = engine->gt;
	int ret;

	GEM_TRACE("%s flags=%lx\n", engine->name, gt->reset.flags);
	GEM_BUG_ON(!test_bit(I915_RESET_ENGINE + engine->id, &gt->reset.flags));

	if (!intel_engine_pm_get_if_awake(engine))
		return 0;

	reset_prepare_engine(engine);

	if (msg)
		dev_notice(engine->i915->drm.dev,
			   "Resetting %s for %s\n", engine->name, msg);
	atomic_inc(&engine->i915->gpu_error.reset_engine_count[engine->uabi_class]);

	if (!engine->gt->uc.guc.execbuf_client)
		ret = intel_gt_reset_engine(engine);
	else
		ret = intel_guc_reset_engine(&engine->gt->uc.guc, engine);
	if (ret) {
		/* If we fail here, we expect to fallback to a global reset */
		DRM_DEBUG_DRIVER("%sFailed to reset %s, ret=%d\n",
				 engine->gt->uc.guc.execbuf_client ? "GuC " : "",
				 engine->name, ret);
		goto out;
	}

	/*
	 * The request that caused the hang is stuck on elsp, we know the
	 * active request and can drop it, adjust head to skip the offending
	 * request to resume executing remaining requests in the queue.
	 */
	__intel_engine_reset(engine, true);

	/*
	 * The engine and its registers (and workarounds in case of render)
	 * have been reset to their default values. Follow the init_ring
	 * process to program RING_MODE, HWSP and re-enable submission.
	 */
	ret = engine->resume(engine);

out:
	intel_engine_cancel_stop_cs(engine);
	reset_finish_engine(engine);
	intel_engine_pm_put(engine);
	return ret;
}

static void intel_gt_reset_global(struct intel_gt *gt,
				  u32 engine_mask,
				  const char *reason)
{
	struct kobject *kobj = &gt->i915->drm.primary->kdev->kobj;
	char *error_event[] = { I915_ERROR_UEVENT "=1", NULL };
	char *reset_event[] = { I915_RESET_UEVENT "=1", NULL };
	char *reset_done_event[] = { I915_ERROR_UEVENT "=0", NULL };
	struct intel_wedge_me w;

	kobject_uevent_env(kobj, KOBJ_CHANGE, error_event);

	DRM_DEBUG_DRIVER("resetting chip\n");
	kobject_uevent_env(kobj, KOBJ_CHANGE, reset_event);

	/* Use a watchdog to ensure that our reset completes */
	intel_wedge_on_timeout(&w, gt, 5 * HZ) {
		intel_prepare_reset(gt->i915);

		/* Flush everyone using a resource about to be clobbered */
		synchronize_srcu_expedited(&gt->reset.backoff_srcu);

		intel_gt_reset(gt, engine_mask, reason);

		intel_finish_reset(gt->i915);
	}

	if (!test_bit(I915_WEDGED, &gt->reset.flags))
		kobject_uevent_env(kobj, KOBJ_CHANGE, reset_done_event);
}

/**
 * intel_gt_handle_error - handle a gpu error
 * @gt: the intel_gt
 * @engine_mask: mask representing engines that are hung
 * @flags: control flags
 * @fmt: Error message format string
 *
 * Do some basic checking of register state at error time and
 * dump it to the syslog.  Also call i915_capture_error_state() to make
 * sure we get a record and make it available in debugfs.  Fire a uevent
 * so userspace knows something bad happened (should trigger collection
 * of a ring dump etc.).
 */
void intel_gt_handle_error(struct intel_gt *gt,
			   intel_engine_mask_t engine_mask,
			   unsigned long flags,
			   const char *fmt, ...)
{
	struct intel_engine_cs *engine;
	intel_wakeref_t wakeref;
	intel_engine_mask_t tmp;
	char error_msg[80];
	char *msg = NULL;

	if (fmt) {
		va_list args;

		va_start(args, fmt);
		vscnprintf(error_msg, sizeof(error_msg), fmt, args);
		va_end(args);

		msg = error_msg;
	}

	/*
	 * In most cases it's guaranteed that we get here with an RPM
	 * reference held, for example because there is a pending GPU
	 * request that won't finish until the reset is done. This
	 * isn't the case at least when we get here by doing a
	 * simulated reset via debugfs, so get an RPM reference.
	 */
	wakeref = intel_runtime_pm_get(&gt->i915->runtime_pm);

	engine_mask &= INTEL_INFO(gt->i915)->engine_mask;

	if (flags & I915_ERROR_CAPTURE) {
		i915_capture_error_state(gt->i915, engine_mask, msg);
		intel_gt_clear_error_registers(gt, engine_mask);
	}

	/*
	 * Try engine reset when available. We fall back to full reset if
	 * single reset fails.
	 */
	if (intel_has_reset_engine(gt->i915) && !intel_gt_is_wedged(gt)) {
		for_each_engine_masked(engine, gt->i915, engine_mask, tmp) {
			BUILD_BUG_ON(I915_RESET_MODESET >= I915_RESET_ENGINE);
			if (test_and_set_bit(I915_RESET_ENGINE + engine->id,
					     &gt->reset.flags))
				continue;

			if (intel_engine_reset(engine, msg) == 0)
				engine_mask &= ~engine->mask;

			clear_and_wake_up_bit(I915_RESET_ENGINE + engine->id,
					      &gt->reset.flags);
		}
	}

	if (!engine_mask)
		goto out;

	/* Full reset needs the mutex, stop any other user trying to do so. */
	if (test_and_set_bit(I915_RESET_BACKOFF, &gt->reset.flags)) {
		wait_event(gt->reset.queue,
			   !test_bit(I915_RESET_BACKOFF, &gt->reset.flags));
		goto out; /* piggy-back on the other reset */
	}

	/* Make sure i915_reset_trylock() sees the I915_RESET_BACKOFF */
	synchronize_rcu_expedited();

	/* Prevent any other reset-engine attempt. */
	for_each_engine(engine, gt->i915, tmp) {
		while (test_and_set_bit(I915_RESET_ENGINE + engine->id,
					&gt->reset.flags))
			wait_on_bit(&gt->reset.flags,
				    I915_RESET_ENGINE + engine->id,
				    TASK_UNINTERRUPTIBLE);
	}

	intel_gt_reset_global(gt, engine_mask, msg);

	for_each_engine(engine, gt->i915, tmp)
		clear_bit_unlock(I915_RESET_ENGINE + engine->id,
				 &gt->reset.flags);
	clear_bit_unlock(I915_RESET_BACKOFF, &gt->reset.flags);
	smp_mb__after_atomic();
	wake_up_all(&gt->reset.queue);

out:
	intel_runtime_pm_put(&gt->i915->runtime_pm, wakeref);
}

int intel_gt_reset_trylock(struct intel_gt *gt, int *srcu)
{
	might_lock(&gt->reset.backoff_srcu);
	might_sleep();

	rcu_read_lock();
	while (test_bit(I915_RESET_BACKOFF, &gt->reset.flags)) {
		rcu_read_unlock();

		if (wait_event_interruptible(gt->reset.queue,
					     !test_bit(I915_RESET_BACKOFF,
						       &gt->reset.flags)))
			return -EINTR;

		rcu_read_lock();
	}
	*srcu = srcu_read_lock(&gt->reset.backoff_srcu);
	rcu_read_unlock();

	return 0;
}

void intel_gt_reset_unlock(struct intel_gt *gt, int tag)
__releases(&gt->reset.backoff_srcu)
{
	srcu_read_unlock(&gt->reset.backoff_srcu, tag);
}

int intel_gt_terminally_wedged(struct intel_gt *gt)
{
	might_sleep();

	if (!intel_gt_is_wedged(gt))
		return 0;

	/* Reset still in progress? Maybe we will recover? */
	if (!test_bit(I915_RESET_BACKOFF, &gt->reset.flags))
		return -EIO;

	/* XXX intel_reset_finish() still takes struct_mutex!!! */
	if (mutex_is_locked(&gt->i915->drm.struct_mutex))
		return -EAGAIN;

	if (wait_event_interruptible(gt->reset.queue,
				     !test_bit(I915_RESET_BACKOFF,
					       &gt->reset.flags)))
		return -EINTR;

	return intel_gt_is_wedged(gt) ? -EIO : 0;
}

void intel_gt_init_reset(struct intel_gt *gt)
{
	init_waitqueue_head(&gt->reset.queue);
	mutex_init(&gt->reset.mutex);
	init_srcu_struct(&gt->reset.backoff_srcu);
}

void intel_gt_fini_reset(struct intel_gt *gt)
{
	cleanup_srcu_struct(&gt->reset.backoff_srcu);
}

static void intel_wedge_me(struct work_struct *work)
{
	struct intel_wedge_me *w = container_of(work, typeof(*w), work.work);

	dev_err(w->gt->i915->drm.dev,
		"%s timed out, cancelling all in-flight rendering.\n",
		w->name);
	intel_gt_set_wedged(w->gt);
}

void __intel_init_wedge(struct intel_wedge_me *w,
			struct intel_gt *gt,
			long timeout,
			const char *name)
{
	w->gt = gt;
	w->name = name;

	INIT_DELAYED_WORK_ONSTACK(&w->work, intel_wedge_me);
	schedule_delayed_work(&w->work, timeout);
}

void __intel_fini_wedge(struct intel_wedge_me *w)
{
	cancel_delayed_work_sync(&w->work);
	destroy_delayed_work_on_stack(&w->work);
	w->gt = NULL;
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftest_reset.c"
#endif
