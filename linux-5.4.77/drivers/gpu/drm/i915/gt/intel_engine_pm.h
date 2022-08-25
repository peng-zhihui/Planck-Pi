/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef INTEL_ENGINE_PM_H
#define INTEL_ENGINE_PM_H

#include "intel_engine_types.h"
#include "intel_wakeref.h"

static inline bool
intel_engine_pm_is_awake(const struct intel_engine_cs *engine)
{
	return intel_wakeref_is_active(&engine->wakeref);
}

static inline void intel_engine_pm_get(struct intel_engine_cs *engine)
{
	intel_wakeref_get(&engine->wakeref);
}

static inline bool intel_engine_pm_get_if_awake(struct intel_engine_cs *engine)
{
	return intel_wakeref_get_if_active(&engine->wakeref);
}

static inline void intel_engine_pm_put(struct intel_engine_cs *engine)
{
	intel_wakeref_put(&engine->wakeref);
}

void intel_engine_init__pm(struct intel_engine_cs *engine);

#endif /* INTEL_ENGINE_PM_H */
