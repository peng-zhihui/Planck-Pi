/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "gem/i915_gem_pm.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"

#include "i915_drv.h"
#include "i915_globals.h"

static void call_idle_barriers(struct intel_engine_cs *engine)
{
	struct llist_node *node, *next;

	llist_for_each_safe(node, next, llist_del_all(&engine->barrier_tasks)) {
		struct i915_active_request *active =
			container_of((struct list_head *)node,
				     typeof(*active), link);

		INIT_LIST_HEAD(&active->link);
		RCU_INIT_POINTER(active->request, NULL);

		active->retire(active, NULL);
	}
}

static void i915_gem_park(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	lockdep_assert_held(&i915->drm.struct_mutex);

	for_each_engine(engine, i915, id)
		call_idle_barriers(engine); /* cleanup after wedging */

	i915_vma_parked(i915);

	i915_globals_park();
}

static void idle_work_handler(struct work_struct *work)
{
	struct drm_i915_private *i915 =
		container_of(work, typeof(*i915), gem.idle_work);
	bool park;

	cancel_delayed_work_sync(&i915->gem.retire_work);
	mutex_lock(&i915->drm.struct_mutex);

	intel_wakeref_lock(&i915->gt.wakeref);
	park = (!intel_wakeref_is_active(&i915->gt.wakeref) &&
		!work_pending(work));
	intel_wakeref_unlock(&i915->gt.wakeref);
	if (park)
		i915_gem_park(i915);
	else
		queue_delayed_work(i915->wq,
				   &i915->gem.retire_work,
				   round_jiffies_up_relative(HZ));

	mutex_unlock(&i915->drm.struct_mutex);
}

static void retire_work_handler(struct work_struct *work)
{
	struct drm_i915_private *i915 =
		container_of(work, typeof(*i915), gem.retire_work.work);

	/* Come back later if the device is busy... */
	if (mutex_trylock(&i915->drm.struct_mutex)) {
		i915_retire_requests(i915);
		mutex_unlock(&i915->drm.struct_mutex);
	}

	queue_delayed_work(i915->wq,
			   &i915->gem.retire_work,
			   round_jiffies_up_relative(HZ));
}

static int pm_notifier(struct notifier_block *nb,
		       unsigned long action,
		       void *data)
{
	struct drm_i915_private *i915 =
		container_of(nb, typeof(*i915), gem.pm_notifier);

	switch (action) {
	case INTEL_GT_UNPARK:
		i915_globals_unpark();
		queue_delayed_work(i915->wq,
				   &i915->gem.retire_work,
				   round_jiffies_up_relative(HZ));
		break;

	case INTEL_GT_PARK:
		queue_work(i915->wq, &i915->gem.idle_work);
		break;
	}

	return NOTIFY_OK;
}

static bool switch_to_kernel_context_sync(struct intel_gt *gt)
{
	bool result = !intel_gt_is_wedged(gt);

	do {
		if (i915_gem_wait_for_idle(gt->i915,
					   I915_WAIT_LOCKED |
					   I915_WAIT_FOR_IDLE_BOOST,
					   I915_GEM_IDLE_TIMEOUT) == -ETIME) {
			/* XXX hide warning from gem_eio */
			if (i915_modparams.reset) {
				dev_err(gt->i915->drm.dev,
					"Failed to idle engines, declaring wedged!\n");
				GEM_TRACE_DUMP();
			}

			/*
			 * Forcibly cancel outstanding work and leave
			 * the gpu quiet.
			 */
			intel_gt_set_wedged(gt);
			result = false;
		}
	} while (i915_retire_requests(gt->i915) && result);

	if (intel_gt_pm_wait_for_idle(gt))
		result = false;

	return result;
}

bool i915_gem_load_power_context(struct drm_i915_private *i915)
{
	return switch_to_kernel_context_sync(&i915->gt);
}

void i915_gem_suspend(struct drm_i915_private *i915)
{
	GEM_TRACE("\n");

	intel_wakeref_auto(&i915->ggtt.userfault_wakeref, 0);
	flush_workqueue(i915->wq);

	mutex_lock(&i915->drm.struct_mutex);

	/*
	 * We have to flush all the executing contexts to main memory so
	 * that they can saved in the hibernation image. To ensure the last
	 * context image is coherent, we have to switch away from it. That
	 * leaves the i915->kernel_context still active when
	 * we actually suspend, and its image in memory may not match the GPU
	 * state. Fortunately, the kernel_context is disposable and we do
	 * not rely on its state.
	 */
	switch_to_kernel_context_sync(&i915->gt);

	mutex_unlock(&i915->drm.struct_mutex);

	cancel_delayed_work_sync(&i915->gt.hangcheck.work);

	i915_gem_drain_freed_objects(i915);

	intel_uc_suspend(&i915->gt.uc);
}

static struct drm_i915_gem_object *first_mm_object(struct list_head *list)
{
	return list_first_entry_or_null(list,
					struct drm_i915_gem_object,
					mm.link);
}

void i915_gem_suspend_late(struct drm_i915_private *i915)
{
	struct drm_i915_gem_object *obj;
	struct list_head *phases[] = {
		&i915->mm.shrink_list,
		&i915->mm.purge_list,
		NULL
	}, **phase;
	unsigned long flags;

	/*
	 * Neither the BIOS, ourselves or any other kernel
	 * expects the system to be in execlists mode on startup,
	 * so we need to reset the GPU back to legacy mode. And the only
	 * known way to disable logical contexts is through a GPU reset.
	 *
	 * So in order to leave the system in a known default configuration,
	 * always reset the GPU upon unload and suspend. Afterwards we then
	 * clean up the GEM state tracking, flushing off the requests and
	 * leaving the system in a known idle state.
	 *
	 * Note that is of the upmost importance that the GPU is idle and
	 * all stray writes are flushed *before* we dismantle the backing
	 * storage for the pinned objects.
	 *
	 * However, since we are uncertain that resetting the GPU on older
	 * machines is a good idea, we don't - just in case it leaves the
	 * machine in an unusable condition.
	 */

	spin_lock_irqsave(&i915->mm.obj_lock, flags);
	for (phase = phases; *phase; phase++) {
		LIST_HEAD(keep);

		while ((obj = first_mm_object(*phase))) {
			list_move_tail(&obj->mm.link, &keep);

			/* Beware the background _i915_gem_free_objects */
			if (!kref_get_unless_zero(&obj->base.refcount))
				continue;

			spin_unlock_irqrestore(&i915->mm.obj_lock, flags);

			i915_gem_object_lock(obj);
			WARN_ON(i915_gem_object_set_to_gtt_domain(obj, false));
			i915_gem_object_unlock(obj);
			i915_gem_object_put(obj);

			spin_lock_irqsave(&i915->mm.obj_lock, flags);
		}

		list_splice_tail(&keep, *phase);
	}
	spin_unlock_irqrestore(&i915->mm.obj_lock, flags);

	i915_gem_sanitize(i915);
}

void i915_gem_resume(struct drm_i915_private *i915)
{
	GEM_TRACE("\n");

	mutex_lock(&i915->drm.struct_mutex);
	intel_uncore_forcewake_get(&i915->uncore, FORCEWAKE_ALL);

	if (i915_gem_init_hw(i915))
		goto err_wedged;

	/*
	 * As we didn't flush the kernel context before suspend, we cannot
	 * guarantee that the context image is complete. So let's just reset
	 * it and start again.
	 */
	if (intel_gt_resume(&i915->gt))
		goto err_wedged;

	intel_uc_resume(&i915->gt.uc);

	/* Always reload a context for powersaving. */
	if (!i915_gem_load_power_context(i915))
		goto err_wedged;

out_unlock:
	intel_uncore_forcewake_put(&i915->uncore, FORCEWAKE_ALL);
	mutex_unlock(&i915->drm.struct_mutex);
	return;

err_wedged:
	if (!intel_gt_is_wedged(&i915->gt)) {
		dev_err(i915->drm.dev,
			"Failed to re-initialize GPU, declaring it wedged!\n");
		intel_gt_set_wedged(&i915->gt);
	}
	goto out_unlock;
}

void i915_gem_init__pm(struct drm_i915_private *i915)
{
	INIT_WORK(&i915->gem.idle_work, idle_work_handler);
	INIT_DELAYED_WORK(&i915->gem.retire_work, retire_work_handler);

	i915->gem.pm_notifier.notifier_call = pm_notifier;
	blocking_notifier_chain_register(&i915->gt.pm_notifications,
					 &i915->gem.pm_notifier);
}
