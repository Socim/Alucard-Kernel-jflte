/*
 * Author: Alucard_24@XDA
 *
 * Copyright 2012 Alucard_24@XDA
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "acpuclock.h"

#if defined(CONFIG_POWERSUSPEND)
#include <linux/powersuspend.h>
#elif defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#endif  /* CONFIG_POWERSUSPEND || CONFIG_HAS_EARLYSUSPEND */

static struct workqueue_struct *alucardhp_wq;

static struct hotplug_cpuinfo {
#ifndef CONFIG_ALUCARD_HOTPLUG_USE_CPU_UTIL
	u64 prev_cpu_wall;
	u64 prev_cpu_idle;
#endif
	unsigned int cpu;
	unsigned int cpu_up_rate;
	unsigned int cpu_down_rate;
	struct delayed_work work;
	struct mutex timer_mutex;
};

static DEFINE_PER_CPU(struct hotplug_cpuinfo, od_hotplug_cpuinfo);

static bool suspended = false;

static struct hotplug_tuners {
	unsigned int hotplug_sampling_rate;
	unsigned int hotplug_enable;
	unsigned int maxcoreslimit;
	unsigned int maxcoreslimit_sleep;
} hotplug_tuners_ins = {
	.hotplug_sampling_rate = 60,
	.hotplug_enable = 0,
	.maxcoreslimit = NR_CPUS,
	.maxcoreslimit_sleep = 1,
};

#define MAX_HOTPLUG_RATE	(40)
#define DOWN_INDEX		(0)
#define UP_INDEX		(1)

static struct runqueue_data {
	unsigned int nr_run_avg;
	int64_t last_time;
	int64_t total_time;
	spinlock_t lock;
};

static struct runqueue_data *rq_data;

static void init_rq_avg_stats(void)
{
	rq_data->nr_run_avg = 0;
	rq_data->last_time = 0;
	rq_data->total_time = 0;

	return;
}

static int __init init_rq_avg(void)
{
	rq_data = kzalloc(sizeof(struct runqueue_data), GFP_KERNEL);
	if (rq_data == NULL) {
		pr_err("%s cannot allocate memory\n", __func__);
		return -ENOMEM;
	}
	spin_lock_init(&rq_data->lock);

	return 0;
}

static int __exit exit_rq_avg(void)
{
	kfree(rq_data);

	return 0;
}

static unsigned int get_nr_run_avg(void)
{
	int64_t time_diff = 0;
	int64_t nr_run = 0;
	unsigned long flags = 0;
	int64_t cur_time;
	unsigned int nr_run_avg;

	cur_time = ktime_to_ns(ktime_get());

	spin_lock_irqsave(&rq_data->lock, flags);

	if (rq_data->last_time == 0)
		rq_data->last_time = cur_time;
	if (rq_data->nr_run_avg == 0)
		rq_data->total_time = 0;

	nr_run = nr_running() * 100;
	time_diff = cur_time - rq_data->last_time;
	do_div(time_diff, 1000 * 1000);

	if (time_diff != 0 && rq_data->total_time != 0) {
		nr_run = (nr_run * time_diff) +
			(rq_data->nr_run_avg * rq_data->total_time);
		do_div(nr_run, rq_data->total_time + time_diff);
	}
	rq_data->nr_run_avg = nr_run;
	rq_data->total_time += time_diff;
	rq_data->last_time = cur_time;

	nr_run_avg = rq_data->nr_run_avg;
	rq_data->nr_run_avg = 0;

	spin_unlock_irqrestore(&rq_data->lock, flags);

	return nr_run_avg;
}

static int hotplug_freq[4][2] = {
	{0, 1242000},
	{810000, 1566000},
	{918000, 1674000},
	{1026000, 0}
};
static int hotplug_load[4][2] = {
	{0, 60},
	{30, 65},
	{30, 65},
	{30, 0}
};
static int hotplug_rq[4][2] = {
	{0, 100},
	{100, 200},
	{200, 300},
	{300, 0}
};

static unsigned int hotplug_rate[4][2] = {
	{1, 1},
	{4, 2},
	{4, 2},
	{4, 1}
};

static bool hotplug_work_fn(struct hotplug_cpuinfo *pcpu_info)
{
#ifndef CONFIG_ALUCARD_HOTPLUG_USE_CPU_UTIL
	u64 cur_wall_time, cur_idle_time;
	unsigned int wall_time, idle_time;
#endif
	unsigned int upmaxcoreslimit = 0;
	unsigned int up_rate;
	unsigned int down_rate;
	unsigned int up_load, up_freq, up_rq;
	unsigned int down_load, down_freq, down_rq;
	bool check_up = false, check_down = false;
	unsigned int cpu;
	unsigned int next_cpu;
	unsigned int rq_avg;
	int cur_load = -1;
	unsigned int cur_freq = 0;
	int ret = 0;

	if (suspended)
		upmaxcoreslimit = hotplug_tuners_ins.maxcoreslimit_sleep;
	else
		upmaxcoreslimit = hotplug_tuners_ins.maxcoreslimit;

	cpu = pcpu_info->cpu;
	next_cpu = (cpu + 1);

	up_load = hotplug_load[cpu][UP_INDEX];
	down_load = hotplug_load[cpu][DOWN_INDEX];

	up_freq = hotplug_freq[cpu][UP_INDEX];
	down_freq = hotplug_freq[cpu][DOWN_INDEX];

	up_rq = hotplug_rq[cpu][UP_INDEX];
	down_rq = hotplug_rq[cpu][DOWN_INDEX];

	up_rate = hotplug_rate[cpu][UP_INDEX];
	down_rate = hotplug_rate[cpu][DOWN_INDEX];

	if (pcpu_info->cpu_up_rate > up_rate)
		pcpu_info->cpu_up_rate = 1;

	if (pcpu_info->cpu_down_rate > down_rate)
		pcpu_info->cpu_down_rate = 1;

	check_up = (pcpu_info->cpu_up_rate % up_rate == 0);
	check_down = (pcpu_info->cpu_down_rate % down_rate == 0);

	rq_avg = get_nr_run_avg();

#ifdef CONFIG_ALUCARD_HOTPLUG_USE_CPU_UTIL
	cur_load = cpufreq_quick_get_util(cpu);
#else
	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, 0);

	wall_time = (unsigned int)
			(cur_wall_time - pcpu_info->prev_cpu_wall);
	pcpu_info->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int)
			(cur_idle_time - pcpu_info->prev_cpu_idle);
	pcpu_info->prev_cpu_idle = cur_idle_time;

	/* if wall_time < idle_time, evaluate cpu load next time */
	if (wall_time >= idle_time) {
		/*
		 * if wall_time is equal to idle_time,
		 * cpu_load is equal to 0
		 */
		cur_load = wall_time > idle_time ? (100 *
			(wall_time - idle_time)) / wall_time : 0;
	}
#endif

	cur_freq = acpuclk_get_rate(cpu);

	/* if cur_load < 0, evaluate cpu load next time */
	if (cur_load >= 0) {
		if (next_cpu < upmaxcoreslimit
			&& cur_load >= up_load
			&& cur_freq >= up_freq
			&& rq_avg > up_rq
			&& !cpu_online(next_cpu)) {
				if (check_up) {
					ret = cpu_up(next_cpu);
					if (!ret)
						pcpu_info->cpu_up_rate = 1;
				} else {
					++pcpu_info->cpu_up_rate;
				}
		} else if (cpu > 0) {
				if (cur_freq <= down_freq
					|| (cur_load < down_load
					&& rq_avg <= down_rq)) {
					if (check_down)
						return false;
					else
						++pcpu_info->cpu_down_rate;			
				}
		}
		//pr_info("CPU[%u], cpu_up_rate[%u], check_up[%u], up_rate[%u]\n", cpu, pcpu_info->cpu_up_rate, check_up, up_rate);
		//pr_info("CPU[%u], cpu_down_rate[%u], check_down[%u], down_rate[%u]\n", cpu, pcpu_info->cpu_down_rate, check_down, down_rate);
	}

	return true;
}

static int __cpuinit hotplug_timer_fn(struct work_struct *work)
{
	struct hotplug_cpuinfo *pcpu_info = container_of(work, struct hotplug_cpuinfo, work.work);
	int delay;
	unsigned int cpu;
	int sampling_rate;
	int ret = 0;

	mutex_lock(&pcpu_info->timer_mutex);

	cpu = pcpu_info->cpu;

	sampling_rate = hotplug_tuners_ins.hotplug_sampling_rate;
	delay = msecs_to_jiffies(sampling_rate);

	if (hotplug_work_fn(pcpu_info) == false) {
		mutex_unlock(&pcpu_info->timer_mutex);
		ret = cpu_down(cpu);
		if (!ret)
			return ret;
		else
			mutex_lock(&pcpu_info->timer_mutex);
	}

	queue_delayed_work_on(cpu, alucardhp_wq, &pcpu_info->work, delay);

	mutex_unlock(&pcpu_info->timer_mutex);

	return ret;
}

#if defined(CONFIG_POWERSUSPEND) || defined(CONFIG_HAS_EARLYSUSPEND)
#ifdef CONFIG_POWERSUSPEND
static void __ref alucard_hotplug_suspend(struct power_suspend *handler)
#else
static void __ref alucard_hotplug_suspend(struct early_suspend *handler)
#endif
{
	if (hotplug_tuners_ins.hotplug_enable > 0) {
		suspended = true;
	}
}

#ifdef CONFIG_POWERSUSPEND
static void __cpuinit alucard_hotplug_resume(struct power_suspend *handler)
#else
static void __cpuinit alucard_hotplug_resume(
				struct early_suspend *handler)
#endif
{
	int maxcoreslimit = 0;
	unsigned int i = 0;

	if (hotplug_tuners_ins.hotplug_enable > 0) {
		/* wake up everyone */
		maxcoreslimit = hotplug_tuners_ins.maxcoreslimit;

		suspended = false;

		for (i = 1; i < maxcoreslimit; i++) {
			if (!cpu_online(i))
				cpu_up(i);
		}
	}
}

#ifdef CONFIG_POWERSUSPEND
static struct power_suspend alucard_hotplug_suspend_driver = {
#else
static struct early_suspend alucard_hotplug_suspend_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
#endif
	.suspend = alucard_hotplug_suspend,
	.resume = alucard_hotplug_resume,
};
#endif  /* CONFIG_POWERSUSPEND || CONFIG_HAS_EARLYSUSPEND */

static void up_cpu_work(unsigned int cpu)
{
	struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);
	int delay;

	mutex_init(&pcpu_info->timer_mutex);

	pcpu_info->cpu_up_rate = 1;

	delay = msecs_to_jiffies(hotplug_tuners_ins.hotplug_sampling_rate);

	INIT_DELAYED_WORK(&pcpu_info->work, hotplug_timer_fn);
	queue_delayed_work_on(cpu, alucardhp_wq, &pcpu_info->work, delay);
}

static void down_cpu_work(unsigned int cpu)
{
	struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);

	cancel_delayed_work(&pcpu_info->work);

	mutex_destroy(&pcpu_info->timer_mutex);

	pcpu_info->cpu_down_rate = 1;	
}

static int __cpuinit alucard_hotplug_callback(struct notifier_block *nb,
			unsigned long val, void *data)
{
	unsigned int cpu = (unsigned long)data;

	switch (val) {
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		up_cpu_work(cpu);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		down_cpu_work(cpu);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata alucard_hotplug_nb =
{
   .notifier_call = alucard_hotplug_callback,
};

static int hotplug_start()
{
	unsigned int cpu;
	int ret = 0;
	int delay = msecs_to_jiffies(hotplug_tuners_ins.hotplug_sampling_rate);

	alucardhp_wq = alloc_workqueue("alucardplug",
				WQ_HIGHPRI, 0);

	if (!alucardhp_wq) {
		printk(KERN_ERR "Failed to create \
				alucardhp workqueue\n");
		return -EFAULT;
	}

	ret = init_rq_avg();
	if (ret) {
		destroy_workqueue(alucardhp_wq);
		return ret;
	}

	suspended = false;

	init_rq_avg_stats();

	get_online_cpus();

	register_hotcpu_notifier(&alucard_hotplug_nb);

	for_each_possible_cpu(cpu) {
		struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);

		pcpu_info->prev_cpu_idle = get_cpu_idle_time(cpu,
				&pcpu_info->prev_cpu_wall, 0);

		pcpu_info->cpu = cpu;

		if (cpu_online(cpu)) {
			mutex_init(&pcpu_info->timer_mutex);
			alucard_hotplug_callback(&alucard_hotplug_nb, CPU_UP_PREPARE, cpu);
			alucard_hotplug_callback(&alucard_hotplug_nb, CPU_ONLINE, cpu);
		}
	}

	put_online_cpus();

#if defined(CONFIG_POWERSUSPEND)
	register_power_suspend(&alucard_hotplug_suspend_driver);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	register_early_suspend(&alucard_hotplug_suspend_driver);
#endif  /* CONFIG_POWERSUSPEND || CONFIG_HAS_EARLYSUSPEND */

	return ret;
}

static void hotplug_stop(void)
{
	unsigned int cpu;

#if defined(CONFIG_POWERSUSPEND)
	unregister_power_suspend(&alucard_hotplug_suspend_driver);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&alucard_hotplug_suspend_driver);
#endif  /* CONFIG_POWERSUSPEND || CONFIG_HAS_EARLYSUSPEND */

	get_online_cpus();

	unregister_hotcpu_notifier(&alucard_hotplug_nb);

	for_each_possible_cpu(cpu) {
		struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);

		cancel_delayed_work_sync(&pcpu_info->work);

		mutex_destroy(&pcpu_info->timer_mutex);
	}

	put_online_cpus();

	exit_rq_avg;

	destroy_workqueue(alucardhp_wq);
}

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", \
			hotplug_tuners_ins.object);			\
}

show_one(hotplug_sampling_rate, hotplug_sampling_rate);
show_one(hotplug_enable, hotplug_enable);
show_one(maxcoreslimit, maxcoreslimit);
show_one(maxcoreslimit_sleep, maxcoreslimit_sleep);

#define show_hotplug_param(file_name, num_core, up_down)		\
static ssize_t show_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", \
			file_name[num_core - 1][up_down]);		\
}

#define store_hotplug_param(file_name, num_core, up_down)		\
static ssize_t store_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	if (input == file_name[num_core - 1][up_down]) {		\
		return count;						\
	}								\
	file_name[num_core - 1][up_down] = input;			\
	return count;							\
}

/* hotplug freq */
show_hotplug_param(hotplug_freq, 1, 1);
show_hotplug_param(hotplug_freq, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_freq, 2, 1);
show_hotplug_param(hotplug_freq, 3, 0);
show_hotplug_param(hotplug_freq, 3, 1);
show_hotplug_param(hotplug_freq, 4, 0);
#endif
/* hotplug load */
show_hotplug_param(hotplug_load, 1, 1);
show_hotplug_param(hotplug_load, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_load, 2, 1);
show_hotplug_param(hotplug_load, 3, 0);
show_hotplug_param(hotplug_load, 3, 1);
show_hotplug_param(hotplug_load, 4, 0);
#endif
/* hotplug rq */
show_hotplug_param(hotplug_rq, 1, 1);
show_hotplug_param(hotplug_rq, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_rq, 2, 1);
show_hotplug_param(hotplug_rq, 3, 0);
show_hotplug_param(hotplug_rq, 3, 1);
show_hotplug_param(hotplug_rq, 4, 0);
#endif
/* hotplug rate */
show_hotplug_param(hotplug_rate, 1, 1);
show_hotplug_param(hotplug_rate, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_rate, 2, 1);
show_hotplug_param(hotplug_rate, 3, 0);
show_hotplug_param(hotplug_rate, 3, 1);
show_hotplug_param(hotplug_rate, 4, 0);
#endif
/* hotplug freq */
store_hotplug_param(hotplug_freq, 1, 1);
store_hotplug_param(hotplug_freq, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_freq, 2, 1);
store_hotplug_param(hotplug_freq, 3, 0);
store_hotplug_param(hotplug_freq, 3, 1);
store_hotplug_param(hotplug_freq, 4, 0);
#endif
/* hotplug load */
store_hotplug_param(hotplug_load, 1, 1);
store_hotplug_param(hotplug_load, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_load, 2, 1);
store_hotplug_param(hotplug_load, 3, 0);
store_hotplug_param(hotplug_load, 3, 1);
store_hotplug_param(hotplug_load, 4, 0);
#endif
/* hotplug rq */
store_hotplug_param(hotplug_rq, 1, 1);
store_hotplug_param(hotplug_rq, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_rq, 2, 1);
store_hotplug_param(hotplug_rq, 3, 0);
store_hotplug_param(hotplug_rq, 3, 1);
store_hotplug_param(hotplug_rq, 4, 0);
#endif
/* hotplug rate */
store_hotplug_param(hotplug_rate, 1, 1);
store_hotplug_param(hotplug_rate, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_rate, 2, 1);
store_hotplug_param(hotplug_rate, 3, 0);
store_hotplug_param(hotplug_rate, 3, 1);
store_hotplug_param(hotplug_rate, 4, 0);
#endif

define_one_global_rw(hotplug_freq_1_1);
define_one_global_rw(hotplug_freq_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_freq_2_1);
define_one_global_rw(hotplug_freq_3_0);
define_one_global_rw(hotplug_freq_3_1);
define_one_global_rw(hotplug_freq_4_0);
#endif

define_one_global_rw(hotplug_load_1_1);
define_one_global_rw(hotplug_load_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_load_2_1);
define_one_global_rw(hotplug_load_3_0);
define_one_global_rw(hotplug_load_3_1);
define_one_global_rw(hotplug_load_4_0);
#endif

define_one_global_rw(hotplug_rq_1_1);
define_one_global_rw(hotplug_rq_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_rq_2_1);
define_one_global_rw(hotplug_rq_3_0);
define_one_global_rw(hotplug_rq_3_1);
define_one_global_rw(hotplug_rq_4_0);
#endif

define_one_global_rw(hotplug_rate_1_1);
define_one_global_rw(hotplug_rate_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_rate_2_1);
define_one_global_rw(hotplug_rate_3_0);
define_one_global_rw(hotplug_rate_3_1);
define_one_global_rw(hotplug_rate_4_0);
#endif

static void cpus_hotplugging(bool status) {
	int ret = 0;

	if (status) {
		ret = hotplug_start();
		if (ret)
			status = false;
	} else {
		hotplug_stop();
	}

	hotplug_tuners_ins.hotplug_enable = status;
}

/* hotplug_sampling_rate */
static ssize_t store_hotplug_sampling_rate(struct kobject *a,
				struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input, 10);

	if (input == hotplug_tuners_ins.hotplug_sampling_rate)
		return count;

	hotplug_tuners_ins.hotplug_sampling_rate = input;

	return count;
}

/* hotplug_enable */
static ssize_t store_hotplug_enable(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0;

	if (hotplug_tuners_ins.hotplug_enable == input)
		return count;

	if (input > 0)
		cpus_hotplugging(true);
	else
		cpus_hotplugging(false);

	return count;
}

/* maxcoreslimit */
static ssize_t store_maxcoreslimit(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1);

	if (hotplug_tuners_ins.maxcoreslimit == input)
		return count;

	hotplug_tuners_ins.maxcoreslimit = input;

	return count;
}

/* maxcoreslimit_sleep */
static ssize_t store_maxcoreslimit_sleep(struct kobject *a,
				struct attribute *b,
				const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1);

	if (hotplug_tuners_ins.maxcoreslimit_sleep == input)
		return count;

	hotplug_tuners_ins.maxcoreslimit_sleep = input;

	return count;
}

define_one_global_rw(hotplug_sampling_rate);
define_one_global_rw(hotplug_enable);
define_one_global_rw(maxcoreslimit);
define_one_global_rw(maxcoreslimit_sleep);

static struct attribute *alucard_hotplug_attributes[] = {
	&hotplug_sampling_rate.attr,
	&hotplug_enable.attr,
	&hotplug_freq_1_1.attr,
	&hotplug_freq_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_freq_2_1.attr,
	&hotplug_freq_3_0.attr,
	&hotplug_freq_3_1.attr,
	&hotplug_freq_4_0.attr,
#endif
	&hotplug_load_1_1.attr,
	&hotplug_load_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_load_2_1.attr,
	&hotplug_load_3_0.attr,
	&hotplug_load_3_1.attr,
	&hotplug_load_4_0.attr,
#endif
	&hotplug_rq_1_1.attr,
	&hotplug_rq_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_rq_2_1.attr,
	&hotplug_rq_3_0.attr,
	&hotplug_rq_3_1.attr,
	&hotplug_rq_4_0.attr,
#endif
	&hotplug_rate_1_1.attr,
	&hotplug_rate_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_rate_2_1.attr,
	&hotplug_rate_3_0.attr,
	&hotplug_rate_3_1.attr,
	&hotplug_rate_4_0.attr,
#endif
	&maxcoreslimit.attr,
	&maxcoreslimit_sleep.attr,
	NULL
};

static struct attribute_group alucard_hotplug_attr_group = {
	.attrs = alucard_hotplug_attributes,
	.name = "alucard_hotplug",
};

static int __init alucard_hotplug_init(void)
{
	int ret;

	ret = sysfs_create_group(kernel_kobj, &alucard_hotplug_attr_group);
	if (ret) {
		printk(KERN_ERR "failed at(%d)\n", __LINE__);
		return ret;
	}

	if (hotplug_tuners_ins.hotplug_enable > 0) {
		ret = hotplug_start();
		if (ret) {
			printk(KERN_ERR "hotplug start failed at(%d)\n", __LINE__);
			return ret;
		}
	}

	return ret;
}

static void __exit alucard_hotplug_exit(void)
{
	if (hotplug_tuners_ins.hotplug_enable > 0) {
		hotplug_stop();
	}

	sysfs_remove_group(kernel_kobj, &alucard_hotplug_attr_group);
}
MODULE_AUTHOR("Alucard_24@XDA");
MODULE_DESCRIPTION("'alucard_hotplug' - A cpu hotplug driver for "
	"capable processors");
MODULE_LICENSE("GPL");

late_initcall(alucard_hotplug_init);
late_initexit(alucard_hotplug_exit);
