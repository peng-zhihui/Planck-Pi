// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/drivers/thermal/cpu_cooling.c
 *
 *  Copyright (C) 2012	Samsung Electronics Co., Ltd(http://www.samsung.com)
 *
 *  Copyright (C) 2012-2018 Linaro Limited.
 *
 *  Authors:	Amit Daniel <amit.kachhap@linaro.org>
 *		Viresh Kumar <viresh.kumar@linaro.org>
 *
 */
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpu_cooling.h>

#include <trace/events/thermal.h>

/*
 * Cooling state <-> CPUFreq frequency
 *
 * Cooling states are translated to frequencies throughout this driver and this
 * is the relation between them.
 *
 * Highest cooling state corresponds to lowest possible frequency.
 *
 * i.e.
 *	level 0 --> 1st Max Freq
 *	level 1 --> 2nd Max Freq
 *	...
 */

/**
 * struct freq_table - frequency table along with power entries
 * @frequency:	frequency in KHz
 * @power:	power in mW
 *
 * This structure is built when the cooling device registers and helps
 * in translating frequency to power and vice versa.
 */
struct freq_table {
	u32 frequency;
	u32 power;
};

/**
 * struct time_in_idle - Idle time stats
 * @time: previous reading of the absolute time that this cpu was idle
 * @timestamp: wall time of the last invocation of get_cpu_idle_time_us()
 */
struct time_in_idle {
	u64 time;
	u64 timestamp;
};

/**
 * struct cpufreq_cooling_device - data for cooling device with cpufreq
 * @id: unique integer value corresponding to each cpufreq_cooling_device
 *	registered.
 * @last_load: load measured by the latest call to cpufreq_get_requested_power()
 * @cpufreq_state: integer value representing the current state of cpufreq
 *	cooling	devices.
 * @max_level: maximum cooling level. One less than total number of valid
 *	cpufreq frequencies.
 * @freq_table: Freq table in descending order of frequencies
 * @cdev: thermal_cooling_device pointer to keep track of the
 *	registered cooling device.
 * @policy: cpufreq policy.
 * @node: list_head to link all cpufreq_cooling_device together.
 * @idle_time: idle time stats
 *
 * This structure is required for keeping information of each registered
 * cpufreq_cooling_device.
 */
struct cpufreq_cooling_device {
	int id;
	u32 last_load;
	unsigned int cpufreq_state;
	unsigned int max_level;
	struct freq_table *freq_table;	/* In descending order */
	struct cpufreq_policy *policy;
	struct list_head node;
	struct time_in_idle *idle_time;
	struct freq_qos_request qos_req;
};

static DEFINE_IDA(cpufreq_ida);
static DEFINE_MUTEX(cooling_list_lock);
static LIST_HEAD(cpufreq_cdev_list);

/* Below code defines functions to be used for cpufreq as cooling device */

/**
 * get_level: Find the level for a particular frequency
 * @cpufreq_cdev: cpufreq_cdev for which the property is required
 * @freq: Frequency
 *
 * Return: level corresponding to the frequency.
 */
static unsigned long get_level(struct cpufreq_cooling_device *cpufreq_cdev,
			       unsigned int freq)
{
	struct freq_table *freq_table = cpufreq_cdev->freq_table;
	unsigned long level;

	for (level = 1; level <= cpufreq_cdev->max_level; level++)
		if (freq > freq_table[level].frequency)
			break;

	return level - 1;
}

/**
 * update_freq_table() - Update the freq table with power numbers
 * @cpufreq_cdev:	the cpufreq cooling device in which to update the table
 * @capacitance: dynamic power coefficient for these cpus
 *
 * Update the freq table with power numbers.  This table will be used in
 * cpu_power_to_freq() and cpu_freq_to_power() to convert between power and
 * frequency efficiently.  Power is stored in mW, frequency in KHz.  The
 * resulting table is in descending order.
 *
 * Return: 0 on success, -EINVAL if there are no OPPs for any CPUs,
 * or -ENOMEM if we run out of memory.
 */
static int update_freq_table(struct cpufreq_cooling_device *cpufreq_cdev,
			     u32 capacitance)
{
	struct freq_table *freq_table = cpufreq_cdev->freq_table;
	struct dev_pm_opp *opp;
	struct device *dev = NULL;
	int num_opps = 0, cpu = cpufreq_cdev->policy->cpu, i;

	dev = get_cpu_device(cpu);
	if (unlikely(!dev)) {
		pr_warn("No cpu device for cpu %d\n", cpu);
		return -ENODEV;
	}

	num_opps = dev_pm_opp_get_opp_count(dev);
	if (num_opps < 0)
		return num_opps;

	/*
	 * The cpufreq table is also built from the OPP table and so the count
	 * should match.
	 */
	if (num_opps != cpufreq_cdev->max_level + 1) {
		dev_warn(dev, "Number of OPPs not matching with max_levels\n");
		return -EINVAL;
	}

	for (i = 0; i <= cpufreq_cdev->max_level; i++) {
		unsigned long freq = freq_table[i].frequency * 1000;
		u32 freq_mhz = freq_table[i].frequency / 1000;
		u64 power;
		u32 voltage_mv;

		/*
		 * Find ceil frequency as 'freq' may be slightly lower than OPP
		 * freq due to truncation while converting to kHz.
		 */
		opp = dev_pm_opp_find_freq_ceil(dev, &freq);
		if (IS_ERR(opp)) {
			dev_err(dev, "failed to get opp for %lu frequency\n",
				freq);
			return -EINVAL;
		}

		voltage_mv = dev_pm_opp_get_voltage(opp) / 1000;
		dev_pm_opp_put(opp);

		/*
		 * Do the multiplication with MHz and millivolt so as
		 * to not overflow.
		 */
		power = (u64)capacitance * freq_mhz * voltage_mv * voltage_mv;
		do_div(power, 1000000000);

		/* power is stored in mW */
		freq_table[i].power = power;
	}

	return 0;
}

static u32 cpu_freq_to_power(struct cpufreq_cooling_device *cpufreq_cdev,
			     u32 freq)
{
	int i;
	struct freq_table *freq_table = cpufreq_cdev->freq_table;

	for (i = 1; i <= cpufreq_cdev->max_level; i++)
		if (freq > freq_table[i].frequency)
			break;

	return freq_table[i - 1].power;
}

static u32 cpu_power_to_freq(struct cpufreq_cooling_device *cpufreq_cdev,
			     u32 power)
{
	int i;
	struct freq_table *freq_table = cpufreq_cdev->freq_table;

	for (i = 0; i < cpufreq_cdev->max_level; i++)
		if (power >= freq_table[i].power)
			break;

	return freq_table[i].frequency;
}

/**
 * get_load() - get load for a cpu since last updated
 * @cpufreq_cdev:	&struct cpufreq_cooling_device for this cpu
 * @cpu:	cpu number
 * @cpu_idx:	index of the cpu in time_in_idle*
 *
 * Return: The average load of cpu @cpu in percentage since this
 * function was last called.
 */
static u32 get_load(struct cpufreq_cooling_device *cpufreq_cdev, int cpu,
		    int cpu_idx)
{
	u32 load;
	u64 now, now_idle, delta_time, delta_idle;
	struct time_in_idle *idle_time = &cpufreq_cdev->idle_time[cpu_idx];

	now_idle = get_cpu_idle_time(cpu, &now, 0);
	delta_idle = now_idle - idle_time->time;
	delta_time = now - idle_time->timestamp;

	if (delta_time <= delta_idle)
		load = 0;
	else
		load = div64_u64(100 * (delta_time - delta_idle), delta_time);

	idle_time->time = now_idle;
	idle_time->timestamp = now;

	return load;
}

/**
 * get_dynamic_power() - calculate the dynamic power
 * @cpufreq_cdev:	&cpufreq_cooling_device for this cdev
 * @freq:	current frequency
 *
 * Return: the dynamic power consumed by the cpus described by
 * @cpufreq_cdev.
 */
static u32 get_dynamic_power(struct cpufreq_cooling_device *cpufreq_cdev,
			     unsigned long freq)
{
	u32 raw_cpu_power;

	raw_cpu_power = cpu_freq_to_power(cpufreq_cdev, freq);
	return (raw_cpu_power * cpufreq_cdev->last_load) / 100;
}

/* cpufreq cooling device callback functions are defined below */

/**
 * cpufreq_get_max_state - callback function to get the max cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the max cooling state.
 *
 * Callback for the thermal cooling device to return the cpufreq
 * max cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int cpufreq_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;

	*state = cpufreq_cdev->max_level;
	return 0;
}

/**
 * cpufreq_get_cur_state - callback function to get the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the current cooling state.
 *
 * Callback for the thermal cooling device to return the cpufreq
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int cpufreq_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;

	*state = cpufreq_cdev->cpufreq_state;

	return 0;
}

/**
 * cpufreq_set_cur_state - callback function to set the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: set this variable to the current cooling state.
 *
 * Callback for the thermal cooling device to change the cpufreq
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int cpufreq_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;

	/* Request state should be less than max_level */
	if (WARN_ON(state > cpufreq_cdev->max_level))
		return -EINVAL;

	/* Check if the old cooling action is same as new cooling action */
	if (cpufreq_cdev->cpufreq_state == state)
		return 0;

	cpufreq_cdev->cpufreq_state = state;

	return freq_qos_update_request(&cpufreq_cdev->qos_req,
				cpufreq_cdev->freq_table[state].frequency);
}

/**
 * cpufreq_get_requested_power() - get the current power
 * @cdev:	&thermal_cooling_device pointer
 * @tz:		a valid thermal zone device pointer
 * @power:	pointer in which to store the resulting power
 *
 * Calculate the current power consumption of the cpus in milliwatts
 * and store it in @power.  This function should actually calculate
 * the requested power, but it's hard to get the frequency that
 * cpufreq would have assigned if there were no thermal limits.
 * Instead, we calculate the current power on the assumption that the
 * immediate future will look like the immediate past.
 *
 * We use the current frequency and the average load since this
 * function was last called.  In reality, there could have been
 * multiple opps since this function was last called and that affects
 * the load calculation.  While it's not perfectly accurate, this
 * simplification is good enough and works.  REVISIT this, as more
 * complex code may be needed if experiments show that it's not
 * accurate enough.
 *
 * Return: 0 on success, -E* if getting the static power failed.
 */
static int cpufreq_get_requested_power(struct thermal_cooling_device *cdev,
				       struct thermal_zone_device *tz,
				       u32 *power)
{
	unsigned long freq;
	int i = 0, cpu;
	u32 total_load = 0;
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;
	struct cpufreq_policy *policy = cpufreq_cdev->policy;
	u32 *load_cpu = NULL;

	freq = cpufreq_quick_get(policy->cpu);

	if (trace_thermal_power_cpu_get_power_enabled()) {
		u32 ncpus = cpumask_weight(policy->related_cpus);

		load_cpu = kcalloc(ncpus, sizeof(*load_cpu), GFP_KERNEL);
	}

	for_each_cpu(cpu, policy->related_cpus) {
		u32 load;

		if (cpu_online(cpu))
			load = get_load(cpufreq_cdev, cpu, i);
		else
			load = 0;

		total_load += load;
		if (load_cpu)
			load_cpu[i] = load;

		i++;
	}

	cpufreq_cdev->last_load = total_load;

	*power = get_dynamic_power(cpufreq_cdev, freq);

	if (load_cpu) {
		trace_thermal_power_cpu_get_power(policy->related_cpus, freq,
						  load_cpu, i, *power);

		kfree(load_cpu);
	}

	return 0;
}

/**
 * cpufreq_state2power() - convert a cpu cdev state to power consumed
 * @cdev:	&thermal_cooling_device pointer
 * @tz:		a valid thermal zone device pointer
 * @state:	cooling device state to be converted
 * @power:	pointer in which to store the resulting power
 *
 * Convert cooling device state @state into power consumption in
 * milliwatts assuming 100% load.  Store the calculated power in
 * @power.
 *
 * Return: 0 on success, -EINVAL if the cooling device state could not
 * be converted into a frequency or other -E* if there was an error
 * when calculating the static power.
 */
static int cpufreq_state2power(struct thermal_cooling_device *cdev,
			       struct thermal_zone_device *tz,
			       unsigned long state, u32 *power)
{
	unsigned int freq, num_cpus;
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;

	/* Request state should be less than max_level */
	if (WARN_ON(state > cpufreq_cdev->max_level))
		return -EINVAL;

	num_cpus = cpumask_weight(cpufreq_cdev->policy->cpus);

	freq = cpufreq_cdev->freq_table[state].frequency;
	*power = cpu_freq_to_power(cpufreq_cdev, freq) * num_cpus;

	return 0;
}

/**
 * cpufreq_power2state() - convert power to a cooling device state
 * @cdev:	&thermal_cooling_device pointer
 * @tz:		a valid thermal zone device pointer
 * @power:	power in milliwatts to be converted
 * @state:	pointer in which to store the resulting state
 *
 * Calculate a cooling device state for the cpus described by @cdev
 * that would allow them to consume at most @power mW and store it in
 * @state.  Note that this calculation depends on external factors
 * such as the cpu load or the current static power.  Calling this
 * function with the same power as input can yield different cooling
 * device states depending on those external factors.
 *
 * Return: 0 on success, -ENODEV if no cpus are online or -EINVAL if
 * the calculated frequency could not be converted to a valid state.
 * The latter should not happen unless the frequencies available to
 * cpufreq have changed since the initialization of the cpu cooling
 * device.
 */
static int cpufreq_power2state(struct thermal_cooling_device *cdev,
			       struct thermal_zone_device *tz, u32 power,
			       unsigned long *state)
{
	unsigned int target_freq;
	u32 last_load, normalised_power;
	struct cpufreq_cooling_device *cpufreq_cdev = cdev->devdata;
	struct cpufreq_policy *policy = cpufreq_cdev->policy;

	last_load = cpufreq_cdev->last_load ?: 1;
	normalised_power = (power * 100) / last_load;
	target_freq = cpu_power_to_freq(cpufreq_cdev, normalised_power);

	*state = get_level(cpufreq_cdev, target_freq);
	trace_thermal_power_cpu_limit(policy->related_cpus, target_freq, *state,
				      power);
	return 0;
}

/* Bind cpufreq callbacks to thermal cooling device ops */

static struct thermal_cooling_device_ops cpufreq_cooling_ops = {
	.get_max_state = cpufreq_get_max_state,
	.get_cur_state = cpufreq_get_cur_state,
	.set_cur_state = cpufreq_set_cur_state,
};

static struct thermal_cooling_device_ops cpufreq_power_cooling_ops = {
	.get_max_state		= cpufreq_get_max_state,
	.get_cur_state		= cpufreq_get_cur_state,
	.set_cur_state		= cpufreq_set_cur_state,
	.get_requested_power	= cpufreq_get_requested_power,
	.state2power		= cpufreq_state2power,
	.power2state		= cpufreq_power2state,
};

static unsigned int find_next_max(struct cpufreq_frequency_table *table,
				  unsigned int prev_max)
{
	struct cpufreq_frequency_table *pos;
	unsigned int max = 0;

	cpufreq_for_each_valid_entry(pos, table) {
		if (pos->frequency > max && pos->frequency < prev_max)
			max = pos->frequency;
	}

	return max;
}

/**
 * __cpufreq_cooling_register - helper function to create cpufreq cooling device
 * @np: a valid struct device_node to the cooling device device tree node
 * @policy: cpufreq policy
 * Normally this should be same as cpufreq policy->related_cpus.
 * @capacitance: dynamic power coefficient for these cpus
 *
 * This interface function registers the cpufreq cooling device with the name
 * "thermal-cpufreq-%x". This api can support multiple instances of cpufreq
 * cooling devices. It also gives the opportunity to link the cooling device
 * with a device tree node, in order to bind it via the thermal DT code.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
static struct thermal_cooling_device *
__cpufreq_cooling_register(struct device_node *np,
			struct cpufreq_policy *policy, u32 capacitance)
{
	struct thermal_cooling_device *cdev;
	struct cpufreq_cooling_device *cpufreq_cdev;
	char dev_name[THERMAL_NAME_LENGTH];
	unsigned int freq, i, num_cpus;
	struct device *dev;
	int ret;
	struct thermal_cooling_device_ops *cooling_ops;

	dev = get_cpu_device(policy->cpu);
	if (unlikely(!dev)) {
		pr_warn("No cpu device for cpu %d\n", policy->cpu);
		return ERR_PTR(-ENODEV);
	}


	if (IS_ERR_OR_NULL(policy)) {
		pr_err("%s: cpufreq policy isn't valid: %p\n", __func__, policy);
		return ERR_PTR(-EINVAL);
	}

	i = cpufreq_table_count_valid_entries(policy);
	if (!i) {
		pr_debug("%s: CPUFreq table not found or has no valid entries\n",
			 __func__);
		return ERR_PTR(-ENODEV);
	}

	cpufreq_cdev = kzalloc(sizeof(*cpufreq_cdev), GFP_KERNEL);
	if (!cpufreq_cdev)
		return ERR_PTR(-ENOMEM);

	cpufreq_cdev->policy = policy;
	num_cpus = cpumask_weight(policy->related_cpus);
	cpufreq_cdev->idle_time = kcalloc(num_cpus,
					 sizeof(*cpufreq_cdev->idle_time),
					 GFP_KERNEL);
	if (!cpufreq_cdev->idle_time) {
		cdev = ERR_PTR(-ENOMEM);
		goto free_cdev;
	}

	/* max_level is an index, not a counter */
	cpufreq_cdev->max_level = i - 1;

	cpufreq_cdev->freq_table = kmalloc_array(i,
					sizeof(*cpufreq_cdev->freq_table),
					GFP_KERNEL);
	if (!cpufreq_cdev->freq_table) {
		cdev = ERR_PTR(-ENOMEM);
		goto free_idle_time;
	}

	ret = ida_simple_get(&cpufreq_ida, 0, 0, GFP_KERNEL);
	if (ret < 0) {
		cdev = ERR_PTR(ret);
		goto free_table;
	}
	cpufreq_cdev->id = ret;

	snprintf(dev_name, sizeof(dev_name), "thermal-cpufreq-%d",
		 cpufreq_cdev->id);

	/* Fill freq-table in descending order of frequencies */
	for (i = 0, freq = -1; i <= cpufreq_cdev->max_level; i++) {
		freq = find_next_max(policy->freq_table, freq);
		cpufreq_cdev->freq_table[i].frequency = freq;

		/* Warn for duplicate entries */
		if (!freq)
			pr_warn("%s: table has duplicate entries\n", __func__);
		else
			pr_debug("%s: freq:%u KHz\n", __func__, freq);
	}

	if (capacitance) {
		ret = update_freq_table(cpufreq_cdev, capacitance);
		if (ret) {
			cdev = ERR_PTR(ret);
			goto remove_ida;
		}

		cooling_ops = &cpufreq_power_cooling_ops;
	} else {
		cooling_ops = &cpufreq_cooling_ops;
	}

	ret = freq_qos_add_request(&policy->constraints,
				   &cpufreq_cdev->qos_req, FREQ_QOS_MAX,
				   cpufreq_cdev->freq_table[0].frequency);
	if (ret < 0) {
		pr_err("%s: Failed to add freq constraint (%d)\n", __func__,
		       ret);
		cdev = ERR_PTR(ret);
		goto remove_ida;
	}

	cdev = thermal_of_cooling_device_register(np, dev_name, cpufreq_cdev,
						  cooling_ops);
	if (IS_ERR(cdev))
		goto remove_qos_req;

	mutex_lock(&cooling_list_lock);
	list_add(&cpufreq_cdev->node, &cpufreq_cdev_list);
	mutex_unlock(&cooling_list_lock);

	return cdev;

remove_qos_req:
	freq_qos_remove_request(&cpufreq_cdev->qos_req);
remove_ida:
	ida_simple_remove(&cpufreq_ida, cpufreq_cdev->id);
free_table:
	kfree(cpufreq_cdev->freq_table);
free_idle_time:
	kfree(cpufreq_cdev->idle_time);
free_cdev:
	kfree(cpufreq_cdev);
	return cdev;
}

/**
 * cpufreq_cooling_register - function to create cpufreq cooling device.
 * @policy: cpufreq policy
 *
 * This interface function registers the cpufreq cooling device with the name
 * "thermal-cpufreq-%x". This api can support multiple instances of cpufreq
 * cooling devices.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
struct thermal_cooling_device *
cpufreq_cooling_register(struct cpufreq_policy *policy)
{
	return __cpufreq_cooling_register(NULL, policy, 0);
}
EXPORT_SYMBOL_GPL(cpufreq_cooling_register);

/**
 * of_cpufreq_cooling_register - function to create cpufreq cooling device.
 * @policy: cpufreq policy
 *
 * This interface function registers the cpufreq cooling device with the name
 * "thermal-cpufreq-%x". This api can support multiple instances of cpufreq
 * cooling devices. Using this API, the cpufreq cooling device will be
 * linked to the device tree node provided.
 *
 * Using this function, the cooling device will implement the power
 * extensions by using a simple cpu power model.  The cpus must have
 * registered their OPPs using the OPP library.
 *
 * It also takes into account, if property present in policy CPU node, the
 * static power consumed by the cpu.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * and NULL on failure.
 */
struct thermal_cooling_device *
of_cpufreq_cooling_register(struct cpufreq_policy *policy)
{
	struct device_node *np = of_get_cpu_node(policy->cpu, NULL);
	struct thermal_cooling_device *cdev = NULL;
	u32 capacitance = 0;

	if (!np) {
		pr_err("cpu_cooling: OF node not available for cpu%d\n",
		       policy->cpu);
		return NULL;
	}

	if (of_find_property(np, "#cooling-cells", NULL)) {
		of_property_read_u32(np, "dynamic-power-coefficient",
				     &capacitance);

		cdev = __cpufreq_cooling_register(np, policy, capacitance);
		if (IS_ERR(cdev)) {
			pr_err("cpu_cooling: cpu%d failed to register as cooling device: %ld\n",
			       policy->cpu, PTR_ERR(cdev));
			cdev = NULL;
		}
	}

	of_node_put(np);
	return cdev;
}
EXPORT_SYMBOL_GPL(of_cpufreq_cooling_register);

/**
 * cpufreq_cooling_unregister - function to remove cpufreq cooling device.
 * @cdev: thermal cooling device pointer.
 *
 * This interface function unregisters the "thermal-cpufreq-%x" cooling device.
 */
void cpufreq_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct cpufreq_cooling_device *cpufreq_cdev;

	if (!cdev)
		return;

	cpufreq_cdev = cdev->devdata;

	mutex_lock(&cooling_list_lock);
	list_del(&cpufreq_cdev->node);
	mutex_unlock(&cooling_list_lock);

	thermal_cooling_device_unregister(cdev);
	freq_qos_remove_request(&cpufreq_cdev->qos_req);
	ida_simple_remove(&cpufreq_ida, cpufreq_cdev->id);
	kfree(cpufreq_cdev->idle_time);
	kfree(cpufreq_cdev->freq_table);
	kfree(cpufreq_cdev);
}
EXPORT_SYMBOL_GPL(cpufreq_cooling_unregister);
