/* Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef MSM_CPU_SUBSYS_H_
#define MSM_CPU_SUBSYS_H_

#ifdef CONFIG_MSM_CPU_PWR_CTL
int msm_unclamp_secondary_arm_cpu_sim(unsigned int cpu);
int msm_unclamp_secondary_arm_cpu(unsigned int cpu);
int msmtitanium_unclamp_secondary_arm_cpu(unsigned int cpu);
int msmthorium_unclamp_secondary_arm_cpu(unsigned int cpu);
#else
static inline int msm_unclamp_secondary_arm_cpu_sim(unsigned int cpu)
{
	return 0;
}
static inline int msm_unclamp_secondary_arm_cpu(unsigned int cpu)
{
	return 0;
}
static inline int msmtitanium_unclamp_secondary_arm_cpu(unsigned int cpu)
{
	return 0;
}
static inline int msmthorium_unclamp_secondary_arm_cpu(unsigned int cpu)
{
	return 0;
}
#endif
#endif /*MSM_CPU_SUBSYS_H_*/