/*
 * Copyright (c) 2012-2013 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <linux/kernel.h>
#include <linux/cpuquiet.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/sched.h>

// from cpuquiet.c
extern unsigned int cpq_max_cpus(void);
extern unsigned int cpq_min_cpus(void);
// from cpuquiet_driver.c
extern unsigned int best_core_to_turn_up (void);
//from core.c
extern unsigned long avg_nr_running(void);
extern unsigned long avg_cpu_nr_running(unsigned int cpu);

// from sysfs.c
extern unsigned int gov_enabled;

typedef enum {
	DISABLED,
	IDLE,
	RUNNING,
} RUNNABLES_STATE;

static struct delayed_work runnables_work;
static struct kobject *runnables_kobject;

/* configurable parameters */
static unsigned int sample_rate = 20;		/* msec */

static RUNNABLES_STATE runnables_state;
static struct workqueue_struct *runnables_wq;

#define NR_FSHIFT_EXP	3
#define NR_FSHIFT	(1 << NR_FSHIFT_EXP)
/* avg run threads * 8 (e.g., 11 = 1.375 threads) */
static unsigned int default_thresholds[] = {
	10, 18, 20, UINT_MAX
};

static unsigned int nr_run_last;
static unsigned int nr_run_hysteresis = 2;		/* 1 / 2 thread */
static unsigned int default_threshold_level = 4;	/* 1 / 4 thread */
static unsigned int nr_run_thresholds[NR_CPUS];

DEFINE_MUTEX(runnables_lock);

/* EXP = alpha in the exponential moving average.
 * Alpha = e ^ (-sample_rate / window_size) * FIXED_1
 * Calculated for sample_rate of 20ms, window size of 100ms
 */
#define EXP    1677

static int get_action(unsigned int nr_run)
{
	unsigned int nr_cpus = num_online_cpus();
	int max_cpus = cpq_max_cpus();
	int min_cpus = cpq_min_cpus();
	
	if ((nr_cpus > max_cpus || nr_run < nr_cpus) && nr_cpus >= min_cpus)
		return -1;

	if (nr_cpus < min_cpus || nr_run > nr_cpus)
		return 1;

	return 0;
}

static void runnables_avg_sampler(unsigned long data)
{
	unsigned int nr_run, avg_nr_run;
	int action;

	rmb();
	if (runnables_state != RUNNING)
		return;

	avg_nr_run = avg_nr_running();

	for (nr_run = 1; nr_run < ARRAY_SIZE(nr_run_thresholds); nr_run++) {
		unsigned int nr_threshold = nr_run_thresholds[nr_run - 1];
		if (nr_run_last <= nr_run)
			nr_threshold += NR_FSHIFT / nr_run_hysteresis;
		if (avg_nr_run <= (nr_threshold << (FSHIFT - NR_FSHIFT_EXP)))
			break;
	}

	nr_run_last = nr_run;

	action = get_action(nr_run);
	if (action != 0) {
		wmb();
	}

	queue_delayed_work(runnables_wq, &runnables_work,
				msecs_to_jiffies(sample_rate));
}

static unsigned int get_lightest_loaded_cpu_n(void)
{
	unsigned long min_avg_runnables = ULONG_MAX;
	unsigned int cpu = nr_cpu_ids;
	int i;

	for_each_online_cpu(i) {
		unsigned int nr_runnables = avg_cpu_nr_running(i);

		if (i > 0 && min_avg_runnables > nr_runnables) {
			cpu = i;
			min_avg_runnables = nr_runnables;
		}
	}

	return cpu;
}

static void runnables_work_func(struct work_struct *work)
{
	unsigned int cpu = nr_cpu_ids;
	int action;

	if (!gov_enabled)
		return;

	if (runnables_state != RUNNING)
		return;

	runnables_avg_sampler(0);

	action = get_action(nr_run_last);
	if (action > 0) {
		cpu = best_core_to_turn_up ();
		if (cpu < nr_cpu_ids)
			cpuquiet_wake_cpu(cpu);
	} else if (action < 0) {
		cpu = get_lightest_loaded_cpu_n();
		if (cpu < nr_cpu_ids)
			cpuquiet_quiesence_cpu(cpu);
	}
}

#define MAX_BYTES 100

static ssize_t show_thresholds(struct cpuquiet_attribute *attr, char *buf)
{
	char buffer[MAX_BYTES];
	unsigned int i;
	int size = 0;
	buffer[0] = 0;
	for_each_possible_cpu(i) {
		if (i == ARRAY_SIZE(nr_run_thresholds) - 1)
			break;
		if (size >= sizeof(buffer))
			break;
		size += snprintf(buffer + size, sizeof(buffer) - size,
			 "%u->%u core threshold: %u\n",
			  i + 1, i + 2, nr_run_thresholds[i]);
	}
	return snprintf(buf, sizeof(buffer), "%s", buffer);
}

static ssize_t store_thresholds(struct cpuquiet_attribute *attr,
					const char *buf, size_t count)
{
	int ret, i = 0;
	char *val, *str, input[MAX_BYTES];
	unsigned int thresholds[CONFIG_NR_CPUS];
	
	if (!count || count >= MAX_BYTES)
		return -EINVAL;
	strncpy(input, buf, count);
	input[count] = '\0';
	str = input;
	memcpy(thresholds, nr_run_thresholds, sizeof(nr_run_thresholds));
	while ((val = strsep(&str, " ")) != NULL) {
		if (*val == '\0')
			continue;
		if (i == ARRAY_SIZE(nr_run_thresholds) - 1)
			break;
		ret = kstrtouint(val, 10, &thresholds[i]);
		if (ret)
		return -EINVAL;
		i++;
	}

	memcpy(nr_run_thresholds, thresholds, sizeof(thresholds));
	return count;
}

CPQ_BASIC_ATTRIBUTE(sample_rate, 0644, uint);
CPQ_BASIC_ATTRIBUTE(nr_run_hysteresis, 0644, uint);
CPQ_ATTRIBUTE_CUSTOM(nr_run_thresholds, 0644,
			show_thresholds, store_thresholds);

static struct attribute *runnables_attributes[] = {
	&sample_rate_attr.attr,
	&nr_run_hysteresis_attr.attr,
	&nr_run_thresholds_attr.attr,
	NULL,
};

static const struct sysfs_ops runnables_sysfs_ops = {
	.show = cpuquiet_auto_sysfs_show,
	.store = cpuquiet_auto_sysfs_store,
};

static struct kobj_type ktype_runnables = {
	.sysfs_ops = &runnables_sysfs_ops,
	.default_attrs = runnables_attributes,
};

static int runnables_sysfs(void)
{
	int err;

	runnables_kobject = kzalloc(sizeof(*runnables_kobject),
				GFP_KERNEL);

	if (!runnables_kobject)
		return -ENOMEM;

	err = cpuquiet_kobject_init(runnables_kobject, &ktype_runnables,
				"runnable_threads");

	if (err)
		kfree(runnables_kobject);

	return err;
}

static void runnables_device_busy(void)
{
	mutex_lock(&runnables_lock);
	if (runnables_state == RUNNING) {
		runnables_state = IDLE;
		cancel_delayed_work_sync(&runnables_work);
	}
	mutex_unlock(&runnables_lock);
}

static void runnables_device_free(void)
{
	mutex_lock(&runnables_lock);
	if (runnables_state == IDLE) {
		runnables_state = RUNNING;
		runnables_work_func(NULL);
	}
	mutex_unlock(&runnables_lock);
}

static void runnables_stop(void)
{
	mutex_lock(&runnables_lock);

	runnables_state = DISABLED;
	cancel_delayed_work_sync(&runnables_work);
	destroy_workqueue(runnables_wq);
	kobject_put(runnables_kobject);
	kfree(runnables_kobject);
	
	mutex_unlock(&runnables_lock);
}

static int runnables_start(void)
{
	int err, i;

	err = runnables_sysfs();
	if (err)
		return err;

	runnables_wq = alloc_workqueue("cpuquiet-runnables", WQ_HIGHPRI, 0);
	if (!runnables_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&runnables_work, runnables_work_func);

	for(i = 0; i < ARRAY_SIZE(nr_run_thresholds); ++i) {
		if (i == (ARRAY_SIZE(nr_run_thresholds) - 1))
			nr_run_thresholds[i] = UINT_MAX;
		else if (i < ARRAY_SIZE(default_thresholds))
			nr_run_thresholds[i] = default_thresholds[i];
		else
			nr_run_thresholds[i] = i + 1 +
				NR_FSHIFT / default_threshold_level;
	}

	mutex_lock(&runnables_lock);
	runnables_state = RUNNING;
	mutex_unlock(&runnables_lock);

    runnables_work_func(NULL);

	return 0;
}

struct cpuquiet_governor runnables_governor = {
	.name		   	  = "runnable",
	.start			  = runnables_start,
	.device_free_notification = runnables_device_free,
	.device_busy_notification = runnables_device_busy,
	.stop			  = runnables_stop,
	.owner		   	  = THIS_MODULE,
};

static int __init init_runnables(void)
{
	return cpuquiet_register_governor(&runnables_governor);
}

static void __exit exit_runnables(void)
{
	cpuquiet_unregister_governor(&runnables_governor);
}

MODULE_LICENSE("GPL");
module_init(init_runnables);
module_exit(exit_runnables);