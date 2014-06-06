/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Added code to work as a standalone intelligent thermal throttling driver
 * for many Qualcomm SOCs by Paul Reioux (Faux123)
 * Modifications copyright (c) 2013~2014
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <mach/cpufreq.h>

#ifdef CONFIG_INTELLI_THERMAL_STATS
#define DEFAULT_POLLING_MS	250
/* last 3 minutes based on 250ms polling cycle */
#define MAX_HISTORY_SZ		((3*60*1000) / DEFAULT_POLLING_MS)

struct msm_thermal_stat_data {
	int32_t temp_history[MAX_HISTORY_SZ];
	uint32_t throttled;
	uint32_t warning;
	uint32_t normal;
};
static struct msm_thermal_stat_data msm_thermal_stats;
static uint32_t hist_index = 0;
#endif

static int enabled;
static struct msm_thermal_data msm_thermal_info = {
	.sensor_id = 0,
	.poll_ms = 250,
	.limit_temp_degC = 70,
	.temp_hysteresis_degC = 10,
	.freq_step = 2,
	.freq_control_mask = 0xf,
	.core_limit_temp_degC = 80,
	.core_temp_hysteresis_degC = 10,
	.core_control_mask = 0xe,
};
static uint32_t limited_max_freq_thermal = MSM_CPUFREQ_NO_LIMIT;
static struct delayed_work check_temp_work;
static bool core_control_enabled;
static uint32_t cpus_offlined;
static DEFINE_MUTEX(core_control_mutex);

static int limit_idx;
static int limit_idx_low;
static int limit_idx_high;
static bool immediately_limit_stop = false;
static struct cpufreq_frequency_table *table;

/* module parameters */
module_param_named(poll_ms, msm_thermal_info.poll_ms, uint, 0664);
module_param_named(limit_temp_degC, msm_thermal_info.limit_temp_degC,
			int, 0664);
module_param_named(temp_hysteresis_degC, msm_thermal_info.temp_hysteresis_degC,
			int, 0664);
module_param_named(freq_step, msm_thermal_info.freq_step,
			int, 0664);
module_param_named(immediately_limit_stop, immediately_limit_stop,
			bool, 0664);
module_param_named(core_limit_temp_degC, msm_thermal_info.core_limit_temp_degC,
			int, 0664);
module_param_named(core_temp_hysteresis_degC,
			msm_thermal_info.core_temp_hysteresis_degC,
			int, 0664);
module_param_named(freq_control_mask, msm_thermal_info.freq_control_mask,
			uint, 0664);
module_param_named(core_control_mask, msm_thermal_info.core_control_mask,
			uint, 0664);

module_param_named(thermal_limit_high, limit_idx_high, int, 0664);
module_param_named(thermal_limit_low, limit_idx_low, int, 0664);

static int msm_thermal_get_freq_table(void)
{
	int ret = 0;
	int i = 0;

	table = cpufreq_frequency_get_table(0);
	if (table == NULL) {
		pr_debug("%s: error reading cpufreq table\n", KBUILD_MODNAME);
		ret = -EINVAL;
		goto fail;
	}

	while (table[i].frequency != CPUFREQ_TABLE_END)
		i++;

	limit_idx_low = 9; // 918000

	limit_idx_high = limit_idx = i - 1;
	BUG_ON(limit_idx_high <= 0 || limit_idx_high <= limit_idx_low);
fail:
	return ret;
}

static int update_cpu_max_freq(int cpu, uint32_t max_freq)
{
	int ret = 0;

	ret = msm_cpufreq_set_freq_limits(cpu, MSM_CPUFREQ_NO_LIMIT, max_freq);
	if (ret)
		return ret;

	limited_max_freq_thermal = max_freq;
	if (max_freq != MSM_CPUFREQ_NO_LIMIT)
		pr_info("%s: Limiting cpu%d max frequency to %d\n",
				KBUILD_MODNAME, cpu, max_freq);
	else
		pr_info("%s: Max frequency reset for cpu%d\n",
				KBUILD_MODNAME, cpu);

#if 0
	/* Frequency of cpu target is already limited so this following code is useless */
	if (cpu_online(cpu)) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return ret;
		ret = cpufreq_driver_target(policy, policy->cur,
				CPUFREQ_RELATION_H);
		cpufreq_cpu_put(policy);
	}
#endif

	return ret;
}

#ifdef CONFIG_SMP
static void __cpuinit do_core_control(long temp)
{
	int i = 0;
	int ret = 0;

	if (!core_control_enabled)
		return;

	/**
	 *  Offline cores starting from the max MPIDR to 1, when above limit,
	 *  The core control mask is non zero and allows the core to be turned
	 *  off.
	 *  The core was not previously offlined by this module
	 *  The core is the next in sequence.
	 *  If the core was online for some reason, even after it was offlined
	 *  by this module, offline it again.
	 *  Online the back on if the temp is below the hysteresis and was
	 *  offlined by this module and not already online.
	 */
	mutex_lock(&core_control_mutex);
	if (msm_thermal_info.core_control_mask &&
		temp >= msm_thermal_info.core_limit_temp_degC) {
		for (i = num_possible_cpus(); i > 0; i--) {
			if (!(msm_thermal_info.core_control_mask & BIT(i)))
				continue;
			if (cpus_offlined & BIT(i) && !cpu_online(i))
				continue;
			pr_info("%s: Set Offline: CPU%d Temp: %ld\n",
					KBUILD_MODNAME, i, temp);
			ret = cpu_down(i);
			if (ret)
				pr_err("%s: Error %d offline core %d\n",
					KBUILD_MODNAME, ret, i);
			cpus_offlined |= BIT(i);
			break;
		}
	} else if (msm_thermal_info.core_control_mask && cpus_offlined &&
		temp <= (msm_thermal_info.core_limit_temp_degC -
			msm_thermal_info.core_temp_hysteresis_degC)) {
		for (i = 0; i < num_possible_cpus(); i++) {
			if (!(cpus_offlined & BIT(i)))
				continue;
			cpus_offlined &= ~BIT(i);
			pr_info("%s: Allow Online CPU%d Temp: %ld\n",
					KBUILD_MODNAME, i, temp);
			/* If this core is already online, then bring up the
			 * next offlined core.
			 */
			if (cpu_online(i))
				continue;
			ret = cpu_up(i);
			if (ret)
				pr_err("%s: Error %d online core %d\n",
						KBUILD_MODNAME, ret, i);
			break;
		}
	}
	mutex_unlock(&core_control_mutex);
}
#else
static void do_core_control(long temp)
{
	return;
}
#endif

static void __cpuinit do_freq_control(long temp)
{
	int ret = 0;
	int cpu = 0;
	uint32_t max_freq = limited_max_freq_thermal;

	if (msm_thermal_info.limit_temp_degC > 75)
		msm_thermal_info.limit_temp_degC = 75;

#if 0
	printk(KERN_ERR "pre-check do_freq_control temp[%u], limit_idx[%u], limit_idx_low[%u], limited_idx_high[%u]\n", temp, limit_idx, limit_idx_low, limit_idx_high);
#endif

	if (temp >= msm_thermal_info.limit_temp_degC) {
		if (limit_idx == limit_idx_low)
			return;

		limit_idx -= msm_thermal_info.freq_step;
		if (limit_idx < limit_idx_low)
			limit_idx = limit_idx_low;
		max_freq = table[limit_idx].frequency;
	} else if (temp < msm_thermal_info.limit_temp_degC -
			msm_thermal_info.temp_hysteresis_degC) {
		if (limit_idx == limit_idx_high)
			return;

		limit_idx += msm_thermal_info.freq_step;

		if (limit_idx >= limit_idx_high || immediately_limit_stop == true) {
			limit_idx = limit_idx_high;
			max_freq = MSM_CPUFREQ_NO_LIMIT;
		} else
			max_freq = table[limit_idx].frequency;
	}

#if 0
	printk(KERN_ERR "do_freq_control temp[%u], limit_idx[%u], max_freq[%u], limited_max_freq_thermal[%u]\n", temp, limit_idx, max_freq, limited_max_freq_thermal);
#endif

	if (max_freq == limited_max_freq_thermal)
		return;

	for_each_possible_cpu(cpu) {
		if (!(msm_thermal_info.freq_control_mask & BIT(cpu)))
			continue;
		ret = update_cpu_max_freq(cpu, max_freq);
		if (ret)
			pr_debug(
			"%s: Unable to limit cpu%d max freq to %d\n",
					KBUILD_MODNAME, cpu, max_freq);
	}
}

static void __cpuinit check_temp(struct work_struct *work)
{
	static int limit_init;
	struct tsens_device tsens_dev;
	long temp = 0;
	int ret = 0;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	ret = tsens_get_temp(&tsens_dev, &temp);
	if (ret) {
		pr_debug("%s: Unable to read TSENS sensor %d\n",
				KBUILD_MODNAME, tsens_dev.sensor_num);
		goto reschedule;
	}

#ifdef CONFIG_INTELLI_THERMAL_STATS
	if (hist_index < MAX_HISTORY_SZ)
		msm_thermal_stats.temp_history[hist_index] = temp;
	else {
		hist_index = 0;
		msm_thermal_stats.temp_history[hist_index] = temp;
	}
	hist_index++;
#endif

	if (!limit_init) {
		ret = msm_thermal_get_freq_table();
		if (ret)
			goto reschedule;
		else
			limit_init = 1;
	}

	do_core_control(temp);
	do_freq_control(temp);
	/* pr_info("msm_thermal: worker is alive!\n"); */
reschedule:
	if (enabled) {
		schedule_delayed_work(&check_temp_work,
						msecs_to_jiffies(msm_thermal_info.poll_ms));
	}
}

static int __cpuinit msm_thermal_cpu_callback(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;

	if (action == CPU_UP_PREPARE || action == CPU_UP_PREPARE_FROZEN) {
		if (core_control_enabled &&
			(msm_thermal_info.core_control_mask & BIT(cpu)) &&
			(cpus_offlined & BIT(cpu))) {
#if 0
			pr_info(
			"%s: Preventing cpu%d from coming online.\n",
				KBUILD_MODNAME, cpu);
#endif
			return NOTIFY_BAD;
		}
	}


	return NOTIFY_OK;
}

static struct notifier_block __refdata msm_thermal_cpu_notifier = {
	.notifier_call = msm_thermal_cpu_callback,
};

/**
 * We will reset the cpu frequencies limits here. The core online/offline
 * status will be carried over to the process stopping the msm_thermal, as
 * we dont want to online a core and bring in the thermal issues.
 */
static void __cpuinit disable_msm_thermal(void)
{
	int cpu = 0;

	/* make sure check_temp is no longer running */
	cancel_delayed_work_sync(&check_temp_work);

	if (limited_max_freq_thermal == MSM_CPUFREQ_NO_LIMIT)
		return;

	for_each_possible_cpu(cpu) {
		update_cpu_max_freq(cpu, MSM_CPUFREQ_NO_LIMIT);
	}
}

static int __cpuinit set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret = 0;

	if (*val == '0' || *val == 'n' || *val == 'N') {
		enabled = 0;
		disable_msm_thermal();
		pr_info("msm_thermal: disabling...\n");
	} else {
		if (!enabled) {
			enabled = 1;
			schedule_delayed_work(&check_temp_work, 10);
			pr_info("msm_thermal: rescheduling...\n");
		} else
			pr_info("msm_thermal: already running...\n");
	}
	pr_info("%s: enabled = %d\n", KBUILD_MODNAME, enabled);
	ret = param_set_bool(val, kp);

	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0664);
MODULE_PARM_DESC(enabled, "enforce thermal limit on cpu");

#ifdef CONFIG_INTELLI_THERMAL_STATS
static ssize_t show_thermal_stats(struct kobject *kobj,
                struct kobj_attribute *attr, char *buf)
{

	int i = 0;
	int tmp = 0;

	/* clear out old stats */
	msm_thermal_stats.throttled = 0;
	msm_thermal_stats.warning = 0;
	msm_thermal_stats.normal = 0;

	for (i = 0; i < MAX_HISTORY_SZ; i++) {
		tmp = msm_thermal_stats.temp_history[i];
		if (tmp >= msm_thermal_info.limit_temp_degC)
			msm_thermal_stats.throttled++;
		else if (tmp < msm_thermal_info.limit_temp_degC &&
			 tmp >= (msm_thermal_info.limit_temp_degC -
				 msm_thermal_info.temp_hysteresis_degC))
			msm_thermal_stats.warning++;
		else
			msm_thermal_stats.normal++;
	}
        return snprintf(buf, PAGE_SIZE, "%u %u %u\n",
			msm_thermal_stats.throttled,
			msm_thermal_stats.warning,
			msm_thermal_stats.normal);
}
static __refdata struct kobj_attribute msm_thermal_stat_attr =
__ATTR(statistics, 0444, show_thermal_stats, NULL);

static __refdata struct attribute *msm_thermal_stat_attrs[] = {
        &msm_thermal_stat_attr.attr,
        NULL,
};

static __refdata struct attribute_group msm_thermal_stat_attr_group = {
        .attrs = msm_thermal_stat_attrs,
};

static __init int msm_thermal_add_stat_nodes(void)
{
	struct kobject *module_kobj = NULL;
	struct kobject *stat_kobj = NULL;
	int ret = 0;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("%s: cannot find kobject for module\n",
			KBUILD_MODNAME);
		ret = -ENOENT;
		goto done_stat_nodes;
	}

	stat_kobj = kobject_create_and_add("thermal_stats", module_kobj);
	if (!stat_kobj) {
		pr_err("%s: cannot create core control kobj\n",
				KBUILD_MODNAME);
		ret = -ENOMEM;
		goto done_stat_nodes;
	}

	ret = sysfs_create_group(stat_kobj, &msm_thermal_stat_attr_group);
	if (ret) {
		pr_err("%s: cannot create group\n", KBUILD_MODNAME);
		goto done_stat_nodes;
	}

	return 0;

done_stat_nodes:
	if (stat_kobj)
		kobject_del(stat_kobj);
	return ret;
}
#endif

#ifdef CONFIG_SMP
/* Call with core_control_mutex locked */
static int __cpuinit update_offline_cores(int val)
{
	int cpu = 0;
	int ret = 0;

	cpus_offlined = msm_thermal_info.core_control_mask & val;
	if (!core_control_enabled)
		return 0;

	for_each_possible_cpu(cpu) {
		if (!(cpus_offlined & BIT(cpu)))
			continue;
		if (!cpu_online(cpu))
			continue;
		ret = cpu_down(cpu);
		if (ret)
			pr_err("%s: Unable to offline cpu%d\n",
				KBUILD_MODNAME, cpu);
	}
	return ret;
}
#else
static int update_offline_cores(int val)
{
	return 0;
}
#endif

static ssize_t show_cc_enabled(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", core_control_enabled);
}

static ssize_t __cpuinit store_cc_enabled(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int val = 0;

	mutex_lock(&core_control_mutex);
	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		pr_err("%s: Invalid input %s\n", KBUILD_MODNAME, buf);
		goto done_store_cc;
	}

	if (core_control_enabled == !!val)
		goto done_store_cc;

	core_control_enabled = !!val;
	if (core_control_enabled) {
		pr_info("%s: Core control enabled\n", KBUILD_MODNAME);
		register_cpu_notifier(&msm_thermal_cpu_notifier);
		update_offline_cores(cpus_offlined);
	} else {
		pr_info("%s: Core control disabled\n", KBUILD_MODNAME);
		unregister_cpu_notifier(&msm_thermal_cpu_notifier);
	}

done_store_cc:
	mutex_unlock(&core_control_mutex);
	return count;
}

static ssize_t show_cpus_offlined(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", cpus_offlined);
}

static ssize_t __cpuinit store_cpus_offlined(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	uint32_t val = 0;

	mutex_lock(&core_control_mutex);
	ret = kstrtouint(buf, 10, &val);
	if (ret) {
		pr_err("%s: Invalid input %s\n", KBUILD_MODNAME, buf);
		goto done_cc;
	}

	if (enabled) {
		pr_err("%s: Ignoring request; polling thread is enabled.\n",
				KBUILD_MODNAME);
		goto done_cc;
	}

	if (cpus_offlined == val)
		goto done_cc;

	update_offline_cores(val);
done_cc:
	mutex_unlock(&core_control_mutex);
	return count;
}

static __cpuinitdata struct kobj_attribute cc_enabled_attr =
__ATTR(enabled, 0664, show_cc_enabled, store_cc_enabled);

static __cpuinitdata struct kobj_attribute cpus_offlined_attr =
__ATTR(cpus_offlined, 0664, show_cpus_offlined, store_cpus_offlined);

static __cpuinitdata struct attribute *cc_attrs[] = {
	&cc_enabled_attr.attr,
	&cpus_offlined_attr.attr,
	NULL,
};

static __cpuinitdata struct attribute_group cc_attr_group = {
	.attrs = cc_attrs,
};

static __init int msm_thermal_add_cc_nodes(void)
{
	struct kobject *module_kobj = NULL;
	struct kobject *cc_kobj = NULL;
	int ret = 0;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("%s: cannot find kobject for module\n",
			KBUILD_MODNAME);
		ret = -ENOENT;
		goto done_cc_nodes;
	}

	cc_kobj = kobject_create_and_add("core_control", module_kobj);
	if (!cc_kobj) {
		pr_err("%s: cannot create core control kobj\n",
				KBUILD_MODNAME);
		ret = -ENOMEM;
		goto done_cc_nodes;
	}

	ret = sysfs_create_group(cc_kobj, &cc_attr_group);
	if (ret) {
		pr_err("%s: cannot create group\n", KBUILD_MODNAME);
		goto done_cc_nodes;
	}

	return 0;

done_cc_nodes:
	if (cc_kobj)
		kobject_del(cc_kobj);
	return ret;
}

int __init msm_thermal_init(struct msm_thermal_data *pdata)
{
	int ret = 0;

	enabled = 1;
	if (num_possible_cpus() > 1)
		core_control_enabled = 1;

	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	schedule_delayed_work(&check_temp_work, 10);

	if (num_possible_cpus() > 1)
		register_cpu_notifier(&msm_thermal_cpu_notifier);

	return ret;
}

int __init msm_thermal_late_init(void)
{
	if (num_possible_cpus() > 1)
		msm_thermal_add_cc_nodes();
#ifdef CONFIG_INTELLI_THERMAL_STATS
	msm_thermal_add_stat_nodes();
#endif

	return 0;
}
late_initcall(msm_thermal_late_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Praveen Chidambaram <pchidamb@codeaurora.org>");
MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("intelligent thermal driver for Qualcomm based SOCs");
MODULE_DESCRIPTION("originally from Qualcomm's open source repo");
