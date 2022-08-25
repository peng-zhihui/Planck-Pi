/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright (C) Intel 2011
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * The PTI (Parallel Trace Interface) driver directs trace data routed from
 * various parts in the system out through the Intel Penwell PTI port and
 * out of the mobile device for analysis with a debugging tool
 * (Lauterbach, Fido). This is part of a solution for the MIPI P1149.7,
 * compact JTAG, standard.
 *
 * This header file will allow other parts of the OS to use the
 * interface to write out it's contents for debugging a mobile system.
 */

#ifndef LINUX_INTEL_PTI_H_
#define LINUX_INTEL_PTI_H_

/* offset for last dword of any PTI message. Part of MIPI P1149.7 */
#define PTI_LASTDWORD_DTS	0x30

/* basic structure used as a write address to the PTI HW */
struct pti_masterchannel {
	u8 master;
	u8 channel;
};

/* the following functions are defined in misc/pti.c */
void pti_writedata(struct pti_masterchannel *mc, u8 *buf, int count);
struct pti_masterchannel *pti_request_masterchannel(u8 type,
						    const char *thread_name);
void pti_release_masterchannel(struct pti_masterchannel *mc);

#endif /* LINUX_INTEL_PTI_H_ */
