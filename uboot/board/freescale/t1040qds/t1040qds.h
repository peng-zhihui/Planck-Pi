/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2013 Freescale Semiconductor, Inc.
 * Copyright 2020 NXP
 */

#ifndef __T1040_QDS_H__
#define __T1040_QDS_H__

void fdt_fixup_board_enet(void *blob);
void pci_of_setup(void *blob, bd_t *bd);
int select_i2c_ch_pca9547(u8 ch, int bus_bum);

#endif
