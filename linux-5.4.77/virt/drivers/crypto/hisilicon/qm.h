/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2019 HiSilicon Limited. */
#ifndef HISI_ACC_QM_H
#define HISI_ACC_QM_H

#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pci.h>

/* qm user domain */
#define QM_ARUSER_M_CFG_1		0x100088
#define AXUSER_SNOOP_ENABLE		BIT(30)
#define AXUSER_CMD_TYPE			GENMASK(14, 12)
#define AXUSER_CMD_SMMU_NORMAL		1
#define AXUSER_NS			BIT(6)
#define AXUSER_NO			BIT(5)
#define AXUSER_FP			BIT(4)
#define AXUSER_SSV			BIT(0)
#define AXUSER_BASE			(AXUSER_SNOOP_ENABLE |		\
					FIELD_PREP(AXUSER_CMD_TYPE,	\
					AXUSER_CMD_SMMU_NORMAL) |	\
					AXUSER_NS | AXUSER_NO | AXUSER_FP)
#define QM_ARUSER_M_CFG_ENABLE		0x100090
#define ARUSER_M_CFG_ENABLE		0xfffffffe
#define QM_AWUSER_M_CFG_1		0x100098
#define QM_AWUSER_M_CFG_ENABLE		0x1000a0
#define AWUSER_M_CFG_ENABLE		0xfffffffe
#define QM_WUSER_M_CFG_ENABLE		0x1000a8
#define WUSER_M_CFG_ENABLE		0xffffffff

/* qm cache */
#define QM_CACHE_CTL			0x100050
#define SQC_CACHE_ENABLE		BIT(0)
#define CQC_CACHE_ENABLE		BIT(1)
#define SQC_CACHE_WB_ENABLE		BIT(4)
#define SQC_CACHE_WB_THRD		GENMASK(10, 5)
#define CQC_CACHE_WB_ENABLE		BIT(11)
#define CQC_CACHE_WB_THRD		GENMASK(17, 12)
#define QM_AXI_M_CFG			0x1000ac
#define AXI_M_CFG			0xffff
#define QM_AXI_M_CFG_ENABLE		0x1000b0
#define AXI_M_CFG_ENABLE		0xffffffff
#define QM_PEH_AXUSER_CFG		0x1000cc
#define QM_PEH_AXUSER_CFG_ENABLE	0x1000d0
#define PEH_AXUSER_CFG			0x401001
#define PEH_AXUSER_CFG_ENABLE		0xffffffff

#define QM_DFX_MB_CNT_VF		0x104010
#define QM_DFX_DB_CNT_VF		0x104020
#define QM_DFX_SQE_CNT_VF_SQN		0x104030
#define QM_DFX_CQE_CNT_VF_CQN		0x104040
#define QM_DFX_QN_SHIFT			16
#define CURRENT_FUN_MASK		GENMASK(5, 0)
#define CURRENT_Q_MASK			GENMASK(31, 16)

#define QM_AXI_RRESP			BIT(0)
#define QM_AXI_BRESP			BIT(1)
#define QM_ECC_MBIT			BIT(2)
#define QM_ECC_1BIT			BIT(3)
#define QM_ACC_GET_TASK_TIMEOUT		BIT(4)
#define QM_ACC_DO_TASK_TIMEOUT		BIT(5)
#define QM_ACC_WB_NOT_READY_TIMEOUT	BIT(6)
#define QM_SQ_CQ_VF_INVALID		BIT(7)
#define QM_CQ_VF_INVALID		BIT(8)
#define QM_SQ_VF_INVALID		BIT(9)
#define QM_DB_TIMEOUT			BIT(10)
#define QM_OF_FIFO_OF			BIT(11)
#define QM_DB_RANDOM_INVALID		BIT(12)

#define QM_BASE_NFE	(QM_AXI_RRESP | QM_AXI_BRESP | QM_ECC_MBIT | \
			 QM_ACC_GET_TASK_TIMEOUT | QM_DB_TIMEOUT | \
			 QM_OF_FIFO_OF)
#define QM_BASE_CE			QM_ECC_1BIT

#define QM_Q_DEPTH			1024

enum qp_state {
	QP_STOP,
};

enum qm_hw_ver {
	QM_HW_UNKNOWN = -1,
	QM_HW_V1 = 0x20,
	QM_HW_V2 = 0x21,
};

enum qm_fun_type {
	QM_HW_PF,
	QM_HW_VF,
};

enum qm_debug_file {
	CURRENT_Q,
	CLEAR_ENABLE,
	DEBUG_FILE_NUM,
};

struct debugfs_file {
	enum qm_debug_file index;
	struct mutex lock;
	struct qm_debug *debug;
};

struct qm_debug {
	u32 curr_qm_qp_num;
	struct dentry *debug_root;
	struct dentry *qm_d;
	struct debugfs_file files[DEBUG_FILE_NUM];
};

struct qm_dma {
	void *va;
	dma_addr_t dma;
	size_t size;
};

struct hisi_qm_status {
	u32 eq_head;
	bool eqc_phase;
	u32 aeq_head;
	bool aeqc_phase;
	unsigned long flags;
};

struct hisi_qm {
	enum qm_hw_ver ver;
	enum qm_fun_type fun_type;
	const char *dev_name;
	struct pci_dev *pdev;
	void __iomem *io_base;
	u32 sqe_size;
	u32 qp_base;
	u32 qp_num;
	u32 ctrl_qp_num;

	struct qm_dma qdma;
	struct qm_sqc *sqc;
	struct qm_cqc *cqc;
	struct qm_eqe *eqe;
	struct qm_aeqe *aeqe;
	dma_addr_t sqc_dma;
	dma_addr_t cqc_dma;
	dma_addr_t eqe_dma;
	dma_addr_t aeqe_dma;

	struct hisi_qm_status status;

	rwlock_t qps_lock;
	unsigned long *qp_bitmap;
	struct hisi_qp **qp_array;

	struct mutex mailbox_lock;

	const struct hisi_qm_hw_ops *ops;

	struct qm_debug debug;

	u32 error_mask;
	u32 msi_mask;

	bool use_dma_api;
};

struct hisi_qp_status {
	atomic_t used;
	u16 sq_tail;
	u16 cq_head;
	bool cqc_phase;
	unsigned long flags;
};

struct hisi_qp_ops {
	int (*fill_sqe)(void *sqe, void *q_parm, void *d_parm);
};

struct hisi_qp {
	u32 qp_id;
	u8 alg_type;
	u8 req_type;

	struct qm_dma qdma;
	void *sqe;
	struct qm_cqe *cqe;
	dma_addr_t sqe_dma;
	dma_addr_t cqe_dma;

	struct hisi_qp_status qp_status;
	struct hisi_qp_ops *hw_ops;
	void *qp_ctx;
	void (*req_cb)(struct hisi_qp *qp, void *data);
	struct work_struct work;
	struct workqueue_struct *wq;

	struct hisi_qm *qm;
};

int hisi_qm_init(struct hisi_qm *qm);
void hisi_qm_uninit(struct hisi_qm *qm);
int hisi_qm_start(struct hisi_qm *qm);
int hisi_qm_stop(struct hisi_qm *qm);
struct hisi_qp *hisi_qm_create_qp(struct hisi_qm *qm, u8 alg_type);
int hisi_qm_start_qp(struct hisi_qp *qp, unsigned long arg);
int hisi_qm_stop_qp(struct hisi_qp *qp);
void hisi_qm_release_qp(struct hisi_qp *qp);
int hisi_qp_send(struct hisi_qp *qp, const void *msg);
int hisi_qm_get_vft(struct hisi_qm *qm, u32 *base, u32 *number);
int hisi_qm_set_vft(struct hisi_qm *qm, u32 fun_num, u32 base, u32 number);
int hisi_qm_debug_init(struct hisi_qm *qm);
void hisi_qm_hw_error_init(struct hisi_qm *qm, u32 ce, u32 nfe, u32 fe,
			   u32 msi);
int hisi_qm_hw_error_handle(struct hisi_qm *qm);
enum qm_hw_ver hisi_qm_get_hw_version(struct pci_dev *pdev);
void hisi_qm_debug_regs_clear(struct hisi_qm *qm);
#endif
