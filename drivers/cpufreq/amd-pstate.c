// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * amd-pstate.c - AMD Processor P-state Frequency Driver
 *
 * Copyright (C) 2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 * Author: Huang Rui <ray.huang@amd.com>
 *
 * AMD P-State introduces a new CPU performance scaling design for AMD
 * processors using the ACPI Collaborative Performance and Power Control (CPPC)
 * feature which works with the AMD SMU firmware providing a finer grained
 * frequency control range. It is to replace the legacy ACPI P-States control,
 * allows a flexible, low-latency interface for the Linux kernel to directly
 * communicate the performance hints to hardware.
 *
 * AMD P-State is supported on recent AMD Zen base CPU series include some of
 * Zen2 and Zen3 processors. _CPC needs to be present in the ACPI tables of AMD
 * P-State supported system. And there are two types of hardware implementations
 * for AMD P-State: 1) Full MSR Solution and 2) Shared Memory Solution.
 * X86_FEATURE_CPPC CPU feature flag is used to distinguish the different types.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/compiler.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/static_call.h>
#include <linux/amd-pstate.h>

#include <acpi/processor.h>
#include <acpi/cppc_acpi.h>

#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/cpu_device_id.h>
#include "amd-pstate-trace.h"

#define AMD_PSTATE_TRANSITION_LATENCY	20000
#define AMD_PSTATE_TRANSITION_DELAY	1000

/*
 * TODO: We need more time to fine tune processors with shared memory solution
 * with community together.
 *
 * There are some performance drops on the CPU benchmarks which reports from
 * Suse. We are co-working with them to fine tune the shared memory solution. So
 * we disable it by default to go acpi-cpufreq on these processors and add a
 * module parameter to be able to enable it manually for debugging.
 */
static bool shared_mem __read_mostly;
static int cppc_active __read_mostly;
static int cppc_load __initdata;
static int epp_off __initdata;

static struct cpufreq_driver *default_pstate_driver;
static struct cpufreq_driver amd_pstate_epp_driver;
static struct cpufreq_driver amd_pstate_driver;
static struct amd_cpudata **all_cpu_data;

static struct amd_pstate_params global_params;

static DEFINE_MUTEX(amd_pstate_limits_lock);
static DEFINE_MUTEX(amd_pstate_driver_lock);

static bool cppc_boost __read_mostly;
struct kobject *amd_pstate_kobj;

#ifdef CONFIG_ACPI_CPPC_LIB
static s16 amd_pstate_get_epp(struct amd_cpudata *cpudata, u64 cppc_req_cached)
{
	s16 epp;
	struct cppc_perf_caps perf_caps;
	int ret;

	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		if (!cppc_req_cached) {
			epp = rdmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ,
					&cppc_req_cached);
			if (epp)
				return epp;
		}
		epp = (cppc_req_cached >> 24) & 0xFF;
	} else {
		ret = cppc_get_epp_caps(cpudata->cpu, &perf_caps);
		if (ret < 0) {
			pr_debug("Could not retrieve energy perf value (%d)\n", ret);
			return -EIO;
		}
		epp = (s16) perf_caps.energy_perf;
	}

	return epp;
}
#endif

static int amd_pstate_get_energy_pref_index(struct amd_cpudata *cpudata)
{
	s16 epp;
	int index = -EINVAL;

	epp = amd_pstate_get_epp(cpudata, 0);
	if (epp < 0)
		return epp;

	switch (epp) {
		case AMD_CPPC_EPP_PERFORMANCE:
			index = EPP_INDEX_PERFORMANCE;
			break;
		case AMD_CPPC_EPP_BALANCE_PERFORMANCE:
			index = EPP_INDEX_BALANCE_PERFORMANCE;
			break;
		case AMD_CPPC_EPP_BALANCE_POWERSAVE:
			index = EPP_INDEX_BALANCE_POWERSAVE;
			break;
		case AMD_CPPC_EPP_POWERSAVE:
			index = EPP_INDEX_POWERSAVE;
			break;
		default:
			break;
	}

	return index;
}

#ifdef CONFIG_ACPI_CPPC_LIB
static int amd_pstate_set_epp(struct amd_cpudata *cpudata, u32 epp)
{
	int ret;
	struct cppc_perf_ctrls perf_ctrls;

	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		u64 value = READ_ONCE(cpudata->cppc_req_cached);

		value &= ~GENMASK_ULL(31, 24);
		value |= (u64)epp << 24;
		WRITE_ONCE(cpudata->cppc_req_cached, value);

		ret = wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, value);
		if (!ret)
			cpudata->epp_cached = epp;
	} else {
		perf_ctrls.energy_perf = epp;
		ret = cppc_set_epp_perf(cpudata->cpu, &perf_ctrls, 1);
		if (ret) {
			pr_debug("failed to set energy perf value (%d)\n", ret);
			return ret;
		}
		cpudata->epp_cached = epp;
	}

	return ret;
}

static int amd_pstate_set_energy_pref_index(struct amd_cpudata *cpudata,
		int pref_index)
{
	int epp = -EINVAL;
	int ret;

	if (!pref_index) {
		pr_debug("EPP pref_index is invalid\n");
		return -EINVAL;
	}

	if (epp == -EINVAL)
		epp = epp_values[pref_index];

	if (epp > 0 && cpudata->policy == CPUFREQ_POLICY_PERFORMANCE) {
		pr_debug("EPP cannot be set under performance policy\n");
		return -EBUSY;
	}

	ret = amd_pstate_set_epp(cpudata, epp);

	return ret;
}
#endif

static inline int pstate_enable(bool enable)
{
	return wrmsrl_safe(MSR_AMD_CPPC_ENABLE, enable);
}

static int cppc_enable(bool enable)
{
	int cpu, ret = 0;
	struct cppc_perf_ctrls perf_ctrls;

	for_each_present_cpu(cpu) {
		ret = cppc_set_enable(cpu, enable);
		if (ret)
			return ret;

		/* Enable autonomous mode for EPP */
		if (!cppc_active) {
			/* Set desired perf as zero to allow EPP firmware control */
			perf_ctrls.desired_perf = 0;
			ret = cppc_set_perf(cpu, &perf_ctrls);
			if (ret)
				return ret;
		}
	}

	return ret;
}

DEFINE_STATIC_CALL(amd_pstate_enable, pstate_enable);

static inline int amd_pstate_enable(bool enable)
{
	return static_call(amd_pstate_enable)(enable);
}

static int pstate_init_perf(struct amd_cpudata *cpudata)
{
	u64 cap1;
	u32 highest_perf;

	int ret = rdmsrl_safe_on_cpu(cpudata->cpu, MSR_AMD_CPPC_CAP1,
				     &cap1);
	if (ret)
		return ret;

	/*
	 * TODO: Introduce AMD specific power feature.
	 *
	 * CPPC entry doesn't indicate the highest performance in some ASICs.
	 */
	highest_perf = amd_get_highest_perf();
	if (highest_perf > AMD_CPPC_HIGHEST_PERF(cap1))
		highest_perf = AMD_CPPC_HIGHEST_PERF(cap1);

	WRITE_ONCE(cpudata->highest_perf, highest_perf);

	WRITE_ONCE(cpudata->nominal_perf, AMD_CPPC_NOMINAL_PERF(cap1));
	WRITE_ONCE(cpudata->lowest_nonlinear_perf, AMD_CPPC_LOWNONLIN_PERF(cap1));
	WRITE_ONCE(cpudata->lowest_perf, AMD_CPPC_LOWEST_PERF(cap1));

	return 0;
}

static int cppc_init_perf(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 highest_perf;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	highest_perf = amd_get_highest_perf();
	if (highest_perf > cppc_perf.highest_perf)
		highest_perf = cppc_perf.highest_perf;

	WRITE_ONCE(cpudata->highest_perf, highest_perf);

	WRITE_ONCE(cpudata->nominal_perf, cppc_perf.nominal_perf);
	WRITE_ONCE(cpudata->lowest_nonlinear_perf,
		   cppc_perf.lowest_nonlinear_perf);
	WRITE_ONCE(cpudata->lowest_perf, cppc_perf.lowest_perf);

	return 0;
}

DEFINE_STATIC_CALL(amd_pstate_init_perf, pstate_init_perf);

static inline int amd_pstate_init_perf(struct amd_cpudata *cpudata)
{
	return static_call(amd_pstate_init_perf)(cpudata);
}

static void pstate_update_perf(struct amd_cpudata *cpudata, u32 min_perf,
			       u32 des_perf, u32 max_perf, bool fast_switch)
{
	if (fast_switch)
		wrmsrl(MSR_AMD_CPPC_REQ, READ_ONCE(cpudata->cppc_req_cached));
	else
		wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ,
			      READ_ONCE(cpudata->cppc_req_cached));
}

static void cppc_update_perf(struct amd_cpudata *cpudata,
			     u32 min_perf, u32 des_perf,
			     u32 max_perf, bool fast_switch)
{
	struct cppc_perf_ctrls perf_ctrls;

	perf_ctrls.max_perf = max_perf;
	perf_ctrls.min_perf = min_perf;
	perf_ctrls.desired_perf = des_perf;

	cppc_set_perf(cpudata->cpu, &perf_ctrls);
}

DEFINE_STATIC_CALL(amd_pstate_update_perf, pstate_update_perf);

static inline void amd_pstate_update_perf(struct amd_cpudata *cpudata,
					  u32 min_perf, u32 des_perf,
					  u32 max_perf, bool fast_switch)
{
	static_call(amd_pstate_update_perf)(cpudata, min_perf, des_perf,
					    max_perf, fast_switch);
}

static inline bool amd_pstate_sample(struct amd_cpudata *cpudata)
{
	u64 aperf, mperf, tsc;
	unsigned long flags;

	local_irq_save(flags);
	rdmsrl(MSR_IA32_APERF, aperf);
	rdmsrl(MSR_IA32_MPERF, mperf);
	tsc = rdtsc();

	if (cpudata->prev.mperf == mperf || cpudata->prev.tsc == tsc) {
		local_irq_restore(flags);
		return false;
	}

	local_irq_restore(flags);

	cpudata->cur.aperf = aperf;
	cpudata->cur.mperf = mperf;
	cpudata->cur.tsc =  tsc;
	cpudata->cur.aperf -= cpudata->prev.aperf;
	cpudata->cur.mperf -= cpudata->prev.mperf;
	cpudata->cur.tsc -= cpudata->prev.tsc;

	cpudata->prev.aperf = aperf;
	cpudata->prev.mperf = mperf;
	cpudata->prev.tsc = tsc;

	cpudata->freq = div64_u64((cpudata->cur.aperf * cpu_khz), cpudata->cur.mperf);

	return true;
}

static void amd_pstate_update(struct amd_cpudata *cpudata, u32 min_perf,
			      u32 des_perf, u32 max_perf, bool fast_switch)
{
	u64 prev = READ_ONCE(cpudata->cppc_req_cached);
	u64 value = prev;

	des_perf = clamp_t(unsigned long, des_perf, min_perf, max_perf);
	value &= ~AMD_CPPC_MIN_PERF(~0L);
	value |= AMD_CPPC_MIN_PERF(min_perf);

	value &= ~AMD_CPPC_DES_PERF(~0L);
	value |= AMD_CPPC_DES_PERF(des_perf);

	value &= ~AMD_CPPC_MAX_PERF(~0L);
	value |= AMD_CPPC_MAX_PERF(max_perf);

	if (trace_amd_pstate_perf_enabled() && amd_pstate_sample(cpudata)) {
		trace_amd_pstate_perf(min_perf, des_perf, max_perf, cpudata->freq,
			cpudata->cur.mperf, cpudata->cur.aperf, cpudata->cur.tsc,
				cpudata->cpu, (value != prev), fast_switch);
	}

	if (value == prev)
		return;

	WRITE_ONCE(cpudata->cppc_req_cached, value);

	amd_pstate_update_perf(cpudata, min_perf, des_perf,
			       max_perf, fast_switch);
}

static int amd_pstate_verify(struct cpufreq_policy_data *policy)
{
	cpufreq_verify_within_cpu_limits(policy);

	return 0;
}

static int amd_pstate_target(struct cpufreq_policy *policy,
			     unsigned int target_freq,
			     unsigned int relation)
{
	struct cpufreq_freqs freqs;
	struct amd_cpudata *cpudata = policy->driver_data;
	unsigned long max_perf, min_perf, des_perf, cap_perf;

	if (!cpudata->max_freq)
		return -ENODEV;

	cap_perf = READ_ONCE(cpudata->highest_perf);
	min_perf = READ_ONCE(cpudata->lowest_perf);
	max_perf = cap_perf;

	freqs.old = policy->cur;
	freqs.new = target_freq;

	des_perf = DIV_ROUND_CLOSEST(target_freq * cap_perf,
				     cpudata->max_freq);

	cpufreq_freq_transition_begin(policy, &freqs);
	amd_pstate_update(cpudata, min_perf, des_perf,
			  max_perf, false);
	cpufreq_freq_transition_end(policy, &freqs, false);

	return 0;
}

static void amd_pstate_adjust_perf(unsigned int cpu,
				   unsigned long _min_perf,
				   unsigned long target_perf,
				   unsigned long capacity)
{
	unsigned long max_perf, min_perf, des_perf,
		      cap_perf, lowest_nonlinear_perf;
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct amd_cpudata *cpudata = policy->driver_data;

	cap_perf = READ_ONCE(cpudata->highest_perf);
	lowest_nonlinear_perf = READ_ONCE(cpudata->lowest_nonlinear_perf);

	des_perf = cap_perf;
	if (target_perf < capacity)
		des_perf = DIV_ROUND_UP(cap_perf * target_perf, capacity);

	min_perf = READ_ONCE(cpudata->highest_perf);
	if (_min_perf < capacity)
		min_perf = DIV_ROUND_UP(cap_perf * _min_perf, capacity);

	if (min_perf < lowest_nonlinear_perf)
		min_perf = lowest_nonlinear_perf;

	max_perf = cap_perf;
	if (max_perf < min_perf)
		max_perf = min_perf;

	amd_pstate_update(cpudata, min_perf, des_perf, max_perf, true);
}

static int amd_get_min_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	/* Switch to khz */
	return cppc_perf.lowest_freq * 1000;
}

static int amd_get_max_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 max_perf, max_freq, nominal_freq, nominal_perf;
	u64 boost_ratio;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	nominal_freq = cppc_perf.nominal_freq;
	nominal_perf = READ_ONCE(cpudata->nominal_perf);
	max_perf = READ_ONCE(cpudata->highest_perf);

	boost_ratio = div_u64(max_perf << SCHED_CAPACITY_SHIFT,
			      nominal_perf);

	max_freq = nominal_freq * boost_ratio >> SCHED_CAPACITY_SHIFT;

	/* Switch to khz */
	return max_freq * 1000;
}

static int amd_get_nominal_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	/* Switch to khz */
	return cppc_perf.nominal_freq * 1000;
}

static int amd_get_lowest_nonlinear_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 lowest_nonlinear_freq, lowest_nonlinear_perf,
	    nominal_freq, nominal_perf;
	u64 lowest_nonlinear_ratio;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	nominal_freq = cppc_perf.nominal_freq;
	nominal_perf = READ_ONCE(cpudata->nominal_perf);

	lowest_nonlinear_perf = cppc_perf.lowest_nonlinear_perf;

	lowest_nonlinear_ratio = div_u64(lowest_nonlinear_perf << SCHED_CAPACITY_SHIFT,
					 nominal_perf);

	lowest_nonlinear_freq = nominal_freq * lowest_nonlinear_ratio >> SCHED_CAPACITY_SHIFT;

	/* Switch to khz */
	return lowest_nonlinear_freq * 1000;
}

static int amd_pstate_set_boost(struct cpufreq_policy *policy, int state)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	int ret;

	if (!cpudata->boost_supported) {
		pr_err("Boost mode is not supported by this processor or SBIOS\n");
		return -EINVAL;
	}

	if (state)
		policy->cpuinfo.max_freq = cpudata->max_freq;
	else
		policy->cpuinfo.max_freq = cpudata->nominal_freq;

	policy->max = policy->cpuinfo.max_freq;

	ret = freq_qos_update_request(&cpudata->req[1],
				      policy->cpuinfo.max_freq);
	if (ret < 0)
		return ret;

	return 0;
}

static void amd_pstate_boost_init(struct amd_cpudata *cpudata)
{
	u32 highest_perf, nominal_perf;

	highest_perf = READ_ONCE(cpudata->highest_perf);
	nominal_perf = READ_ONCE(cpudata->nominal_perf);

	if (highest_perf <= nominal_perf)
		return;

	cpudata->boost_supported = true;
	default_pstate_driver->boost_enabled = true;
}

static int amd_pstate_cpu_init(struct cpufreq_policy *policy)
{
	int min_freq, max_freq, nominal_freq, lowest_nonlinear_freq, ret;
	struct device *dev;
	struct amd_cpudata *cpudata;

	dev = get_cpu_device(policy->cpu);
	if (!dev)
		return -ENODEV;

	cpudata = kzalloc(sizeof(*cpudata), GFP_KERNEL);
	if (!cpudata)
		return -ENOMEM;

	cpudata->cpu = policy->cpu;

	ret = amd_pstate_init_perf(cpudata);
	if (ret)
		goto free_cpudata1;

	min_freq = amd_get_min_freq(cpudata);
	max_freq = amd_get_max_freq(cpudata);
	nominal_freq = amd_get_nominal_freq(cpudata);
	lowest_nonlinear_freq = amd_get_lowest_nonlinear_freq(cpudata);

	if (min_freq < 0 || max_freq < 0 || min_freq > max_freq) {
		dev_err(dev, "min_freq(%d) or max_freq(%d) value is incorrect\n",
			min_freq, max_freq);
		ret = -EINVAL;
		goto free_cpudata1;
	}

	policy->cpuinfo.transition_latency = AMD_PSTATE_TRANSITION_LATENCY;
	policy->transition_delay_us = AMD_PSTATE_TRANSITION_DELAY;

	policy->min = min_freq;
	policy->max = max_freq;

	policy->cpuinfo.min_freq = min_freq;
	policy->cpuinfo.max_freq = max_freq;

	/* It will be updated by governor */
	policy->cur = policy->cpuinfo.min_freq;

	if (boot_cpu_has(X86_FEATURE_CPPC))
		policy->fast_switch_possible = true;

	ret = freq_qos_add_request(&policy->constraints, &cpudata->req[0],
				   FREQ_QOS_MIN, policy->cpuinfo.min_freq);
	if (ret < 0) {
		dev_err(dev, "Failed to add min-freq constraint (%d)\n", ret);
		goto free_cpudata1;
	}

	ret = freq_qos_add_request(&policy->constraints, &cpudata->req[1],
				   FREQ_QOS_MAX, policy->cpuinfo.max_freq);
	if (ret < 0) {
		dev_err(dev, "Failed to add max-freq constraint (%d)\n", ret);
		goto free_cpudata2;
	}

	/* Initial processor data capability frequencies */
	cpudata->max_freq = max_freq;
	cpudata->min_freq = min_freq;
	cpudata->nominal_freq = nominal_freq;
	cpudata->lowest_nonlinear_freq = lowest_nonlinear_freq;

	policy->driver_data = cpudata;

	amd_pstate_boost_init(cpudata);

	return 0;

free_cpudata2:
	freq_qos_remove_request(&cpudata->req[0]);
free_cpudata1:
	kfree(cpudata);
	return ret;
}

static int amd_pstate_cpu_exit(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = policy->driver_data;

	freq_qos_remove_request(&cpudata->req[1]);
	freq_qos_remove_request(&cpudata->req[0]);
	kfree(cpudata);

	return 0;
}

static int amd_pstate_cpu_resume(struct cpufreq_policy *policy)
{
	int ret;

	ret = amd_pstate_enable(true);
	if (ret)
		pr_err("failed to enable amd-pstate during resume, return %d\n", ret);

	return ret;
}

static int amd_pstate_cpu_suspend(struct cpufreq_policy *policy)
{
	int ret;

	ret = amd_pstate_enable(false);
	if (ret)
		pr_err("failed to disable amd-pstate during suspend, return %d\n", ret);

	return ret;
}

/* Sysfs attributes */

/*
 * This frequency is to indicate the maximum hardware frequency.
 * If boost is not active but supported, the frequency will be larger than the
 * one in cpuinfo.
 */
static ssize_t show_amd_pstate_max_freq(struct cpufreq_policy *policy,
					char *buf)
{
	int max_freq;
	struct amd_cpudata *cpudata = policy->driver_data;

	max_freq = amd_get_max_freq(cpudata);
	if (max_freq < 0)
		return max_freq;

	return sprintf(&buf[0], "%u\n", max_freq);
}

static ssize_t show_amd_pstate_lowest_nonlinear_freq(struct cpufreq_policy *policy,
						     char *buf)
{
	int freq;
	struct amd_cpudata *cpudata = policy->driver_data;

	freq = amd_get_lowest_nonlinear_freq(cpudata);
	if (freq < 0)
		return freq;

	return sprintf(&buf[0], "%u\n", freq);
}

/*
 * In some of ASICs, the highest_perf is not the one in the _CPC table, so we
 * need to expose it to sysfs.
 */
static ssize_t show_amd_pstate_highest_perf(struct cpufreq_policy *policy,
					    char *buf)
{
	u32 perf;
	struct amd_cpudata *cpudata = policy->driver_data;

	perf = READ_ONCE(cpudata->highest_perf);

	return sprintf(&buf[0], "%u\n", perf);
}

static ssize_t show_energy_performance_available_preferences(
				struct cpufreq_policy *policy, char *buf)
{
	int i = 0;
	int ret = 0;

	while (energy_perf_strings[i] != NULL)
		ret += sprintf(&buf[ret], "%s ", energy_perf_strings[i++]);

	ret += sprintf(&buf[ret], "\n");

	return ret;
}

static ssize_t store_energy_performance_preference(
		struct cpufreq_policy *policy, const char *buf, size_t count)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	char str_preference[21];
	ssize_t ret;
	u32 epp = 0;

	ret = sscanf(buf, "%20s", str_preference);
	if (ret != 1)
		return -EINVAL;

	ret = match_string(energy_perf_strings, -1, str_preference);
	if (ret < 0)
		return -EINVAL;

	mutex_lock(&amd_pstate_limits_lock);
	ret = amd_pstate_set_energy_pref_index(cpudata, ret);
	mutex_unlock(&amd_pstate_limits_lock);

	return ret ?: count;
}

static ssize_t show_energy_performance_preference(
				struct cpufreq_policy *policy, char *buf)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	int preference;

	preference = amd_pstate_get_energy_pref_index(cpudata);
	if (preference < 0)
		return preference;

	return  sprintf(buf, "%s\n", energy_perf_strings[preference]);
}

static void amd_pstate_update_policies(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		cpufreq_update_policy(cpu);
}

static ssize_t show_boost(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", cppc_boost);
}

static ssize_t store_boost(struct kobject *a,
				       struct kobj_attribute *b,
				       const char *buf, size_t count)
{
	bool new_state;
	int ret;

	ret = kstrtobool(buf, &new_state);
	if (ret)
		return -EINVAL;

	mutex_lock(&amd_pstate_driver_lock);
	cppc_boost = !!new_state;
	amd_pstate_update_policies();
	mutex_unlock(&amd_pstate_driver_lock);

	return count;
}

static ssize_t amd_pstate_show_status(char *buf)
{
	if (!default_pstate_driver)
		return sprintf(buf, "off\n");

	return sprintf(buf, "%s\n", default_pstate_driver == &amd_pstate_epp_driver ?
					"active" : "passive");
}

static int amd_pstate_update_status(const char *buf, size_t size)
{
	/* FIXME! */
	return -EOPNOTSUPP;
}

static ssize_t show_status(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;

	mutex_lock(&amd_pstate_driver_lock);
	ret = amd_pstate_show_status(buf);
	mutex_unlock(&amd_pstate_driver_lock);

	return ret;
}

static ssize_t store_status(struct kobject *a, struct kobj_attribute *b,
			    const char *buf, size_t count)
{
	char *p = memchr(buf, '\n', count);
	int ret;

	mutex_lock(&amd_pstate_driver_lock);
	ret = amd_pstate_update_status(buf, p ? p - buf : count);
	mutex_unlock(&amd_pstate_driver_lock);

	return ret < 0 ? ret : count;
}

cpufreq_freq_attr_ro(amd_pstate_max_freq);
cpufreq_freq_attr_ro(amd_pstate_lowest_nonlinear_freq);

cpufreq_freq_attr_ro(amd_pstate_highest_perf);
cpufreq_freq_attr_rw(energy_performance_preference);
cpufreq_freq_attr_ro(energy_performance_available_preferences);
define_one_global_rw(boost);
define_one_global_rw(status);

static struct freq_attr *amd_pstate_attr[] = {
	&amd_pstate_max_freq,
	&amd_pstate_lowest_nonlinear_freq,
	&amd_pstate_highest_perf,
	NULL,
};

static struct freq_attr *amd_pstate_epp_attr[] = {
	&amd_pstate_max_freq,
	&amd_pstate_lowest_nonlinear_freq,
	&amd_pstate_highest_perf,
	&energy_performance_preference,
	&energy_performance_available_preferences,
	NULL,
};

static struct attribute *pstate_global_attributes[] = {
	&boost.attr,
	&status.attr,
	NULL
};

static const struct attribute_group amd_pstate_global_attr_group = {
	.attrs = pstate_global_attributes,
};

static inline void update_boost_state(void)
{
	u64 misc_en;
	struct amd_cpudata *cpudata;

	cpudata = all_cpu_data[0];
	rdmsrl(MSR_K7_HWCR, misc_en);
	global_params.cppc_boost_disabled = misc_en & BIT_ULL(25);
}

static int amd_pstate_init_cpu(unsigned int cpunum)
{
	struct amd_cpudata *cpudata;

	cpudata = all_cpu_data[cpunum];
	if (!cpudata) {
		cpudata = kzalloc(sizeof(*cpudata), GFP_KERNEL);
		if (!cpudata)
			return -ENOMEM;
		WRITE_ONCE(all_cpu_data[cpunum], cpudata);

		cpudata->cpu = cpunum;
	}
	cpudata->epp_powersave = -EINVAL;
	cpudata->epp_policy = 0;
	pr_debug("controlling: cpu %d\n", cpunum);
	return 0;
}

static int __amd_pstate_cpu_init(struct cpufreq_policy *policy)
{
	int min_freq, max_freq, nominal_freq, lowest_nonlinear_freq, ret;
	struct amd_cpudata *cpudata;
	struct device *dev;
	int rc;
	u64 value;

	rc = amd_pstate_init_cpu(policy->cpu);
	if (rc)
		return rc;

	cpudata = all_cpu_data[policy->cpu];

	dev = get_cpu_device(policy->cpu);
	if (!dev)
		goto free_cpudata1;

	rc = amd_pstate_init_perf(cpudata);
	if (rc)
		goto free_cpudata1;

	min_freq = amd_get_min_freq(cpudata);
	max_freq = amd_get_max_freq(cpudata);
	nominal_freq = amd_get_nominal_freq(cpudata);
	lowest_nonlinear_freq = amd_get_lowest_nonlinear_freq(cpudata);
	if (min_freq < 0 || max_freq < 0 || min_freq > max_freq) {
		dev_err(dev, "min_freq(%d) or max_freq(%d) value is incorrect\n",
				min_freq, max_freq);
		ret = -EINVAL;
		goto free_cpudata1;
	}

	policy->min = min_freq;
	policy->max = max_freq;

	policy->cpuinfo.min_freq = min_freq;
	policy->cpuinfo.max_freq = max_freq;
	/* It will be updated by governor */
	policy->cur = policy->cpuinfo.min_freq;

	/* Initial processor data capability frequencies */
	cpudata->max_freq = max_freq;
	cpudata->min_freq = min_freq;
	cpudata->nominal_freq = nominal_freq;
	cpudata->lowest_nonlinear_freq = lowest_nonlinear_freq;

	policy->driver_data = cpudata;

	update_boost_state();
	cpudata->epp_cached = amd_pstate_get_epp(cpudata, value);

	policy->min = policy->cpuinfo.min_freq;
	policy->max = policy->cpuinfo.max_freq;

	if (boot_cpu_has(X86_FEATURE_CPPC))
		policy->fast_switch_possible = true;

	if (!shared_mem && boot_cpu_has(X86_FEATURE_CPPC)) {
		ret = rdmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, &value);
		if (ret)
			return ret;
		WRITE_ONCE(cpudata->cppc_req_cached, value);

		ret = rdmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_CAP1, &value);
		if (ret)
			return ret;
		WRITE_ONCE(cpudata->cppc_cap1_cached, value);
	}
	amd_pstate_boost_init(cpudata);

	return 0;

free_cpudata1:
	kfree(cpudata);
	return ret;
}

static int amd_pstate_epp_cpu_init(struct cpufreq_policy *policy)
{
	int ret;

	ret = __amd_pstate_cpu_init(policy);
	if (ret)
		return ret;
	/*
	 * Set the policy to powersave to provide a valid fallback value in case
	 * the default cpufreq governor is neither powersave nor performance.
	 */
	policy->policy = CPUFREQ_POLICY_POWERSAVE;

	return 0;
}

static int amd_pstate_epp_cpu_exit(struct cpufreq_policy *policy)
{
	pr_debug("CPU %d exiting\n", policy->cpu);
	policy->fast_switch_possible = false;
	return 0;
}

static void amd_pstate_update_max_freq(unsigned int cpu)
{
	struct cpufreq_policy *policy = policy = cpufreq_cpu_get(cpu);

	if (!policy)
		return;

	refresh_frequency_limits(policy);
	cpufreq_cpu_put(policy);
}

static void amd_pstate_epp_update_limits(unsigned int cpu)
{
	mutex_lock(&amd_pstate_driver_lock);
	update_boost_state();
	if (global_params.cppc_boost_disabled) {
		for_each_possible_cpu(cpu)
			amd_pstate_update_max_freq(cpu);
	} else {
		cpufreq_update_policy(cpu);
	}
	mutex_unlock(&amd_pstate_driver_lock);
}

static int cppc_boost_hold_time_ns = 3 * NSEC_PER_MSEC;

static inline void amd_pstate_boost_up(struct amd_cpudata *cpudata)
{
	u64 hwp_req = READ_ONCE(cpudata->cppc_req_cached);
	u64 hwp_cap = READ_ONCE(cpudata->cppc_cap1_cached);
	u32 max_limit = (hwp_req & 0xff);
	u32 min_limit = (hwp_req & 0xff00) >> 8;
	u32 boost_level1;

	/* If max and min are equal or already at max, nothing to boost */
	if (max_limit == min_limit)
		return;

	/* Set boost max and min to initial value */
	if (!cpudata->cppc_boost_min)
		cpudata->cppc_boost_min = min_limit;

	boost_level1 = ((AMD_CPPC_NOMINAL_PERF(hwp_cap) + min_limit) >> 1);

	if (cpudata->cppc_boost_min < boost_level1)
		cpudata->cppc_boost_min = boost_level1;
	else if (cpudata->cppc_boost_min < AMD_CPPC_NOMINAL_PERF(hwp_cap))
		cpudata->cppc_boost_min = AMD_CPPC_NOMINAL_PERF(hwp_cap);
	else if (cpudata->cppc_boost_min == AMD_CPPC_NOMINAL_PERF(hwp_cap))
		cpudata->cppc_boost_min = max_limit;
	else
		return;

	hwp_req &= ~AMD_CPPC_MIN_PERF(~0L);
	hwp_req |= AMD_CPPC_MIN_PERF(cpudata->cppc_boost_min);
	wrmsrl_safe_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, hwp_req);
	cpudata->last_update = cpudata->sample.time;
}

static inline void amd_pstate_boost_down(struct amd_cpudata *cpudata)
{
	bool expired;

	if (cpudata->cppc_boost_min) {
		expired = time_after64(cpudata->sample.time, cpudata->last_update +
					cppc_boost_hold_time_ns);

		if (expired) {
			wrmsrl_safe_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ,
						cpudata->cppc_req_cached);
			cpudata->cppc_boost_min = 0;
		}
	}

	cpudata->last_update = cpudata->sample.time;
}

static inline void amd_pstate_boost_update_util(struct amd_cpudata *cpudata,
						      u64 time)
{
	cpudata->sample.time = time;
	if (smp_processor_id() != cpudata->cpu)
		return;

	if (cpudata->sched_flags & SCHED_CPUFREQ_IOWAIT) {
		bool do_io = false;

		cpudata->sched_flags = 0;
		/*
		 * Set iowait_boost flag and update time. Since IO WAIT flag
		 * is set all the time, we can't just conclude that there is
		 * some IO bound activity is scheduled on this CPU with just
		 * one occurrence. If we receive at least two in two
		 * consecutive ticks, then we treat as boost candidate.
		 * This is leveraged from Intel Pstate driver.
		 */
		if (time_before64(time, cpudata->last_io_update + 2 * TICK_NSEC))
			do_io = true;

		cpudata->last_io_update = time;

		if (do_io)
			amd_pstate_boost_up(cpudata);

	} else {
		amd_pstate_boost_down(cpudata);
	}
}

static inline void amd_pstate_cppc_update_hook(struct update_util_data *data,
						u64 time, unsigned int flags)
{
	struct amd_cpudata *cpudata = container_of(data,
				struct amd_cpudata, update_util);

	cpudata->sched_flags |= flags;

	if (smp_processor_id() == cpudata->cpu)
		amd_pstate_boost_update_util(cpudata, time);
}

static void amd_pstate_clear_update_util_hook(unsigned int cpu)
{
	struct amd_cpudata *cpudata = all_cpu_data[cpu];

	if (!cpudata->update_util_set)
		return;

	cpufreq_remove_update_util_hook(cpu);
	cpudata->update_util_set = false;
	synchronize_rcu();
}

static void amd_pstate_set_update_util_hook(unsigned int cpu_num)
{
	struct amd_cpudata *cpudata = all_cpu_data[cpu_num];

	if (!cppc_boost) {
		if (cpudata->update_util_set)
			amd_pstate_clear_update_util_hook(cpudata->cpu);
		return;
	}

	if (cpudata->update_util_set)
		return;

	cpudata->sample.time = 0;
	cpufreq_add_update_util_hook(cpu_num, &cpudata->update_util,
						amd_pstate_cppc_update_hook);
	cpudata->update_util_set = true;
}

static void amd_pstate_epp_init(unsigned int cpu)
{
	struct amd_cpudata *cpudata = all_cpu_data[cpu];
	u32 max_perf, min_perf;
	u64 value;
	s16 epp;
	int ret;

	max_perf = READ_ONCE(cpudata->highest_perf);
	min_perf = READ_ONCE(cpudata->lowest_perf);

	value = READ_ONCE(cpudata->cppc_req_cached);

	if (cpudata->policy == CPUFREQ_POLICY_PERFORMANCE)
		min_perf = max_perf;

	/* Initial min/max values for CPPC Performance Controls Register */
	value &= ~AMD_CPPC_MIN_PERF(~0L);
	value |= AMD_CPPC_MIN_PERF(min_perf);

	value &= ~AMD_CPPC_MAX_PERF(~0L);
	value |= AMD_CPPC_MAX_PERF(max_perf);

	/* CPPC EPP feature require to set zero to the desire perf bit */
	value &= ~AMD_CPPC_DES_PERF(~0L);
	value |= AMD_CPPC_DES_PERF(0);

	if (cpudata->epp_policy == cpudata->policy)
		goto skip_epp;

	cpudata->epp_policy = cpudata->policy;

	if (cpudata->policy == CPUFREQ_POLICY_PERFORMANCE) {
		epp = amd_pstate_get_epp(cpudata, value);
		cpudata->epp_powersave = epp;
		if (epp < 0)
			goto skip_epp;
		/* force the epp value to be zero for performance policy */
		epp = 0;
	} else {
		if (cpudata->epp_powersave < 0)
			goto skip_epp;
		/* Get BIOS pre-defined epp value */
		epp = amd_pstate_get_epp(cpudata, value);
		if (epp)
			goto skip_epp;
		epp = cpudata->epp_powersave;
	}
	/* Set initial EPP value */
	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		value &= ~GENMASK_ULL(31, 24);
		value |= (u64)epp << 24;
	}

skip_epp:
	WRITE_ONCE(cpudata->cppc_req_cached, value);
	ret = wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, value);
	if (!ret)
		cpudata->epp_cached = epp;
}

static void amd_pstate_set_max_limits(struct amd_cpudata *cpudata)
{
	u64 hwp_cap = READ_ONCE(cpudata->cppc_cap1_cached);
	u64 hwp_req = READ_ONCE(cpudata->cppc_req_cached);
	u32 max_limit = (hwp_cap >> 24) & 0xff;

	hwp_req &= ~AMD_CPPC_MIN_PERF(~0L);
	hwp_req |= AMD_CPPC_MIN_PERF(max_limit);
	wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, hwp_req);
}

static int amd_pstate_epp_set_policy(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata;

	if (!policy->cpuinfo.max_freq)
		return -ENODEV;

	pr_debug("set_policy: cpuinfo.max %u policy->max %u\n",
				policy->cpuinfo.max_freq, policy->max);

	cpudata = all_cpu_data[policy->cpu];
	cpudata->policy = policy->policy;

	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		mutex_lock(&amd_pstate_limits_lock);

		if (cpudata->policy == CPUFREQ_POLICY_PERFORMANCE) {
			amd_pstate_clear_update_util_hook(policy->cpu);
			amd_pstate_set_max_limits(cpudata);
		} else {
			amd_pstate_set_update_util_hook(policy->cpu);
		}

		if (boot_cpu_has(X86_FEATURE_CPPC))
			amd_pstate_epp_init(policy->cpu);

		mutex_unlock(&amd_pstate_limits_lock);
	}

	return 0;
}

static void amd_pstate_epp_reenable(struct amd_cpudata *cpudata)
{
	struct cppc_perf_ctrls perf_ctrls;
	u64 value, max_perf;
	int ret;

	ret = amd_pstate_enable(true);
	if (ret)
		pr_err("failed to enable amd pstate during resume, return %d\n", ret);

	value = READ_ONCE(cpudata->cppc_req_cached);
	max_perf = READ_ONCE(cpudata->highest_perf);

	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, value);
	} else {
		perf_ctrls.max_perf = max_perf;
		perf_ctrls.energy_perf = AMD_CPPC_ENERGY_PERF_PREF(cpudata->epp_cached);
		cppc_set_perf(cpudata->cpu, &perf_ctrls);
	}
}

static int amd_pstate_epp_cpu_online(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = all_cpu_data[policy->cpu];

	pr_debug("AMD CPU Core %d going online\n", cpudata->cpu);

	if (cppc_active) {
		amd_pstate_epp_reenable(cpudata);
		cpudata->suspended = false;
	}

	return 0;
}

static void amd_pstate_epp_offline(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = all_cpu_data[policy->cpu];
	struct cppc_perf_ctrls perf_ctrls;
	int min_perf;
	u64 value;

	min_perf = READ_ONCE(cpudata->lowest_perf);
	value = READ_ONCE(cpudata->cppc_req_cached);

	mutex_lock(&amd_pstate_limits_lock);
	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		cpudata->epp_policy = CPUFREQ_POLICY_UNKNOWN;

		/* Set max perf same as min perf */
		value &= ~AMD_CPPC_MAX_PERF(~0L);
		value |= AMD_CPPC_MAX_PERF(min_perf);
		value &= ~AMD_CPPC_MIN_PERF(~0L);
		value |= AMD_CPPC_MIN_PERF(min_perf);
		wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, value);
	} else {
		perf_ctrls.desired_perf = 0;
		perf_ctrls.max_perf = min_perf;
		perf_ctrls.energy_perf = AMD_CPPC_ENERGY_PERF_PREF(AMD_CPPC_EPP_POWERSAVE);
		cppc_set_perf(cpudata->cpu, &perf_ctrls);
	}
	mutex_unlock(&amd_pstate_limits_lock);
}

static int amd_pstate_cpu_offline(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = all_cpu_data[policy->cpu];

	pr_debug("AMD CPU Core %d going offline\n", cpudata->cpu);

	if (cpudata->suspended)
		return 0;

	if (cppc_active)
		amd_pstate_epp_offline(policy);

	return 0;
}

static int amd_pstate_epp_cpu_offline(struct cpufreq_policy *policy)
{
	amd_pstate_clear_update_util_hook(policy->cpu);

	return amd_pstate_cpu_offline(policy);
}

static int amd_pstate_epp_suspend(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = all_cpu_data[policy->cpu];
	int ret;

	/* avoid suspending when EPP is not enabled */
	if (!cppc_active)
		return 0;

	/* set this flag to avoid setting core offline*/
	cpudata->suspended = true;

	/* disable CPPC in lowlevel firmware */
	ret = amd_pstate_enable(false);
	if (ret)
		pr_err("failed to suspend, return %d\n", ret);

	return 0;
}

static int amd_pstate_epp_resume(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = all_cpu_data[policy->cpu];

	if (cpudata->suspended) {
		mutex_lock(&amd_pstate_limits_lock);

		/* enable amd pstate from suspend state*/
		amd_pstate_epp_reenable(cpudata);

		mutex_unlock(&amd_pstate_limits_lock);

		cpudata->suspended = false;
	}

	return 0;
}

static void amd_pstate_verify_cpu_policy(struct amd_cpudata *cpudata,
					   struct cpufreq_policy_data *policy)
{
	update_boost_state();
	cpufreq_verify_within_cpu_limits(policy);
}

static int amd_pstate_epp_verify_policy(struct cpufreq_policy_data *policy)
{
	amd_pstate_verify_cpu_policy(all_cpu_data[policy->cpu], policy);
	pr_debug("policy_max =%d, policy_min=%d\n", policy->max, policy->min);
	return 0;
}

static struct cpufreq_driver amd_pstate_driver = {
	.flags		= CPUFREQ_CONST_LOOPS | CPUFREQ_NEED_UPDATE_LIMITS,
	.verify		= amd_pstate_verify,
	.target		= amd_pstate_target,
	.init		= amd_pstate_cpu_init,
	.exit		= amd_pstate_cpu_exit,
	.suspend	= amd_pstate_cpu_suspend,
	.resume		= amd_pstate_cpu_resume,
	.set_boost	= amd_pstate_set_boost,
	.name		= "amd-pstate",
	.attr		= amd_pstate_attr,
};

static struct cpufreq_driver amd_pstate_epp_driver = {
	.flags		= CPUFREQ_CONST_LOOPS,
	.verify		= amd_pstate_epp_verify_policy,
	.setpolicy	= amd_pstate_epp_set_policy,
	.init		= amd_pstate_epp_cpu_init,
	.exit		= amd_pstate_epp_cpu_exit,
	.update_limits	= amd_pstate_epp_update_limits,
	.offline	= amd_pstate_epp_cpu_offline,
	.online		= amd_pstate_epp_cpu_online,
	.suspend	= amd_pstate_epp_suspend,
	.resume		= amd_pstate_epp_resume,
	.name		= "amd_pstate_epp",
	.attr		= amd_pstate_epp_attr,
};

static int __init amd_pstate_init(void)
{
	static struct amd_cpudata **cpudata;
	int ret;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD)
		return -ENODEV;

	if (!acpi_cpc_valid()) {
		pr_warn_once("the _CPC object is not present in SBIOS or ACPI disabled\n");
		return -ENODEV;
	}

	/* don't keep reloading if cpufreq_driver exists */
	if (cpufreq_get_current_driver())
		return -EEXIST;

	/* set the pstate driver disabled by default if there
	 * is no early parameters set
	 */
	if (!cppc_load) {
		pr_debug("disabled to load\n");
		return -ENODEV;
	}

	if (!epp_off) {
		WRITE_ONCE(cppc_active, 1);
		if (!default_pstate_driver)
			default_pstate_driver = &amd_pstate_epp_driver;
	}

	/* capability check */
	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		if (!cppc_active)
			default_pstate_driver->adjust_perf = amd_pstate_adjust_perf;
	} else if (shared_mem) {
		static_call_update(amd_pstate_enable, cppc_enable);
		static_call_update(amd_pstate_init_perf, cppc_init_perf);
		static_call_update(amd_pstate_update_perf, cppc_update_perf);
	} else {
		pr_info("This processor supports shared memory solution, enable it with amd_pstate=legacy_cppc\n");
		return -ENODEV;
	}

	cpudata = vzalloc(array_size(sizeof(void *), num_possible_cpus()));
	if (!cpudata)
		return -ENOMEM;
	WRITE_ONCE(all_cpu_data, cpudata);
	/* enable amd pstate feature */
	ret = amd_pstate_enable(true);
	if (ret) {
		pr_err("failed to enable amd-pstate with return %d\n", ret);
		return ret;
	}

	ret = cpufreq_register_driver(default_pstate_driver);
	if (ret)
		pr_err("failed to register driver with return %d\n", ret);

	amd_pstate_kobj = kobject_create_and_add("amd-pstate", &cpu_subsys.dev_root->kobj);
	if (!amd_pstate_kobj) {
		ret = -EINVAL;
		pr_err("global sysfs registration failed.\n");
		goto kobject_free;
	}

	ret = sysfs_create_group(amd_pstate_kobj, &amd_pstate_global_attr_group);
	if (ret) {
		pr_err("sysfs attribute export failed with error %d.\n", ret);
		goto global_attr_free;
	}

	return ret;

global_attr_free:
	kobject_put(amd_pstate_kobj);
kobject_free:
	kfree(cpudata);
	cpufreq_unregister_driver(default_pstate_driver);
	return ret;
}
device_initcall(amd_pstate_init);

static int __init amd_pstate_param(char *str)
{
	// if (!str)
	// 	return -EINVAL;

	// /*
	//  * Disable amd-pstate driver by default for now
	//  * append amd-pstate=active or amd-pstate=active
	//  * to enable driver loaded
	//  */
	// if (!strcmp(str, "active")) {
	// 	cppc_load = 1;
	// 	default_pstate_driver = &amd_pstate_epp_driver;
	// } else if (!strcmp(str, "passive")) {
	// 	epp_off = 1;
	// 	cppc_load = 1;
	// 	default_pstate_driver = &amd_pstate_driver;
	// }

	// /*
	//  * support shared memory type CPPC which has no MSR function.
	//  * enable amd-pstate on processors with shared memory solution
	//  * (amd-pstate=legacy_cppc enabled), it is disabled by default.
	//  */
	// if (!strcmp(str, "legacy_cppc"))
	// 	shared_mem = true;

	cppc_load = 1;
	default_pstate_driver = &amd_pstate_epp_driver;

	return 0;
}
early_param("amd-pstate", amd_pstate_param);

MODULE_AUTHOR("Huang Rui <ray.huang@amd.com>");
MODULE_DESCRIPTION("AMD Processor P-state Frequency Driver");
MODULE_LICENSE("GPL");
