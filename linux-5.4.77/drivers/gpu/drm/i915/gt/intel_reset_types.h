/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_RESET_TYPES_H_
#define __INTEL_RESET_TYPES_H_

#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/srcu.h>

struct intel_reset {
	/**
	 * flags: Control various stages of the GPU reset
	 *
	 * #I915_RESET_BACKOFF - When we start a global reset, we need to
	 * serialise with any other users attempting to do the same, and
	 * any global resources that may be clobber by the reset (such as
	 * FENCE registers).
	 *
	 * #I915_RESET_ENGINE[num_engines] - Since the driver doesn't need to
	 * acquire the struct_mutex to reset an engine, we need an explicit
	 * flag to prevent two concurrent reset attempts in the same engine.
	 * As the number of engines continues to grow, allocate the flags from
	 * the most significant bits.
	 *
	 * #I915_WEDGED - If reset fails and we can no longer use the GPU,
	 * we set the #I915_WEDGED bit. Prior to command submission, e.g.
	 * i915_request_alloc(), this bit is checked and the sequence
	 * aborted (with -EIO reported to userspace) if set.
	 */
	unsigned long flags;
#define I915_RESET_BACKOFF	0
#define I915_RESET_MODESET	1
#define I915_RESET_ENGINE	2
#define I915_WEDGED		(BITS_PER_LONG - 1)

	struct mutex mutex; /* serialises wedging/unwedging */

	/**
	 * Waitqueue to signal when the reset has completed. Used by clients
	 * that wait for dev_priv->mm.wedged to settle.
	 */
	wait_queue_head_t queue;

	struct srcu_struct backoff_srcu;
};

#endif /* _INTEL_RESET_TYPES_H_ */
