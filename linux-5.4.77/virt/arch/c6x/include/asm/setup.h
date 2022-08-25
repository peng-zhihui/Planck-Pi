/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Port on Texas Instruments TMS320C6x architecture
 *
 *  Copyright (C) 2004, 2009, 2010 2011 Texas Instruments Incorporated
 *  Author: Aurelien Jacquiot (aurelien.jacquiot@jaluna.com)
 */
#ifndef _ASM_C6X_SETUP_H
#define _ASM_C6X_SETUP_H

#include <uapi/asm/setup.h>
#include <linux/types.h>

#ifndef __ASSEMBLY__
extern int c6x_add_memory(phys_addr_t start, unsigned long size);

extern unsigned long ram_start;
extern unsigned long ram_end;

extern int c6x_num_cores;
extern unsigned int c6x_silicon_rev;
extern unsigned int c6x_devstat;
extern unsigned char c6x_fuse_mac[6];

extern void machine_init(unsigned long dt_ptr);
extern void time_init(void);

extern void coherent_mem_init(u32 start, u32 size);

#endif /* !__ASSEMBLY__ */
#endif /* _ASM_C6X_SETUP_H */
