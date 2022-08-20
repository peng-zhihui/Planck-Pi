/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2012,2015 Stephen Warren
 */

#ifndef _BCM2835_TIMER_H
#define _BCM2835_TIMER_H

#ifndef __ASSEMBLY__
#include <asm/arch/base.h>
#include <linux/bug.h>
#endif

#define BCM2835_TIMER_PHYSADDR ({ BUG_ON(!rpi_bcm283x_base); \
				  rpi_bcm283x_base + 0x00003000; })

#define BCM2835_TIMER_CS_M3	(1 << 3)
#define BCM2835_TIMER_CS_M2	(1 << 2)
#define BCM2835_TIMER_CS_M1	(1 << 1)
#define BCM2835_TIMER_CS_M0	(1 << 0)

#ifndef __ASSEMBLY__
#include <linux/types.h>

struct bcm2835_timer_regs {
	u32 cs;
	u32 clo;
	u32 chi;
	u32 c0;
	u32 c1;
	u32 c2;
	u32 c3;
};
#endif

#endif
