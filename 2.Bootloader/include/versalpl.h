/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2019 Xilinx, Inc,
 * Siva Durga Prasad Paladugu <siva.durga.paladugu@xilinx.com>
 */

#ifndef _VERSALPL_H_
#define _VERSALPL_H_

#include <xilinx.h>

#define VERSAL_PM_LOAD_PDI	0x701
#define VERSAL_PM_PDI_TYPE	0xF

extern struct xilinx_fpga_op versal_op;

#define XILINX_VERSAL_DESC \
{ xilinx_versal, csu_dma, 1, &versal_op, 0, &versal_op }

#endif /* _VERSALPL_H_ */
