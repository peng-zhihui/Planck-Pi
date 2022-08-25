/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef INTEL_WAKEREF_H
#define INTEL_WAKEREF_H

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/stackdepot.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_DRM_I915_DEBUG)
#define INTEL_WAKEREF_BUG_ON(expr) BUG_ON(expr)
#else
#define INTEL_WAKEREF_BUG_ON(expr) BUILD_BUG_ON_INVALID(expr)
#endif

struct intel_runtime_pm;
struct intel_wakeref;

typedef depot_stack_handle_t intel_wakeref_t;

struct intel_wakeref_ops {
	int (*get)(struct intel_wakeref *wf);
	int (*put)(struct intel_wakeref *wf);

	unsigned long flags;
#define INTEL_WAKEREF_PUT_ASYNC BIT(0)
};

struct intel_wakeref {
	atomic_t count;
	struct mutex mutex;

	intel_wakeref_t wakeref;

	struct intel_runtime_pm *rpm;
	const struct intel_wakeref_ops *ops;

	struct work_struct work;
};

void __intel_wakeref_init(struct intel_wakeref *wf,
			  struct intel_runtime_pm *rpm,
			  const struct intel_wakeref_ops *ops,
			  struct lock_class_key *key);
#define intel_wakeref_init(wf, rpm, ops) do {				\
	static struct lock_class_key __key;				\
									\
	__intel_wakeref_init((wf), (rpm), (ops), &__key);		\
} while (0)

int __intel_wakeref_get_first(struct intel_wakeref *wf);
void __intel_wakeref_put_last(struct intel_wakeref *wf);

/**
 * intel_wakeref_get: Acquire the wakeref
 * @i915: the drm_i915_private device
 * @wf: the wakeref
 * @fn: callback for acquired the wakeref, called only on first acquire.
 *
 * Acquire a hold on the wakeref. The first user to do so, will acquire
 * the runtime pm wakeref and then call the @fn underneath the wakeref
 * mutex.
 *
 * Note that @fn is allowed to fail, in which case the runtime-pm wakeref
 * will be released and the acquisition unwound, and an error reported.
 *
 * Returns: 0 if the wakeref was acquired successfully, or a negative error
 * code otherwise.
 */
static inline int
intel_wakeref_get(struct intel_wakeref *wf)
{
	if (unlikely(!atomic_inc_not_zero(&wf->count)))
		return __intel_wakeref_get_first(wf);

	return 0;
}

/**
 * intel_wakeref_get_if_in_use: Acquire the wakeref
 * @wf: the wakeref
 *
 * Acquire a hold on the wakeref, but only if the wakeref is already
 * active.
 *
 * Returns: true if the wakeref was acquired, false otherwise.
 */
static inline bool
intel_wakeref_get_if_active(struct intel_wakeref *wf)
{
	return atomic_inc_not_zero(&wf->count);
}

/**
 * intel_wakeref_put: Release the wakeref
 * @i915: the drm_i915_private device
 * @wf: the wakeref
 * @fn: callback for releasing the wakeref, called only on final release.
 *
 * Release our hold on the wakeref. When there are no more users,
 * the runtime pm wakeref will be released after the @fn callback is called
 * underneath the wakeref mutex.
 *
 * Note that @fn is allowed to fail, in which case the runtime-pm wakeref
 * is retained and an error reported.
 *
 * Returns: 0 if the wakeref was released successfully, or a negative error
 * code otherwise.
 */
static inline void
intel_wakeref_put(struct intel_wakeref *wf)
{
	INTEL_WAKEREF_BUG_ON(atomic_read(&wf->count) <= 0);
	if (unlikely(!atomic_add_unless(&wf->count, -1, 1)))
		__intel_wakeref_put_last(wf);
}

/**
 * intel_wakeref_lock: Lock the wakeref (mutex)
 * @wf: the wakeref
 *
 * Locks the wakeref to prevent it being acquired or released. New users
 * can still adjust the counter, but the wakeref itself (and callback)
 * cannot be acquired or released.
 */
static inline void
intel_wakeref_lock(struct intel_wakeref *wf)
	__acquires(wf->mutex)
{
	mutex_lock(&wf->mutex);
}

/**
 * intel_wakeref_unlock: Unlock the wakeref
 * @wf: the wakeref
 *
 * Releases a previously acquired intel_wakeref_lock().
 */
static inline void
intel_wakeref_unlock(struct intel_wakeref *wf)
	__releases(wf->mutex)
{
	mutex_unlock(&wf->mutex);
}

/**
 * intel_wakeref_is_active: Query whether the wakeref is currently held
 * @wf: the wakeref
 *
 * Returns: true if the wakeref is currently held.
 */
static inline bool
intel_wakeref_is_active(const struct intel_wakeref *wf)
{
	return READ_ONCE(wf->wakeref);
}

/**
 * __intel_wakeref_defer_park: Defer the current park callback
 * @wf: the wakeref
 */
static inline void
__intel_wakeref_defer_park(struct intel_wakeref *wf)
{
	INTEL_WAKEREF_BUG_ON(atomic_read(&wf->count));
	atomic_set_release(&wf->count, 1);
}

/**
 * intel_wakeref_wait_for_idle: Wait until the wakeref is idle
 * @wf: the wakeref
 *
 * Wait for the earlier asynchronous release of the wakeref. Note
 * this will wait for any third party as well, so make sure you only wait
 * when you have control over the wakeref and trust no one else is acquiring
 * it.
 *
 * Return: 0 on success, error code if killed.
 */
int intel_wakeref_wait_for_idle(struct intel_wakeref *wf);

struct intel_wakeref_auto {
	struct intel_runtime_pm *rpm;
	struct timer_list timer;
	intel_wakeref_t wakeref;
	spinlock_t lock;
	refcount_t count;
};

/**
 * intel_wakeref_auto: Delay the runtime-pm autosuspend
 * @wf: the wakeref
 * @timeout: relative timeout in jiffies
 *
 * The runtime-pm core uses a suspend delay after the last wakeref
 * is released before triggering runtime suspend of the device. That
 * delay is configurable via sysfs with little regard to the device
 * characteristics. Instead, we want to tune the autosuspend based on our
 * HW knowledge. intel_wakeref_auto() delays the sleep by the supplied
 * timeout.
 *
 * Pass @timeout = 0 to cancel a previous autosuspend by executing the
 * suspend immediately.
 */
void intel_wakeref_auto(struct intel_wakeref_auto *wf, unsigned long timeout);

void intel_wakeref_auto_init(struct intel_wakeref_auto *wf,
			     struct intel_runtime_pm *rpm);
void intel_wakeref_auto_fini(struct intel_wakeref_auto *wf);

#endif /* INTEL_WAKEREF_H */
