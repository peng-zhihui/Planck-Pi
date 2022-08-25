/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_DATAPAGE_H
#define __VDSO_DATAPAGE_H

#ifndef __ASSEMBLY__

#include <linux/bits.h>
#include <linux/time.h>
#include <linux/types.h>

#define VDSO_BASES	(CLOCK_TAI + 1)
#define VDSO_HRES	(BIT(CLOCK_REALTIME)		| \
			 BIT(CLOCK_MONOTONIC)		| \
			 BIT(CLOCK_BOOTTIME)		| \
			 BIT(CLOCK_TAI))
#define VDSO_COARSE	(BIT(CLOCK_REALTIME_COARSE)	| \
			 BIT(CLOCK_MONOTONIC_COARSE))
#define VDSO_RAW	(BIT(CLOCK_MONOTONIC_RAW))

#define CS_HRES_COARSE	0
#define CS_RAW		1
#define CS_BASES	(CS_RAW + 1)

/**
 * struct vdso_timestamp - basetime per clock_id
 * @sec:	seconds
 * @nsec:	nanoseconds
 *
 * There is one vdso_timestamp object in vvar for each vDSO-accelerated
 * clock_id. For high-resolution clocks, this encodes the time
 * corresponding to vdso_data.cycle_last. For coarse clocks this encodes
 * the actual time.
 *
 * To be noticed that for highres clocks nsec is left-shifted by
 * vdso_data.cs[x].shift.
 */
struct vdso_timestamp {
	u64	sec;
	u64	nsec;
};

/**
 * struct vdso_data - vdso datapage representation
 * @seq:		timebase sequence counter
 * @clock_mode:		clock mode
 * @cycle_last:		timebase at clocksource init
 * @mask:		clocksource mask
 * @mult:		clocksource multiplier
 * @shift:		clocksource shift
 * @basetime[clock_id]:	basetime per clock_id
 * @tz_minuteswest:	minutes west of Greenwich
 * @tz_dsttime:		type of DST correction
 * @hrtimer_res:	hrtimer resolution
 * @__unused:		unused
 *
 * vdso_data will be accessed by 64 bit and compat code at the same time
 * so we should be careful before modifying this structure.
 */
struct vdso_data {
	u32			seq;

	s32			clock_mode;
	u64			cycle_last;
	u64			mask;
	u32			mult;
	u32			shift;

	struct vdso_timestamp	basetime[VDSO_BASES];

	s32			tz_minuteswest;
	s32			tz_dsttime;
	u32			hrtimer_res;
	u32			__unused;
};

/*
 * We use the hidden visibility to prevent the compiler from generating a GOT
 * relocation. Not only is going through a GOT useless (the entry couldn't and
 * must not be overridden by another library), it does not even work: the linker
 * cannot generate an absolute address to the data page.
 *
 * With the hidden visibility, the compiler simply generates a PC-relative
 * relocation, and this is what we need.
 */
extern struct vdso_data _vdso_data[CS_BASES] __attribute__((visibility("hidden")));

#endif /* !__ASSEMBLY__ */

#endif /* __VDSO_DATAPAGE_H */
