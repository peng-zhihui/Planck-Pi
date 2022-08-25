/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2017 The Linux Foundation. All rights reserved. */

#ifndef _A6XX_GMU_H_
#define _A6XX_GMU_H_

#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include "msm_drv.h"
#include "a6xx_hfi.h"

struct a6xx_gmu_bo {
	void *virt;
	size_t size;
	u64 iova;
	struct page **pages;
};

/*
 * These define the different GMU wake up options - these define how both the
 * CPU and the GMU bring up the hardware
 */

/* THe GMU has already been booted and the rentention registers are active */
#define GMU_WARM_BOOT 0

/* the GMU is coming up for the first time or back from a power collapse */
#define GMU_COLD_BOOT 1

/*
 * These define the level of control that the GMU has - the higher the number
 * the more things that the GMU hardware controls on its own.
 */

/* The GMU does not do any idle state management */
#define GMU_IDLE_STATE_ACTIVE 0

/* The GMU manages SPTP power collapse */
#define GMU_IDLE_STATE_SPTP 2

/* The GMU does automatic IFPC (intra-frame power collapse) */
#define GMU_IDLE_STATE_IFPC 3

struct a6xx_gmu {
	struct device *dev;

	void * __iomem mmio;

	int hfi_irq;
	int gmu_irq;

	struct iommu_domain *domain;
	u64 uncached_iova_base;

	struct device *gxpd;

	int idle_level;

	struct a6xx_gmu_bo *hfi;
	struct a6xx_gmu_bo *debug;

	int nr_clocks;
	struct clk_bulk_data *clocks;
	struct clk *core_clk;

	int nr_gpu_freqs;
	unsigned long gpu_freqs[16];
	u32 gx_arc_votes[16];

	int nr_gmu_freqs;
	unsigned long gmu_freqs[4];
	u32 cx_arc_votes[4];

	unsigned long freq;

	struct a6xx_hfi_queue queues[2];

	bool initialized;
	bool hung;
};

static inline u32 gmu_read(struct a6xx_gmu *gmu, u32 offset)
{
	return msm_readl(gmu->mmio + (offset << 2));
}

static inline void gmu_write(struct a6xx_gmu *gmu, u32 offset, u32 value)
{
	return msm_writel(value, gmu->mmio + (offset << 2));
}

static inline void gmu_rmw(struct a6xx_gmu *gmu, u32 reg, u32 mask, u32 or)
{
	u32 val = gmu_read(gmu, reg);

	val &= ~mask;

	gmu_write(gmu, reg, val | or);
}

static inline u64 gmu_read64(struct a6xx_gmu *gmu, u32 lo, u32 hi)
{
	u64 val;

	val = (u64) msm_readl(gmu->mmio + (lo << 2));
	val |= ((u64) msm_readl(gmu->mmio + (hi << 2)) << 32);

	return val;
}

#define gmu_poll_timeout(gmu, addr, val, cond, interval, timeout) \
	readl_poll_timeout((gmu)->mmio + ((addr) << 2), val, cond, \
		interval, timeout)

/*
 * These are the available OOB (out of band requests) to the GMU where "out of
 * band" means that the CPU talks to the GMU directly and not through HFI.
 * Normally this works by writing a ITCM/DTCM register and then triggering a
 * interrupt (the "request" bit) and waiting for an acknowledgment (the "ack"
 * bit). The state is cleared by writing the "clear' bit to the GMU interrupt.
 *
 * These are used to force the GMU/GPU to stay on during a critical sequence or
 * for hardware workarounds.
 */

enum a6xx_gmu_oob_state {
	GMU_OOB_BOOT_SLUMBER = 0,
	GMU_OOB_GPU_SET,
	GMU_OOB_DCVS_SET,
};

/* These are the interrupt / ack bits for each OOB request that are set
 * in a6xx_gmu_set_oob and a6xx_clear_oob
 */

/*
 * Let the GMU know that a boot or slumber operation has started. The value in
 * REG_A6XX_GMU_BOOT_SLUMBER_OPTION lets the GMU know which operation we are
 * doing
 */
#define GMU_OOB_BOOT_SLUMBER_REQUEST	22
#define GMU_OOB_BOOT_SLUMBER_ACK	30
#define GMU_OOB_BOOT_SLUMBER_CLEAR	30

/*
 * Set a new power level for the GPU when the CPU is doing frequency scaling
 */
#define GMU_OOB_DCVS_REQUEST	23
#define GMU_OOB_DCVS_ACK	31
#define GMU_OOB_DCVS_CLEAR	31

/*
 * Let the GMU know to not turn off any GPU registers while the CPU is in a
 * critical section
 */
#define GMU_OOB_GPU_SET_REQUEST	16
#define GMU_OOB_GPU_SET_ACK	24
#define GMU_OOB_GPU_SET_CLEAR	24


void a6xx_hfi_init(struct a6xx_gmu *gmu);
int a6xx_hfi_start(struct a6xx_gmu *gmu, int boot_state);
void a6xx_hfi_stop(struct a6xx_gmu *gmu);

bool a6xx_gmu_gx_is_on(struct a6xx_gmu *gmu);
bool a6xx_gmu_sptprac_is_on(struct a6xx_gmu *gmu);

#endif
