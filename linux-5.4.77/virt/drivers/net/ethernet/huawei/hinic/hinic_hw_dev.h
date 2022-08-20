/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Huawei HiNIC PCI Express Linux driver
 * Copyright(c) 2017 Huawei Technologies Co., Ltd
 */

#ifndef HINIC_HW_DEV_H
#define HINIC_HW_DEV_H

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/bitops.h>

#include "hinic_hw_if.h"
#include "hinic_hw_eqs.h"
#include "hinic_hw_mgmt.h"
#include "hinic_hw_qp.h"
#include "hinic_hw_io.h"

#define HINIC_MAX_QPS   32

#define HINIC_MGMT_NUM_MSG_CMD  (HINIC_MGMT_MSG_CMD_MAX - \
				 HINIC_MGMT_MSG_CMD_BASE)

struct hinic_cap {
	u16     max_qps;
	u16     num_qps;
};

enum hinic_port_cmd {
	HINIC_PORT_CMD_CHANGE_MTU       = 2,

	HINIC_PORT_CMD_ADD_VLAN         = 3,
	HINIC_PORT_CMD_DEL_VLAN         = 4,

	HINIC_PORT_CMD_SET_MAC          = 9,
	HINIC_PORT_CMD_GET_MAC          = 10,
	HINIC_PORT_CMD_DEL_MAC          = 11,

	HINIC_PORT_CMD_SET_RX_MODE      = 12,

	HINIC_PORT_CMD_GET_LINK_STATE   = 24,

	HINIC_PORT_CMD_SET_LRO		= 25,

	HINIC_PORT_CMD_SET_RX_CSUM	= 26,

	HINIC_PORT_CMD_SET_RX_VLAN_OFFLOAD = 27,

	HINIC_PORT_CMD_GET_PORT_STATISTICS = 28,

	HINIC_PORT_CMD_CLEAR_PORT_STATISTICS = 29,

	HINIC_PORT_CMD_GET_VPORT_STAT	= 30,

	HINIC_PORT_CMD_CLEAN_VPORT_STAT	= 31,

	HINIC_PORT_CMD_GET_RSS_TEMPLATE_INDIR_TBL = 37,

	HINIC_PORT_CMD_SET_PORT_STATE   = 41,

	HINIC_PORT_CMD_SET_RSS_TEMPLATE_TBL = 43,

	HINIC_PORT_CMD_GET_RSS_TEMPLATE_TBL = 44,

	HINIC_PORT_CMD_SET_RSS_HASH_ENGINE = 45,

	HINIC_PORT_CMD_GET_RSS_HASH_ENGINE = 46,

	HINIC_PORT_CMD_GET_RSS_CTX_TBL  = 47,

	HINIC_PORT_CMD_SET_RSS_CTX_TBL  = 48,

	HINIC_PORT_CMD_RSS_TEMP_MGR	= 49,

	HINIC_PORT_CMD_RSS_CFG		= 66,

	HINIC_PORT_CMD_FWCTXT_INIT      = 69,

	HINIC_PORT_CMD_GET_MGMT_VERSION = 88,

	HINIC_PORT_CMD_SET_FUNC_STATE   = 93,

	HINIC_PORT_CMD_GET_GLOBAL_QPN   = 102,

	HINIC_PORT_CMD_SET_TSO          = 112,

	HINIC_PORT_CMD_SET_RQ_IQ_MAP	= 115,

	HINIC_PORT_CMD_GET_CAP          = 170,

	HINIC_PORT_CMD_SET_LRO_TIMER	= 244,
};

enum hinic_ucode_cmd {
	HINIC_UCODE_CMD_MODIFY_QUEUE_CONTEXT    = 0,
	HINIC_UCODE_CMD_CLEAN_QUEUE_CONTEXT,
	HINIC_UCODE_CMD_ARM_SQ,
	HINIC_UCODE_CMD_ARM_RQ,
	HINIC_UCODE_CMD_SET_RSS_INDIR_TABLE,
	HINIC_UCODE_CMD_SET_RSS_CONTEXT_TABLE,
	HINIC_UCODE_CMD_GET_RSS_INDIR_TABLE,
	HINIC_UCODE_CMD_GET_RSS_CONTEXT_TABLE,
	HINIC_UCODE_CMD_SET_IQ_ENABLE,
	HINIC_UCODE_CMD_SET_RQ_FLUSH            = 10
};

#define NIC_RSS_CMD_TEMP_ALLOC  0x01
#define NIC_RSS_CMD_TEMP_FREE   0x02

enum hinic_mgmt_msg_cmd {
	HINIC_MGMT_MSG_CMD_BASE         = 160,

	HINIC_MGMT_MSG_CMD_LINK_STATUS  = 160,

	HINIC_MGMT_MSG_CMD_MAX,
};

enum hinic_cb_state {
	HINIC_CB_ENABLED = BIT(0),
	HINIC_CB_RUNNING = BIT(1),
};

enum hinic_res_state {
	HINIC_RES_CLEAN         = 0,
	HINIC_RES_ACTIVE        = 1,
};

struct hinic_cmd_fw_ctxt {
	u8      status;
	u8      version;
	u8      rsvd0[6];

	u16     func_idx;
	u16     rx_buf_sz;

	u32     rsvd1;
};

struct hinic_cmd_hw_ioctxt {
	u8      status;
	u8      version;
	u8      rsvd0[6];

	u16     func_idx;

	u16     rsvd1;

	u8      set_cmdq_depth;
	u8      cmdq_depth;

	u8      lro_en;
	u8      rsvd3;
	u8      ppf_idx;
	u8      rsvd4;

	u16     rq_depth;
	u16     rx_buf_sz_idx;
	u16     sq_depth;
};

struct hinic_cmd_io_status {
	u8      status;
	u8      version;
	u8      rsvd0[6];

	u16     func_idx;
	u8      rsvd1;
	u8      rsvd2;
	u32     io_status;
};

struct hinic_cmd_clear_io_res {
	u8      status;
	u8      version;
	u8      rsvd0[6];

	u16     func_idx;
	u8      rsvd1;
	u8      rsvd2;
};

struct hinic_cmd_set_res_state {
	u8      status;
	u8      version;
	u8      rsvd0[6];

	u16     func_idx;
	u8      state;
	u8      rsvd1;
	u32     rsvd2;
};

struct hinic_cmd_base_qpn {
	u8      status;
	u8      version;
	u8      rsvd0[6];

	u16     func_idx;
	u16     qpn;
};

struct hinic_cmd_hw_ci {
	u8      status;
	u8      version;
	u8      rsvd0[6];

	u16     func_idx;

	u8      dma_attr_off;
	u8      pending_limit;
	u8      coalesc_timer;

	u8      msix_en;
	u16     msix_entry_idx;

	u32     sq_id;
	u32     rsvd1;
	u64     ci_addr;
};

struct hinic_hwdev {
	struct hinic_hwif               *hwif;
	struct msix_entry               *msix_entries;

	struct hinic_aeqs               aeqs;
	struct hinic_func_to_io         func_to_io;

	struct hinic_cap                nic_cap;
};

struct hinic_nic_cb {
	void    (*handler)(void *handle, void *buf_in,
			   u16 in_size, void *buf_out,
			   u16 *out_size);

	void            *handle;
	unsigned long   cb_state;
};

struct hinic_pfhwdev {
	struct hinic_hwdev              hwdev;

	struct hinic_pf_to_mgmt         pf_to_mgmt;

	struct hinic_nic_cb             nic_cb[HINIC_MGMT_NUM_MSG_CMD];
};

void hinic_hwdev_cb_register(struct hinic_hwdev *hwdev,
			     enum hinic_mgmt_msg_cmd cmd, void *handle,
			     void (*handler)(void *handle, void *buf_in,
					     u16 in_size, void *buf_out,
					     u16 *out_size));

void hinic_hwdev_cb_unregister(struct hinic_hwdev *hwdev,
			       enum hinic_mgmt_msg_cmd cmd);

int hinic_port_msg_cmd(struct hinic_hwdev *hwdev, enum hinic_port_cmd cmd,
		       void *buf_in, u16 in_size, void *buf_out,
		       u16 *out_size);

int hinic_hwdev_ifup(struct hinic_hwdev *hwdev);

void hinic_hwdev_ifdown(struct hinic_hwdev *hwdev);

struct hinic_hwdev *hinic_init_hwdev(struct pci_dev *pdev);

void hinic_free_hwdev(struct hinic_hwdev *hwdev);

int hinic_hwdev_max_num_qps(struct hinic_hwdev *hwdev);

int hinic_hwdev_num_qps(struct hinic_hwdev *hwdev);

struct hinic_sq *hinic_hwdev_get_sq(struct hinic_hwdev *hwdev, int i);

struct hinic_rq *hinic_hwdev_get_rq(struct hinic_hwdev *hwdev, int i);

int hinic_hwdev_msix_cnt_set(struct hinic_hwdev *hwdev, u16 msix_index);

int hinic_hwdev_msix_set(struct hinic_hwdev *hwdev, u16 msix_index,
			 u8 pending_limit, u8 coalesc_timer,
			 u8 lli_timer_cfg, u8 lli_credit_limit,
			 u8 resend_timer);

int hinic_hwdev_hw_ci_addr_set(struct hinic_hwdev *hwdev, struct hinic_sq *sq,
			       u8 pending_limit, u8 coalesc_timer);

void hinic_hwdev_set_msix_state(struct hinic_hwdev *hwdev, u16 msix_index,
				enum hinic_msix_state flag);

#endif
