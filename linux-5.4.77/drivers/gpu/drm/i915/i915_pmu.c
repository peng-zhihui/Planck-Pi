/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2017-2018 Intel Corporation
 */

#include <linux/irq.h>
#include <linux/pm_runtime.h>

#include "gt/intel_engine.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_gt_pm.h"

#include "i915_drv.h"
#include "i915_pmu.h"
#include "intel_pm.h"

/* Frequency for the sampling timer for events which need it. */
#define FREQUENCY 200
#define PERIOD max_t(u64, 10000, NSEC_PER_SEC / FREQUENCY)

#define ENGINE_SAMPLE_MASK \
	(BIT(I915_SAMPLE_BUSY) | \
	 BIT(I915_SAMPLE_WAIT) | \
	 BIT(I915_SAMPLE_SEMA))

#define ENGINE_SAMPLE_BITS (1 << I915_PMU_SAMPLE_BITS)

static cpumask_t i915_pmu_cpumask;

static u8 engine_config_sample(u64 config)
{
	return config & I915_PMU_SAMPLE_MASK;
}

static u8 engine_event_sample(struct perf_event *event)
{
	return engine_config_sample(event->attr.config);
}

static u8 engine_event_class(struct perf_event *event)
{
	return (event->attr.config >> I915_PMU_CLASS_SHIFT) & 0xff;
}

static u8 engine_event_instance(struct perf_event *event)
{
	return (event->attr.config >> I915_PMU_SAMPLE_BITS) & 0xff;
}

static bool is_engine_config(u64 config)
{
	return config < __I915_PMU_OTHER(0);
}

static unsigned int config_enabled_bit(u64 config)
{
	if (is_engine_config(config))
		return engine_config_sample(config);
	else
		return ENGINE_SAMPLE_BITS + (config - __I915_PMU_OTHER(0));
}

static u64 config_enabled_mask(u64 config)
{
	return BIT_ULL(config_enabled_bit(config));
}

static bool is_engine_event(struct perf_event *event)
{
	return is_engine_config(event->attr.config);
}

static unsigned int event_enabled_bit(struct perf_event *event)
{
	return config_enabled_bit(event->attr.config);
}

static bool pmu_needs_timer(struct i915_pmu *pmu, bool gpu_active)
{
	struct drm_i915_private *i915 = container_of(pmu, typeof(*i915), pmu);
	u64 enable;

	/*
	 * Only some counters need the sampling timer.
	 *
	 * We start with a bitmask of all currently enabled events.
	 */
	enable = pmu->enable;

	/*
	 * Mask out all the ones which do not need the timer, or in
	 * other words keep all the ones that could need the timer.
	 */
	enable &= config_enabled_mask(I915_PMU_ACTUAL_FREQUENCY) |
		  config_enabled_mask(I915_PMU_REQUESTED_FREQUENCY) |
		  ENGINE_SAMPLE_MASK;

	/*
	 * When the GPU is idle per-engine counters do not need to be
	 * running so clear those bits out.
	 */
	if (!gpu_active)
		enable &= ~ENGINE_SAMPLE_MASK;
	/*
	 * Also there is software busyness tracking available we do not
	 * need the timer for I915_SAMPLE_BUSY counter.
	 */
	else if (i915->caps.scheduler & I915_SCHEDULER_CAP_ENGINE_BUSY_STATS)
		enable &= ~BIT(I915_SAMPLE_BUSY);

	/*
	 * If some bits remain it means we need the sampling timer running.
	 */
	return enable;
}

void i915_pmu_gt_parked(struct drm_i915_private *i915)
{
	struct i915_pmu *pmu = &i915->pmu;

	if (!pmu->base.event_init)
		return;

	spin_lock_irq(&pmu->lock);
	/*
	 * Signal sampling timer to stop if only engine events are enabled and
	 * GPU went idle.
	 */
	pmu->timer_enabled = pmu_needs_timer(pmu, false);
	spin_unlock_irq(&pmu->lock);
}

static void __i915_pmu_maybe_start_timer(struct i915_pmu *pmu)
{
	if (!pmu->timer_enabled && pmu_needs_timer(pmu, true)) {
		pmu->timer_enabled = true;
		pmu->timer_last = ktime_get();
		hrtimer_start_range_ns(&pmu->timer,
				       ns_to_ktime(PERIOD), 0,
				       HRTIMER_MODE_REL_PINNED);
	}
}

void i915_pmu_gt_unparked(struct drm_i915_private *i915)
{
	struct i915_pmu *pmu = &i915->pmu;

	if (!pmu->base.event_init)
		return;

	spin_lock_irq(&pmu->lock);
	/*
	 * Re-enable sampling timer when GPU goes active.
	 */
	__i915_pmu_maybe_start_timer(pmu);
	spin_unlock_irq(&pmu->lock);
}

static void
add_sample(struct i915_pmu_sample *sample, u32 val)
{
	sample->cur += val;
}

static void
engines_sample(struct intel_gt *gt, unsigned int period_ns)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	if ((i915->pmu.enable & ENGINE_SAMPLE_MASK) == 0)
		return;

	for_each_engine(engine, i915, id) {
		struct intel_engine_pmu *pmu = &engine->pmu;
		unsigned long flags;
		bool busy;
		u32 val;

		if (!intel_engine_pm_get_if_awake(engine))
			continue;

		spin_lock_irqsave(&engine->uncore->lock, flags);

		val = ENGINE_READ_FW(engine, RING_CTL);
		if (val == 0) /* powerwell off => engine idle */
			goto skip;

		if (val & RING_WAIT)
			add_sample(&pmu->sample[I915_SAMPLE_WAIT], period_ns);
		if (val & RING_WAIT_SEMAPHORE)
			add_sample(&pmu->sample[I915_SAMPLE_SEMA], period_ns);

		/*
		 * While waiting on a semaphore or event, MI_MODE reports the
		 * ring as idle. However, previously using the seqno, and with
		 * execlists sampling, we account for the ring waiting as the
		 * engine being busy. Therefore, we record the sample as being
		 * busy if either waiting or !idle.
		 */
		busy = val & (RING_WAIT_SEMAPHORE | RING_WAIT);
		if (!busy) {
			val = ENGINE_READ_FW(engine, RING_MI_MODE);
			busy = !(val & MODE_IDLE);
		}
		if (busy)
			add_sample(&pmu->sample[I915_SAMPLE_BUSY], period_ns);

skip:
		spin_unlock_irqrestore(&engine->uncore->lock, flags);
		intel_engine_pm_put(engine);
	}
}

static void
add_sample_mult(struct i915_pmu_sample *sample, u32 val, u32 mul)
{
	sample->cur += mul_u32_u32(val, mul);
}

static void
frequency_sample(struct intel_gt *gt, unsigned int period_ns)
{
	struct drm_i915_private *i915 = gt->i915;
	struct intel_uncore *uncore = gt->uncore;
	struct i915_pmu *pmu = &i915->pmu;

	if (pmu->enable & config_enabled_mask(I915_PMU_ACTUAL_FREQUENCY)) {
		u32 val;

		val = i915->gt_pm.rps.cur_freq;
		if (intel_gt_pm_get_if_awake(gt)) {
			val = intel_uncore_read_notrace(uncore, GEN6_RPSTAT1);
			val = intel_get_cagf(i915, val);
			intel_gt_pm_put(gt);
		}

		add_sample_mult(&pmu->sample[__I915_SAMPLE_FREQ_ACT],
				intel_gpu_freq(i915, val),
				period_ns / 1000);
	}

	if (pmu->enable & config_enabled_mask(I915_PMU_REQUESTED_FREQUENCY)) {
		add_sample_mult(&pmu->sample[__I915_SAMPLE_FREQ_REQ],
				intel_gpu_freq(i915, i915->gt_pm.rps.cur_freq),
				period_ns / 1000);
	}
}

static enum hrtimer_restart i915_sample(struct hrtimer *hrtimer)
{
	struct drm_i915_private *i915 =
		container_of(hrtimer, struct drm_i915_private, pmu.timer);
	struct i915_pmu *pmu = &i915->pmu;
	struct intel_gt *gt = &i915->gt;
	unsigned int period_ns;
	ktime_t now;

	if (!READ_ONCE(pmu->timer_enabled))
		return HRTIMER_NORESTART;

	now = ktime_get();
	period_ns = ktime_to_ns(ktime_sub(now, pmu->timer_last));
	pmu->timer_last = now;

	/*
	 * Strictly speaking the passed in period may not be 100% accurate for
	 * all internal calculation, since some amount of time can be spent on
	 * grabbing the forcewake. However the potential error from timer call-
	 * back delay greatly dominates this so we keep it simple.
	 */
	engines_sample(gt, period_ns);
	frequency_sample(gt, period_ns);

	hrtimer_forward(hrtimer, now, ns_to_ktime(PERIOD));

	return HRTIMER_RESTART;
}

static u64 count_interrupts(struct drm_i915_private *i915)
{
	/* open-coded kstat_irqs() */
	struct irq_desc *desc = irq_to_desc(i915->drm.pdev->irq);
	u64 sum = 0;
	int cpu;

	if (!desc || !desc->kstat_irqs)
		return 0;

	for_each_possible_cpu(cpu)
		sum += *per_cpu_ptr(desc->kstat_irqs, cpu);

	return sum;
}

static void engine_event_destroy(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	struct intel_engine_cs *engine;

	engine = intel_engine_lookup_user(i915,
					  engine_event_class(event),
					  engine_event_instance(event));
	if (WARN_ON_ONCE(!engine))
		return;

	if (engine_event_sample(event) == I915_SAMPLE_BUSY &&
	    intel_engine_supports_stats(engine))
		intel_disable_engine_stats(engine);
}

static void i915_pmu_event_destroy(struct perf_event *event)
{
	WARN_ON(event->parent);

	if (is_engine_event(event))
		engine_event_destroy(event);
}

static int
engine_event_status(struct intel_engine_cs *engine,
		    enum drm_i915_pmu_engine_sample sample)
{
	switch (sample) {
	case I915_SAMPLE_BUSY:
	case I915_SAMPLE_WAIT:
		break;
	case I915_SAMPLE_SEMA:
		if (INTEL_GEN(engine->i915) < 6)
			return -ENODEV;
		break;
	default:
		return -ENOENT;
	}

	return 0;
}

static int
config_status(struct drm_i915_private *i915, u64 config)
{
	switch (config) {
	case I915_PMU_ACTUAL_FREQUENCY:
		if (IS_VALLEYVIEW(i915) || IS_CHERRYVIEW(i915))
			/* Requires a mutex for sampling! */
			return -ENODEV;
		/* Fall-through. */
	case I915_PMU_REQUESTED_FREQUENCY:
		if (INTEL_GEN(i915) < 6)
			return -ENODEV;
		break;
	case I915_PMU_INTERRUPTS:
		break;
	case I915_PMU_RC6_RESIDENCY:
		if (!HAS_RC6(i915))
			return -ENODEV;
		break;
	default:
		return -ENOENT;
	}

	return 0;
}

static int engine_event_init(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	struct intel_engine_cs *engine;
	u8 sample;
	int ret;

	engine = intel_engine_lookup_user(i915, engine_event_class(event),
					  engine_event_instance(event));
	if (!engine)
		return -ENODEV;

	sample = engine_event_sample(event);
	ret = engine_event_status(engine, sample);
	if (ret)
		return ret;

	if (sample == I915_SAMPLE_BUSY && intel_engine_supports_stats(engine))
		ret = intel_enable_engine_stats(engine);

	return ret;
}

static int i915_pmu_event_init(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	int ret;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* unsupported modes and filters */
	if (event->attr.sample_period) /* no sampling */
		return -EINVAL;

	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	if (event->cpu < 0)
		return -EINVAL;

	/* only allow running on one cpu at a time */
	if (!cpumask_test_cpu(event->cpu, &i915_pmu_cpumask))
		return -EINVAL;

	if (is_engine_event(event))
		ret = engine_event_init(event);
	else
		ret = config_status(i915, event->attr.config);
	if (ret)
		return ret;

	if (!event->parent)
		event->destroy = i915_pmu_event_destroy;

	return 0;
}

static u64 __get_rc6(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	u64 val;

	val = intel_rc6_residency_ns(i915,
				     IS_VALLEYVIEW(i915) ?
				     VLV_GT_RENDER_RC6 :
				     GEN6_GT_GFX_RC6);

	if (HAS_RC6p(i915))
		val += intel_rc6_residency_ns(i915, GEN6_GT_GFX_RC6p);

	if (HAS_RC6pp(i915))
		val += intel_rc6_residency_ns(i915, GEN6_GT_GFX_RC6pp);

	return val;
}

static u64 get_rc6(struct intel_gt *gt)
{
#if IS_ENABLED(CONFIG_PM)
	struct drm_i915_private *i915 = gt->i915;
	struct intel_runtime_pm *rpm = &i915->runtime_pm;
	struct i915_pmu *pmu = &i915->pmu;
	intel_wakeref_t wakeref;
	unsigned long flags;
	u64 val;

	wakeref = intel_runtime_pm_get_if_in_use(rpm);
	if (wakeref) {
		val = __get_rc6(gt);
		intel_runtime_pm_put(rpm, wakeref);

		/*
		 * If we are coming back from being runtime suspended we must
		 * be careful not to report a larger value than returned
		 * previously.
		 */

		spin_lock_irqsave(&pmu->lock, flags);

		if (val >= pmu->sample[__I915_SAMPLE_RC6_ESTIMATED].cur) {
			pmu->sample[__I915_SAMPLE_RC6_ESTIMATED].cur = 0;
			pmu->sample[__I915_SAMPLE_RC6].cur = val;
		} else {
			val = pmu->sample[__I915_SAMPLE_RC6_ESTIMATED].cur;
		}

		spin_unlock_irqrestore(&pmu->lock, flags);
	} else {
		struct device *kdev = rpm->kdev;

		/*
		 * We are runtime suspended.
		 *
		 * Report the delta from when the device was suspended to now,
		 * on top of the last known real value, as the approximated RC6
		 * counter value.
		 */
		spin_lock_irqsave(&pmu->lock, flags);

		/*
		 * After the above branch intel_runtime_pm_get_if_in_use failed
		 * to get the runtime PM reference we cannot assume we are in
		 * runtime suspend since we can either: a) race with coming out
		 * of it before we took the power.lock, or b) there are other
		 * states than suspended which can bring us here.
		 *
		 * We need to double-check that we are indeed currently runtime
		 * suspended and if not we cannot do better than report the last
		 * known RC6 value.
		 */
		if (pm_runtime_status_suspended(kdev)) {
			val = pm_runtime_suspended_time(kdev);

			if (!pmu->sample[__I915_SAMPLE_RC6_ESTIMATED].cur)
				pmu->suspended_time_last = val;

			val -= pmu->suspended_time_last;
			val += pmu->sample[__I915_SAMPLE_RC6].cur;

			pmu->sample[__I915_SAMPLE_RC6_ESTIMATED].cur = val;
		} else if (pmu->sample[__I915_SAMPLE_RC6_ESTIMATED].cur) {
			val = pmu->sample[__I915_SAMPLE_RC6_ESTIMATED].cur;
		} else {
			val = pmu->sample[__I915_SAMPLE_RC6].cur;
		}

		spin_unlock_irqrestore(&pmu->lock, flags);
	}

	return val;
#else
	return __get_rc6(gt);
#endif
}

static u64 __i915_pmu_event_read(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	struct i915_pmu *pmu = &i915->pmu;
	u64 val = 0;

	if (is_engine_event(event)) {
		u8 sample = engine_event_sample(event);
		struct intel_engine_cs *engine;

		engine = intel_engine_lookup_user(i915,
						  engine_event_class(event),
						  engine_event_instance(event));

		if (WARN_ON_ONCE(!engine)) {
			/* Do nothing */
		} else if (sample == I915_SAMPLE_BUSY &&
			   intel_engine_supports_stats(engine)) {
			val = ktime_to_ns(intel_engine_get_busy_time(engine));
		} else {
			val = engine->pmu.sample[sample].cur;
		}
	} else {
		switch (event->attr.config) {
		case I915_PMU_ACTUAL_FREQUENCY:
			val =
			   div_u64(pmu->sample[__I915_SAMPLE_FREQ_ACT].cur,
				   USEC_PER_SEC /* to MHz */);
			break;
		case I915_PMU_REQUESTED_FREQUENCY:
			val =
			   div_u64(pmu->sample[__I915_SAMPLE_FREQ_REQ].cur,
				   USEC_PER_SEC /* to MHz */);
			break;
		case I915_PMU_INTERRUPTS:
			val = count_interrupts(i915);
			break;
		case I915_PMU_RC6_RESIDENCY:
			val = get_rc6(&i915->gt);
			break;
		}
	}

	return val;
}

static void i915_pmu_event_read(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 prev, new;

again:
	prev = local64_read(&hwc->prev_count);
	new = __i915_pmu_event_read(event);

	if (local64_cmpxchg(&hwc->prev_count, prev, new) != prev)
		goto again;

	local64_add(new - prev, &event->count);
}

static void i915_pmu_enable(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	unsigned int bit = event_enabled_bit(event);
	struct i915_pmu *pmu = &i915->pmu;
	unsigned long flags;

	spin_lock_irqsave(&pmu->lock, flags);

	/*
	 * Update the bitmask of enabled events and increment
	 * the event reference counter.
	 */
	BUILD_BUG_ON(ARRAY_SIZE(pmu->enable_count) != I915_PMU_MASK_BITS);
	GEM_BUG_ON(bit >= ARRAY_SIZE(pmu->enable_count));
	GEM_BUG_ON(pmu->enable_count[bit] == ~0);
	pmu->enable |= BIT_ULL(bit);
	pmu->enable_count[bit]++;

	/*
	 * Start the sampling timer if needed and not already enabled.
	 */
	__i915_pmu_maybe_start_timer(pmu);

	/*
	 * For per-engine events the bitmask and reference counting
	 * is stored per engine.
	 */
	if (is_engine_event(event)) {
		u8 sample = engine_event_sample(event);
		struct intel_engine_cs *engine;

		engine = intel_engine_lookup_user(i915,
						  engine_event_class(event),
						  engine_event_instance(event));

		BUILD_BUG_ON(ARRAY_SIZE(engine->pmu.enable_count) !=
			     I915_ENGINE_SAMPLE_COUNT);
		BUILD_BUG_ON(ARRAY_SIZE(engine->pmu.sample) !=
			     I915_ENGINE_SAMPLE_COUNT);
		GEM_BUG_ON(sample >= ARRAY_SIZE(engine->pmu.enable_count));
		GEM_BUG_ON(sample >= ARRAY_SIZE(engine->pmu.sample));
		GEM_BUG_ON(engine->pmu.enable_count[sample] == ~0);

		engine->pmu.enable |= BIT(sample);
		engine->pmu.enable_count[sample]++;
	}

	spin_unlock_irqrestore(&pmu->lock, flags);

	/*
	 * Store the current counter value so we can report the correct delta
	 * for all listeners. Even when the event was already enabled and has
	 * an existing non-zero value.
	 */
	local64_set(&event->hw.prev_count, __i915_pmu_event_read(event));
}

static void i915_pmu_disable(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	unsigned int bit = event_enabled_bit(event);
	struct i915_pmu *pmu = &i915->pmu;
	unsigned long flags;

	spin_lock_irqsave(&pmu->lock, flags);

	if (is_engine_event(event)) {
		u8 sample = engine_event_sample(event);
		struct intel_engine_cs *engine;

		engine = intel_engine_lookup_user(i915,
						  engine_event_class(event),
						  engine_event_instance(event));

		GEM_BUG_ON(sample >= ARRAY_SIZE(engine->pmu.enable_count));
		GEM_BUG_ON(sample >= ARRAY_SIZE(engine->pmu.sample));
		GEM_BUG_ON(engine->pmu.enable_count[sample] == 0);

		/*
		 * Decrement the reference count and clear the enabled
		 * bitmask when the last listener on an event goes away.
		 */
		if (--engine->pmu.enable_count[sample] == 0)
			engine->pmu.enable &= ~BIT(sample);
	}

	GEM_BUG_ON(bit >= ARRAY_SIZE(pmu->enable_count));
	GEM_BUG_ON(pmu->enable_count[bit] == 0);
	/*
	 * Decrement the reference count and clear the enabled
	 * bitmask when the last listener on an event goes away.
	 */
	if (--pmu->enable_count[bit] == 0) {
		pmu->enable &= ~BIT_ULL(bit);
		pmu->timer_enabled &= pmu_needs_timer(pmu, true);
	}

	spin_unlock_irqrestore(&pmu->lock, flags);
}

static void i915_pmu_event_start(struct perf_event *event, int flags)
{
	i915_pmu_enable(event);
	event->hw.state = 0;
}

static void i915_pmu_event_stop(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_UPDATE)
		i915_pmu_event_read(event);
	i915_pmu_disable(event);
	event->hw.state = PERF_HES_STOPPED;
}

static int i915_pmu_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		i915_pmu_event_start(event, flags);

	return 0;
}

static void i915_pmu_event_del(struct perf_event *event, int flags)
{
	i915_pmu_event_stop(event, PERF_EF_UPDATE);
}

static int i915_pmu_event_event_idx(struct perf_event *event)
{
	return 0;
}

struct i915_str_attribute {
	struct device_attribute attr;
	const char *str;
};

static ssize_t i915_pmu_format_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct i915_str_attribute *eattr;

	eattr = container_of(attr, struct i915_str_attribute, attr);
	return sprintf(buf, "%s\n", eattr->str);
}

#define I915_PMU_FORMAT_ATTR(_name, _config) \
	(&((struct i915_str_attribute[]) { \
		{ .attr = __ATTR(_name, 0444, i915_pmu_format_show, NULL), \
		  .str = _config, } \
	})[0].attr.attr)

static struct attribute *i915_pmu_format_attrs[] = {
	I915_PMU_FORMAT_ATTR(i915_eventid, "config:0-20"),
	NULL,
};

static const struct attribute_group i915_pmu_format_attr_group = {
	.name = "format",
	.attrs = i915_pmu_format_attrs,
};

struct i915_ext_attribute {
	struct device_attribute attr;
	unsigned long val;
};

static ssize_t i915_pmu_event_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct i915_ext_attribute *eattr;

	eattr = container_of(attr, struct i915_ext_attribute, attr);
	return sprintf(buf, "config=0x%lx\n", eattr->val);
}

static struct attribute_group i915_pmu_events_attr_group = {
	.name = "events",
	/* Patch in attrs at runtime. */
};

static ssize_t
i915_pmu_get_attr_cpumask(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	return cpumap_print_to_pagebuf(true, buf, &i915_pmu_cpumask);
}

static DEVICE_ATTR(cpumask, 0444, i915_pmu_get_attr_cpumask, NULL);

static struct attribute *i915_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group i915_pmu_cpumask_attr_group = {
	.attrs = i915_cpumask_attrs,
};

static const struct attribute_group *i915_pmu_attr_groups[] = {
	&i915_pmu_format_attr_group,
	&i915_pmu_events_attr_group,
	&i915_pmu_cpumask_attr_group,
	NULL
};

#define __event(__config, __name, __unit) \
{ \
	.config = (__config), \
	.name = (__name), \
	.unit = (__unit), \
}

#define __engine_event(__sample, __name) \
{ \
	.sample = (__sample), \
	.name = (__name), \
}

static struct i915_ext_attribute *
add_i915_attr(struct i915_ext_attribute *attr, const char *name, u64 config)
{
	sysfs_attr_init(&attr->attr.attr);
	attr->attr.attr.name = name;
	attr->attr.attr.mode = 0444;
	attr->attr.show = i915_pmu_event_show;
	attr->val = config;

	return ++attr;
}

static struct perf_pmu_events_attr *
add_pmu_attr(struct perf_pmu_events_attr *attr, const char *name,
	     const char *str)
{
	sysfs_attr_init(&attr->attr.attr);
	attr->attr.attr.name = name;
	attr->attr.attr.mode = 0444;
	attr->attr.show = perf_event_sysfs_show;
	attr->event_str = str;

	return ++attr;
}

static struct attribute **
create_event_attributes(struct i915_pmu *pmu)
{
	struct drm_i915_private *i915 = container_of(pmu, typeof(*i915), pmu);
	static const struct {
		u64 config;
		const char *name;
		const char *unit;
	} events[] = {
		__event(I915_PMU_ACTUAL_FREQUENCY, "actual-frequency", "M"),
		__event(I915_PMU_REQUESTED_FREQUENCY, "requested-frequency", "M"),
		__event(I915_PMU_INTERRUPTS, "interrupts", NULL),
		__event(I915_PMU_RC6_RESIDENCY, "rc6-residency", "ns"),
	};
	static const struct {
		enum drm_i915_pmu_engine_sample sample;
		char *name;
	} engine_events[] = {
		__engine_event(I915_SAMPLE_BUSY, "busy"),
		__engine_event(I915_SAMPLE_SEMA, "sema"),
		__engine_event(I915_SAMPLE_WAIT, "wait"),
	};
	unsigned int count = 0;
	struct perf_pmu_events_attr *pmu_attr = NULL, *pmu_iter;
	struct i915_ext_attribute *i915_attr = NULL, *i915_iter;
	struct attribute **attr = NULL, **attr_iter;
	struct intel_engine_cs *engine;
	unsigned int i;

	/* Count how many counters we will be exposing. */
	for (i = 0; i < ARRAY_SIZE(events); i++) {
		if (!config_status(i915, events[i].config))
			count++;
	}

	for_each_uabi_engine(engine, i915) {
		for (i = 0; i < ARRAY_SIZE(engine_events); i++) {
			if (!engine_event_status(engine,
						 engine_events[i].sample))
				count++;
		}
	}

	/* Allocate attribute objects and table. */
	i915_attr = kcalloc(count, sizeof(*i915_attr), GFP_KERNEL);
	if (!i915_attr)
		goto err_alloc;

	pmu_attr = kcalloc(count, sizeof(*pmu_attr), GFP_KERNEL);
	if (!pmu_attr)
		goto err_alloc;

	/* Max one pointer of each attribute type plus a termination entry. */
	attr = kcalloc(count * 2 + 1, sizeof(*attr), GFP_KERNEL);
	if (!attr)
		goto err_alloc;

	i915_iter = i915_attr;
	pmu_iter = pmu_attr;
	attr_iter = attr;

	/* Initialize supported non-engine counters. */
	for (i = 0; i < ARRAY_SIZE(events); i++) {
		char *str;

		if (config_status(i915, events[i].config))
			continue;

		str = kstrdup(events[i].name, GFP_KERNEL);
		if (!str)
			goto err;

		*attr_iter++ = &i915_iter->attr.attr;
		i915_iter = add_i915_attr(i915_iter, str, events[i].config);

		if (events[i].unit) {
			str = kasprintf(GFP_KERNEL, "%s.unit", events[i].name);
			if (!str)
				goto err;

			*attr_iter++ = &pmu_iter->attr.attr;
			pmu_iter = add_pmu_attr(pmu_iter, str, events[i].unit);
		}
	}

	/* Initialize supported engine counters. */
	for_each_uabi_engine(engine, i915) {
		for (i = 0; i < ARRAY_SIZE(engine_events); i++) {
			char *str;

			if (engine_event_status(engine,
						engine_events[i].sample))
				continue;

			str = kasprintf(GFP_KERNEL, "%s-%s",
					engine->name, engine_events[i].name);
			if (!str)
				goto err;

			*attr_iter++ = &i915_iter->attr.attr;
			i915_iter =
				add_i915_attr(i915_iter, str,
					      __I915_PMU_ENGINE(engine->uabi_class,
								engine->uabi_instance,
								engine_events[i].sample));

			str = kasprintf(GFP_KERNEL, "%s-%s.unit",
					engine->name, engine_events[i].name);
			if (!str)
				goto err;

			*attr_iter++ = &pmu_iter->attr.attr;
			pmu_iter = add_pmu_attr(pmu_iter, str, "ns");
		}
	}

	pmu->i915_attr = i915_attr;
	pmu->pmu_attr = pmu_attr;

	return attr;

err:;
	for (attr_iter = attr; *attr_iter; attr_iter++)
		kfree((*attr_iter)->name);

err_alloc:
	kfree(attr);
	kfree(i915_attr);
	kfree(pmu_attr);

	return NULL;
}

static void free_event_attributes(struct i915_pmu *pmu)
{
	struct attribute **attr_iter = i915_pmu_events_attr_group.attrs;

	for (; *attr_iter; attr_iter++)
		kfree((*attr_iter)->name);

	kfree(i915_pmu_events_attr_group.attrs);
	kfree(pmu->i915_attr);
	kfree(pmu->pmu_attr);

	i915_pmu_events_attr_group.attrs = NULL;
	pmu->i915_attr = NULL;
	pmu->pmu_attr = NULL;
}

static int i915_pmu_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct i915_pmu *pmu = hlist_entry_safe(node, typeof(*pmu), node);

	GEM_BUG_ON(!pmu->base.event_init);

	/* Select the first online CPU as a designated reader. */
	if (!cpumask_weight(&i915_pmu_cpumask))
		cpumask_set_cpu(cpu, &i915_pmu_cpumask);

	return 0;
}

static int i915_pmu_cpu_offline(unsigned int cpu, struct hlist_node *node)
{
	struct i915_pmu *pmu = hlist_entry_safe(node, typeof(*pmu), node);
	unsigned int target;

	GEM_BUG_ON(!pmu->base.event_init);

	if (cpumask_test_and_clear_cpu(cpu, &i915_pmu_cpumask)) {
		target = cpumask_any_but(topology_sibling_cpumask(cpu), cpu);
		/* Migrate events if there is a valid target */
		if (target < nr_cpu_ids) {
			cpumask_set_cpu(target, &i915_pmu_cpumask);
			perf_pmu_migrate_context(&pmu->base, cpu, target);
		}
	}

	return 0;
}

static enum cpuhp_state cpuhp_slot = CPUHP_INVALID;

static int i915_pmu_register_cpuhp_state(struct i915_pmu *pmu)
{
	enum cpuhp_state slot;
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
				      "perf/x86/intel/i915:online",
				      i915_pmu_cpu_online,
				      i915_pmu_cpu_offline);
	if (ret < 0)
		return ret;

	slot = ret;
	ret = cpuhp_state_add_instance(slot, &pmu->node);
	if (ret) {
		cpuhp_remove_multi_state(slot);
		return ret;
	}

	cpuhp_slot = slot;
	return 0;
}

static void i915_pmu_unregister_cpuhp_state(struct i915_pmu *pmu)
{
	WARN_ON(cpuhp_slot == CPUHP_INVALID);
	WARN_ON(cpuhp_state_remove_instance(cpuhp_slot, &pmu->node));
	cpuhp_remove_multi_state(cpuhp_slot);
}

void i915_pmu_register(struct drm_i915_private *i915)
{
	struct i915_pmu *pmu = &i915->pmu;
	int ret;

	if (INTEL_GEN(i915) <= 2) {
		dev_info(i915->drm.dev, "PMU not supported for this GPU.");
		return;
	}

	i915_pmu_events_attr_group.attrs = create_event_attributes(pmu);
	if (!i915_pmu_events_attr_group.attrs) {
		ret = -ENOMEM;
		goto err;
	}

	pmu->base.attr_groups	= i915_pmu_attr_groups;
	pmu->base.task_ctx_nr	= perf_invalid_context;
	pmu->base.event_init	= i915_pmu_event_init;
	pmu->base.add		= i915_pmu_event_add;
	pmu->base.del		= i915_pmu_event_del;
	pmu->base.start		= i915_pmu_event_start;
	pmu->base.stop		= i915_pmu_event_stop;
	pmu->base.read		= i915_pmu_event_read;
	pmu->base.event_idx	= i915_pmu_event_event_idx;

	spin_lock_init(&pmu->lock);
	hrtimer_init(&pmu->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pmu->timer.function = i915_sample;

	ret = perf_pmu_register(&pmu->base, "i915", -1);
	if (ret)
		goto err;

	ret = i915_pmu_register_cpuhp_state(pmu);
	if (ret)
		goto err_unreg;

	return;

err_unreg:
	perf_pmu_unregister(&pmu->base);
err:
	pmu->base.event_init = NULL;
	free_event_attributes(pmu);
	DRM_NOTE("Failed to register PMU! (err=%d)\n", ret);
}

void i915_pmu_unregister(struct drm_i915_private *i915)
{
	struct i915_pmu *pmu = &i915->pmu;

	if (!pmu->base.event_init)
		return;

	WARN_ON(pmu->enable);

	hrtimer_cancel(&pmu->timer);

	i915_pmu_unregister_cpuhp_state(pmu);

	perf_pmu_unregister(&pmu->base);
	pmu->base.event_init = NULL;
	free_event_attributes(pmu);
}
