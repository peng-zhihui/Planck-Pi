/*
 * Copyright (c) 2013-2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/mlx5/cmd.h>
#include <linux/mlx5/qp.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/transobj.h>

#include "mlx5_core.h"
#include "lib/eq.h"

static int mlx5_core_drain_dct(struct mlx5_core_dev *dev,
			       struct mlx5_core_dct *dct);

static struct mlx5_core_rsc_common *
mlx5_get_rsc(struct mlx5_qp_table *table, u32 rsn)
{
	struct mlx5_core_rsc_common *common;
	unsigned long flags;

	spin_lock_irqsave(&table->lock, flags);

	common = radix_tree_lookup(&table->tree, rsn);
	if (common)
		refcount_inc(&common->refcount);

	spin_unlock_irqrestore(&table->lock, flags);

	return common;
}

void mlx5_core_put_rsc(struct mlx5_core_rsc_common *common)
{
	if (refcount_dec_and_test(&common->refcount))
		complete(&common->free);
}

static u64 qp_allowed_event_types(void)
{
	u64 mask;

	mask = BIT(MLX5_EVENT_TYPE_PATH_MIG) |
	       BIT(MLX5_EVENT_TYPE_COMM_EST) |
	       BIT(MLX5_EVENT_TYPE_SQ_DRAINED) |
	       BIT(MLX5_EVENT_TYPE_SRQ_LAST_WQE) |
	       BIT(MLX5_EVENT_TYPE_WQ_CATAS_ERROR) |
	       BIT(MLX5_EVENT_TYPE_PATH_MIG_FAILED) |
	       BIT(MLX5_EVENT_TYPE_WQ_INVAL_REQ_ERROR) |
	       BIT(MLX5_EVENT_TYPE_WQ_ACCESS_ERROR);

	return mask;
}

static u64 rq_allowed_event_types(void)
{
	u64 mask;

	mask = BIT(MLX5_EVENT_TYPE_SRQ_LAST_WQE) |
	       BIT(MLX5_EVENT_TYPE_WQ_CATAS_ERROR);

	return mask;
}

static u64 sq_allowed_event_types(void)
{
	return BIT(MLX5_EVENT_TYPE_WQ_CATAS_ERROR);
}

static u64 dct_allowed_event_types(void)
{
	return BIT(MLX5_EVENT_TYPE_DCT_DRAINED);
}

static bool is_event_type_allowed(int rsc_type, int event_type)
{
	switch (rsc_type) {
	case MLX5_EVENT_QUEUE_TYPE_QP:
		return BIT(event_type) & qp_allowed_event_types();
	case MLX5_EVENT_QUEUE_TYPE_RQ:
		return BIT(event_type) & rq_allowed_event_types();
	case MLX5_EVENT_QUEUE_TYPE_SQ:
		return BIT(event_type) & sq_allowed_event_types();
	case MLX5_EVENT_QUEUE_TYPE_DCT:
		return BIT(event_type) & dct_allowed_event_types();
	default:
		WARN(1, "Event arrived for unknown resource type");
		return false;
	}
}

static int rsc_event_notifier(struct notifier_block *nb,
			      unsigned long type, void *data)
{
	struct mlx5_core_rsc_common *common;
	struct mlx5_qp_table *table;
	struct mlx5_core_dev *dev;
	struct mlx5_core_dct *dct;
	u8 event_type = (u8)type;
	struct mlx5_core_qp *qp;
	struct mlx5_priv *priv;
	struct mlx5_eqe *eqe;
	u32 rsn;

	switch (event_type) {
	case MLX5_EVENT_TYPE_DCT_DRAINED:
		eqe = data;
		rsn = be32_to_cpu(eqe->data.dct.dctn) & 0xffffff;
		rsn |= (MLX5_RES_DCT << MLX5_USER_INDEX_LEN);
		break;
	case MLX5_EVENT_TYPE_PATH_MIG:
	case MLX5_EVENT_TYPE_COMM_EST:
	case MLX5_EVENT_TYPE_SQ_DRAINED:
	case MLX5_EVENT_TYPE_SRQ_LAST_WQE:
	case MLX5_EVENT_TYPE_WQ_CATAS_ERROR:
	case MLX5_EVENT_TYPE_PATH_MIG_FAILED:
	case MLX5_EVENT_TYPE_WQ_INVAL_REQ_ERROR:
	case MLX5_EVENT_TYPE_WQ_ACCESS_ERROR:
		eqe = data;
		rsn = be32_to_cpu(eqe->data.qp_srq.qp_srq_n) & 0xffffff;
		rsn |= (eqe->data.qp_srq.type << MLX5_USER_INDEX_LEN);
		break;
	default:
		return NOTIFY_DONE;
	}

	table = container_of(nb, struct mlx5_qp_table, nb);
	priv  = container_of(table, struct mlx5_priv, qp_table);
	dev   = container_of(priv, struct mlx5_core_dev, priv);

	mlx5_core_dbg(dev, "event (%d) arrived on resource 0x%x\n", eqe->type, rsn);

	common = mlx5_get_rsc(table, rsn);
	if (!common) {
		mlx5_core_dbg(dev, "Async event for unknown resource 0x%x\n", rsn);
		return NOTIFY_OK;
	}

	if (!is_event_type_allowed((rsn >> MLX5_USER_INDEX_LEN), event_type)) {
		mlx5_core_warn(dev, "event 0x%.2x is not allowed on resource 0x%.8x\n",
			       event_type, rsn);
		goto out;
	}

	switch (common->res) {
	case MLX5_RES_QP:
	case MLX5_RES_RQ:
	case MLX5_RES_SQ:
		qp = (struct mlx5_core_qp *)common;
		qp->event(qp, event_type);
		break;
	case MLX5_RES_DCT:
		dct = (struct mlx5_core_dct *)common;
		if (event_type == MLX5_EVENT_TYPE_DCT_DRAINED)
			complete(&dct->drained);
		break;
	default:
		mlx5_core_warn(dev, "invalid resource type for 0x%x\n", rsn);
	}
out:
	mlx5_core_put_rsc(common);

	return NOTIFY_OK;
}

static int create_resource_common(struct mlx5_core_dev *dev,
				  struct mlx5_core_qp *qp,
				  int rsc_type)
{
	struct mlx5_qp_table *table = &dev->priv.qp_table;
	int err;

	qp->common.res = rsc_type;
	spin_lock_irq(&table->lock);
	err = radix_tree_insert(&table->tree,
				qp->qpn | (rsc_type << MLX5_USER_INDEX_LEN),
				qp);
	spin_unlock_irq(&table->lock);
	if (err)
		return err;

	refcount_set(&qp->common.refcount, 1);
	init_completion(&qp->common.free);
	qp->pid = current->pid;

	return 0;
}

static void destroy_resource_common(struct mlx5_core_dev *dev,
				    struct mlx5_core_qp *qp)
{
	struct mlx5_qp_table *table = &dev->priv.qp_table;
	unsigned long flags;

	spin_lock_irqsave(&table->lock, flags);
	radix_tree_delete(&table->tree,
			  qp->qpn | (qp->common.res << MLX5_USER_INDEX_LEN));
	spin_unlock_irqrestore(&table->lock, flags);
	mlx5_core_put_rsc((struct mlx5_core_rsc_common *)qp);
	wait_for_completion(&qp->common.free);
}

static int _mlx5_core_destroy_dct(struct mlx5_core_dev *dev,
				  struct mlx5_core_dct *dct, bool need_cleanup)
{
	u32 out[MLX5_ST_SZ_DW(destroy_dct_out)] = {0};
	u32 in[MLX5_ST_SZ_DW(destroy_dct_in)]   = {0};
	struct mlx5_core_qp *qp = &dct->mqp;
	int err;

	err = mlx5_core_drain_dct(dev, dct);
	if (err) {
		if (dev->state == MLX5_DEVICE_STATE_INTERNAL_ERROR) {
			goto destroy;
		} else {
			mlx5_core_warn(
				dev, "failed drain DCT 0x%x with error 0x%x\n",
				qp->qpn, err);
			return err;
		}
	}
	wait_for_completion(&dct->drained);
destroy:
	if (need_cleanup)
		destroy_resource_common(dev, &dct->mqp);
	MLX5_SET(destroy_dct_in, in, opcode, MLX5_CMD_OP_DESTROY_DCT);
	MLX5_SET(destroy_dct_in, in, dctn, qp->qpn);
	MLX5_SET(destroy_dct_in, in, uid, qp->uid);
	err = mlx5_cmd_exec(dev, (void *)&in, sizeof(in),
			    (void *)&out, sizeof(out));
	return err;
}

int mlx5_core_create_dct(struct mlx5_core_dev *dev,
			 struct mlx5_core_dct *dct,
			 u32 *in, int inlen,
			 u32 *out, int outlen)
{
	struct mlx5_core_qp *qp = &dct->mqp;
	int err;

	init_completion(&dct->drained);
	MLX5_SET(create_dct_in, in, opcode, MLX5_CMD_OP_CREATE_DCT);

	err = mlx5_cmd_exec(dev, in, inlen, out, outlen);
	if (err) {
		mlx5_core_warn(dev, "create DCT failed, ret %d\n", err);
		return err;
	}

	qp->qpn = MLX5_GET(create_dct_out, out, dctn);
	qp->uid = MLX5_GET(create_dct_in, in, uid);
	err = create_resource_common(dev, qp, MLX5_RES_DCT);
	if (err)
		goto err_cmd;

	return 0;
err_cmd:
	_mlx5_core_destroy_dct(dev, dct, false);
	return err;
}
EXPORT_SYMBOL_GPL(mlx5_core_create_dct);

int mlx5_core_create_qp(struct mlx5_core_dev *dev,
			struct mlx5_core_qp *qp,
			u32 *in, int inlen)
{
	u32 out[MLX5_ST_SZ_DW(create_qp_out)] = {0};
	u32 dout[MLX5_ST_SZ_DW(destroy_qp_out)];
	u32 din[MLX5_ST_SZ_DW(destroy_qp_in)];
	int err;

	MLX5_SET(create_qp_in, in, opcode, MLX5_CMD_OP_CREATE_QP);

	err = mlx5_cmd_exec(dev, in, inlen, out, sizeof(out));
	if (err)
		return err;

	qp->uid = MLX5_GET(create_qp_in, in, uid);
	qp->qpn = MLX5_GET(create_qp_out, out, qpn);
	mlx5_core_dbg(dev, "qpn = 0x%x\n", qp->qpn);

	err = create_resource_common(dev, qp, MLX5_RES_QP);
	if (err)
		goto err_cmd;

	err = mlx5_debug_qp_add(dev, qp);
	if (err)
		mlx5_core_dbg(dev, "failed adding QP 0x%x to debug file system\n",
			      qp->qpn);

	atomic_inc(&dev->num_qps);

	return 0;

err_cmd:
	memset(din, 0, sizeof(din));
	memset(dout, 0, sizeof(dout));
	MLX5_SET(destroy_qp_in, din, opcode, MLX5_CMD_OP_DESTROY_QP);
	MLX5_SET(destroy_qp_in, din, qpn, qp->qpn);
	MLX5_SET(destroy_qp_in, din, uid, qp->uid);
	mlx5_cmd_exec(dev, din, sizeof(din), dout, sizeof(dout));
	return err;
}
EXPORT_SYMBOL_GPL(mlx5_core_create_qp);

static int mlx5_core_drain_dct(struct mlx5_core_dev *dev,
			       struct mlx5_core_dct *dct)
{
	u32 out[MLX5_ST_SZ_DW(drain_dct_out)] = {0};
	u32 in[MLX5_ST_SZ_DW(drain_dct_in)]   = {0};
	struct mlx5_core_qp *qp = &dct->mqp;

	MLX5_SET(drain_dct_in, in, opcode, MLX5_CMD_OP_DRAIN_DCT);
	MLX5_SET(drain_dct_in, in, dctn, qp->qpn);
	MLX5_SET(drain_dct_in, in, uid, qp->uid);
	return mlx5_cmd_exec(dev, (void *)&in, sizeof(in),
			     (void *)&out, sizeof(out));
}

int mlx5_core_destroy_dct(struct mlx5_core_dev *dev,
			  struct mlx5_core_dct *dct)
{
	return _mlx5_core_destroy_dct(dev, dct, true);
}
EXPORT_SYMBOL_GPL(mlx5_core_destroy_dct);

int mlx5_core_destroy_qp(struct mlx5_core_dev *dev,
			 struct mlx5_core_qp *qp)
{
	u32 out[MLX5_ST_SZ_DW(destroy_qp_out)] = {0};
	u32 in[MLX5_ST_SZ_DW(destroy_qp_in)]   = {0};
	int err;

	mlx5_debug_qp_remove(dev, qp);

	destroy_resource_common(dev, qp);

	MLX5_SET(destroy_qp_in, in, opcode, MLX5_CMD_OP_DESTROY_QP);
	MLX5_SET(destroy_qp_in, in, qpn, qp->qpn);
	MLX5_SET(destroy_qp_in, in, uid, qp->uid);
	err = mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
	if (err)
		return err;

	atomic_dec(&dev->num_qps);
	return 0;
}
EXPORT_SYMBOL_GPL(mlx5_core_destroy_qp);

int mlx5_core_set_delay_drop(struct mlx5_core_dev *dev,
			     u32 timeout_usec)
{
	u32 out[MLX5_ST_SZ_DW(set_delay_drop_params_out)] = {0};
	u32 in[MLX5_ST_SZ_DW(set_delay_drop_params_in)]   = {0};

	MLX5_SET(set_delay_drop_params_in, in, opcode,
		 MLX5_CMD_OP_SET_DELAY_DROP_PARAMS);
	MLX5_SET(set_delay_drop_params_in, in, delay_drop_timeout,
		 timeout_usec / 100);
	return mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
}
EXPORT_SYMBOL_GPL(mlx5_core_set_delay_drop);

struct mbox_info {
	u32 *in;
	u32 *out;
	int inlen;
	int outlen;
};

static int mbox_alloc(struct mbox_info *mbox, int inlen, int outlen)
{
	mbox->inlen  = inlen;
	mbox->outlen = outlen;
	mbox->in = kzalloc(mbox->inlen, GFP_KERNEL);
	mbox->out = kzalloc(mbox->outlen, GFP_KERNEL);
	if (!mbox->in || !mbox->out) {
		kfree(mbox->in);
		kfree(mbox->out);
		return -ENOMEM;
	}

	return 0;
}

static void mbox_free(struct mbox_info *mbox)
{
	kfree(mbox->in);
	kfree(mbox->out);
}

static int modify_qp_mbox_alloc(struct mlx5_core_dev *dev, u16 opcode, int qpn,
				u32 opt_param_mask, void *qpc,
				struct mbox_info *mbox, u16 uid)
{
	mbox->out = NULL;
	mbox->in = NULL;

#define MBOX_ALLOC(mbox, typ)  \
	mbox_alloc(mbox, MLX5_ST_SZ_BYTES(typ##_in), MLX5_ST_SZ_BYTES(typ##_out))

#define MOD_QP_IN_SET(typ, in, _opcode, _qpn, _uid)                            \
	do {                                                                   \
		MLX5_SET(typ##_in, in, opcode, _opcode);                       \
		MLX5_SET(typ##_in, in, qpn, _qpn);                             \
		MLX5_SET(typ##_in, in, uid, _uid);                             \
	} while (0)

#define MOD_QP_IN_SET_QPC(typ, in, _opcode, _qpn, _opt_p, _qpc, _uid)          \
	do {                                                                   \
		MOD_QP_IN_SET(typ, in, _opcode, _qpn, _uid);                   \
		MLX5_SET(typ##_in, in, opt_param_mask, _opt_p);                \
		memcpy(MLX5_ADDR_OF(typ##_in, in, qpc), _qpc,                  \
		       MLX5_ST_SZ_BYTES(qpc));                                 \
	} while (0)

	switch (opcode) {
	/* 2RST & 2ERR */
	case MLX5_CMD_OP_2RST_QP:
		if (MBOX_ALLOC(mbox, qp_2rst))
			return -ENOMEM;
		MOD_QP_IN_SET(qp_2rst, mbox->in, opcode, qpn, uid);
		break;
	case MLX5_CMD_OP_2ERR_QP:
		if (MBOX_ALLOC(mbox, qp_2err))
			return -ENOMEM;
		MOD_QP_IN_SET(qp_2err, mbox->in, opcode, qpn, uid);
		break;

	/* MODIFY with QPC */
	case MLX5_CMD_OP_RST2INIT_QP:
		if (MBOX_ALLOC(mbox, rst2init_qp))
			return -ENOMEM;
		MOD_QP_IN_SET_QPC(rst2init_qp, mbox->in, opcode, qpn,
				  opt_param_mask, qpc, uid);
		break;
	case MLX5_CMD_OP_INIT2RTR_QP:
		if (MBOX_ALLOC(mbox, init2rtr_qp))
			return -ENOMEM;
		MOD_QP_IN_SET_QPC(init2rtr_qp, mbox->in, opcode, qpn,
				  opt_param_mask, qpc, uid);
		break;
	case MLX5_CMD_OP_RTR2RTS_QP:
		if (MBOX_ALLOC(mbox, rtr2rts_qp))
			return -ENOMEM;
		MOD_QP_IN_SET_QPC(rtr2rts_qp, mbox->in, opcode, qpn,
				  opt_param_mask, qpc, uid);
		break;
	case MLX5_CMD_OP_RTS2RTS_QP:
		if (MBOX_ALLOC(mbox, rts2rts_qp))
			return -ENOMEM;
		MOD_QP_IN_SET_QPC(rts2rts_qp, mbox->in, opcode, qpn,
				  opt_param_mask, qpc, uid);
		break;
	case MLX5_CMD_OP_SQERR2RTS_QP:
		if (MBOX_ALLOC(mbox, sqerr2rts_qp))
			return -ENOMEM;
		MOD_QP_IN_SET_QPC(sqerr2rts_qp, mbox->in, opcode, qpn,
				  opt_param_mask, qpc, uid);
		break;
	case MLX5_CMD_OP_INIT2INIT_QP:
		if (MBOX_ALLOC(mbox, init2init_qp))
			return -ENOMEM;
		MOD_QP_IN_SET_QPC(init2init_qp, mbox->in, opcode, qpn,
				  opt_param_mask, qpc, uid);
		break;
	default:
		mlx5_core_err(dev, "Unknown transition for modify QP: OP(0x%x) QPN(0x%x)\n",
			      opcode, qpn);
		return -EINVAL;
	}
	return 0;
}

int mlx5_core_qp_modify(struct mlx5_core_dev *dev, u16 opcode,
			u32 opt_param_mask, void *qpc,
			struct mlx5_core_qp *qp)
{
	struct mbox_info mbox;
	int err;

	err = modify_qp_mbox_alloc(dev, opcode, qp->qpn,
				   opt_param_mask, qpc, &mbox, qp->uid);
	if (err)
		return err;

	err = mlx5_cmd_exec(dev, mbox.in, mbox.inlen, mbox.out, mbox.outlen);
	mbox_free(&mbox);
	return err;
}
EXPORT_SYMBOL_GPL(mlx5_core_qp_modify);

void mlx5_init_qp_table(struct mlx5_core_dev *dev)
{
	struct mlx5_qp_table *table = &dev->priv.qp_table;

	memset(table, 0, sizeof(*table));
	spin_lock_init(&table->lock);
	INIT_RADIX_TREE(&table->tree, GFP_ATOMIC);
	mlx5_qp_debugfs_init(dev);

	table->nb.notifier_call = rsc_event_notifier;
	mlx5_notifier_register(dev, &table->nb);
}

void mlx5_cleanup_qp_table(struct mlx5_core_dev *dev)
{
	struct mlx5_qp_table *table = &dev->priv.qp_table;

	mlx5_notifier_unregister(dev, &table->nb);
	mlx5_qp_debugfs_cleanup(dev);
}

int mlx5_core_qp_query(struct mlx5_core_dev *dev, struct mlx5_core_qp *qp,
		       u32 *out, int outlen)
{
	u32 in[MLX5_ST_SZ_DW(query_qp_in)] = {0};

	MLX5_SET(query_qp_in, in, opcode, MLX5_CMD_OP_QUERY_QP);
	MLX5_SET(query_qp_in, in, qpn, qp->qpn);
	return mlx5_cmd_exec(dev, in, sizeof(in), out, outlen);
}
EXPORT_SYMBOL_GPL(mlx5_core_qp_query);

int mlx5_core_dct_query(struct mlx5_core_dev *dev, struct mlx5_core_dct *dct,
			u32 *out, int outlen)
{
	u32 in[MLX5_ST_SZ_DW(query_dct_in)] = {0};
	struct mlx5_core_qp *qp = &dct->mqp;

	MLX5_SET(query_dct_in, in, opcode, MLX5_CMD_OP_QUERY_DCT);
	MLX5_SET(query_dct_in, in, dctn, qp->qpn);

	return mlx5_cmd_exec(dev, (void *)&in, sizeof(in),
			     (void *)out, outlen);
}
EXPORT_SYMBOL_GPL(mlx5_core_dct_query);

int mlx5_core_xrcd_alloc(struct mlx5_core_dev *dev, u32 *xrcdn)
{
	u32 out[MLX5_ST_SZ_DW(alloc_xrcd_out)] = {0};
	u32 in[MLX5_ST_SZ_DW(alloc_xrcd_in)]   = {0};
	int err;

	MLX5_SET(alloc_xrcd_in, in, opcode, MLX5_CMD_OP_ALLOC_XRCD);
	err = mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
	if (!err)
		*xrcdn = MLX5_GET(alloc_xrcd_out, out, xrcd);
	return err;
}
EXPORT_SYMBOL_GPL(mlx5_core_xrcd_alloc);

int mlx5_core_xrcd_dealloc(struct mlx5_core_dev *dev, u32 xrcdn)
{
	u32 out[MLX5_ST_SZ_DW(dealloc_xrcd_out)] = {0};
	u32 in[MLX5_ST_SZ_DW(dealloc_xrcd_in)]   = {0};

	MLX5_SET(dealloc_xrcd_in, in, opcode, MLX5_CMD_OP_DEALLOC_XRCD);
	MLX5_SET(dealloc_xrcd_in, in, xrcd, xrcdn);
	return mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
}
EXPORT_SYMBOL_GPL(mlx5_core_xrcd_dealloc);

static void destroy_rq_tracked(struct mlx5_core_dev *dev, u32 rqn, u16 uid)
{
	u32 in[MLX5_ST_SZ_DW(destroy_rq_in)]   = {};
	u32 out[MLX5_ST_SZ_DW(destroy_rq_out)] = {};

	MLX5_SET(destroy_rq_in, in, opcode, MLX5_CMD_OP_DESTROY_RQ);
	MLX5_SET(destroy_rq_in, in, rqn, rqn);
	MLX5_SET(destroy_rq_in, in, uid, uid);
	mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
}

int mlx5_core_create_rq_tracked(struct mlx5_core_dev *dev, u32 *in, int inlen,
				struct mlx5_core_qp *rq)
{
	int err;
	u32 rqn;

	err = mlx5_core_create_rq(dev, in, inlen, &rqn);
	if (err)
		return err;

	rq->uid = MLX5_GET(create_rq_in, in, uid);
	rq->qpn = rqn;
	err = create_resource_common(dev, rq, MLX5_RES_RQ);
	if (err)
		goto err_destroy_rq;

	return 0;

err_destroy_rq:
	destroy_rq_tracked(dev, rq->qpn, rq->uid);

	return err;
}
EXPORT_SYMBOL(mlx5_core_create_rq_tracked);

void mlx5_core_destroy_rq_tracked(struct mlx5_core_dev *dev,
				  struct mlx5_core_qp *rq)
{
	destroy_resource_common(dev, rq);
	destroy_rq_tracked(dev, rq->qpn, rq->uid);
}
EXPORT_SYMBOL(mlx5_core_destroy_rq_tracked);

static void destroy_sq_tracked(struct mlx5_core_dev *dev, u32 sqn, u16 uid)
{
	u32 in[MLX5_ST_SZ_DW(destroy_sq_in)]   = {};
	u32 out[MLX5_ST_SZ_DW(destroy_sq_out)] = {};

	MLX5_SET(destroy_sq_in, in, opcode, MLX5_CMD_OP_DESTROY_SQ);
	MLX5_SET(destroy_sq_in, in, sqn, sqn);
	MLX5_SET(destroy_sq_in, in, uid, uid);
	mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
}

int mlx5_core_create_sq_tracked(struct mlx5_core_dev *dev, u32 *in, int inlen,
				struct mlx5_core_qp *sq)
{
	int err;
	u32 sqn;

	err = mlx5_core_create_sq(dev, in, inlen, &sqn);
	if (err)
		return err;

	sq->uid = MLX5_GET(create_sq_in, in, uid);
	sq->qpn = sqn;
	err = create_resource_common(dev, sq, MLX5_RES_SQ);
	if (err)
		goto err_destroy_sq;

	return 0;

err_destroy_sq:
	destroy_sq_tracked(dev, sq->qpn, sq->uid);

	return err;
}
EXPORT_SYMBOL(mlx5_core_create_sq_tracked);

void mlx5_core_destroy_sq_tracked(struct mlx5_core_dev *dev,
				  struct mlx5_core_qp *sq)
{
	destroy_resource_common(dev, sq);
	destroy_sq_tracked(dev, sq->qpn, sq->uid);
}
EXPORT_SYMBOL(mlx5_core_destroy_sq_tracked);

int mlx5_core_alloc_q_counter(struct mlx5_core_dev *dev, u16 *counter_id)
{
	u32 in[MLX5_ST_SZ_DW(alloc_q_counter_in)]   = {0};
	u32 out[MLX5_ST_SZ_DW(alloc_q_counter_out)] = {0};
	int err;

	MLX5_SET(alloc_q_counter_in, in, opcode, MLX5_CMD_OP_ALLOC_Q_COUNTER);
	err = mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
	if (!err)
		*counter_id = MLX5_GET(alloc_q_counter_out, out,
				       counter_set_id);
	return err;
}
EXPORT_SYMBOL_GPL(mlx5_core_alloc_q_counter);

int mlx5_core_dealloc_q_counter(struct mlx5_core_dev *dev, u16 counter_id)
{
	u32 in[MLX5_ST_SZ_DW(dealloc_q_counter_in)]   = {0};
	u32 out[MLX5_ST_SZ_DW(dealloc_q_counter_out)] = {0};

	MLX5_SET(dealloc_q_counter_in, in, opcode,
		 MLX5_CMD_OP_DEALLOC_Q_COUNTER);
	MLX5_SET(dealloc_q_counter_in, in, counter_set_id, counter_id);
	return mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
}
EXPORT_SYMBOL_GPL(mlx5_core_dealloc_q_counter);

int mlx5_core_query_q_counter(struct mlx5_core_dev *dev, u16 counter_id,
			      int reset, void *out, int out_size)
{
	u32 in[MLX5_ST_SZ_DW(query_q_counter_in)] = {0};

	MLX5_SET(query_q_counter_in, in, opcode, MLX5_CMD_OP_QUERY_Q_COUNTER);
	MLX5_SET(query_q_counter_in, in, clear, reset);
	MLX5_SET(query_q_counter_in, in, counter_set_id, counter_id);
	return mlx5_cmd_exec(dev, in, sizeof(in), out, out_size);
}
EXPORT_SYMBOL_GPL(mlx5_core_query_q_counter);

struct mlx5_core_rsc_common *mlx5_core_res_hold(struct mlx5_core_dev *dev,
						int res_num,
						enum mlx5_res_type res_type)
{
	u32 rsn = res_num | (res_type << MLX5_USER_INDEX_LEN);
	struct mlx5_qp_table *table = &dev->priv.qp_table;

	return mlx5_get_rsc(table, rsn);
}
EXPORT_SYMBOL_GPL(mlx5_core_res_hold);

void mlx5_core_res_put(struct mlx5_core_rsc_common *res)
{
	mlx5_core_put_rsc(res);
}
EXPORT_SYMBOL_GPL(mlx5_core_res_put);
