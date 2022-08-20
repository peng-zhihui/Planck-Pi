// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2014 Google, Inc
 *
 * Memory Type Range Regsters - these are used to tell the CPU whether
 * memory is cacheable and if so the cache write mode to use.
 *
 * These can speed up booting. See the mtrr command.
 *
 * Reference: Intel Architecture Software Developer's Manual, Volume 3:
 * System Programming
 */

/*
 * Note that any console output (e.g. debug()) in this file will likely fail
 * since the MTRR registers are sometimes in flux.
 */

#include <common.h>
#include <cpu_func.h>
#include <log.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/msr.h>
#include <asm/mtrr.h>

DECLARE_GLOBAL_DATA_PTR;

/* Prepare to adjust MTRRs */
void mtrr_open(struct mtrr_state *state, bool do_caches)
{
	if (!gd->arch.has_mtrr)
		return;

	if (do_caches) {
		state->enable_cache = dcache_status();

		if (state->enable_cache)
			disable_caches();
	}
	state->deftype = native_read_msr(MTRR_DEF_TYPE_MSR);
	wrmsrl(MTRR_DEF_TYPE_MSR, state->deftype & ~MTRR_DEF_TYPE_EN);
}

/* Clean up after adjusting MTRRs, and enable them */
void mtrr_close(struct mtrr_state *state, bool do_caches)
{
	if (!gd->arch.has_mtrr)
		return;

	wrmsrl(MTRR_DEF_TYPE_MSR, state->deftype | MTRR_DEF_TYPE_EN);
	if (do_caches && state->enable_cache)
		enable_caches();
}

static void set_var_mtrr(uint reg, uint type, uint64_t start, uint64_t size)
{
	u64 mask;

	wrmsrl(MTRR_PHYS_BASE_MSR(reg), start | type);
	mask = ~(size - 1);
	mask &= (1ULL << CONFIG_CPU_ADDR_BITS) - 1;
	wrmsrl(MTRR_PHYS_MASK_MSR(reg), mask | MTRR_PHYS_MASK_VALID);
}

int mtrr_commit(bool do_caches)
{
	struct mtrr_request *req = gd->arch.mtrr_req;
	struct mtrr_state state;
	int i;

	debug("%s: enabled=%d, count=%d\n", __func__, gd->arch.has_mtrr,
	      gd->arch.mtrr_req_count);
	if (!gd->arch.has_mtrr)
		return -ENOSYS;

	debug("open\n");
	mtrr_open(&state, do_caches);
	debug("open done\n");
	for (i = 0; i < gd->arch.mtrr_req_count; i++, req++)
		set_var_mtrr(i, req->type, req->start, req->size);

	/* Clear the ones that are unused */
	debug("clear\n");
	for (; i < MTRR_COUNT; i++)
		wrmsrl(MTRR_PHYS_MASK_MSR(i), 0);
	debug("close\n");
	mtrr_close(&state, do_caches);
	debug("mtrr done\n");

	return 0;
}

int mtrr_add_request(int type, uint64_t start, uint64_t size)
{
	struct mtrr_request *req;
	uint64_t mask;

	debug("%s: count=%d\n", __func__, gd->arch.mtrr_req_count);
	if (!gd->arch.has_mtrr)
		return -ENOSYS;

	if (gd->arch.mtrr_req_count == MAX_MTRR_REQUESTS)
		return -ENOSPC;
	req = &gd->arch.mtrr_req[gd->arch.mtrr_req_count++];
	req->type = type;
	req->start = start;
	req->size = size;
	debug("%d: type=%d, %08llx  %08llx\n", gd->arch.mtrr_req_count - 1,
	      req->type, req->start, req->size);
	mask = ~(req->size - 1);
	mask &= (1ULL << CONFIG_CPU_ADDR_BITS) - 1;
	mask |= MTRR_PHYS_MASK_VALID;
	debug("   %016llx %016llx\n", req->start | req->type, mask);

	return 0;
}

static int get_var_mtrr_count(void)
{
	return msr_read(MSR_MTRR_CAP_MSR).lo & MSR_MTRR_CAP_VCNT;
}

static int get_free_var_mtrr(void)
{
	struct msr_t maskm;
	int vcnt;
	int i;

	vcnt = get_var_mtrr_count();

	/* Identify the first var mtrr which is not valid */
	for (i = 0; i < vcnt; i++) {
		maskm = msr_read(MTRR_PHYS_MASK_MSR(i));
		if ((maskm.lo & MTRR_PHYS_MASK_VALID) == 0)
			return i;
	}

	/* No free var mtrr */
	return -ENOSPC;
}

int mtrr_set_next_var(uint type, uint64_t start, uint64_t size)
{
	int mtrr;

	mtrr = get_free_var_mtrr();
	if (mtrr < 0)
		return mtrr;

	set_var_mtrr(mtrr, type, start, size);
	debug("MTRR %x: start=%x, size=%x\n", mtrr, (uint)start, (uint)size);

	return 0;
}
