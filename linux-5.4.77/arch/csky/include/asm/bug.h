/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (C) 2018 Hangzhou C-SKY Microsystems co.,ltd.

#ifndef __ASM_CSKY_BUG_H
#define __ASM_CSKY_BUG_H

#include <linux/compiler.h>
#include <linux/const.h>
#include <linux/types.h>

#define BUG()				\
do {					\
	asm volatile ("bkpt\n");	\
	unreachable();			\
} while (0)

#define HAVE_ARCH_BUG

#include <asm-generic/bug.h>

struct pt_regs;

void die_if_kernel(char *str, struct pt_regs *regs, int nr);
void show_regs(struct pt_regs *regs);

#endif /* __ASM_CSKY_BUG_H */
