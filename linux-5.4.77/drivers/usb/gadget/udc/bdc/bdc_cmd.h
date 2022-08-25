// SPDX-License-Identifier: GPL-2.0+
/*
 * bdc_cmd.h - header for the BDC debug functions
 *
 * Copyright (C) 2014 Broadcom Corporation
 *
 * Author: Ashwini Pahuja
 */
#ifndef __LINUX_BDC_CMD_H__
#define __LINUX_BDC_CMD_H__

/* Command operations */
int bdc_address_device(struct bdc *, u32);
int bdc_config_ep(struct bdc *, struct bdc_ep *);
int bdc_dconfig_ep(struct bdc *, struct bdc_ep *);
int bdc_stop_ep(struct bdc *, int);
int bdc_ep_set_stall(struct bdc *, int);
int bdc_ep_clear_stall(struct bdc *, int);
int bdc_ep_set_halt(struct bdc_ep *, u32 , int);
int bdc_ep_bla(struct bdc *, struct bdc_ep *, dma_addr_t);
int bdc_function_wake(struct bdc*, u8);
int bdc_function_wake_fh(struct bdc*, u8);

#endif /* __LINUX_BDC_CMD_H__ */
