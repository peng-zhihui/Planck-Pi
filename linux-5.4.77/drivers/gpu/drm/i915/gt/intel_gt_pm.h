/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef INTEL_GT_PM_H
#define INTEL_GT_PM_H

#include <linux/types.h>

#include "intel_gt_types.h"
#include "intel_wakeref.h"

enum {
	INTEL_GT_UNPARK,
	INTEL_GT_PARK,
};

static inline bool intel_gt_pm_is_awake(const struct intel_gt *gt)
{
	return intel_wakeref_is_active(&gt->wakeref);
}

static inline void intel_gt_pm_get(struct intel_gt *gt)
{
	intel_wakeref_get(&gt->wakeref);
}

static inline bool intel_gt_pm_get_if_awake(struct intel_gt *gt)
{
	return intel_wakeref_get_if_active(&gt->wakeref);
}

static inline void intel_gt_pm_put(struct intel_gt *gt)
{
	intel_wakeref_put(&gt->wakeref);
}

static inline int intel_gt_pm_wait_for_idle(struct intel_gt *gt)
{
	return intel_wakeref_wait_for_idle(&gt->wakeref);
}

void intel_gt_pm_init_early(struct intel_gt *gt);

void intel_gt_sanitize(struct intel_gt *gt, bool force);
int intel_gt_resume(struct intel_gt *gt);
void intel_gt_runtime_suspend(struct intel_gt *gt);
int intel_gt_runtime_resume(struct intel_gt *gt);

#endif /* INTEL_GT_PM_H */
