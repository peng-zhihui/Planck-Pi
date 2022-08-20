/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#include "gem/i915_gem_context.h"
#include "gt/intel_gt.h"

#include "i915_drv.h"
#include "i915_selftest.h"

#include "igt_flush_test.h"

int igt_flush_test(struct drm_i915_private *i915, unsigned int flags)
{
	int ret = intel_gt_is_wedged(&i915->gt) ? -EIO : 0;
	int repeat = !!(flags & I915_WAIT_LOCKED);

	cond_resched();

	do {
		if (i915_gem_wait_for_idle(i915, flags, HZ / 5) == -ETIME) {
			pr_err("%pS timed out, cancelling all further testing.\n",
			       __builtin_return_address(0));

			GEM_TRACE("%pS timed out.\n",
				  __builtin_return_address(0));
			GEM_TRACE_DUMP();

			intel_gt_set_wedged(&i915->gt);
			repeat = 0;
			ret = -EIO;
		}

		/* Ensure we also flush after wedging. */
		if (flags & I915_WAIT_LOCKED)
			i915_retire_requests(i915);
	} while (repeat--);

	return ret;
}
