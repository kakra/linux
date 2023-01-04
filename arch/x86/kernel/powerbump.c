// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2023 Intel Corporation
 *  Author: Arjan van de Ven <arjan@linux.intel.com>
 *
 * Kernel power-bump infrastructructure
 */
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/jiffies.h>

static DEFINE_PER_CPU(unsigned long, bump_timeout); /* jiffies at which the lease for the bump times out */



/*
 * a note about the use of the current cpu versus preemption.
 *
 * Most uses of in_power_bump() are inside local power management code,
 * and are pinned to that cpu already.
 *
 * On the "set" side, interrupt level code is obviously also fully
 * migration-race free.
 *
 * All other cases are exposed to a migration-race.
 * 
 * The goal of powerbump is statistical rather than deterministic,
 * e.g. on average the CPU that hits event X will go towards Y more
 * often than not, and the impact of being wrong is a bit of extra
 * power potentially for some short durations.
 * Weighted against the costs in performance and complexity of dealing
 * with the race, the race condition is acceptable.
 *
 * The second known race is where interrupt context might set a bump
 * time in the middle of process context setting a different but smaller bump time,
 * with the result that process context will win incorrectly, and the
 * actual bump time will be less than expected, but still non-zero.
 * Here also the cost of dealing with the raice is outweight with the
 * limited impact.
 */


int in_power_bump(void)
{
	int cpu = raw_smp_processor_id();
	if (time_before(jiffies, per_cpu(bump_timeout, cpu)))
		return 1;
	
	/* deal with wrap issues by keeping the stored bump value close to current */
	per_cpu(bump_timeout, cpu) = jiffies;
	return 0;
}
EXPORT_SYMBOL_GPL(in_power_bump);

void give_power_bump(int msecs)
{
	unsigned long nextjiffies;
	int cpu;
	/* we need to round up an extra jiffie */
	nextjiffies = jiffies + msecs_to_jiffies(msecs) + 1;

	cpu = raw_smp_processor_id();
	if (time_before(per_cpu(bump_timeout, cpu), nextjiffies))
		 per_cpu(bump_timeout, cpu) = nextjiffies;
	
}
EXPORT_SYMBOL_GPL(give_power_bump);

static __init int powerbump_init(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		per_cpu(bump_timeout, cpu) = jiffies;
	}

	return 0;
}

late_initcall(powerbump_init);