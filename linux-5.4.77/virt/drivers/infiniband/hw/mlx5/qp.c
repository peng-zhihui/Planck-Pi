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

#include <linux/module.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_cache.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/rdma_counter.h>
#include <linux/mlx5/fs.h>
#include "mlx5_ib.h"
#include "ib_rep.h"
#include "cmd.h"

/* not supported currently */
static int wq_signature;

enum {
	MLX5_IB_ACK_REQ_FREQ	= 8,
};

enum {
	MLX5_IB_DEFAULT_SCHED_QUEUE	= 0x83,
	MLX5_IB_DEFAULT_QP0_SCHED_QUEUE	= 0x3f,
	MLX5_IB_LINK_TYPE_IB		= 0,
	MLX5_IB_LINK_TYPE_ETH		= 1
};

enum {
	MLX5_IB_SQ_STRIDE	= 6,
	MLX5_IB_SQ_UMR_INLINE_THRESHOLD = 64,
};

static const u32 mlx5_ib_opcode[] = {
	[IB_WR_SEND]				= MLX5_OPCODE_SEND,
	[IB_WR_LSO]				= MLX5_OPCODE_LSO,
	[IB_WR_SEND_WITH_IMM]			= MLX5_OPCODE_SEND_IMM,
	[IB_WR_RDMA_WRITE]			= MLX5_OPCODE_RDMA_WRITE,
	[IB_WR_RDMA_WRITE_WITH_IMM]		= MLX5_OPCODE_RDMA_WRITE_IMM,
	[IB_WR_RDMA_READ]			= MLX5_OPCODE_RDMA_READ,
	[IB_WR_ATOMIC_CMP_AND_SWP]		= MLX5_OPCODE_ATOMIC_CS,
	[IB_WR_ATOMIC_FETCH_AND_ADD]		= MLX5_OPCODE_ATOMIC_FA,
	[IB_WR_SEND_WITH_INV]			= MLX5_OPCODE_SEND_INVAL,
	[IB_WR_LOCAL_INV]			= MLX5_OPCODE_UMR,
	[IB_WR_REG_MR]				= MLX5_OPCODE_UMR,
	[IB_WR_MASKED_ATOMIC_CMP_AND_SWP]	= MLX5_OPCODE_ATOMIC_MASKED_CS,
	[IB_WR_MASKED_ATOMIC_FETCH_AND_ADD]	= MLX5_OPCODE_ATOMIC_MASKED_FA,
	[MLX5_IB_WR_UMR]			= MLX5_OPCODE_UMR,
};

struct mlx5_wqe_eth_pad {
	u8 rsvd0[16];
};

enum raw_qp_set_mask_map {
	MLX5_RAW_QP_MOD_SET_RQ_Q_CTR_ID		= 1UL << 0,
	MLX5_RAW_QP_RATE_LIMIT			= 1UL << 1,
};

struct mlx5_modify_raw_qp_param {
	u16 operation;

	u32 set_mask; /* raw_qp_set_mask_map */

	struct mlx5_rate_limit rl;

	u8 rq_q_ctr_id;
	u16 port;
};

static void get_cqs(enum ib_qp_type qp_type,
		    struct ib_cq *ib_send_cq, struct ib_cq *ib_recv_cq,
		    struct mlx5_ib_cq **send_cq, struct mlx5_ib_cq **recv_cq);

static int is_qp0(enum ib_qp_type qp_type)
{
	return qp_type == IB_QPT_SMI;
}

static int is_sqp(enum ib_qp_type qp_type)
{
	return is_qp0(qp_type) || is_qp1(qp_type);
}

/**
 * mlx5_ib_read_user_wqe_common() - Copy a WQE (or part of) from user WQ
 * to kernel buffer
 *
 * @umem: User space memory where the WQ is
 * @buffer: buffer to copy to
 * @buflen: buffer length
 * @wqe_index: index of WQE to copy from
 * @wq_offset: offset to start of WQ
 * @wq_wqe_cnt: number of WQEs in WQ
 * @wq_wqe_shift: log2 of WQE size
 * @bcnt: number of bytes to copy
 * @bytes_copied: number of bytes to copy (return value)
 *
 * Copies from start of WQE bcnt or less bytes.
 * Does not gurantee to copy the entire WQE.
 *
 * Return: zero on success, or an error code.
 */
static int mlx5_ib_read_user_wqe_common(struct ib_umem *umem,
					void *buffer,
					u32 buflen,
					int wqe_index,
					int wq_offset,
					int wq_wqe_cnt,
					int wq_wqe_shift,
					int bcnt,
					size_t *bytes_copied)
{
	size_t offset = wq_offset + ((wqe_index % wq_wqe_cnt) << wq_wqe_shift);
	size_t wq_end = wq_offset + (wq_wqe_cnt << wq_wqe_shift);
	size_t copy_length;
	int ret;

	/* don't copy more than requested, more than buffer length or
	 * beyond WQ end
	 */
	copy_length = min_t(u32, buflen, wq_end - offset);
	copy_length = min_t(u32, copy_length, bcnt);

	ret = ib_umem_copy_from(buffer, umem, offset, copy_length);
	if (ret)
		return ret;

	if (!ret && bytes_copied)
		*bytes_copied = copy_length;

	return 0;
}

int mlx5_ib_read_user_wqe_sq(struct mlx5_ib_qp *qp,
			     int wqe_index,
			     void *buffer,
			     int buflen,
			     size_t *bc)
{
	struct mlx5_ib_qp_base *base = &qp->trans_qp.base;
	struct ib_umem *umem = base->ubuffer.umem;
	struct mlx5_ib_wq *wq = &qp->sq;
	struct mlx5_wqe_ctrl_seg *ctrl;
	size_t bytes_copied;
	size_t bytes_copied2;
	size_t wqe_length;
	int ret;
	int ds;

	if (buflen < sizeof(*ctrl))
		return -EINVAL;

	/* at first read as much as possible */
	ret = mlx5_ib_read_user_wqe_common(umem,
					   buffer,
					   buflen,
					   wqe_index,
					   wq->offset,
					   wq->wqe_cnt,
					   wq->wqe_shift,
					   buflen,
					   &bytes_copied);
	if (ret)
		return ret;

	/* we need at least control segment size to proceed */
	if (bytes_copied < sizeof(*ctrl))
		return -EINVAL;

	ctrl = buffer;
	ds = be32_to_cpu(ctrl->qpn_ds) & MLX5_WQE_CTRL_DS_MASK;
	wqe_length = ds * MLX5_WQE_DS_UNITS;

	/* if we copied enough then we are done */
	if (bytes_copied >= wqe_length) {
		*bc = bytes_copied;
		return 0;
	}

	/* otherwise this a wrapped around wqe
	 * so read the remaining bytes starting
	 * from  wqe_index 0
	 */
	ret = mlx5_ib_read_user_wqe_common(umem,
					   buffer + bytes_copied,
					   buflen - bytes_copied,
					   0,
					   wq->offset,
					   wq->wqe_cnt,
					   wq->wqe_shift,
					   wqe_length - bytes_copied,
					   &bytes_copied2);

	if (ret)
		return ret;
	*bc = bytes_copied + bytes_copied2;
	return 0;
}

int mlx5_ib_read_user_wqe_rq(struct mlx5_ib_qp *qp,
			     int wqe_index,
			     void *buffer,
			     int buflen,
			     size_t *bc)
{
	struct mlx5_ib_qp_base *base = &qp->trans_qp.base;
	struct ib_umem *umem = base->ubuffer.umem;
	struct mlx5_ib_wq *wq = &qp->rq;
	size_t bytes_copied;
	int ret;

	ret = mlx5_ib_read_user_wqe_common(umem,
					   buffer,
					   buflen,
					   wqe_index,
					   wq->offset,
					   wq->wqe_cnt,
					   wq->wqe_shift,
					   buflen,
					   &bytes_copied);

	if (ret)
		return ret;
	*bc = bytes_copied;
	return 0;
}

int mlx5_ib_read_user_wqe_srq(struct mlx5_ib_srq *srq,
			      int wqe_index,
			      void *buffer,
			      int buflen,
			      size_t *bc)
{
	struct ib_umem *umem = srq->umem;
	size_t bytes_copied;
	int ret;

	ret = mlx5_ib_read_user_wqe_common(umem,
					   buffer,
					   buflen,
					   wqe_index,
					   0,
					   srq->msrq.max,
					   srq->msrq.wqe_shift,
					   buflen,
					   &bytes_copied);

	if (ret)
		return ret;
	*bc = bytes_copied;
	return 0;
}

static void mlx5_ib_qp_event(struct mlx5_core_qp *qp, int type)
{
	struct ib_qp *ibqp = &to_mibqp(qp)->ibqp;
	struct ib_event event;

	if (type == MLX5_EVENT_TYPE_PATH_MIG) {
		/* This event is only valid for trans_qps */
		to_mibqp(qp)->port = to_mibqp(qp)->trans_qp.alt_port;
	}

	if (ibqp->event_handler) {
		event.device     = ibqp->device;
		event.element.qp = ibqp;
		switch (type) {
		case MLX5_EVENT_TYPE_PATH_MIG:
			event.event = IB_EVENT_PATH_MIG;
			break;
		case MLX5_EVENT_TYPE_COMM_EST:
			event.event = IB_EVENT_COMM_EST;
			break;
		case MLX5_EVENT_TYPE_SQ_DRAINED:
			event.event = IB_EVENT_SQ_DRAINED;
			break;
		case MLX5_EVENT_TYPE_SRQ_LAST_WQE:
			event.event = IB_EVENT_QP_LAST_WQE_REACHED;
			break;
		case MLX5_EVENT_TYPE_WQ_CATAS_ERROR:
			event.event = IB_EVENT_QP_FATAL;
			break;
		case MLX5_EVENT_TYPE_PATH_MIG_FAILED:
			event.event = IB_EVENT_PATH_MIG_ERR;
			break;
		case MLX5_EVENT_TYPE_WQ_INVAL_REQ_ERROR:
			event.event = IB_EVENT_QP_REQ_ERR;
			break;
		case MLX5_EVENT_TYPE_WQ_ACCESS_ERROR:
			event.event = IB_EVENT_QP_ACCESS_ERR;
			break;
		default:
			pr_warn("mlx5_ib: Unexpected event type %d on QP %06x\n", type, qp->qpn);
			return;
		}

		ibqp->event_handler(&event, ibqp->qp_context);
	}
}

static int set_rq_size(struct mlx5_ib_dev *dev, struct ib_qp_cap *cap,
		       int has_rq, struct mlx5_ib_qp *qp, struct mlx5_ib_create_qp *ucmd)
{
	int wqe_size;
	int wq_size;

	/* Sanity check RQ size before proceeding */
	if (cap->max_recv_wr > (1 << MLX5_CAP_GEN(dev->mdev, log_max_qp_sz)))
		return -EINVAL;

	if (!has_rq) {
		qp->rq.max_gs = 0;
		qp->rq.wqe_cnt = 0;
		qp->rq.wqe_shift = 0;
		cap->max_recv_wr = 0;
		cap->max_recv_sge = 0;
	} else {
		if (ucmd) {
			qp->rq.wqe_cnt = ucmd->rq_wqe_count;
			if (ucmd->rq_wqe_shift > BITS_PER_BYTE * sizeof(ucmd->rq_wqe_shift))
				return -EINVAL;
			qp->rq.wqe_shift = ucmd->rq_wqe_shift;
			if ((1 << qp->rq.wqe_shift) / sizeof(struct mlx5_wqe_data_seg) < qp->wq_sig)
				return -EINVAL;
			qp->rq.max_gs = (1 << qp->rq.wqe_shift) / sizeof(struct mlx5_wqe_data_seg) - qp->wq_sig;
			qp->rq.max_post = qp->rq.wqe_cnt;
		} else {
			wqe_size = qp->wq_sig ? sizeof(struct mlx5_wqe_signature_seg) : 0;
			wqe_size += cap->max_recv_sge * sizeof(struct mlx5_wqe_data_seg);
			wqe_size = roundup_pow_of_two(wqe_size);
			wq_size = roundup_pow_of_two(cap->max_recv_wr) * wqe_size;
			wq_size = max_t(int, wq_size, MLX5_SEND_WQE_BB);
			qp->rq.wqe_cnt = wq_size / wqe_size;
			if (wqe_size > MLX5_CAP_GEN(dev->mdev, max_wqe_sz_rq)) {
				mlx5_ib_dbg(dev, "wqe_size %d, max %d\n",
					    wqe_size,
					    MLX5_CAP_GEN(dev->mdev,
							 max_wqe_sz_rq));
				return -EINVAL;
			}
			qp->rq.wqe_shift = ilog2(wqe_size);
			qp->rq.max_gs = (1 << qp->rq.wqe_shift) / sizeof(struct mlx5_wqe_data_seg) - qp->wq_sig;
			qp->rq.max_post = qp->rq.wqe_cnt;
		}
	}

	return 0;
}

static int sq_overhead(struct ib_qp_init_attr *attr)
{
	int size = 0;

	switch (attr->qp_type) {
	case IB_QPT_XRC_INI:
		size += sizeof(struct mlx5_wqe_xrc_seg);
		/* fall through */
	case IB_QPT_RC:
		size += sizeof(struct mlx5_wqe_ctrl_seg) +
			max(sizeof(struct mlx5_wqe_atomic_seg) +
			    sizeof(struct mlx5_wqe_raddr_seg),
			    sizeof(struct mlx5_wqe_umr_ctrl_seg) +
			    sizeof(struct mlx5_mkey_seg) +
			    MLX5_IB_SQ_UMR_INLINE_THRESHOLD /
			    MLX5_IB_UMR_OCTOWORD);
		break;

	case IB_QPT_XRC_TGT:
		return 0;

	case IB_QPT_UC:
		size += sizeof(struct mlx5_wqe_ctrl_seg) +
			max(sizeof(struct mlx5_wqe_raddr_seg),
			    sizeof(struct mlx5_wqe_umr_ctrl_seg) +
			    sizeof(struct mlx5_mkey_seg));
		break;

	case IB_QPT_UD:
		if (attr->create_flags & IB_QP_CREATE_IPOIB_UD_LSO)
			size += sizeof(struct mlx5_wqe_eth_pad) +
				sizeof(struct mlx5_wqe_eth_seg);
		/* fall through */
	case IB_QPT_SMI:
	case MLX5_IB_QPT_HW_GSI:
		size += sizeof(struct mlx5_wqe_ctrl_seg) +
			sizeof(struct mlx5_wqe_datagram_seg);
		break;

	case MLX5_IB_QPT_REG_UMR:
		size += sizeof(struct mlx5_wqe_ctrl_seg) +
			sizeof(struct mlx5_wqe_umr_ctrl_seg) +
			sizeof(struct mlx5_mkey_seg);
		break;

	default:
		return -EINVAL;
	}

	return size;
}

static int calc_send_wqe(struct ib_qp_init_attr *attr)
{
	int inl_size = 0;
	int size;

	size = sq_overhead(attr);
	if (size < 0)
		return size;

	if (attr->cap.max_inline_data) {
		inl_size = size + sizeof(struct mlx5_wqe_inline_seg) +
			attr->cap.max_inline_data;
	}

	size += attr->cap.max_send_sge * sizeof(struct mlx5_wqe_data_seg);
	if (attr->create_flags & IB_QP_CREATE_INTEGRITY_EN &&
	    ALIGN(max_t(int, inl_size, size), MLX5_SEND_WQE_BB) < MLX5_SIG_WQE_SIZE)
		return MLX5_SIG_WQE_SIZE;
	else
		return ALIGN(max_t(int, inl_size, size), MLX5_SEND_WQE_BB);
}

static int get_send_sge(struct ib_qp_init_attr *attr, int wqe_size)
{
	int max_sge;

	if (attr->qp_type == IB_QPT_RC)
		max_sge = (min_t(int, wqe_size, 512) -
			   sizeof(struct mlx5_wqe_ctrl_seg) -
			   sizeof(struct mlx5_wqe_raddr_seg)) /
			sizeof(struct mlx5_wqe_data_seg);
	else if (attr->qp_type == IB_QPT_XRC_INI)
		max_sge = (min_t(int, wqe_size, 512) -
			   sizeof(struct mlx5_wqe_ctrl_seg) -
			   sizeof(struct mlx5_wqe_xrc_seg) -
			   sizeof(struct mlx5_wqe_raddr_seg)) /
			sizeof(struct mlx5_wqe_data_seg);
	else
		max_sge = (wqe_size - sq_overhead(attr)) /
			sizeof(struct mlx5_wqe_data_seg);

	return min_t(int, max_sge, wqe_size - sq_overhead(attr) /
		     sizeof(struct mlx5_wqe_data_seg));
}

static int calc_sq_size(struct mlx5_ib_dev *dev, struct ib_qp_init_attr *attr,
			struct mlx5_ib_qp *qp)
{
	int wqe_size;
	int wq_size;

	if (!attr->cap.max_send_wr)
		return 0;

	wqe_size = calc_send_wqe(attr);
	mlx5_ib_dbg(dev, "wqe_size %d\n", wqe_size);
	if (wqe_size < 0)
		return wqe_size;

	if (wqe_size > MLX5_CAP_GEN(dev->mdev, max_wqe_sz_sq)) {
		mlx5_ib_dbg(dev, "wqe_size(%d) > max_sq_desc_sz(%d)\n",
			    wqe_size, MLX5_CAP_GEN(dev->mdev, max_wqe_sz_sq));
		return -EINVAL;
	}

	qp->max_inline_data = wqe_size - sq_overhead(attr) -
			      sizeof(struct mlx5_wqe_inline_seg);
	attr->cap.max_inline_data = qp->max_inline_data;

	wq_size = roundup_pow_of_two(attr->cap.max_send_wr * wqe_size);
	qp->sq.wqe_cnt = wq_size / MLX5_SEND_WQE_BB;
	if (qp->sq.wqe_cnt > (1 << MLX5_CAP_GEN(dev->mdev, log_max_qp_sz))) {
		mlx5_ib_dbg(dev, "send queue size (%d * %d / %d -> %d) exceeds limits(%d)\n",
			    attr->cap.max_send_wr, wqe_size, MLX5_SEND_WQE_BB,
			    qp->sq.wqe_cnt,
			    1 << MLX5_CAP_GEN(dev->mdev, log_max_qp_sz));
		return -ENOMEM;
	}
	qp->sq.wqe_shift = ilog2(MLX5_SEND_WQE_BB);
	qp->sq.max_gs = get_send_sge(attr, wqe_size);
	if (qp->sq.max_gs < attr->cap.max_send_sge)
		return -ENOMEM;

	attr->cap.max_send_sge = qp->sq.max_gs;
	qp->sq.max_post = wq_size / wqe_size;
	attr->cap.max_send_wr = qp->sq.max_post;

	return wq_size;
}

static int set_user_buf_size(struct mlx5_ib_dev *dev,
			    struct mlx5_ib_qp *qp,
			    struct mlx5_ib_create_qp *ucmd,
			    struct mlx5_ib_qp_base *base,
			    struct ib_qp_init_attr *attr)
{
	int desc_sz = 1 << qp->sq.wqe_shift;

	if (desc_sz > MLX5_CAP_GEN(dev->mdev, max_wqe_sz_sq)) {
		mlx5_ib_warn(dev, "desc_sz %d, max_sq_desc_sz %d\n",
			     desc_sz, MLX5_CAP_GEN(dev->mdev, max_wqe_sz_sq));
		return -EINVAL;
	}

	if (ucmd->sq_wqe_count && !is_power_of_2(ucmd->sq_wqe_count)) {
		mlx5_ib_warn(dev, "sq_wqe_count %d is not a power of two\n",
			     ucmd->sq_wqe_count);
		return -EINVAL;
	}

	qp->sq.wqe_cnt = ucmd->sq_wqe_count;

	if (qp->sq.wqe_cnt > (1 << MLX5_CAP_GEN(dev->mdev, log_max_qp_sz))) {
		mlx5_ib_warn(dev, "wqe_cnt %d, max_wqes %d\n",
			     qp->sq.wqe_cnt,
			     1 << MLX5_CAP_GEN(dev->mdev, log_max_qp_sz));
		return -EINVAL;
	}

	if (attr->qp_type == IB_QPT_RAW_PACKET ||
	    qp->flags & MLX5_IB_QP_UNDERLAY) {
		base->ubuffer.buf_size = qp->rq.wqe_cnt << qp->rq.wqe_shift;
		qp->raw_packet_qp.sq.ubuffer.buf_size = qp->sq.wqe_cnt << 6;
	} else {
		base->ubuffer.buf_size = (qp->rq.wqe_cnt << qp->rq.wqe_shift) +
					 (qp->sq.wqe_cnt << 6);
	}

	return 0;
}

static int qp_has_rq(struct ib_qp_init_attr *attr)
{
	if (attr->qp_type == IB_QPT_XRC_INI ||
	    attr->qp_type == IB_QPT_XRC_TGT || attr->srq ||
	    attr->qp_type == MLX5_IB_QPT_REG_UMR ||
	    !attr->cap.max_recv_wr)
		return 0;

	return 1;
}

enum {
	/* this is the first blue flame register in the array of bfregs assigned
	 * to a processes. Since we do not use it for blue flame but rather
	 * regular 64 bit doorbells, we do not need a lock for maintaiing
	 * "odd/even" order
	 */
	NUM_NON_BLUE_FLAME_BFREGS = 1,
};

static int max_bfregs(struct mlx5_ib_dev *dev, struct mlx5_bfreg_info *bfregi)
{
	return get_num_static_uars(dev, bfregi) * MLX5_NON_FP_BFREGS_PER_UAR;
}

static int num_med_bfreg(struct mlx5_ib_dev *dev,
			 struct mlx5_bfreg_info *bfregi)
{
	int n;

	n = max_bfregs(dev, bfregi) - bfregi->num_low_latency_bfregs -
	    NUM_NON_BLUE_FLAME_BFREGS;

	return n >= 0 ? n : 0;
}

static int first_med_bfreg(struct mlx5_ib_dev *dev,
			   struct mlx5_bfreg_info *bfregi)
{
	return num_med_bfreg(dev, bfregi) ? 1 : -ENOMEM;
}

static int first_hi_bfreg(struct mlx5_ib_dev *dev,
			  struct mlx5_bfreg_info *bfregi)
{
	int med;

	med = num_med_bfreg(dev, bfregi);
	return ++med;
}

static int alloc_high_class_bfreg(struct mlx5_ib_dev *dev,
				  struct mlx5_bfreg_info *bfregi)
{
	int i;

	for (i = first_hi_bfreg(dev, bfregi); i < max_bfregs(dev, bfregi); i++) {
		if (!bfregi->count[i]) {
			bfregi->count[i]++;
			return i;
		}
	}

	return -ENOMEM;
}

static int alloc_med_class_bfreg(struct mlx5_ib_dev *dev,
				 struct mlx5_bfreg_info *bfregi)
{
	int minidx = first_med_bfreg(dev, bfregi);
	int i;

	if (minidx < 0)
		return minidx;

	for (i = minidx; i < first_hi_bfreg(dev, bfregi); i++) {
		if (bfregi->count[i] < bfregi->count[minidx])
			minidx = i;
		if (!bfregi->count[minidx])
			break;
	}

	bfregi->count[minidx]++;
	return minidx;
}

static int alloc_bfreg(struct mlx5_ib_dev *dev,
		       struct mlx5_bfreg_info *bfregi)
{
	int bfregn = -ENOMEM;

	mutex_lock(&bfregi->lock);
	if (bfregi->ver >= 2) {
		bfregn = alloc_high_class_bfreg(dev, bfregi);
		if (bfregn < 0)
			bfregn = alloc_med_class_bfreg(dev, bfregi);
	}

	if (bfregn < 0) {
		BUILD_BUG_ON(NUM_NON_BLUE_FLAME_BFREGS != 1);
		bfregn = 0;
		bfregi->count[bfregn]++;
	}
	mutex_unlock(&bfregi->lock);

	return bfregn;
}

void mlx5_ib_free_bfreg(struct mlx5_ib_dev *dev, struct mlx5_bfreg_info *bfregi, int bfregn)
{
	mutex_lock(&bfregi->lock);
	bfregi->count[bfregn]--;
	mutex_unlock(&bfregi->lock);
}

static enum mlx5_qp_state to_mlx5_state(enum ib_qp_state state)
{
	switch (state) {
	case IB_QPS_RESET:	return MLX5_QP_STATE_RST;
	case IB_QPS_INIT:	return MLX5_QP_STATE_INIT;
	case IB_QPS_RTR:	return MLX5_QP_STATE_RTR;
	case IB_QPS_RTS:	return MLX5_QP_STATE_RTS;
	case IB_QPS_SQD:	return MLX5_QP_STATE_SQD;
	case IB_QPS_SQE:	return MLX5_QP_STATE_SQER;
	case IB_QPS_ERR:	return MLX5_QP_STATE_ERR;
	default:		return -1;
	}
}

static int to_mlx5_st(enum ib_qp_type type)
{
	switch (type) {
	case IB_QPT_RC:			return MLX5_QP_ST_RC;
	case IB_QPT_UC:			return MLX5_QP_ST_UC;
	case IB_QPT_UD:			return MLX5_QP_ST_UD;
	case MLX5_IB_QPT_REG_UMR:	return MLX5_QP_ST_REG_UMR;
	case IB_QPT_XRC_INI:
	case IB_QPT_XRC_TGT:		return MLX5_QP_ST_XRC;
	case IB_QPT_SMI:		return MLX5_QP_ST_QP0;
	case MLX5_IB_QPT_HW_GSI:	return MLX5_QP_ST_QP1;
	case MLX5_IB_QPT_DCI:		return MLX5_QP_ST_DCI;
	case IB_QPT_RAW_IPV6:		return MLX5_QP_ST_RAW_IPV6;
	case IB_QPT_RAW_PACKET:
	case IB_QPT_RAW_ETHERTYPE:	return MLX5_QP_ST_RAW_ETHERTYPE;
	case IB_QPT_MAX:
	default:		return -EINVAL;
	}
}

static void mlx5_ib_lock_cqs(struct mlx5_ib_cq *send_cq,
			     struct mlx5_ib_cq *recv_cq);
static void mlx5_ib_unlock_cqs(struct mlx5_ib_cq *send_cq,
			       struct mlx5_ib_cq *recv_cq);

int bfregn_to_uar_index(struct mlx5_ib_dev *dev,
			struct mlx5_bfreg_info *bfregi, u32 bfregn,
			bool dyn_bfreg)
{
	unsigned int bfregs_per_sys_page;
	u32 index_of_sys_page;
	u32 offset;

	bfregs_per_sys_page = get_uars_per_sys_page(dev, bfregi->lib_uar_4k) *
				MLX5_NON_FP_BFREGS_PER_UAR;
	index_of_sys_page = bfregn / bfregs_per_sys_page;

	if (dyn_bfreg) {
		index_of_sys_page += bfregi->num_static_sys_pages;

		if (index_of_sys_page >= bfregi->num_sys_pages)
			return -EINVAL;

		if (bfregn > bfregi->num_dyn_bfregs ||
		    bfregi->sys_pages[index_of_sys_page] == MLX5_IB_INVALID_UAR_INDEX) {
			mlx5_ib_dbg(dev, "Invalid dynamic uar index\n");
			return -EINVAL;
		}
	}

	offset = bfregn % bfregs_per_sys_page / MLX5_NON_FP_BFREGS_PER_UAR;
	return bfregi->sys_pages[index_of_sys_page] + offset;
}

static int mlx5_ib_umem_get(struct mlx5_ib_dev *dev, struct ib_udata *udata,
			    unsigned long addr, size_t size,
			    struct ib_umem **umem, int *npages, int *page_shift,
			    int *ncont, u32 *offset)
{
	int err;

	*umem = ib_umem_get(udata, addr, size, 0, 0);
	if (IS_ERR(*umem)) {
		mlx5_ib_dbg(dev, "umem_get failed\n");
		return PTR_ERR(*umem);
	}

	mlx5_ib_cont_pages(*umem, addr, 0, npages, page_shift, ncont, NULL);

	err = mlx5_ib_get_buf_offset(addr, *page_shift, offset);
	if (err) {
		mlx5_ib_warn(dev, "bad offset\n");
		goto err_umem;
	}

	mlx5_ib_dbg(dev, "addr 0x%lx, size %zu, npages %d, page_shift %d, ncont %d, offset %d\n",
		    addr, size, *npages, *page_shift, *ncont, *offset);

	return 0;

err_umem:
	ib_umem_release(*umem);
	*umem = NULL;

	return err;
}

static void destroy_user_rq(struct mlx5_ib_dev *dev, struct ib_pd *pd,
			    struct mlx5_ib_rwq *rwq, struct ib_udata *udata)
{
	struct mlx5_ib_ucontext *context =
		rdma_udata_to_drv_context(
			udata,
			struct mlx5_ib_ucontext,
			ibucontext);

	if (rwq->create_flags & MLX5_IB_WQ_FLAGS_DELAY_DROP)
		atomic_dec(&dev->delay_drop.rqs_cnt);

	mlx5_ib_db_unmap_user(context, &rwq->db);
	ib_umem_release(rwq->umem);
}

static int create_user_rq(struct mlx5_ib_dev *dev, struct ib_pd *pd,
			  struct ib_udata *udata, struct mlx5_ib_rwq *rwq,
			  struct mlx5_ib_create_wq *ucmd)
{
	struct mlx5_ib_ucontext *ucontext = rdma_udata_to_drv_context(
		udata, struct mlx5_ib_ucontext, ibucontext);
	int page_shift = 0;
	int npages;
	u32 offset = 0;
	int ncont = 0;
	int err;

	if (!ucmd->buf_addr)
		return -EINVAL;

	rwq->umem = ib_umem_get(udata, ucmd->buf_addr, rwq->buf_size, 0, 0);
	if (IS_ERR(rwq->umem)) {
		mlx5_ib_dbg(dev, "umem_get failed\n");
		err = PTR_ERR(rwq->umem);
		return err;
	}

	mlx5_ib_cont_pages(rwq->umem, ucmd->buf_addr, 0, &npages, &page_shift,
			   &ncont, NULL);
	err = mlx5_ib_get_buf_offset(ucmd->buf_addr, page_shift,
				     &rwq->rq_page_offset);
	if (err) {
		mlx5_ib_warn(dev, "bad offset\n");
		goto err_umem;
	}

	rwq->rq_num_pas = ncont;
	rwq->page_shift = page_shift;
	rwq->log_page_size =  page_shift - MLX5_ADAPTER_PAGE_SHIFT;
	rwq->wq_sig = !!(ucmd->flags & MLX5_WQ_FLAG_SIGNATURE);

	mlx5_ib_dbg(dev, "addr 0x%llx, size %zd, npages %d, page_shift %d, ncont %d, offset %d\n",
		    (unsigned long long)ucmd->buf_addr, rwq->buf_size,
		    npages, page_shift, ncont, offset);

	err = mlx5_ib_db_map_user(ucontext, udata, ucmd->db_addr, &rwq->db);
	if (err) {
		mlx5_ib_dbg(dev, "map failed\n");
		goto err_umem;
	}

	rwq->create_type = MLX5_WQ_USER;
	return 0;

err_umem:
	ib_umem_release(rwq->umem);
	return err;
}

static int adjust_bfregn(struct mlx5_ib_dev *dev,
			 struct mlx5_bfreg_info *bfregi, int bfregn)
{
	return bfregn / MLX5_NON_FP_BFREGS_PER_UAR * MLX5_BFREGS_PER_UAR +
				bfregn % MLX5_NON_FP_BFREGS_PER_UAR;
}

static int create_user_qp(struct mlx5_ib_dev *dev, struct ib_pd *pd,
			  struct mlx5_ib_qp *qp, struct ib_udata *udata,
			  struct ib_qp_init_attr *attr,
			  u32 **in,
			  struct mlx5_ib_create_qp_resp *resp, int *inlen,
			  struct mlx5_ib_qp_base *base)
{
	struct mlx5_ib_ucontext *context;
	struct mlx5_ib_create_qp ucmd;
	struct mlx5_ib_ubuffer *ubuffer = &base->ubuffer;
	int page_shift = 0;
	int uar_index = 0;
	int npages;
	u32 offset = 0;
	int bfregn;
	int ncont = 0;
	__be64 *pas;
	void *qpc;
	int err;
	u16 uid;

	err = ib_copy_from_udata(&ucmd, udata, sizeof(ucmd));
	if (err) {
		mlx5_ib_dbg(dev, "copy failed\n");
		return err;
	}

	context = rdma_udata_to_drv_context(udata, struct mlx5_ib_ucontext,
					    ibucontext);
	if (ucmd.flags & MLX5_QP_FLAG_BFREG_INDEX) {
		uar_index = bfregn_to_uar_index(dev, &context->bfregi,
						ucmd.bfreg_index, true);
		if (uar_index < 0)
			return uar_index;

		bfregn = MLX5_IB_INVALID_BFREG;
	} else if (qp->flags & MLX5_IB_QP_CROSS_CHANNEL) {
		/*
		 * TBD: should come from the verbs when we have the API
		 */
		/* In CROSS_CHANNEL CQ and QP must use the same UAR */
		bfregn = MLX5_CROSS_CHANNEL_BFREG;
	}
	else {
		bfregn = alloc_bfreg(dev, &context->bfregi);
		if (bfregn < 0)
			return bfregn;
	}

	mlx5_ib_dbg(dev, "bfregn 0x%x, uar_index 0x%x\n", bfregn, uar_index);
	if (bfregn != MLX5_IB_INVALID_BFREG)
		uar_index = bfregn_to_uar_index(dev, &context->bfregi, bfregn,
						false);

	qp->rq.offset = 0;
	qp->sq.wqe_shift = ilog2(MLX5_SEND_WQE_BB);
	qp->sq.offset = qp->rq.wqe_cnt << qp->rq.wqe_shift;

	err = set_user_buf_size(dev, qp, &ucmd, base, attr);
	if (err)
		goto err_bfreg;

	if (ucmd.buf_addr && ubuffer->buf_size) {
		ubuffer->buf_addr = ucmd.buf_addr;
		err = mlx5_ib_umem_get(dev, udata, ubuffer->buf_addr,
				       ubuffer->buf_size, &ubuffer->umem,
				       &npages, &page_shift, &ncont, &offset);
		if (err)
			goto err_bfreg;
	} else {
		ubuffer->umem = NULL;
	}

	*inlen = MLX5_ST_SZ_BYTES(create_qp_in) +
		 MLX5_FLD_SZ_BYTES(create_qp_in, pas[0]) * ncont;
	*in = kvzalloc(*inlen, GFP_KERNEL);
	if (!*in) {
		err = -ENOMEM;
		goto err_umem;
	}

	uid = (attr->qp_type != IB_QPT_XRC_TGT &&
	       attr->qp_type != IB_QPT_XRC_INI) ? to_mpd(pd)->uid : 0;
	MLX5_SET(create_qp_in, *in, uid, uid);
	pas = (__be64 *)MLX5_ADDR_OF(create_qp_in, *in, pas);
	if (ubuffer->umem)
		mlx5_ib_populate_pas(dev, ubuffer->umem, page_shift, pas, 0);

	qpc = MLX5_ADDR_OF(create_qp_in, *in, qpc);

	MLX5_SET(qpc, qpc, log_page_size, page_shift - MLX5_ADAPTER_PAGE_SHIFT);
	MLX5_SET(qpc, qpc, page_offset, offset);

	MLX5_SET(qpc, qpc, uar_page, uar_index);
	if (bfregn != MLX5_IB_INVALID_BFREG)
		resp->bfreg_index = adjust_bfregn(dev, &context->bfregi, bfregn);
	else
		resp->bfreg_index = MLX5_IB_INVALID_BFREG;
	qp->bfregn = bfregn;

	err = mlx5_ib_db_map_user(context, udata, ucmd.db_addr, &qp->db);
	if (err) {
		mlx5_ib_dbg(dev, "map failed\n");
		goto err_free;
	}

	err = ib_copy_to_udata(udata, resp, min(udata->outlen, sizeof(*resp)));
	if (err) {
		mlx5_ib_dbg(dev, "copy failed\n");
		goto err_unmap;
	}
	qp->create_type = MLX5_QP_USER;

	return 0;

err_unmap:
	mlx5_ib_db_unmap_user(context, &qp->db);

err_free:
	kvfree(*in);

err_umem:
	ib_umem_release(ubuffer->umem);

err_bfreg:
	if (bfregn != MLX5_IB_INVALID_BFREG)
		mlx5_ib_free_bfreg(dev, &context->bfregi, bfregn);
	return err;
}

static void destroy_qp_user(struct mlx5_ib_dev *dev, struct ib_pd *pd,
			    struct mlx5_ib_qp *qp, struct mlx5_ib_qp_base *base,
			    struct ib_udata *udata)
{
	struct mlx5_ib_ucontext *context =
		rdma_udata_to_drv_context(
			udata,
			struct mlx5_ib_ucontext,
			ibucontext);

	mlx5_ib_db_unmap_user(context, &qp->db);
	ib_umem_release(base->ubuffer.umem);

	/*
	 * Free only the BFREGs which are handled by the kernel.
	 * BFREGs of UARs allocated dynamically are handled by user.
	 */
	if (qp->bfregn != MLX5_IB_INVALID_BFREG)
		mlx5_ib_free_bfreg(dev, &context->bfregi, qp->bfregn);
}

/* get_sq_edge - Get the next nearby edge.
 *
 * An 'edge' is defined as the first following address after the end
 * of the fragment or the SQ. Accordingly, during the WQE construction
 * which repetitively increases the pointer to write the next data, it
 * simply should check if it gets to an edge.
 *
 * @sq - SQ buffer.
 * @idx - Stride index in the SQ buffer.
 *
 * Return:
 *	The new edge.
 */
static void *get_sq_edge(struct mlx5_ib_wq *sq, u32 idx)
{
	void *fragment_end;

	fragment_end = mlx5_frag_buf_get_wqe
		(&sq->fbc,
		 mlx5_frag_buf_get_idx_last_contig_stride(&sq->fbc, idx));

	return fragment_end + MLX5_SEND_WQE_BB;
}

static int create_kernel_qp(struct mlx5_ib_dev *dev,
			    struct ib_qp_init_attr *init_attr,
			    struct mlx5_ib_qp *qp,
			    u32 **in, int *inlen,
			    struct mlx5_ib_qp_base *base)
{
	int uar_index;
	void *qpc;
	int err;

	if (init_attr->create_flags & ~(IB_QP_CREATE_INTEGRITY_EN |
					IB_QP_CREATE_BLOCK_MULTICAST_LOOPBACK |
					IB_QP_CREATE_IPOIB_UD_LSO |
					IB_QP_CREATE_NETIF_QP |
					mlx5_ib_create_qp_sqpn_qp1()))
		return -EINVAL;

	if (init_attr->qp_type == MLX5_IB_QPT_REG_UMR)
		qp->bf.bfreg = &dev->fp_bfreg;
	else
		qp->bf.bfreg = &dev->bfreg;

	/* We need to divide by two since each register is comprised of
	 * two buffers of identical size, namely odd and even
	 */
	qp->bf.buf_size = (1 << MLX5_CAP_GEN(dev->mdev, log_bf_reg_size)) / 2;
	uar_index = qp->bf.bfreg->index;

	err = calc_sq_size(dev, init_attr, qp);
	if (err < 0) {
		mlx5_ib_dbg(dev, "err %d\n", err);
		return err;
	}

	qp->rq.offset = 0;
	qp->sq.offset = qp->rq.wqe_cnt << qp->rq.wqe_shift;
	base->ubuffer.buf_size = err + (qp->rq.wqe_cnt << qp->rq.wqe_shift);

	err = mlx5_frag_buf_alloc_node(dev->mdev, base->ubuffer.buf_size,
				       &qp->buf, dev->mdev->priv.numa_node);
	if (err) {
		mlx5_ib_dbg(dev, "err %d\n", err);
		return err;
	}

	if (qp->rq.wqe_cnt)
		mlx5_init_fbc(qp->buf.frags, qp->rq.wqe_shift,
			      ilog2(qp->rq.wqe_cnt), &qp->rq.fbc);

	if (qp->sq.wqe_cnt) {
		int sq_strides_offset = (qp->sq.offset  & (PAGE_SIZE - 1)) /
					MLX5_SEND_WQE_BB;
		mlx5_init_fbc_offset(qp->buf.frags +
				     (qp->sq.offset / PAGE_SIZE),
				     ilog2(MLX5_SEND_WQE_BB),
				     ilog2(qp->sq.wqe_cnt),
				     sq_strides_offset, &qp->sq.fbc);

		qp->sq.cur_edge = get_sq_edge(&qp->sq, 0);
	}

	*inlen = MLX5_ST_SZ_BYTES(create_qp_in) +
		 MLX5_FLD_SZ_BYTES(create_qp_in, pas[0]) * qp->buf.npages;
	*in = kvzalloc(*inlen, GFP_KERNEL);
	if (!*in) {
		err = -ENOMEM;
		goto err_buf;
	}

	qpc = MLX5_ADDR_OF(create_qp_in, *in, qpc);
	MLX5_SET(qpc, qpc, uar_page, uar_index);
	MLX5_SET(qpc, qpc, log_page_size, qp->buf.page_shift - MLX5_ADAPTER_PAGE_SHIFT);

	/* Set "fast registration enabled" for all kernel QPs */
	MLX5_SET(qpc, qpc, fre, 1);
	MLX5_SET(qpc, qpc, rlky, 1);

	if (init_attr->create_flags & mlx5_ib_create_qp_sqpn_qp1()) {
		MLX5_SET(qpc, qpc, deth_sqpn, 1);
		qp->flags |= MLX5_IB_QP_SQPN_QP1;
	}

	mlx5_fill_page_frag_array(&qp->buf,
				  (__be64 *)MLX5_ADDR_OF(create_qp_in,
							 *in, pas));

	err = mlx5_db_alloc(dev->mdev, &qp->db);
	if (err) {
		mlx5_ib_dbg(dev, "err %d\n", err);
		goto err_free;
	}

	qp->sq.wrid = kvmalloc_array(qp->sq.wqe_cnt,
				     sizeof(*qp->sq.wrid), GFP_KERNEL);
	qp->sq.wr_data = kvmalloc_array(qp->sq.wqe_cnt,
					sizeof(*qp->sq.wr_data), GFP_KERNEL);
	qp->rq.wrid = kvmalloc_array(qp->rq.wqe_cnt,
				     sizeof(*qp->rq.wrid), GFP_KERNEL);
	qp->sq.w_list = kvmalloc_array(qp->sq.wqe_cnt,
				       sizeof(*qp->sq.w_list), GFP_KERNEL);
	qp->sq.wqe_head = kvmalloc_array(qp->sq.wqe_cnt,
					 sizeof(*qp->sq.wqe_head), GFP_KERNEL);

	if (!qp->sq.wrid || !qp->sq.wr_data || !qp->rq.wrid ||
	    !qp->sq.w_list || !qp->sq.wqe_head) {
		err = -ENOMEM;
		goto err_wrid;
	}
	qp->create_type = MLX5_QP_KERNEL;

	return 0;

err_wrid:
	kvfree(qp->sq.wqe_head);
	kvfree(qp->sq.w_list);
	kvfree(qp->sq.wrid);
	kvfree(qp->sq.wr_data);
	kvfree(qp->rq.wrid);
	mlx5_db_free(dev->mdev, &qp->db);

err_free:
	kvfree(*in);

err_buf:
	mlx5_frag_buf_free(dev->mdev, &qp->buf);
	return err;
}

static void destroy_qp_kernel(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp)
{
	kvfree(qp->sq.wqe_head);
	kvfree(qp->sq.w_list);
	kvfree(qp->sq.wrid);
	kvfree(qp->sq.wr_data);
	kvfree(qp->rq.wrid);
	mlx5_db_free(dev->mdev, &qp->db);
	mlx5_frag_buf_free(dev->mdev, &qp->buf);
}

static u32 get_rx_type(struct mlx5_ib_qp *qp, struct ib_qp_init_attr *attr)
{
	if (attr->srq || (attr->qp_type == IB_QPT_XRC_TGT) ||
	    (attr->qp_type == MLX5_IB_QPT_DCI) ||
	    (attr->qp_type == IB_QPT_XRC_INI))
		return MLX5_SRQ_RQ;
	else if (!qp->has_rq)
		return MLX5_ZERO_LEN_RQ;
	else
		return MLX5_NON_ZERO_RQ;
}

static int is_connected(enum ib_qp_type qp_type)
{
	if (qp_type == IB_QPT_RC || qp_type == IB_QPT_UC ||
	    qp_type == MLX5_IB_QPT_DCI)
		return 1;

	return 0;
}

static int create_raw_packet_qp_tis(struct mlx5_ib_dev *dev,
				    struct mlx5_ib_qp *qp,
				    struct mlx5_ib_sq *sq, u32 tdn,
				    struct ib_pd *pd)
{
	u32 in[MLX5_ST_SZ_DW(create_tis_in)] = {0};
	void *tisc = MLX5_ADDR_OF(create_tis_in, in, ctx);

	MLX5_SET(create_tis_in, in, uid, to_mpd(pd)->uid);
	MLX5_SET(tisc, tisc, transport_domain, tdn);
	if (qp->flags & MLX5_IB_QP_UNDERLAY)
		MLX5_SET(tisc, tisc, underlay_qpn, qp->underlay_qpn);

	return mlx5_core_create_tis(dev->mdev, in, sizeof(in), &sq->tisn);
}

static void destroy_raw_packet_qp_tis(struct mlx5_ib_dev *dev,
				      struct mlx5_ib_sq *sq, struct ib_pd *pd)
{
	mlx5_cmd_destroy_tis(dev->mdev, sq->tisn, to_mpd(pd)->uid);
}

static void destroy_flow_rule_vport_sq(struct mlx5_ib_sq *sq)
{
	if (sq->flow_rule)
		mlx5_del_flow_rules(sq->flow_rule);
	sq->flow_rule = NULL;
}

static int create_raw_packet_qp_sq(struct mlx5_ib_dev *dev,
				   struct ib_udata *udata,
				   struct mlx5_ib_sq *sq, void *qpin,
				   struct ib_pd *pd)
{
	struct mlx5_ib_ubuffer *ubuffer = &sq->ubuffer;
	__be64 *pas;
	void *in;
	void *sqc;
	void *qpc = MLX5_ADDR_OF(create_qp_in, qpin, qpc);
	void *wq;
	int inlen;
	int err;
	int page_shift = 0;
	int npages;
	int ncont = 0;
	u32 offset = 0;

	err = mlx5_ib_umem_get(dev, udata, ubuffer->buf_addr, ubuffer->buf_size,
			       &sq->ubuffer.umem, &npages, &page_shift, &ncont,
			       &offset);
	if (err)
		return err;

	inlen = MLX5_ST_SZ_BYTES(create_sq_in) + sizeof(u64) * ncont;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in) {
		err = -ENOMEM;
		goto err_umem;
	}

	MLX5_SET(create_sq_in, in, uid, to_mpd(pd)->uid);
	sqc = MLX5_ADDR_OF(create_sq_in, in, ctx);
	MLX5_SET(sqc, sqc, flush_in_error_en, 1);
	if (MLX5_CAP_ETH(dev->mdev, multi_pkt_send_wqe))
		MLX5_SET(sqc, sqc, allow_multi_pkt_send_wqe, 1);
	MLX5_SET(sqc, sqc, state, MLX5_SQC_STATE_RST);
	MLX5_SET(sqc, sqc, user_index, MLX5_GET(qpc, qpc, user_index));
	MLX5_SET(sqc, sqc, cqn, MLX5_GET(qpc, qpc, cqn_snd));
	MLX5_SET(sqc, sqc, tis_lst_sz, 1);
	MLX5_SET(sqc, sqc, tis_num_0, sq->tisn);
	if (MLX5_CAP_GEN(dev->mdev, eth_net_offloads) &&
	    MLX5_CAP_ETH(dev->mdev, swp))
		MLX5_SET(sqc, sqc, allow_swp, 1);

	wq = MLX5_ADDR_OF(sqc, sqc, wq);
	MLX5_SET(wq, wq, wq_type, MLX5_WQ_TYPE_CYCLIC);
	MLX5_SET(wq, wq, pd, MLX5_GET(qpc, qpc, pd));
	MLX5_SET(wq, wq, uar_page, MLX5_GET(qpc, qpc, uar_page));
	MLX5_SET64(wq, wq, dbr_addr, MLX5_GET64(qpc, qpc, dbr_addr));
	MLX5_SET(wq, wq, log_wq_stride, ilog2(MLX5_SEND_WQE_BB));
	MLX5_SET(wq, wq, log_wq_sz, MLX5_GET(qpc, qpc, log_sq_size));
	MLX5_SET(wq, wq, log_wq_pg_sz,  page_shift - MLX5_ADAPTER_PAGE_SHIFT);
	MLX5_SET(wq, wq, page_offset, offset);

	pas = (__be64 *)MLX5_ADDR_OF(wq, wq, pas);
	mlx5_ib_populate_pas(dev, sq->ubuffer.umem, page_shift, pas, 0);

	err = mlx5_core_create_sq_tracked(dev->mdev, in, inlen, &sq->base.mqp);

	kvfree(in);

	if (err)
		goto err_umem;

	return 0;

err_umem:
	ib_umem_release(sq->ubuffer.umem);
	sq->ubuffer.umem = NULL;

	return err;
}

static void destroy_raw_packet_qp_sq(struct mlx5_ib_dev *dev,
				     struct mlx5_ib_sq *sq)
{
	destroy_flow_rule_vport_sq(sq);
	mlx5_core_destroy_sq_tracked(dev->mdev, &sq->base.mqp);
	ib_umem_release(sq->ubuffer.umem);
}

static size_t get_rq_pas_size(void *qpc)
{
	u32 log_page_size = MLX5_GET(qpc, qpc, log_page_size) + 12;
	u32 log_rq_stride = MLX5_GET(qpc, qpc, log_rq_stride);
	u32 log_rq_size   = MLX5_GET(qpc, qpc, log_rq_size);
	u32 page_offset   = MLX5_GET(qpc, qpc, page_offset);
	u32 po_quanta	  = 1 << (log_page_size - 6);
	u32 rq_sz	  = 1 << (log_rq_size + 4 + log_rq_stride);
	u32 page_size	  = 1 << log_page_size;
	u32 rq_sz_po      = rq_sz + (page_offset * po_quanta);
	u32 rq_num_pas	  = (rq_sz_po + page_size - 1) / page_size;

	return rq_num_pas * sizeof(u64);
}

static int create_raw_packet_qp_rq(struct mlx5_ib_dev *dev,
				   struct mlx5_ib_rq *rq, void *qpin,
				   size_t qpinlen, struct ib_pd *pd)
{
	struct mlx5_ib_qp *mqp = rq->base.container_mibqp;
	__be64 *pas;
	__be64 *qp_pas;
	void *in;
	void *rqc;
	void *wq;
	void *qpc = MLX5_ADDR_OF(create_qp_in, qpin, qpc);
	size_t rq_pas_size = get_rq_pas_size(qpc);
	size_t inlen;
	int err;

	if (qpinlen < rq_pas_size + MLX5_BYTE_OFF(create_qp_in, pas))
		return -EINVAL;

	inlen = MLX5_ST_SZ_BYTES(create_rq_in) + rq_pas_size;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	MLX5_SET(create_rq_in, in, uid, to_mpd(pd)->uid);
	rqc = MLX5_ADDR_OF(create_rq_in, in, ctx);
	if (!(rq->flags & MLX5_IB_RQ_CVLAN_STRIPPING))
		MLX5_SET(rqc, rqc, vsd, 1);
	MLX5_SET(rqc, rqc, mem_rq_type, MLX5_RQC_MEM_RQ_TYPE_MEMORY_RQ_INLINE);
	MLX5_SET(rqc, rqc, state, MLX5_RQC_STATE_RST);
	MLX5_SET(rqc, rqc, flush_in_error_en, 1);
	MLX5_SET(rqc, rqc, user_index, MLX5_GET(qpc, qpc, user_index));
	MLX5_SET(rqc, rqc, cqn, MLX5_GET(qpc, qpc, cqn_rcv));

	if (mqp->flags & MLX5_IB_QP_CAP_SCATTER_FCS)
		MLX5_SET(rqc, rqc, scatter_fcs, 1);

	wq = MLX5_ADDR_OF(rqc, rqc, wq);
	MLX5_SET(wq, wq, wq_type, MLX5_WQ_TYPE_CYCLIC);
	if (rq->flags & MLX5_IB_RQ_PCI_WRITE_END_PADDING)
		MLX5_SET(wq, wq, end_padding_mode, MLX5_WQ_END_PAD_MODE_ALIGN);
	MLX5_SET(wq, wq, page_offset, MLX5_GET(qpc, qpc, page_offset));
	MLX5_SET(wq, wq, pd, MLX5_GET(qpc, qpc, pd));
	MLX5_SET64(wq, wq, dbr_addr, MLX5_GET64(qpc, qpc, dbr_addr));
	MLX5_SET(wq, wq, log_wq_stride, MLX5_GET(qpc, qpc, log_rq_stride) + 4);
	MLX5_SET(wq, wq, log_wq_pg_sz, MLX5_GET(qpc, qpc, log_page_size));
	MLX5_SET(wq, wq, log_wq_sz, MLX5_GET(qpc, qpc, log_rq_size));

	pas = (__be64 *)MLX5_ADDR_OF(wq, wq, pas);
	qp_pas = (__be64 *)MLX5_ADDR_OF(create_qp_in, qpin, pas);
	memcpy(pas, qp_pas, rq_pas_size);

	err = mlx5_core_create_rq_tracked(dev->mdev, in, inlen, &rq->base.mqp);

	kvfree(in);

	return err;
}

static void destroy_raw_packet_qp_rq(struct mlx5_ib_dev *dev,
				     struct mlx5_ib_rq *rq)
{
	mlx5_core_destroy_rq_tracked(dev->mdev, &rq->base.mqp);
}

static bool tunnel_offload_supported(struct mlx5_core_dev *dev)
{
	return  (MLX5_CAP_ETH(dev, tunnel_stateless_vxlan) ||
		 MLX5_CAP_ETH(dev, tunnel_stateless_gre) ||
		 MLX5_CAP_ETH(dev, tunnel_stateless_geneve_rx));
}

static void destroy_raw_packet_qp_tir(struct mlx5_ib_dev *dev,
				      struct mlx5_ib_rq *rq,
				      u32 qp_flags_en,
				      struct ib_pd *pd)
{
	if (qp_flags_en & (MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC |
			   MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC))
		mlx5_ib_disable_lb(dev, false, true);
	mlx5_cmd_destroy_tir(dev->mdev, rq->tirn, to_mpd(pd)->uid);
}

static int create_raw_packet_qp_tir(struct mlx5_ib_dev *dev,
				    struct mlx5_ib_rq *rq, u32 tdn,
				    u32 *qp_flags_en,
				    struct ib_pd *pd,
				    u32 *out, int outlen)
{
	u8 lb_flag = 0;
	u32 *in;
	void *tirc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_tir_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	MLX5_SET(create_tir_in, in, uid, to_mpd(pd)->uid);
	tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);
	MLX5_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_DIRECT);
	MLX5_SET(tirc, tirc, inline_rqn, rq->base.mqp.qpn);
	MLX5_SET(tirc, tirc, transport_domain, tdn);
	if (*qp_flags_en & MLX5_QP_FLAG_TUNNEL_OFFLOADS)
		MLX5_SET(tirc, tirc, tunneled_offload_en, 1);

	if (*qp_flags_en & MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC)
		lb_flag |= MLX5_TIRC_SELF_LB_BLOCK_BLOCK_UNICAST;

	if (*qp_flags_en & MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC)
		lb_flag |= MLX5_TIRC_SELF_LB_BLOCK_BLOCK_MULTICAST;

	if (dev->is_rep) {
		lb_flag |= MLX5_TIRC_SELF_LB_BLOCK_BLOCK_UNICAST;
		*qp_flags_en |= MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC;
	}

	MLX5_SET(tirc, tirc, self_lb_block, lb_flag);

	err = mlx5_core_create_tir_out(dev->mdev, in, inlen, out, outlen);

	rq->tirn = MLX5_GET(create_tir_out, out, tirn);
	if (!err && MLX5_GET(tirc, tirc, self_lb_block)) {
		err = mlx5_ib_enable_lb(dev, false, true);

		if (err)
			destroy_raw_packet_qp_tir(dev, rq, 0, pd);
	}
	kvfree(in);

	return err;
}

static int create_raw_packet_qp(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp,
				u32 *in, size_t inlen,
				struct ib_pd *pd,
				struct ib_udata *udata,
				struct mlx5_ib_create_qp_resp *resp)
{
	struct mlx5_ib_raw_packet_qp *raw_packet_qp = &qp->raw_packet_qp;
	struct mlx5_ib_sq *sq = &raw_packet_qp->sq;
	struct mlx5_ib_rq *rq = &raw_packet_qp->rq;
	struct mlx5_ib_ucontext *mucontext = rdma_udata_to_drv_context(
		udata, struct mlx5_ib_ucontext, ibucontext);
	int err;
	u32 tdn = mucontext->tdn;
	u16 uid = to_mpd(pd)->uid;
	u32 out[MLX5_ST_SZ_DW(create_tir_out)] = {};

	if (!qp->sq.wqe_cnt && !qp->rq.wqe_cnt)
		return -EINVAL;
	if (qp->sq.wqe_cnt) {
		err = create_raw_packet_qp_tis(dev, qp, sq, tdn, pd);
		if (err)
			return err;

		err = create_raw_packet_qp_sq(dev, udata, sq, in, pd);
		if (err)
			goto err_destroy_tis;

		if (uid) {
			resp->tisn = sq->tisn;
			resp->comp_mask |= MLX5_IB_CREATE_QP_RESP_MASK_TISN;
			resp->sqn = sq->base.mqp.qpn;
			resp->comp_mask |= MLX5_IB_CREATE_QP_RESP_MASK_SQN;
		}

		sq->base.container_mibqp = qp;
		sq->base.mqp.event = mlx5_ib_qp_event;
	}

	if (qp->rq.wqe_cnt) {
		rq->base.container_mibqp = qp;

		if (qp->flags & MLX5_IB_QP_CVLAN_STRIPPING)
			rq->flags |= MLX5_IB_RQ_CVLAN_STRIPPING;
		if (qp->flags & MLX5_IB_QP_PCI_WRITE_END_PADDING)
			rq->flags |= MLX5_IB_RQ_PCI_WRITE_END_PADDING;
		err = create_raw_packet_qp_rq(dev, rq, in, inlen, pd);
		if (err)
			goto err_destroy_sq;

		err = create_raw_packet_qp_tir(
			dev, rq, tdn, &qp->flags_en, pd, out,
			MLX5_ST_SZ_BYTES(create_tir_out));
		if (err)
			goto err_destroy_rq;

		if (uid) {
			resp->rqn = rq->base.mqp.qpn;
			resp->comp_mask |= MLX5_IB_CREATE_QP_RESP_MASK_RQN;
			resp->tirn = rq->tirn;
			resp->comp_mask |= MLX5_IB_CREATE_QP_RESP_MASK_TIRN;
			if (MLX5_CAP_FLOWTABLE_NIC_RX(dev->mdev, sw_owner)) {
				resp->tir_icm_addr = MLX5_GET(
					create_tir_out, out, icm_address_31_0);
				resp->tir_icm_addr |=
					(u64)MLX5_GET(create_tir_out, out,
						      icm_address_39_32)
					<< 32;
				resp->tir_icm_addr |=
					(u64)MLX5_GET(create_tir_out, out,
						      icm_address_63_40)
					<< 40;
				resp->comp_mask |=
					MLX5_IB_CREATE_QP_RESP_MASK_TIR_ICM_ADDR;
			}
		}
	}

	qp->trans_qp.base.mqp.qpn = qp->sq.wqe_cnt ? sq->base.mqp.qpn :
						     rq->base.mqp.qpn;
	err = ib_copy_to_udata(udata, resp, min(udata->outlen, sizeof(*resp)));
	if (err)
		goto err_destroy_tir;

	return 0;

err_destroy_tir:
	destroy_raw_packet_qp_tir(dev, rq, qp->flags_en, pd);
err_destroy_rq:
	destroy_raw_packet_qp_rq(dev, rq);
err_destroy_sq:
	if (!qp->sq.wqe_cnt)
		return err;
	destroy_raw_packet_qp_sq(dev, sq);
err_destroy_tis:
	destroy_raw_packet_qp_tis(dev, sq, pd);

	return err;
}

static void destroy_raw_packet_qp(struct mlx5_ib_dev *dev,
				  struct mlx5_ib_qp *qp)
{
	struct mlx5_ib_raw_packet_qp *raw_packet_qp = &qp->raw_packet_qp;
	struct mlx5_ib_sq *sq = &raw_packet_qp->sq;
	struct mlx5_ib_rq *rq = &raw_packet_qp->rq;

	if (qp->rq.wqe_cnt) {
		destroy_raw_packet_qp_tir(dev, rq, qp->flags_en, qp->ibqp.pd);
		destroy_raw_packet_qp_rq(dev, rq);
	}

	if (qp->sq.wqe_cnt) {
		destroy_raw_packet_qp_sq(dev, sq);
		destroy_raw_packet_qp_tis(dev, sq, qp->ibqp.pd);
	}
}

static void raw_packet_qp_copy_info(struct mlx5_ib_qp *qp,
				    struct mlx5_ib_raw_packet_qp *raw_packet_qp)
{
	struct mlx5_ib_sq *sq = &raw_packet_qp->sq;
	struct mlx5_ib_rq *rq = &raw_packet_qp->rq;

	sq->sq = &qp->sq;
	rq->rq = &qp->rq;
	sq->doorbell = &qp->db;
	rq->doorbell = &qp->db;
}

static void destroy_rss_raw_qp_tir(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp)
{
	if (qp->flags_en & (MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC |
			    MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC))
		mlx5_ib_disable_lb(dev, false, true);
	mlx5_cmd_destroy_tir(dev->mdev, qp->rss_qp.tirn,
			     to_mpd(qp->ibqp.pd)->uid);
}

static int create_rss_raw_qp_tir(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp,
				 struct ib_pd *pd,
				 struct ib_qp_init_attr *init_attr,
				 struct ib_udata *udata)
{
	struct mlx5_ib_ucontext *mucontext = rdma_udata_to_drv_context(
		udata, struct mlx5_ib_ucontext, ibucontext);
	struct mlx5_ib_create_qp_resp resp = {};
	int inlen;
	int outlen;
	int err;
	u32 *in;
	u32 *out;
	void *tirc;
	void *hfso;
	u32 selected_fields = 0;
	u32 outer_l4;
	size_t min_resp_len;
	u32 tdn = mucontext->tdn;
	struct mlx5_ib_create_qp_rss ucmd = {};
	size_t required_cmd_sz;
	u8 lb_flag = 0;

	if (init_attr->qp_type != IB_QPT_RAW_PACKET)
		return -EOPNOTSUPP;

	if (init_attr->create_flags || init_attr->send_cq)
		return -EINVAL;

	min_resp_len = offsetof(typeof(resp), bfreg_index) + sizeof(resp.bfreg_index);
	if (udata->outlen < min_resp_len)
		return -EINVAL;

	required_cmd_sz = offsetof(typeof(ucmd), flags) + sizeof(ucmd.flags);
	if (udata->inlen < required_cmd_sz) {
		mlx5_ib_dbg(dev, "invalid inlen\n");
		return -EINVAL;
	}

	if (udata->inlen > sizeof(ucmd) &&
	    !ib_is_udata_cleared(udata, sizeof(ucmd),
				 udata->inlen - sizeof(ucmd))) {
		mlx5_ib_dbg(dev, "inlen is not supported\n");
		return -EOPNOTSUPP;
	}

	if (ib_copy_from_udata(&ucmd, udata, min(sizeof(ucmd), udata->inlen))) {
		mlx5_ib_dbg(dev, "copy failed\n");
		return -EFAULT;
	}

	if (ucmd.comp_mask) {
		mlx5_ib_dbg(dev, "invalid comp mask\n");
		return -EOPNOTSUPP;
	}

	if (ucmd.flags & ~(MLX5_QP_FLAG_TUNNEL_OFFLOADS |
			   MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC |
			   MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC)) {
		mlx5_ib_dbg(dev, "invalid flags\n");
		return -EOPNOTSUPP;
	}

	if (ucmd.flags & MLX5_QP_FLAG_TUNNEL_OFFLOADS &&
	    !tunnel_offload_supported(dev->mdev)) {
		mlx5_ib_dbg(dev, "tunnel offloads isn't supported\n");
		return -EOPNOTSUPP;
	}

	if (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_INNER &&
	    !(ucmd.flags & MLX5_QP_FLAG_TUNNEL_OFFLOADS)) {
		mlx5_ib_dbg(dev, "Tunnel offloads must be set for inner RSS\n");
		return -EOPNOTSUPP;
	}

	if (ucmd.flags & MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC || dev->is_rep) {
		lb_flag |= MLX5_TIRC_SELF_LB_BLOCK_BLOCK_UNICAST;
		qp->flags_en |= MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC;
	}

	if (ucmd.flags & MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC) {
		lb_flag |= MLX5_TIRC_SELF_LB_BLOCK_BLOCK_MULTICAST;
		qp->flags_en |= MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC;
	}

	err = ib_copy_to_udata(udata, &resp, min(udata->outlen, sizeof(resp)));
	if (err) {
		mlx5_ib_dbg(dev, "copy failed\n");
		return -EINVAL;
	}

	inlen = MLX5_ST_SZ_BYTES(create_tir_in);
	outlen = MLX5_ST_SZ_BYTES(create_tir_out);
	in = kvzalloc(inlen + outlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	out = in + MLX5_ST_SZ_DW(create_tir_in);
	MLX5_SET(create_tir_in, in, uid, to_mpd(pd)->uid);
	tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);
	MLX5_SET(tirc, tirc, disp_type,
		 MLX5_TIRC_DISP_TYPE_INDIRECT);
	MLX5_SET(tirc, tirc, indirect_table,
		 init_attr->rwq_ind_tbl->ind_tbl_num);
	MLX5_SET(tirc, tirc, transport_domain, tdn);

	hfso = MLX5_ADDR_OF(tirc, tirc, rx_hash_field_selector_outer);

	if (ucmd.flags & MLX5_QP_FLAG_TUNNEL_OFFLOADS)
		MLX5_SET(tirc, tirc, tunneled_offload_en, 1);

	MLX5_SET(tirc, tirc, self_lb_block, lb_flag);

	if (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_INNER)
		hfso = MLX5_ADDR_OF(tirc, tirc, rx_hash_field_selector_inner);
	else
		hfso = MLX5_ADDR_OF(tirc, tirc, rx_hash_field_selector_outer);

	switch (ucmd.rx_hash_function) {
	case MLX5_RX_HASH_FUNC_TOEPLITZ:
	{
		void *rss_key = MLX5_ADDR_OF(tirc, tirc, rx_hash_toeplitz_key);
		size_t len = MLX5_FLD_SZ_BYTES(tirc, rx_hash_toeplitz_key);

		if (len != ucmd.rx_key_len) {
			err = -EINVAL;
			goto err;
		}

		MLX5_SET(tirc, tirc, rx_hash_fn, MLX5_RX_HASH_FN_TOEPLITZ);
		memcpy(rss_key, ucmd.rx_hash_key, len);
		break;
	}
	default:
		err = -EOPNOTSUPP;
		goto err;
	}

	if (!ucmd.rx_hash_fields_mask) {
		/* special case when this TIR serves as steering entry without hashing */
		if (!init_attr->rwq_ind_tbl->log_ind_tbl_size)
			goto create_tir;
		err = -EINVAL;
		goto err;
	}

	if (((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_IPV4) ||
	     (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_IPV4)) &&
	     ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_IPV6) ||
	     (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_IPV6))) {
		err = -EINVAL;
		goto err;
	}

	/* If none of IPV4 & IPV6 SRC/DST was set - this bit field is ignored */
	if ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_IPV4) ||
	    (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_IPV4))
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
	else if ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_IPV6) ||
		 (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_IPV6))
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);

	outer_l4 = ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_PORT_TCP) ||
		    (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_PORT_TCP)) << 0 |
		   ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_PORT_UDP) ||
		    (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_PORT_UDP)) << 1 |
		   (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_IPSEC_SPI) << 2;

	/* Check that only one l4 protocol is set */
	if (outer_l4 & (outer_l4 - 1)) {
		err = -EINVAL;
		goto err;
	}

	/* If none of TCP & UDP SRC/DST was set - this bit field is ignored */
	if ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_PORT_TCP) ||
	    (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_PORT_TCP))
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_TCP);
	else if ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_PORT_UDP) ||
		 (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_PORT_UDP))
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_UDP);

	if ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_IPV4) ||
	    (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_IPV6))
		selected_fields |= MLX5_HASH_FIELD_SEL_SRC_IP;

	if ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_IPV4) ||
	    (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_IPV6))
		selected_fields |= MLX5_HASH_FIELD_SEL_DST_IP;

	if ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_PORT_TCP) ||
	    (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_SRC_PORT_UDP))
		selected_fields |= MLX5_HASH_FIELD_SEL_L4_SPORT;

	if ((ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_PORT_TCP) ||
	    (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_DST_PORT_UDP))
		selected_fields |= MLX5_HASH_FIELD_SEL_L4_DPORT;

	if (ucmd.rx_hash_fields_mask & MLX5_RX_HASH_IPSEC_SPI)
		selected_fields |= MLX5_HASH_FIELD_SEL_IPSEC_SPI;

	MLX5_SET(rx_hash_field_select, hfso, selected_fields, selected_fields);

create_tir:
	err = mlx5_core_create_tir_out(dev->mdev, in, inlen, out, outlen);

	qp->rss_qp.tirn = MLX5_GET(create_tir_out, out, tirn);
	if (!err && MLX5_GET(tirc, tirc, self_lb_block)) {
		err = mlx5_ib_enable_lb(dev, false, true);

		if (err)
			mlx5_cmd_destroy_tir(dev->mdev, qp->rss_qp.tirn,
					     to_mpd(pd)->uid);
	}

	if (err)
		goto err;

	if (mucontext->devx_uid) {
		resp.comp_mask |= MLX5_IB_CREATE_QP_RESP_MASK_TIRN;
		resp.tirn = qp->rss_qp.tirn;
		if (MLX5_CAP_FLOWTABLE_NIC_RX(dev->mdev, sw_owner)) {
			resp.tir_icm_addr =
				MLX5_GET(create_tir_out, out, icm_address_31_0);
			resp.tir_icm_addr |= (u64)MLX5_GET(create_tir_out, out,
							   icm_address_39_32)
					     << 32;
			resp.tir_icm_addr |= (u64)MLX5_GET(create_tir_out, out,
							   icm_address_63_40)
					     << 40;
			resp.comp_mask |=
				MLX5_IB_CREATE_QP_RESP_MASK_TIR_ICM_ADDR;
		}
	}

	err = ib_copy_to_udata(udata, &resp, min(udata->outlen, sizeof(resp)));
	if (err)
		goto err_copy;

	kvfree(in);
	/* qpn is reserved for that QP */
	qp->trans_qp.base.mqp.qpn = 0;
	qp->flags |= MLX5_IB_QP_RSS;
	return 0;

err_copy:
	mlx5_cmd_destroy_tir(dev->mdev, qp->rss_qp.tirn, mucontext->devx_uid);
err:
	kvfree(in);
	return err;
}

static void configure_responder_scat_cqe(struct ib_qp_init_attr *init_attr,
					 void *qpc)
{
	int rcqe_sz;

	if (init_attr->qp_type == MLX5_IB_QPT_DCI)
		return;

	rcqe_sz = mlx5_ib_get_cqe_size(init_attr->recv_cq);

	if (init_attr->qp_type == MLX5_IB_QPT_DCT) {
		if (rcqe_sz == 128)
			MLX5_SET(dctc, qpc, cs_res, MLX5_RES_SCAT_DATA64_CQE);

		return;
	}

	MLX5_SET(qpc, qpc, cs_res,
		 rcqe_sz == 128 ? MLX5_RES_SCAT_DATA64_CQE :
				  MLX5_RES_SCAT_DATA32_CQE);
}

static void configure_requester_scat_cqe(struct mlx5_ib_dev *dev,
					 struct ib_qp_init_attr *init_attr,
					 struct mlx5_ib_create_qp *ucmd,
					 void *qpc)
{
	enum ib_qp_type qpt = init_attr->qp_type;
	int scqe_sz;
	bool allow_scat_cqe = 0;

	if (qpt == IB_QPT_UC || qpt == IB_QPT_UD)
		return;

	if (ucmd)
		allow_scat_cqe = ucmd->flags & MLX5_QP_FLAG_ALLOW_SCATTER_CQE;

	if (!allow_scat_cqe && init_attr->sq_sig_type != IB_SIGNAL_ALL_WR)
		return;

	scqe_sz = mlx5_ib_get_cqe_size(init_attr->send_cq);
	if (scqe_sz == 128) {
		MLX5_SET(qpc, qpc, cs_req, MLX5_REQ_SCAT_DATA64_CQE);
		return;
	}

	if (init_attr->qp_type != MLX5_IB_QPT_DCI ||
	    MLX5_CAP_GEN(dev->mdev, dc_req_scat_data_cqe))
		MLX5_SET(qpc, qpc, cs_req, MLX5_REQ_SCAT_DATA32_CQE);
}

static int atomic_size_to_mode(int size_mask)
{
	/* driver does not support atomic_size > 256B
	 * and does not know how to translate bigger sizes
	 */
	int supported_size_mask = size_mask & 0x1ff;
	int log_max_size;

	if (!supported_size_mask)
		return -EOPNOTSUPP;

	log_max_size = __fls(supported_size_mask);

	if (log_max_size > 3)
		return log_max_size;

	return MLX5_ATOMIC_MODE_8B;
}

static int get_atomic_mode(struct mlx5_ib_dev *dev,
			   enum ib_qp_type qp_type)
{
	u8 atomic_operations = MLX5_CAP_ATOMIC(dev->mdev, atomic_operations);
	u8 atomic = MLX5_CAP_GEN(dev->mdev, atomic);
	int atomic_mode = -EOPNOTSUPP;
	int atomic_size_mask;

	if (!atomic)
		return -EOPNOTSUPP;

	if (qp_type == MLX5_IB_QPT_DCT)
		atomic_size_mask = MLX5_CAP_ATOMIC(dev->mdev, atomic_size_dc);
	else
		atomic_size_mask = MLX5_CAP_ATOMIC(dev->mdev, atomic_size_qp);

	if ((atomic_operations & MLX5_ATOMIC_OPS_EXTENDED_CMP_SWAP) ||
	    (atomic_operations & MLX5_ATOMIC_OPS_EXTENDED_FETCH_ADD))
		atomic_mode = atomic_size_to_mode(atomic_size_mask);

	if (atomic_mode <= 0 &&
	    (atomic_operations & MLX5_ATOMIC_OPS_CMP_SWAP &&
	     atomic_operations & MLX5_ATOMIC_OPS_FETCH_ADD))
		atomic_mode = MLX5_ATOMIC_MODE_IB_COMP;

	return atomic_mode;
}

static inline bool check_flags_mask(uint64_t input, uint64_t supported)
{
	return (input & ~supported) == 0;
}

static int create_qp_common(struct mlx5_ib_dev *dev, struct ib_pd *pd,
			    struct ib_qp_init_attr *init_attr,
			    struct ib_udata *udata, struct mlx5_ib_qp *qp)
{
	struct mlx5_ib_resources *devr = &dev->devr;
	int inlen = MLX5_ST_SZ_BYTES(create_qp_in);
	struct mlx5_core_dev *mdev = dev->mdev;
	struct mlx5_ib_create_qp_resp resp = {};
	struct mlx5_ib_ucontext *ucontext = rdma_udata_to_drv_context(
		udata, struct mlx5_ib_ucontext, ibucontext);
	struct mlx5_ib_cq *send_cq;
	struct mlx5_ib_cq *recv_cq;
	unsigned long flags;
	u32 uidx = MLX5_IB_DEFAULT_UIDX;
	struct mlx5_ib_create_qp ucmd;
	struct mlx5_ib_qp_base *base;
	int mlx5_st;
	void *qpc;
	u32 *in;
	int err;

	mutex_init(&qp->mutex);
	spin_lock_init(&qp->sq.lock);
	spin_lock_init(&qp->rq.lock);

	mlx5_st = to_mlx5_st(init_attr->qp_type);
	if (mlx5_st < 0)
		return -EINVAL;

	if (init_attr->rwq_ind_tbl) {
		if (!udata)
			return -ENOSYS;

		err = create_rss_raw_qp_tir(dev, qp, pd, init_attr, udata);
		return err;
	}

	if (init_attr->create_flags & IB_QP_CREATE_BLOCK_MULTICAST_LOOPBACK) {
		if (!MLX5_CAP_GEN(mdev, block_lb_mc)) {
			mlx5_ib_dbg(dev, "block multicast loopback isn't supported\n");
			return -EINVAL;
		} else {
			qp->flags |= MLX5_IB_QP_BLOCK_MULTICAST_LOOPBACK;
		}
	}

	if (init_attr->create_flags &
			(IB_QP_CREATE_CROSS_CHANNEL |
			 IB_QP_CREATE_MANAGED_SEND |
			 IB_QP_CREATE_MANAGED_RECV)) {
		if (!MLX5_CAP_GEN(mdev, cd)) {
			mlx5_ib_dbg(dev, "cross-channel isn't supported\n");
			return -EINVAL;
		}
		if (init_attr->create_flags & IB_QP_CREATE_CROSS_CHANNEL)
			qp->flags |= MLX5_IB_QP_CROSS_CHANNEL;
		if (init_attr->create_flags & IB_QP_CREATE_MANAGED_SEND)
			qp->flags |= MLX5_IB_QP_MANAGED_SEND;
		if (init_attr->create_flags & IB_QP_CREATE_MANAGED_RECV)
			qp->flags |= MLX5_IB_QP_MANAGED_RECV;
	}

	if (init_attr->qp_type == IB_QPT_UD &&
	    (init_attr->create_flags & IB_QP_CREATE_IPOIB_UD_LSO))
		if (!MLX5_CAP_GEN(mdev, ipoib_basic_offloads)) {
			mlx5_ib_dbg(dev, "ipoib UD lso qp isn't supported\n");
			return -EOPNOTSUPP;
		}

	if (init_attr->create_flags & IB_QP_CREATE_SCATTER_FCS) {
		if (init_attr->qp_type != IB_QPT_RAW_PACKET) {
			mlx5_ib_dbg(dev, "Scatter FCS is supported only for Raw Packet QPs");
			return -EOPNOTSUPP;
		}
		if (!MLX5_CAP_GEN(dev->mdev, eth_net_offloads) ||
		    !MLX5_CAP_ETH(dev->mdev, scatter_fcs)) {
			mlx5_ib_dbg(dev, "Scatter FCS isn't supported\n");
			return -EOPNOTSUPP;
		}
		qp->flags |= MLX5_IB_QP_CAP_SCATTER_FCS;
	}

	if (init_attr->sq_sig_type == IB_SIGNAL_ALL_WR)
		qp->sq_signal_bits = MLX5_WQE_CTRL_CQ_UPDATE;

	if (init_attr->create_flags & IB_QP_CREATE_CVLAN_STRIPPING) {
		if (!(MLX5_CAP_GEN(dev->mdev, eth_net_offloads) &&
		      MLX5_CAP_ETH(dev->mdev, vlan_cap)) ||
		    (init_attr->qp_type != IB_QPT_RAW_PACKET))
			return -EOPNOTSUPP;
		qp->flags |= MLX5_IB_QP_CVLAN_STRIPPING;
	}

	if (udata) {
		if (ib_copy_from_udata(&ucmd, udata, sizeof(ucmd))) {
			mlx5_ib_dbg(dev, "copy failed\n");
			return -EFAULT;
		}

		if (!check_flags_mask(ucmd.flags,
				      MLX5_QP_FLAG_ALLOW_SCATTER_CQE |
				      MLX5_QP_FLAG_BFREG_INDEX |
				      MLX5_QP_FLAG_PACKET_BASED_CREDIT_MODE |
				      MLX5_QP_FLAG_SCATTER_CQE |
				      MLX5_QP_FLAG_SIGNATURE |
				      MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC |
				      MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC |
				      MLX5_QP_FLAG_TUNNEL_OFFLOADS |
				      MLX5_QP_FLAG_TYPE_DCI |
				      MLX5_QP_FLAG_TYPE_DCT))
			return -EINVAL;

		err = get_qp_user_index(ucontext, &ucmd, udata->inlen, &uidx);
		if (err)
			return err;

		qp->wq_sig = !!(ucmd.flags & MLX5_QP_FLAG_SIGNATURE);
		if (MLX5_CAP_GEN(dev->mdev, sctr_data_cqe))
			qp->scat_cqe = !!(ucmd.flags & MLX5_QP_FLAG_SCATTER_CQE);
		if (ucmd.flags & MLX5_QP_FLAG_TUNNEL_OFFLOADS) {
			if (init_attr->qp_type != IB_QPT_RAW_PACKET ||
			    !tunnel_offload_supported(mdev)) {
				mlx5_ib_dbg(dev, "Tunnel offload isn't supported\n");
				return -EOPNOTSUPP;
			}
			qp->flags_en |= MLX5_QP_FLAG_TUNNEL_OFFLOADS;
		}

		if (ucmd.flags & MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC) {
			if (init_attr->qp_type != IB_QPT_RAW_PACKET) {
				mlx5_ib_dbg(dev, "Self-LB UC isn't supported\n");
				return -EOPNOTSUPP;
			}
			qp->flags_en |= MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_UC;
		}

		if (ucmd.flags & MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC) {
			if (init_attr->qp_type != IB_QPT_RAW_PACKET) {
				mlx5_ib_dbg(dev, "Self-LB UM isn't supported\n");
				return -EOPNOTSUPP;
			}
			qp->flags_en |= MLX5_QP_FLAG_TIR_ALLOW_SELF_LB_MC;
		}

		if (ucmd.flags & MLX5_QP_FLAG_PACKET_BASED_CREDIT_MODE) {
			if (init_attr->qp_type != IB_QPT_RC ||
				!MLX5_CAP_GEN(dev->mdev, qp_packet_based)) {
				mlx5_ib_dbg(dev, "packet based credit mode isn't supported\n");
				return -EOPNOTSUPP;
			}
			qp->flags |= MLX5_IB_QP_PACKET_BASED_CREDIT;
		}

		if (init_attr->create_flags & IB_QP_CREATE_SOURCE_QPN) {
			if (init_attr->qp_type != IB_QPT_UD ||
			    (MLX5_CAP_GEN(dev->mdev, port_type) !=
			     MLX5_CAP_PORT_TYPE_IB) ||
			    !mlx5_get_flow_namespace(dev->mdev, MLX5_FLOW_NAMESPACE_BYPASS)) {
				mlx5_ib_dbg(dev, "Source QP option isn't supported\n");
				return -EOPNOTSUPP;
			}

			qp->flags |= MLX5_IB_QP_UNDERLAY;
			qp->underlay_qpn = init_attr->source_qpn;
		}
	} else {
		qp->wq_sig = !!wq_signature;
	}

	base = (init_attr->qp_type == IB_QPT_RAW_PACKET ||
		qp->flags & MLX5_IB_QP_UNDERLAY) ?
	       &qp->raw_packet_qp.rq.base :
	       &qp->trans_qp.base;

	qp->has_rq = qp_has_rq(init_attr);
	err = set_rq_size(dev, &init_attr->cap, qp->has_rq,
			  qp, udata ? &ucmd : NULL);
	if (err) {
		mlx5_ib_dbg(dev, "err %d\n", err);
		return err;
	}

	if (pd) {
		if (udata) {
			__u32 max_wqes =
				1 << MLX5_CAP_GEN(mdev, log_max_qp_sz);
			mlx5_ib_dbg(dev, "requested sq_wqe_count (%d)\n", ucmd.sq_wqe_count);
			if (ucmd.rq_wqe_shift != qp->rq.wqe_shift ||
			    ucmd.rq_wqe_count != qp->rq.wqe_cnt) {
				mlx5_ib_dbg(dev, "invalid rq params\n");
				return -EINVAL;
			}
			if (ucmd.sq_wqe_count > max_wqes) {
				mlx5_ib_dbg(dev, "requested sq_wqe_count (%d) > max allowed (%d)\n",
					    ucmd.sq_wqe_count, max_wqes);
				return -EINVAL;
			}
			if (init_attr->create_flags &
			    mlx5_ib_create_qp_sqpn_qp1()) {
				mlx5_ib_dbg(dev, "user-space is not allowed to create UD QPs spoofing as QP1\n");
				return -EINVAL;
			}
			err = create_user_qp(dev, pd, qp, udata, init_attr, &in,
					     &resp, &inlen, base);
			if (err)
				mlx5_ib_dbg(dev, "err %d\n", err);
		} else {
			err = create_kernel_qp(dev, init_attr, qp, &in, &inlen,
					       base);
			if (err)
				mlx5_ib_dbg(dev, "err %d\n", err);
		}

		if (err)
			return err;
	} else {
		in = kvzalloc(inlen, GFP_KERNEL);
		if (!in)
			return -ENOMEM;

		qp->create_type = MLX5_QP_EMPTY;
	}

	if (is_sqp(init_attr->qp_type))
		qp->port = init_attr->port_num;

	qpc = MLX5_ADDR_OF(create_qp_in, in, qpc);

	MLX5_SET(qpc, qpc, st, mlx5_st);
	MLX5_SET(qpc, qpc, pm_state, MLX5_QP_PM_MIGRATED);

	if (init_attr->qp_type != MLX5_IB_QPT_REG_UMR)
		MLX5_SET(qpc, qpc, pd, to_mpd(pd ? pd : devr->p0)->pdn);
	else
		MLX5_SET(qpc, qpc, latency_sensitive, 1);


	if (qp->wq_sig)
		MLX5_SET(qpc, qpc, wq_signature, 1);

	if (qp->flags & MLX5_IB_QP_BLOCK_MULTICAST_LOOPBACK)
		MLX5_SET(qpc, qpc, block_lb_mc, 1);

	if (qp->flags & MLX5_IB_QP_CROSS_CHANNEL)
		MLX5_SET(qpc, qpc, cd_master, 1);
	if (qp->flags & MLX5_IB_QP_MANAGED_SEND)
		MLX5_SET(qpc, qpc, cd_slave_send, 1);
	if (qp->flags & MLX5_IB_QP_MANAGED_RECV)
		MLX5_SET(qpc, qpc, cd_slave_receive, 1);
	if (qp->flags & MLX5_IB_QP_PACKET_BASED_CREDIT)
		MLX5_SET(qpc, qpc, req_e2e_credit_mode, 1);
	if (qp->scat_cqe && is_connected(init_attr->qp_type)) {
		configure_responder_scat_cqe(init_attr, qpc);
		configure_requester_scat_cqe(dev, init_attr,
					     udata ? &ucmd : NULL,
					     qpc);
	}

	if (qp->rq.wqe_cnt) {
		MLX5_SET(qpc, qpc, log_rq_stride, qp->rq.wqe_shift - 4);
		MLX5_SET(qpc, qpc, log_rq_size, ilog2(qp->rq.wqe_cnt));
	}

	MLX5_SET(qpc, qpc, rq_type, get_rx_type(qp, init_attr));

	if (qp->sq.wqe_cnt) {
		MLX5_SET(qpc, qpc, log_sq_size, ilog2(qp->sq.wqe_cnt));
	} else {
		MLX5_SET(qpc, qpc, no_sq, 1);
		if (init_attr->srq &&
		    init_attr->srq->srq_type == IB_SRQT_TM)
			MLX5_SET(qpc, qpc, offload_type,
				 MLX5_QPC_OFFLOAD_TYPE_RNDV);
	}

	/* Set default resources */
	switch (init_attr->qp_type) {
	case IB_QPT_XRC_TGT:
		MLX5_SET(qpc, qpc, cqn_rcv, to_mcq(devr->c0)->mcq.cqn);
		MLX5_SET(qpc, qpc, cqn_snd, to_mcq(devr->c0)->mcq.cqn);
		MLX5_SET(qpc, qpc, srqn_rmpn_xrqn, to_msrq(devr->s0)->msrq.srqn);
		MLX5_SET(qpc, qpc, xrcd, to_mxrcd(init_attr->xrcd)->xrcdn);
		break;
	case IB_QPT_XRC_INI:
		MLX5_SET(qpc, qpc, cqn_rcv, to_mcq(devr->c0)->mcq.cqn);
		MLX5_SET(qpc, qpc, xrcd, to_mxrcd(devr->x1)->xrcdn);
		MLX5_SET(qpc, qpc, srqn_rmpn_xrqn, to_msrq(devr->s0)->msrq.srqn);
		break;
	default:
		if (init_attr->srq) {
			MLX5_SET(qpc, qpc, xrcd, to_mxrcd(devr->x0)->xrcdn);
			MLX5_SET(qpc, qpc, srqn_rmpn_xrqn, to_msrq(init_attr->srq)->msrq.srqn);
		} else {
			MLX5_SET(qpc, qpc, xrcd, to_mxrcd(devr->x1)->xrcdn);
			MLX5_SET(qpc, qpc, srqn_rmpn_xrqn, to_msrq(devr->s1)->msrq.srqn);
		}
	}

	if (init_attr->send_cq)
		MLX5_SET(qpc, qpc, cqn_snd, to_mcq(init_attr->send_cq)->mcq.cqn);

	if (init_attr->recv_cq)
		MLX5_SET(qpc, qpc, cqn_rcv, to_mcq(init_attr->recv_cq)->mcq.cqn);

	MLX5_SET64(qpc, qpc, dbr_addr, qp->db.dma);

	/* 0xffffff means we ask to work with cqe version 0 */
	if (MLX5_CAP_GEN(mdev, cqe_version) == MLX5_CQE_VERSION_V1)
		MLX5_SET(qpc, qpc, user_index, uidx);

	/* we use IB_QP_CREATE_IPOIB_UD_LSO to indicates ipoib qp */
	if (init_attr->qp_type == IB_QPT_UD &&
	    (init_attr->create_flags & IB_QP_CREATE_IPOIB_UD_LSO)) {
		MLX5_SET(qpc, qpc, ulp_stateless_offload_mode, 1);
		qp->flags |= MLX5_IB_QP_LSO;
	}

	if (init_attr->create_flags & IB_QP_CREATE_PCI_WRITE_END_PADDING) {
		if (!MLX5_CAP_GEN(dev->mdev, end_pad)) {
			mlx5_ib_dbg(dev, "scatter end padding is not supported\n");
			err = -EOPNOTSUPP;
			goto err;
		} else if (init_attr->qp_type != IB_QPT_RAW_PACKET) {
			MLX5_SET(qpc, qpc, end_padding_mode,
				 MLX5_WQ_END_PAD_MODE_ALIGN);
		} else {
			qp->flags |= MLX5_IB_QP_PCI_WRITE_END_PADDING;
		}
	}

	if (inlen < 0) {
		err = -EINVAL;
		goto err;
	}

	if (init_attr->qp_type == IB_QPT_RAW_PACKET ||
	    qp->flags & MLX5_IB_QP_UNDERLAY) {
		qp->raw_packet_qp.sq.ubuffer.buf_addr = ucmd.sq_buf_addr;
		raw_packet_qp_copy_info(qp, &qp->raw_packet_qp);
		err = create_raw_packet_qp(dev, qp, in, inlen, pd, udata,
					   &resp);
	} else {
		err = mlx5_core_create_qp(dev->mdev, &base->mqp, in, inlen);
	}

	if (err) {
		mlx5_ib_dbg(dev, "create qp failed\n");
		goto err_create;
	}

	kvfree(in);

	base->container_mibqp = qp;
	base->mqp.event = mlx5_ib_qp_event;

	get_cqs(init_attr->qp_type, init_attr->send_cq, init_attr->recv_cq,
		&send_cq, &recv_cq);
	spin_lock_irqsave(&dev->reset_flow_resource_lock, flags);
	mlx5_ib_lock_cqs(send_cq, recv_cq);
	/* Maintain device to QPs access, needed for further handling via reset
	 * flow
	 */
	list_add_tail(&qp->qps_list, &dev->qp_list);
	/* Maintain CQ to QPs access, needed for further handling via reset flow
	 */
	if (send_cq)
		list_add_tail(&qp->cq_send_list, &send_cq->list_send_qp);
	if (recv_cq)
		list_add_tail(&qp->cq_recv_list, &recv_cq->list_recv_qp);
	mlx5_ib_unlock_cqs(send_cq, recv_cq);
	spin_unlock_irqrestore(&dev->reset_flow_resource_lock, flags);

	return 0;

err_create:
	if (qp->create_type == MLX5_QP_USER)
		destroy_qp_user(dev, pd, qp, base, udata);
	else if (qp->create_type == MLX5_QP_KERNEL)
		destroy_qp_kernel(dev, qp);

err:
	kvfree(in);
	return err;
}

static void mlx5_ib_lock_cqs(struct mlx5_ib_cq *send_cq, struct mlx5_ib_cq *recv_cq)
	__acquires(&send_cq->lock) __acquires(&recv_cq->lock)
{
	if (send_cq) {
		if (recv_cq) {
			if (send_cq->mcq.cqn < recv_cq->mcq.cqn)  {
				spin_lock(&send_cq->lock);
				spin_lock_nested(&recv_cq->lock,
						 SINGLE_DEPTH_NESTING);
			} else if (send_cq->mcq.cqn == recv_cq->mcq.cqn) {
				spin_lock(&send_cq->lock);
				__acquire(&recv_cq->lock);
			} else {
				spin_lock(&recv_cq->lock);
				spin_lock_nested(&send_cq->lock,
						 SINGLE_DEPTH_NESTING);
			}
		} else {
			spin_lock(&send_cq->lock);
			__acquire(&recv_cq->lock);
		}
	} else if (recv_cq) {
		spin_lock(&recv_cq->lock);
		__acquire(&send_cq->lock);
	} else {
		__acquire(&send_cq->lock);
		__acquire(&recv_cq->lock);
	}
}

static void mlx5_ib_unlock_cqs(struct mlx5_ib_cq *send_cq, struct mlx5_ib_cq *recv_cq)
	__releases(&send_cq->lock) __releases(&recv_cq->lock)
{
	if (send_cq) {
		if (recv_cq) {
			if (send_cq->mcq.cqn < recv_cq->mcq.cqn)  {
				spin_unlock(&recv_cq->lock);
				spin_unlock(&send_cq->lock);
			} else if (send_cq->mcq.cqn == recv_cq->mcq.cqn) {
				__release(&recv_cq->lock);
				spin_unlock(&send_cq->lock);
			} else {
				spin_unlock(&send_cq->lock);
				spin_unlock(&recv_cq->lock);
			}
		} else {
			__release(&recv_cq->lock);
			spin_unlock(&send_cq->lock);
		}
	} else if (recv_cq) {
		__release(&send_cq->lock);
		spin_unlock(&recv_cq->lock);
	} else {
		__release(&recv_cq->lock);
		__release(&send_cq->lock);
	}
}

static struct mlx5_ib_pd *get_pd(struct mlx5_ib_qp *qp)
{
	return to_mpd(qp->ibqp.pd);
}

static void get_cqs(enum ib_qp_type qp_type,
		    struct ib_cq *ib_send_cq, struct ib_cq *ib_recv_cq,
		    struct mlx5_ib_cq **send_cq, struct mlx5_ib_cq **recv_cq)
{
	switch (qp_type) {
	case IB_QPT_XRC_TGT:
		*send_cq = NULL;
		*recv_cq = NULL;
		break;
	case MLX5_IB_QPT_REG_UMR:
	case IB_QPT_XRC_INI:
		*send_cq = ib_send_cq ? to_mcq(ib_send_cq) : NULL;
		*recv_cq = NULL;
		break;

	case IB_QPT_SMI:
	case MLX5_IB_QPT_HW_GSI:
	case IB_QPT_RC:
	case IB_QPT_UC:
	case IB_QPT_UD:
	case IB_QPT_RAW_IPV6:
	case IB_QPT_RAW_ETHERTYPE:
	case IB_QPT_RAW_PACKET:
		*send_cq = ib_send_cq ? to_mcq(ib_send_cq) : NULL;
		*recv_cq = ib_recv_cq ? to_mcq(ib_recv_cq) : NULL;
		break;

	case IB_QPT_MAX:
	default:
		*send_cq = NULL;
		*recv_cq = NULL;
		break;
	}
}

static int modify_raw_packet_qp(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp,
				const struct mlx5_modify_raw_qp_param *raw_qp_param,
				u8 lag_tx_affinity);

static void destroy_qp_common(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp,
			      struct ib_udata *udata)
{
	struct mlx5_ib_cq *send_cq, *recv_cq;
	struct mlx5_ib_qp_base *base;
	unsigned long flags;
	int err;

	if (qp->ibqp.rwq_ind_tbl) {
		destroy_rss_raw_qp_tir(dev, qp);
		return;
	}

	base = (qp->ibqp.qp_type == IB_QPT_RAW_PACKET ||
		qp->flags & MLX5_IB_QP_UNDERLAY) ?
	       &qp->raw_packet_qp.rq.base :
	       &qp->trans_qp.base;

	if (qp->state != IB_QPS_RESET) {
		if (qp->ibqp.qp_type != IB_QPT_RAW_PACKET &&
		    !(qp->flags & MLX5_IB_QP_UNDERLAY)) {
			err = mlx5_core_qp_modify(dev->mdev,
						  MLX5_CMD_OP_2RST_QP, 0,
						  NULL, &base->mqp);
		} else {
			struct mlx5_modify_raw_qp_param raw_qp_param = {
				.operation = MLX5_CMD_OP_2RST_QP
			};

			err = modify_raw_packet_qp(dev, qp, &raw_qp_param, 0);
		}
		if (err)
			mlx5_ib_warn(dev, "mlx5_ib: modify QP 0x%06x to RESET failed\n",
				     base->mqp.qpn);
	}

	get_cqs(qp->ibqp.qp_type, qp->ibqp.send_cq, qp->ibqp.recv_cq,
		&send_cq, &recv_cq);

	spin_lock_irqsave(&dev->reset_flow_resource_lock, flags);
	mlx5_ib_lock_cqs(send_cq, recv_cq);
	/* del from lists under both locks above to protect reset flow paths */
	list_del(&qp->qps_list);
	if (send_cq)
		list_del(&qp->cq_send_list);

	if (recv_cq)
		list_del(&qp->cq_recv_list);

	if (qp->create_type == MLX5_QP_KERNEL) {
		__mlx5_ib_cq_clean(recv_cq, base->mqp.qpn,
				   qp->ibqp.srq ? to_msrq(qp->ibqp.srq) : NULL);
		if (send_cq != recv_cq)
			__mlx5_ib_cq_clean(send_cq, base->mqp.qpn,
					   NULL);
	}
	mlx5_ib_unlock_cqs(send_cq, recv_cq);
	spin_unlock_irqrestore(&dev->reset_flow_resource_lock, flags);

	if (qp->ibqp.qp_type == IB_QPT_RAW_PACKET ||
	    qp->flags & MLX5_IB_QP_UNDERLAY) {
		destroy_raw_packet_qp(dev, qp);
	} else {
		err = mlx5_core_destroy_qp(dev->mdev, &base->mqp);
		if (err)
			mlx5_ib_warn(dev, "failed to destroy QP 0x%x\n",
				     base->mqp.qpn);
	}

	if (qp->create_type == MLX5_QP_KERNEL)
		destroy_qp_kernel(dev, qp);
	else if (qp->create_type == MLX5_QP_USER)
		destroy_qp_user(dev, &get_pd(qp)->ibpd, qp, base, udata);
}

static const char *ib_qp_type_str(enum ib_qp_type type)
{
	switch (type) {
	case IB_QPT_SMI:
		return "IB_QPT_SMI";
	case IB_QPT_GSI:
		return "IB_QPT_GSI";
	case IB_QPT_RC:
		return "IB_QPT_RC";
	case IB_QPT_UC:
		return "IB_QPT_UC";
	case IB_QPT_UD:
		return "IB_QPT_UD";
	case IB_QPT_RAW_IPV6:
		return "IB_QPT_RAW_IPV6";
	case IB_QPT_RAW_ETHERTYPE:
		return "IB_QPT_RAW_ETHERTYPE";
	case IB_QPT_XRC_INI:
		return "IB_QPT_XRC_INI";
	case IB_QPT_XRC_TGT:
		return "IB_QPT_XRC_TGT";
	case IB_QPT_RAW_PACKET:
		return "IB_QPT_RAW_PACKET";
	case MLX5_IB_QPT_REG_UMR:
		return "MLX5_IB_QPT_REG_UMR";
	case IB_QPT_DRIVER:
		return "IB_QPT_DRIVER";
	case IB_QPT_MAX:
	default:
		return "Invalid QP type";
	}
}

static struct ib_qp *mlx5_ib_create_dct(struct ib_pd *pd,
					struct ib_qp_init_attr *attr,
					struct mlx5_ib_create_qp *ucmd,
					struct ib_udata *udata)
{
	struct mlx5_ib_ucontext *ucontext = rdma_udata_to_drv_context(
		udata, struct mlx5_ib_ucontext, ibucontext);
	struct mlx5_ib_qp *qp;
	int err = 0;
	u32 uidx = MLX5_IB_DEFAULT_UIDX;
	void *dctc;

	if (!attr->srq || !attr->recv_cq)
		return ERR_PTR(-EINVAL);

	err = get_qp_user_index(ucontext, ucmd, sizeof(*ucmd), &uidx);
	if (err)
		return ERR_PTR(err);

	qp = kzalloc(sizeof(*qp), GFP_KERNEL);
	if (!qp)
		return ERR_PTR(-ENOMEM);

	qp->dct.in = kzalloc(MLX5_ST_SZ_BYTES(create_dct_in), GFP_KERNEL);
	if (!qp->dct.in) {
		err = -ENOMEM;
		goto err_free;
	}

	MLX5_SET(create_dct_in, qp->dct.in, uid, to_mpd(pd)->uid);
	dctc = MLX5_ADDR_OF(create_dct_in, qp->dct.in, dct_context_entry);
	qp->qp_sub_type = MLX5_IB_QPT_DCT;
	MLX5_SET(dctc, dctc, pd, to_mpd(pd)->pdn);
	MLX5_SET(dctc, dctc, srqn_xrqn, to_msrq(attr->srq)->msrq.srqn);
	MLX5_SET(dctc, dctc, cqn, to_mcq(attr->recv_cq)->mcq.cqn);
	MLX5_SET64(dctc, dctc, dc_access_key, ucmd->access_key);
	MLX5_SET(dctc, dctc, user_index, uidx);

	if (ucmd->flags & MLX5_QP_FLAG_SCATTER_CQE)
		configure_responder_scat_cqe(attr, dctc);

	qp->state = IB_QPS_RESET;

	return &qp->ibqp;
err_free:
	kfree(qp);
	return ERR_PTR(err);
}

static int set_mlx_qp_type(struct mlx5_ib_dev *dev,
			   struct ib_qp_init_attr *init_attr,
			   struct mlx5_ib_create_qp *ucmd,
			   struct ib_udata *udata)
{
	enum { MLX_QP_FLAGS = MLX5_QP_FLAG_TYPE_DCT | MLX5_QP_FLAG_TYPE_DCI };
	int err;

	if (!udata)
		return -EINVAL;

	if (udata->inlen < sizeof(*ucmd)) {
		mlx5_ib_dbg(dev, "create_qp user command is smaller than expected\n");
		return -EINVAL;
	}
	err = ib_copy_from_udata(ucmd, udata, sizeof(*ucmd));
	if (err)
		return err;

	if ((ucmd->flags & MLX_QP_FLAGS) == MLX5_QP_FLAG_TYPE_DCI) {
		init_attr->qp_type = MLX5_IB_QPT_DCI;
	} else {
		if ((ucmd->flags & MLX_QP_FLAGS) == MLX5_QP_FLAG_TYPE_DCT) {
			init_attr->qp_type = MLX5_IB_QPT_DCT;
		} else {
			mlx5_ib_dbg(dev, "Invalid QP flags\n");
			return -EINVAL;
		}
	}

	if (!MLX5_CAP_GEN(dev->mdev, dct)) {
		mlx5_ib_dbg(dev, "DC transport is not supported\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

struct ib_qp *mlx5_ib_create_qp(struct ib_pd *pd,
				struct ib_qp_init_attr *verbs_init_attr,
				struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev;
	struct mlx5_ib_qp *qp;
	u16 xrcdn = 0;
	int err;
	struct ib_qp_init_attr mlx_init_attr;
	struct ib_qp_init_attr *init_attr = verbs_init_attr;
	struct mlx5_ib_ucontext *ucontext = rdma_udata_to_drv_context(
		udata, struct mlx5_ib_ucontext, ibucontext);

	if (pd) {
		dev = to_mdev(pd->device);

		if (init_attr->qp_type == IB_QPT_RAW_PACKET) {
			if (!ucontext) {
				mlx5_ib_dbg(dev, "Raw Packet QP is not supported for kernel consumers\n");
				return ERR_PTR(-EINVAL);
			} else if (!ucontext->cqe_version) {
				mlx5_ib_dbg(dev, "Raw Packet QP is only supported for CQE version > 0\n");
				return ERR_PTR(-EINVAL);
			}
		}
	} else {
		/* being cautious here */
		if (init_attr->qp_type != IB_QPT_XRC_TGT &&
		    init_attr->qp_type != MLX5_IB_QPT_REG_UMR) {
			pr_warn("%s: no PD for transport %s\n", __func__,
				ib_qp_type_str(init_attr->qp_type));
			return ERR_PTR(-EINVAL);
		}
		dev = to_mdev(to_mxrcd(init_attr->xrcd)->ibxrcd.device);
	}

	if (init_attr->qp_type == IB_QPT_DRIVER) {
		struct mlx5_ib_create_qp ucmd;

		init_attr = &mlx_init_attr;
		memcpy(init_attr, verbs_init_attr, sizeof(*verbs_init_attr));
		err = set_mlx_qp_type(dev, init_attr, &ucmd, udata);
		if (err)
			return ERR_PTR(err);

		if (init_attr->qp_type == MLX5_IB_QPT_DCI) {
			if (init_attr->cap.max_recv_wr ||
			    init_attr->cap.max_recv_sge) {
				mlx5_ib_dbg(dev, "DCI QP requires zero size receive queue\n");
				return ERR_PTR(-EINVAL);
			}
		} else {
			return mlx5_ib_create_dct(pd, init_attr, &ucmd, udata);
		}
	}

	switch (init_attr->qp_type) {
	case IB_QPT_XRC_TGT:
	case IB_QPT_XRC_INI:
		if (!MLX5_CAP_GEN(dev->mdev, xrc)) {
			mlx5_ib_dbg(dev, "XRC not supported\n");
			return ERR_PTR(-ENOSYS);
		}
		init_attr->recv_cq = NULL;
		if (init_attr->qp_type == IB_QPT_XRC_TGT) {
			xrcdn = to_mxrcd(init_attr->xrcd)->xrcdn;
			init_attr->send_cq = NULL;
		}

		/* fall through */
	case IB_QPT_RAW_PACKET:
	case IB_QPT_RC:
	case IB_QPT_UC:
	case IB_QPT_UD:
	case IB_QPT_SMI:
	case MLX5_IB_QPT_HW_GSI:
	case MLX5_IB_QPT_REG_UMR:
	case MLX5_IB_QPT_DCI:
		qp = kzalloc(sizeof(*qp), GFP_KERNEL);
		if (!qp)
			return ERR_PTR(-ENOMEM);

		err = create_qp_common(dev, pd, init_attr, udata, qp);
		if (err) {
			mlx5_ib_dbg(dev, "create_qp_common failed\n");
			kfree(qp);
			return ERR_PTR(err);
		}

		if (is_qp0(init_attr->qp_type))
			qp->ibqp.qp_num = 0;
		else if (is_qp1(init_attr->qp_type))
			qp->ibqp.qp_num = 1;
		else
			qp->ibqp.qp_num = qp->trans_qp.base.mqp.qpn;

		mlx5_ib_dbg(dev, "ib qpnum 0x%x, mlx qpn 0x%x, rcqn 0x%x, scqn 0x%x\n",
			    qp->ibqp.qp_num, qp->trans_qp.base.mqp.qpn,
			    init_attr->recv_cq ? to_mcq(init_attr->recv_cq)->mcq.cqn : -1,
			    init_attr->send_cq ? to_mcq(init_attr->send_cq)->mcq.cqn : -1);

		qp->trans_qp.xrcdn = xrcdn;

		break;

	case IB_QPT_GSI:
		return mlx5_ib_gsi_create_qp(pd, init_attr);

	case IB_QPT_RAW_IPV6:
	case IB_QPT_RAW_ETHERTYPE:
	case IB_QPT_MAX:
	default:
		mlx5_ib_dbg(dev, "unsupported qp type %d\n",
			    init_attr->qp_type);
		/* Don't support raw QPs */
		return ERR_PTR(-EINVAL);
	}

	if (verbs_init_attr->qp_type == IB_QPT_DRIVER)
		qp->qp_sub_type = init_attr->qp_type;

	return &qp->ibqp;
}

static int mlx5_ib_destroy_dct(struct mlx5_ib_qp *mqp)
{
	struct mlx5_ib_dev *dev = to_mdev(mqp->ibqp.device);

	if (mqp->state == IB_QPS_RTR) {
		int err;

		err = mlx5_core_destroy_dct(dev->mdev, &mqp->dct.mdct);
		if (err) {
			mlx5_ib_warn(dev, "failed to destroy DCT %d\n", err);
			return err;
		}
	}

	kfree(mqp->dct.in);
	kfree(mqp);
	return 0;
}

int mlx5_ib_destroy_qp(struct ib_qp *qp, struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev = to_mdev(qp->device);
	struct mlx5_ib_qp *mqp = to_mqp(qp);

	if (unlikely(qp->qp_type == IB_QPT_GSI))
		return mlx5_ib_gsi_destroy_qp(qp);

	if (mqp->qp_sub_type == MLX5_IB_QPT_DCT)
		return mlx5_ib_destroy_dct(mqp);

	destroy_qp_common(dev, mqp, udata);

	kfree(mqp);

	return 0;
}

static int to_mlx5_access_flags(struct mlx5_ib_qp *qp,
				const struct ib_qp_attr *attr,
				int attr_mask, __be32 *hw_access_flags_be)
{
	u8 dest_rd_atomic;
	u32 access_flags, hw_access_flags = 0;

	struct mlx5_ib_dev *dev = to_mdev(qp->ibqp.device);

	if (attr_mask & IB_QP_MAX_DEST_RD_ATOMIC)
		dest_rd_atomic = attr->max_dest_rd_atomic;
	else
		dest_rd_atomic = qp->trans_qp.resp_depth;

	if (attr_mask & IB_QP_ACCESS_FLAGS)
		access_flags = attr->qp_access_flags;
	else
		access_flags = qp->trans_qp.atomic_rd_en;

	if (!dest_rd_atomic)
		access_flags &= IB_ACCESS_REMOTE_WRITE;

	if (access_flags & IB_ACCESS_REMOTE_READ)
		hw_access_flags |= MLX5_QP_BIT_RRE;
	if (access_flags & IB_ACCESS_REMOTE_ATOMIC) {
		int atomic_mode;

		atomic_mode = get_atomic_mode(dev, qp->ibqp.qp_type);
		if (atomic_mode < 0)
			return -EOPNOTSUPP;

		hw_access_flags |= MLX5_QP_BIT_RAE;
		hw_access_flags |= atomic_mode << MLX5_ATOMIC_MODE_OFFSET;
	}

	if (access_flags & IB_ACCESS_REMOTE_WRITE)
		hw_access_flags |= MLX5_QP_BIT_RWE;

	*hw_access_flags_be = cpu_to_be32(hw_access_flags);

	return 0;
}

enum {
	MLX5_PATH_FLAG_FL	= 1 << 0,
	MLX5_PATH_FLAG_FREE_AR	= 1 << 1,
	MLX5_PATH_FLAG_COUNTER	= 1 << 2,
};

static int ib_rate_to_mlx5(struct mlx5_ib_dev *dev, u8 rate)
{
	if (rate == IB_RATE_PORT_CURRENT)
		return 0;

	if (rate < IB_RATE_2_5_GBPS || rate > IB_RATE_600_GBPS)
		return -EINVAL;

	while (rate != IB_RATE_PORT_CURRENT &&
	       !(1 << (rate + MLX5_STAT_RATE_OFFSET) &
		 MLX5_CAP_GEN(dev->mdev, stat_rate_support)))
		--rate;

	return rate ? rate + MLX5_STAT_RATE_OFFSET : rate;
}

static int modify_raw_packet_eth_prio(struct mlx5_core_dev *dev,
				      struct mlx5_ib_sq *sq, u8 sl,
				      struct ib_pd *pd)
{
	void *in;
	void *tisc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_tis_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	MLX5_SET(modify_tis_in, in, bitmask.prio, 1);
	MLX5_SET(modify_tis_in, in, uid, to_mpd(pd)->uid);

	tisc = MLX5_ADDR_OF(modify_tis_in, in, ctx);
	MLX5_SET(tisc, tisc, prio, ((sl & 0x7) << 1));

	err = mlx5_core_modify_tis(dev, sq->tisn, in, inlen);

	kvfree(in);

	return err;
}

static int modify_raw_packet_tx_affinity(struct mlx5_core_dev *dev,
					 struct mlx5_ib_sq *sq, u8 tx_affinity,
					 struct ib_pd *pd)
{
	void *in;
	void *tisc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_tis_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	MLX5_SET(modify_tis_in, in, bitmask.lag_tx_port_affinity, 1);
	MLX5_SET(modify_tis_in, in, uid, to_mpd(pd)->uid);

	tisc = MLX5_ADDR_OF(modify_tis_in, in, ctx);
	MLX5_SET(tisc, tisc, lag_tx_port_affinity, tx_affinity);

	err = mlx5_core_modify_tis(dev, sq->tisn, in, inlen);

	kvfree(in);

	return err;
}

static int mlx5_set_path(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp,
			 const struct rdma_ah_attr *ah,
			 struct mlx5_qp_path *path, u8 port, int attr_mask,
			 u32 path_flags, const struct ib_qp_attr *attr,
			 bool alt)
{
	const struct ib_global_route *grh = rdma_ah_read_grh(ah);
	int err;
	enum ib_gid_type gid_type;
	u8 ah_flags = rdma_ah_get_ah_flags(ah);
	u8 sl = rdma_ah_get_sl(ah);

	if (attr_mask & IB_QP_PKEY_INDEX)
		path->pkey_index = cpu_to_be16(alt ? attr->alt_pkey_index :
						     attr->pkey_index);

	if (ah_flags & IB_AH_GRH) {
		if (grh->sgid_index >=
		    dev->mdev->port_caps[port - 1].gid_table_len) {
			pr_err("sgid_index (%u) too large. max is %d\n",
			       grh->sgid_index,
			       dev->mdev->port_caps[port - 1].gid_table_len);
			return -EINVAL;
		}
	}

	if (ah->type == RDMA_AH_ATTR_TYPE_ROCE) {
		if (!(ah_flags & IB_AH_GRH))
			return -EINVAL;

		memcpy(path->rmac, ah->roce.dmac, sizeof(ah->roce.dmac));
		if (qp->ibqp.qp_type == IB_QPT_RC ||
		    qp->ibqp.qp_type == IB_QPT_UC ||
		    qp->ibqp.qp_type == IB_QPT_XRC_INI ||
		    qp->ibqp.qp_type == IB_QPT_XRC_TGT)
			path->udp_sport =
				mlx5_get_roce_udp_sport(dev, ah->grh.sgid_attr);
		path->dci_cfi_prio_sl = (sl & 0x7) << 4;
		gid_type = ah->grh.sgid_attr->gid_type;
		if (gid_type == IB_GID_TYPE_ROCE_UDP_ENCAP)
			path->ecn_dscp = (grh->traffic_class >> 2) & 0x3f;
	} else {
		path->fl_free_ar = (path_flags & MLX5_PATH_FLAG_FL) ? 0x80 : 0;
		path->fl_free_ar |=
			(path_flags & MLX5_PATH_FLAG_FREE_AR) ? 0x40 : 0;
		path->rlid = cpu_to_be16(rdma_ah_get_dlid(ah));
		path->grh_mlid = rdma_ah_get_path_bits(ah) & 0x7f;
		if (ah_flags & IB_AH_GRH)
			path->grh_mlid	|= 1 << 7;
		path->dci_cfi_prio_sl = sl & 0xf;
	}

	if (ah_flags & IB_AH_GRH) {
		path->mgid_index = grh->sgid_index;
		path->hop_limit  = grh->hop_limit;
		path->tclass_flowlabel =
			cpu_to_be32((grh->traffic_class << 20) |
				    (grh->flow_label));
		memcpy(path->rgid, grh->dgid.raw, 16);
	}

	err = ib_rate_to_mlx5(dev, rdma_ah_get_static_rate(ah));
	if (err < 0)
		return err;
	path->static_rate = err;
	path->port = port;

	if (attr_mask & IB_QP_TIMEOUT)
		path->ackto_lt = (alt ? attr->alt_timeout : attr->timeout) << 3;

	if ((qp->ibqp.qp_type == IB_QPT_RAW_PACKET) && qp->sq.wqe_cnt)
		return modify_raw_packet_eth_prio(dev->mdev,
						  &qp->raw_packet_qp.sq,
						  sl & 0xf, qp->ibqp.pd);

	return 0;
}

static enum mlx5_qp_optpar opt_mask[MLX5_QP_NUM_STATE][MLX5_QP_NUM_STATE][MLX5_QP_ST_MAX] = {
	[MLX5_QP_STATE_INIT] = {
		[MLX5_QP_STATE_INIT] = {
			[MLX5_QP_ST_RC] = MLX5_QP_OPTPAR_RRE		|
					  MLX5_QP_OPTPAR_RAE		|
					  MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_PKEY_INDEX	|
					  MLX5_QP_OPTPAR_PRI_PORT,
			[MLX5_QP_ST_UC] = MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_PKEY_INDEX	|
					  MLX5_QP_OPTPAR_PRI_PORT,
			[MLX5_QP_ST_UD] = MLX5_QP_OPTPAR_PKEY_INDEX	|
					  MLX5_QP_OPTPAR_Q_KEY		|
					  MLX5_QP_OPTPAR_PRI_PORT,
			[MLX5_QP_ST_XRC] = MLX5_QP_OPTPAR_RRE		|
					  MLX5_QP_OPTPAR_RAE		|
					  MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_PKEY_INDEX	|
					  MLX5_QP_OPTPAR_PRI_PORT,
		},
		[MLX5_QP_STATE_RTR] = {
			[MLX5_QP_ST_RC] = MLX5_QP_OPTPAR_ALT_ADDR_PATH  |
					  MLX5_QP_OPTPAR_RRE            |
					  MLX5_QP_OPTPAR_RAE            |
					  MLX5_QP_OPTPAR_RWE            |
					  MLX5_QP_OPTPAR_PKEY_INDEX,
			[MLX5_QP_ST_UC] = MLX5_QP_OPTPAR_ALT_ADDR_PATH  |
					  MLX5_QP_OPTPAR_RWE            |
					  MLX5_QP_OPTPAR_PKEY_INDEX,
			[MLX5_QP_ST_UD] = MLX5_QP_OPTPAR_PKEY_INDEX     |
					  MLX5_QP_OPTPAR_Q_KEY,
			[MLX5_QP_ST_MLX] = MLX5_QP_OPTPAR_PKEY_INDEX	|
					   MLX5_QP_OPTPAR_Q_KEY,
			[MLX5_QP_ST_XRC] = MLX5_QP_OPTPAR_ALT_ADDR_PATH |
					  MLX5_QP_OPTPAR_RRE            |
					  MLX5_QP_OPTPAR_RAE            |
					  MLX5_QP_OPTPAR_RWE            |
					  MLX5_QP_OPTPAR_PKEY_INDEX,
		},
	},
	[MLX5_QP_STATE_RTR] = {
		[MLX5_QP_STATE_RTS] = {
			[MLX5_QP_ST_RC] = MLX5_QP_OPTPAR_ALT_ADDR_PATH	|
					  MLX5_QP_OPTPAR_RRE		|
					  MLX5_QP_OPTPAR_RAE		|
					  MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_PM_STATE	|
					  MLX5_QP_OPTPAR_RNR_TIMEOUT,
			[MLX5_QP_ST_UC] = MLX5_QP_OPTPAR_ALT_ADDR_PATH	|
					  MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_PM_STATE,
			[MLX5_QP_ST_UD] = MLX5_QP_OPTPAR_Q_KEY,
			[MLX5_QP_ST_XRC] = MLX5_QP_OPTPAR_ALT_ADDR_PATH	|
					  MLX5_QP_OPTPAR_RRE		|
					  MLX5_QP_OPTPAR_RAE		|
					  MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_PM_STATE	|
					  MLX5_QP_OPTPAR_RNR_TIMEOUT,
		},
	},
	[MLX5_QP_STATE_RTS] = {
		[MLX5_QP_STATE_RTS] = {
			[MLX5_QP_ST_RC] = MLX5_QP_OPTPAR_RRE		|
					  MLX5_QP_OPTPAR_RAE		|
					  MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_RNR_TIMEOUT	|
					  MLX5_QP_OPTPAR_PM_STATE	|
					  MLX5_QP_OPTPAR_ALT_ADDR_PATH,
			[MLX5_QP_ST_UC] = MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_PM_STATE	|
					  MLX5_QP_OPTPAR_ALT_ADDR_PATH,
			[MLX5_QP_ST_UD] = MLX5_QP_OPTPAR_Q_KEY		|
					  MLX5_QP_OPTPAR_SRQN		|
					  MLX5_QP_OPTPAR_CQN_RCV,
			[MLX5_QP_ST_XRC] = MLX5_QP_OPTPAR_RRE		|
					  MLX5_QP_OPTPAR_RAE		|
					  MLX5_QP_OPTPAR_RWE		|
					  MLX5_QP_OPTPAR_RNR_TIMEOUT	|
					  MLX5_QP_OPTPAR_PM_STATE	|
					  MLX5_QP_OPTPAR_ALT_ADDR_PATH,
		},
	},
	[MLX5_QP_STATE_SQER] = {
		[MLX5_QP_STATE_RTS] = {
			[MLX5_QP_ST_UD]	 = MLX5_QP_OPTPAR_Q_KEY,
			[MLX5_QP_ST_MLX] = MLX5_QP_OPTPAR_Q_KEY,
			[MLX5_QP_ST_UC]	 = MLX5_QP_OPTPAR_RWE,
			[MLX5_QP_ST_RC]	 = MLX5_QP_OPTPAR_RNR_TIMEOUT	|
					   MLX5_QP_OPTPAR_RWE		|
					   MLX5_QP_OPTPAR_RAE		|
					   MLX5_QP_OPTPAR_RRE,
			[MLX5_QP_ST_XRC]  = MLX5_QP_OPTPAR_RNR_TIMEOUT	|
					   MLX5_QP_OPTPAR_RWE		|
					   MLX5_QP_OPTPAR_RAE		|
					   MLX5_QP_OPTPAR_RRE,
		},
	},
};

static int ib_nr_to_mlx5_nr(int ib_mask)
{
	switch (ib_mask) {
	case IB_QP_STATE:
		return 0;
	case IB_QP_CUR_STATE:
		return 0;
	case IB_QP_EN_SQD_ASYNC_NOTIFY:
		return 0;
	case IB_QP_ACCESS_FLAGS:
		return MLX5_QP_OPTPAR_RWE | MLX5_QP_OPTPAR_RRE |
			MLX5_QP_OPTPAR_RAE;
	case IB_QP_PKEY_INDEX:
		return MLX5_QP_OPTPAR_PKEY_INDEX;
	case IB_QP_PORT:
		return MLX5_QP_OPTPAR_PRI_PORT;
	case IB_QP_QKEY:
		return MLX5_QP_OPTPAR_Q_KEY;
	case IB_QP_AV:
		return MLX5_QP_OPTPAR_PRIMARY_ADDR_PATH |
			MLX5_QP_OPTPAR_PRI_PORT;
	case IB_QP_PATH_MTU:
		return 0;
	case IB_QP_TIMEOUT:
		return MLX5_QP_OPTPAR_ACK_TIMEOUT;
	case IB_QP_RETRY_CNT:
		return MLX5_QP_OPTPAR_RETRY_COUNT;
	case IB_QP_RNR_RETRY:
		return MLX5_QP_OPTPAR_RNR_RETRY;
	case IB_QP_RQ_PSN:
		return 0;
	case IB_QP_MAX_QP_RD_ATOMIC:
		return MLX5_QP_OPTPAR_SRA_MAX;
	case IB_QP_ALT_PATH:
		return MLX5_QP_OPTPAR_ALT_ADDR_PATH;
	case IB_QP_MIN_RNR_TIMER:
		return MLX5_QP_OPTPAR_RNR_TIMEOUT;
	case IB_QP_SQ_PSN:
		return 0;
	case IB_QP_MAX_DEST_RD_ATOMIC:
		return MLX5_QP_OPTPAR_RRA_MAX | MLX5_QP_OPTPAR_RWE |
			MLX5_QP_OPTPAR_RRE | MLX5_QP_OPTPAR_RAE;
	case IB_QP_PATH_MIG_STATE:
		return MLX5_QP_OPTPAR_PM_STATE;
	case IB_QP_CAP:
		return 0;
	case IB_QP_DEST_QPN:
		return 0;
	}
	return 0;
}

static int ib_mask_to_mlx5_opt(int ib_mask)
{
	int result = 0;
	int i;

	for (i = 0; i < 8 * sizeof(int); i++) {
		if ((1 << i) & ib_mask)
			result |= ib_nr_to_mlx5_nr(1 << i);
	}

	return result;
}

static int modify_raw_packet_qp_rq(
	struct mlx5_ib_dev *dev, struct mlx5_ib_rq *rq, int new_state,
	const struct mlx5_modify_raw_qp_param *raw_qp_param, struct ib_pd *pd)
{
	void *in;
	void *rqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_rq_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	MLX5_SET(modify_rq_in, in, rq_state, rq->state);
	MLX5_SET(modify_rq_in, in, uid, to_mpd(pd)->uid);

	rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);
	MLX5_SET(rqc, rqc, state, new_state);

	if (raw_qp_param->set_mask & MLX5_RAW_QP_MOD_SET_RQ_Q_CTR_ID) {
		if (MLX5_CAP_GEN(dev->mdev, modify_rq_counter_set_id)) {
			MLX5_SET64(modify_rq_in, in, modify_bitmask,
				   MLX5_MODIFY_RQ_IN_MODIFY_BITMASK_RQ_COUNTER_SET_ID);
			MLX5_SET(rqc, rqc, counter_set_id, raw_qp_param->rq_q_ctr_id);
		} else
			dev_info_once(
				&dev->ib_dev.dev,
				"RAW PACKET QP counters are not supported on current FW\n");
	}

	err = mlx5_core_modify_rq(dev->mdev, rq->base.mqp.qpn, in, inlen);
	if (err)
		goto out;

	rq->state = new_state;

out:
	kvfree(in);
	return err;
}

static int modify_raw_packet_qp_sq(
	struct mlx5_core_dev *dev, struct mlx5_ib_sq *sq, int new_state,
	const struct mlx5_modify_raw_qp_param *raw_qp_param, struct ib_pd *pd)
{
	struct mlx5_ib_qp *ibqp = sq->base.container_mibqp;
	struct mlx5_rate_limit old_rl = ibqp->rl;
	struct mlx5_rate_limit new_rl = old_rl;
	bool new_rate_added = false;
	u16 rl_index = 0;
	void *in;
	void *sqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_sq_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	MLX5_SET(modify_sq_in, in, uid, to_mpd(pd)->uid);
	MLX5_SET(modify_sq_in, in, sq_state, sq->state);

	sqc = MLX5_ADDR_OF(modify_sq_in, in, ctx);
	MLX5_SET(sqc, sqc, state, new_state);

	if (raw_qp_param->set_mask & MLX5_RAW_QP_RATE_LIMIT) {
		if (new_state != MLX5_SQC_STATE_RDY)
			pr_warn("%s: Rate limit can only be changed when SQ is moving to RDY\n",
				__func__);
		else
			new_rl = raw_qp_param->rl;
	}

	if (!mlx5_rl_are_equal(&old_rl, &new_rl)) {
		if (new_rl.rate) {
			err = mlx5_rl_add_rate(dev, &rl_index, &new_rl);
			if (err) {
				pr_err("Failed configuring rate limit(err %d): \
				       rate %u, max_burst_sz %u, typical_pkt_sz %u\n",
				       err, new_rl.rate, new_rl.max_burst_sz,
				       new_rl.typical_pkt_sz);

				goto out;
			}
			new_rate_added = true;
		}

		MLX5_SET64(modify_sq_in, in, modify_bitmask, 1);
		/* index 0 means no limit */
		MLX5_SET(sqc, sqc, packet_pacing_rate_limit_index, rl_index);
	}

	err = mlx5_core_modify_sq(dev, sq->base.mqp.qpn, in, inlen);
	if (err) {
		/* Remove new rate from table if failed */
		if (new_rate_added)
			mlx5_rl_remove_rate(dev, &new_rl);
		goto out;
	}

	/* Only remove the old rate after new rate was set */
	if ((old_rl.rate && !mlx5_rl_are_equal(&old_rl, &new_rl)) ||
	    (new_state != MLX5_SQC_STATE_RDY)) {
		mlx5_rl_remove_rate(dev, &old_rl);
		if (new_state != MLX5_SQC_STATE_RDY)
			memset(&new_rl, 0, sizeof(new_rl));
	}

	ibqp->rl = new_rl;
	sq->state = new_state;

out:
	kvfree(in);
	return err;
}

static int modify_raw_packet_qp(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp,
				const struct mlx5_modify_raw_qp_param *raw_qp_param,
				u8 tx_affinity)
{
	struct mlx5_ib_raw_packet_qp *raw_packet_qp = &qp->raw_packet_qp;
	struct mlx5_ib_rq *rq = &raw_packet_qp->rq;
	struct mlx5_ib_sq *sq = &raw_packet_qp->sq;
	int modify_rq = !!qp->rq.wqe_cnt;
	int modify_sq = !!qp->sq.wqe_cnt;
	int rq_state;
	int sq_state;
	int err;

	switch (raw_qp_param->operation) {
	case MLX5_CMD_OP_RST2INIT_QP:
		rq_state = MLX5_RQC_STATE_RDY;
		sq_state = MLX5_SQC_STATE_RDY;
		break;
	case MLX5_CMD_OP_2ERR_QP:
		rq_state = MLX5_RQC_STATE_ERR;
		sq_state = MLX5_SQC_STATE_ERR;
		break;
	case MLX5_CMD_OP_2RST_QP:
		rq_state = MLX5_RQC_STATE_RST;
		sq_state = MLX5_SQC_STATE_RST;
		break;
	case MLX5_CMD_OP_RTR2RTS_QP:
	case MLX5_CMD_OP_RTS2RTS_QP:
		if (raw_qp_param->set_mask ==
		    MLX5_RAW_QP_RATE_LIMIT) {
			modify_rq = 0;
			sq_state = sq->state;
		} else {
			return raw_qp_param->set_mask ? -EINVAL : 0;
		}
		break;
	case MLX5_CMD_OP_INIT2INIT_QP:
	case MLX5_CMD_OP_INIT2RTR_QP:
		if (raw_qp_param->set_mask)
			return -EINVAL;
		else
			return 0;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	if (modify_rq) {
		err =  modify_raw_packet_qp_rq(dev, rq, rq_state, raw_qp_param,
					       qp->ibqp.pd);
		if (err)
			return err;
	}

	if (modify_sq) {
		struct mlx5_flow_handle *flow_rule;

		if (tx_affinity) {
			err = modify_raw_packet_tx_affinity(dev->mdev, sq,
							    tx_affinity,
							    qp->ibqp.pd);
			if (err)
				return err;
		}

		flow_rule = create_flow_rule_vport_sq(dev, sq,
						      raw_qp_param->port);
		if (IS_ERR(flow_rule))
			return PTR_ERR(flow_rule);

		err = modify_raw_packet_qp_sq(dev->mdev, sq, sq_state,
					      raw_qp_param, qp->ibqp.pd);
		if (err) {
			if (flow_rule)
				mlx5_del_flow_rules(flow_rule);
			return err;
		}

		if (flow_rule) {
			destroy_flow_rule_vport_sq(sq);
			sq->flow_rule = flow_rule;
		}

		return err;
	}

	return 0;
}

static unsigned int get_tx_affinity(struct mlx5_ib_dev *dev,
				    struct mlx5_ib_pd *pd,
				    struct mlx5_ib_qp_base *qp_base,
				    u8 port_num, struct ib_udata *udata)
{
	struct mlx5_ib_ucontext *ucontext = rdma_udata_to_drv_context(
		udata, struct mlx5_ib_ucontext, ibucontext);
	unsigned int tx_port_affinity;

	if (ucontext) {
		tx_port_affinity = (unsigned int)atomic_add_return(
					   1, &ucontext->tx_port_affinity) %
					   MLX5_MAX_PORTS +
				   1;
		mlx5_ib_dbg(dev, "Set tx affinity 0x%x to qpn 0x%x ucontext %p\n",
				tx_port_affinity, qp_base->mqp.qpn, ucontext);
	} else {
		tx_port_affinity =
			(unsigned int)atomic_add_return(
				1, &dev->port[port_num].roce.tx_port_affinity) %
				MLX5_MAX_PORTS +
			1;
		mlx5_ib_dbg(dev, "Set tx affinity 0x%x to qpn 0x%x\n",
				tx_port_affinity, qp_base->mqp.qpn);
	}

	return tx_port_affinity;
}

static int __mlx5_ib_qp_set_counter(struct ib_qp *qp,
				    struct rdma_counter *counter)
{
	struct mlx5_ib_dev *dev = to_mdev(qp->device);
	struct mlx5_ib_qp *mqp = to_mqp(qp);
	struct mlx5_qp_context context = {};
	struct mlx5_ib_qp_base *base;
	u32 set_id;

	if (counter)
		set_id = counter->id;
	else
		set_id = mlx5_ib_get_counters_id(dev, mqp->port - 1);

	base = &mqp->trans_qp.base;
	context.qp_counter_set_usr_page &= cpu_to_be32(0xffffff);
	context.qp_counter_set_usr_page |= cpu_to_be32(set_id << 24);
	return mlx5_core_qp_modify(dev->mdev,
				   MLX5_CMD_OP_RTS2RTS_QP,
				   MLX5_QP_OPTPAR_COUNTER_SET_ID,
				   &context, &base->mqp);
}

static int __mlx5_ib_modify_qp(struct ib_qp *ibqp,
			       const struct ib_qp_attr *attr, int attr_mask,
			       enum ib_qp_state cur_state,
			       enum ib_qp_state new_state,
			       const struct mlx5_ib_modify_qp *ucmd,
			       struct ib_udata *udata)
{
	static const u16 optab[MLX5_QP_NUM_STATE][MLX5_QP_NUM_STATE] = {
		[MLX5_QP_STATE_RST] = {
			[MLX5_QP_STATE_RST]	= MLX5_CMD_OP_2RST_QP,
			[MLX5_QP_STATE_ERR]	= MLX5_CMD_OP_2ERR_QP,
			[MLX5_QP_STATE_INIT]	= MLX5_CMD_OP_RST2INIT_QP,
		},
		[MLX5_QP_STATE_INIT]  = {
			[MLX5_QP_STATE_RST]	= MLX5_CMD_OP_2RST_QP,
			[MLX5_QP_STATE_ERR]	= MLX5_CMD_OP_2ERR_QP,
			[MLX5_QP_STATE_INIT]	= MLX5_CMD_OP_INIT2INIT_QP,
			[MLX5_QP_STATE_RTR]	= MLX5_CMD_OP_INIT2RTR_QP,
		},
		[MLX5_QP_STATE_RTR]   = {
			[MLX5_QP_STATE_RST]	= MLX5_CMD_OP_2RST_QP,
			[MLX5_QP_STATE_ERR]	= MLX5_CMD_OP_2ERR_QP,
			[MLX5_QP_STATE_RTS]	= MLX5_CMD_OP_RTR2RTS_QP,
		},
		[MLX5_QP_STATE_RTS]   = {
			[MLX5_QP_STATE_RST]	= MLX5_CMD_OP_2RST_QP,
			[MLX5_QP_STATE_ERR]	= MLX5_CMD_OP_2ERR_QP,
			[MLX5_QP_STATE_RTS]	= MLX5_CMD_OP_RTS2RTS_QP,
		},
		[MLX5_QP_STATE_SQD] = {
			[MLX5_QP_STATE_RST]	= MLX5_CMD_OP_2RST_QP,
			[MLX5_QP_STATE_ERR]	= MLX5_CMD_OP_2ERR_QP,
		},
		[MLX5_QP_STATE_SQER] = {
			[MLX5_QP_STATE_RST]	= MLX5_CMD_OP_2RST_QP,
			[MLX5_QP_STATE_ERR]	= MLX5_CMD_OP_2ERR_QP,
			[MLX5_QP_STATE_RTS]	= MLX5_CMD_OP_SQERR2RTS_QP,
		},
		[MLX5_QP_STATE_ERR] = {
			[MLX5_QP_STATE_RST]	= MLX5_CMD_OP_2RST_QP,
			[MLX5_QP_STATE_ERR]	= MLX5_CMD_OP_2ERR_QP,
		}
	};

	struct mlx5_ib_dev *dev = to_mdev(ibqp->device);
	struct mlx5_ib_qp *qp = to_mqp(ibqp);
	struct mlx5_ib_qp_base *base = &qp->trans_qp.base;
	struct mlx5_ib_cq *send_cq, *recv_cq;
	struct mlx5_qp_context *context;
	struct mlx5_ib_pd *pd;
	enum mlx5_qp_state mlx5_cur, mlx5_new;
	enum mlx5_qp_optpar optpar;
	u32 set_id = 0;
	int mlx5_st;
	int err;
	u16 op;
	u8 tx_affinity = 0;

	mlx5_st = to_mlx5_st(ibqp->qp_type == IB_QPT_DRIVER ?
			     qp->qp_sub_type : ibqp->qp_type);
	if (mlx5_st < 0)
		return -EINVAL;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	pd = get_pd(qp);
	context->flags = cpu_to_be32(mlx5_st << 16);

	if (!(attr_mask & IB_QP_PATH_MIG_STATE)) {
		context->flags |= cpu_to_be32(MLX5_QP_PM_MIGRATED << 11);
	} else {
		switch (attr->path_mig_state) {
		case IB_MIG_MIGRATED:
			context->flags |= cpu_to_be32(MLX5_QP_PM_MIGRATED << 11);
			break;
		case IB_MIG_REARM:
			context->flags |= cpu_to_be32(MLX5_QP_PM_REARM << 11);
			break;
		case IB_MIG_ARMED:
			context->flags |= cpu_to_be32(MLX5_QP_PM_ARMED << 11);
			break;
		}
	}

	if ((cur_state == IB_QPS_RESET) && (new_state == IB_QPS_INIT)) {
		if ((ibqp->qp_type == IB_QPT_RC) ||
		    (ibqp->qp_type == IB_QPT_UD &&
		     !(qp->flags & MLX5_IB_QP_SQPN_QP1)) ||
		    (ibqp->qp_type == IB_QPT_UC) ||
		    (ibqp->qp_type == IB_QPT_RAW_PACKET) ||
		    (ibqp->qp_type == IB_QPT_XRC_INI) ||
		    (ibqp->qp_type == IB_QPT_XRC_TGT)) {
			if (dev->lag_active) {
				u8 p = mlx5_core_native_port_num(dev->mdev) - 1;
				tx_affinity = get_tx_affinity(dev, pd, base, p,
							      udata);
				context->flags |= cpu_to_be32(tx_affinity << 24);
			}
		}
	}

	if (is_sqp(ibqp->qp_type)) {
		context->mtu_msgmax = (IB_MTU_256 << 5) | 8;
	} else if ((ibqp->qp_type == IB_QPT_UD &&
		    !(qp->flags & MLX5_IB_QP_UNDERLAY)) ||
		   ibqp->qp_type == MLX5_IB_QPT_REG_UMR) {
		context->mtu_msgmax = (IB_MTU_4096 << 5) | 12;
	} else if (attr_mask & IB_QP_PATH_MTU) {
		if (attr->path_mtu < IB_MTU_256 ||
		    attr->path_mtu > IB_MTU_4096) {
			mlx5_ib_warn(dev, "invalid mtu %d\n", attr->path_mtu);
			err = -EINVAL;
			goto out;
		}
		context->mtu_msgmax = (attr->path_mtu << 5) |
				      (u8)MLX5_CAP_GEN(dev->mdev, log_max_msg);
	}

	if (attr_mask & IB_QP_DEST_QPN)
		context->log_pg_sz_remote_qpn = cpu_to_be32(attr->dest_qp_num);

	if (attr_mask & IB_QP_PKEY_INDEX)
		context->pri_path.pkey_index = cpu_to_be16(attr->pkey_index);

	/* todo implement counter_index functionality */

	if (is_sqp(ibqp->qp_type))
		context->pri_path.port = qp->port;

	if (attr_mask & IB_QP_PORT)
		context->pri_path.port = attr->port_num;

	if (attr_mask & IB_QP_AV) {
		err = mlx5_set_path(dev, qp, &attr->ah_attr, &context->pri_path,
				    attr_mask & IB_QP_PORT ? attr->port_num : qp->port,
				    attr_mask, 0, attr, false);
		if (err)
			goto out;
	}

	if (attr_mask & IB_QP_TIMEOUT)
		context->pri_path.ackto_lt |= attr->timeout << 3;

	if (attr_mask & IB_QP_ALT_PATH) {
		err = mlx5_set_path(dev, qp, &attr->alt_ah_attr,
				    &context->alt_path,
				    attr->alt_port_num,
				    attr_mask | IB_QP_PKEY_INDEX | IB_QP_TIMEOUT,
				    0, attr, true);
		if (err)
			goto out;
	}

	get_cqs(qp->ibqp.qp_type, qp->ibqp.send_cq, qp->ibqp.recv_cq,
		&send_cq, &recv_cq);

	context->flags_pd = cpu_to_be32(pd ? pd->pdn : to_mpd(dev->devr.p0)->pdn);
	context->cqn_send = send_cq ? cpu_to_be32(send_cq->mcq.cqn) : 0;
	context->cqn_recv = recv_cq ? cpu_to_be32(recv_cq->mcq.cqn) : 0;
	context->params1  = cpu_to_be32(MLX5_IB_ACK_REQ_FREQ << 28);

	if (attr_mask & IB_QP_RNR_RETRY)
		context->params1 |= cpu_to_be32(attr->rnr_retry << 13);

	if (attr_mask & IB_QP_RETRY_CNT)
		context->params1 |= cpu_to_be32(attr->retry_cnt << 16);

	if (attr_mask & IB_QP_MAX_QP_RD_ATOMIC) {
		if (attr->max_rd_atomic)
			context->params1 |=
				cpu_to_be32(fls(attr->max_rd_atomic - 1) << 21);
	}

	if (attr_mask & IB_QP_SQ_PSN)
		context->next_send_psn = cpu_to_be32(attr->sq_psn);

	if (attr_mask & IB_QP_MAX_DEST_RD_ATOMIC) {
		if (attr->max_dest_rd_atomic)
			context->params2 |=
				cpu_to_be32(fls(attr->max_dest_rd_atomic - 1) << 21);
	}

	if (attr_mask & (IB_QP_ACCESS_FLAGS | IB_QP_MAX_DEST_RD_ATOMIC)) {
		__be32 access_flags;

		err = to_mlx5_access_flags(qp, attr, attr_mask, &access_flags);
		if (err)
			goto out;

		context->params2 |= access_flags;
	}

	if (attr_mask & IB_QP_MIN_RNR_TIMER)
		context->rnr_nextrecvpsn |= cpu_to_be32(attr->min_rnr_timer << 24);

	if (attr_mask & IB_QP_RQ_PSN)
		context->rnr_nextrecvpsn |= cpu_to_be32(attr->rq_psn);

	if (attr_mask & IB_QP_QKEY)
		context->qkey = cpu_to_be32(attr->qkey);

	if (qp->rq.wqe_cnt && cur_state == IB_QPS_RESET && new_state == IB_QPS_INIT)
		context->db_rec_addr = cpu_to_be64(qp->db.dma);

	if (cur_state == IB_QPS_RESET && new_state == IB_QPS_INIT) {
		u8 port_num = (attr_mask & IB_QP_PORT ? attr->port_num :
			       qp->port) - 1;

		/* Underlay port should be used - index 0 function per port */
		if (qp->flags & MLX5_IB_QP_UNDERLAY)
			port_num = 0;

		if (ibqp->counter)
			set_id = ibqp->counter->id;
		else
			set_id = mlx5_ib_get_counters_id(dev, port_num);
		context->qp_counter_set_usr_page |=
			cpu_to_be32(set_id << 24);
	}

	if (!ibqp->uobject && cur_state == IB_QPS_RESET && new_state == IB_QPS_INIT)
		context->sq_crq_size |= cpu_to_be16(1 << 4);

	if (qp->flags & MLX5_IB_QP_SQPN_QP1)
		context->deth_sqpn = cpu_to_be32(1);

	mlx5_cur = to_mlx5_state(cur_state);
	mlx5_new = to_mlx5_state(new_state);

	if (mlx5_cur >= MLX5_QP_NUM_STATE || mlx5_new >= MLX5_QP_NUM_STATE ||
	    !optab[mlx5_cur][mlx5_new]) {
		err = -EINVAL;
		goto out;
	}

	op = optab[mlx5_cur][mlx5_new];
	optpar = ib_mask_to_mlx5_opt(attr_mask);
	optpar &= opt_mask[mlx5_cur][mlx5_new][mlx5_st];

	if (qp->ibqp.qp_type == IB_QPT_RAW_PACKET ||
	    qp->flags & MLX5_IB_QP_UNDERLAY) {
		struct mlx5_modify_raw_qp_param raw_qp_param = {};

		raw_qp_param.operation = op;
		if (cur_state == IB_QPS_RESET && new_state == IB_QPS_INIT) {
			raw_qp_param.rq_q_ctr_id = set_id;
			raw_qp_param.set_mask |= MLX5_RAW_QP_MOD_SET_RQ_Q_CTR_ID;
		}

		if (attr_mask & IB_QP_PORT)
			raw_qp_param.port = attr->port_num;

		if (attr_mask & IB_QP_RATE_LIMIT) {
			raw_qp_param.rl.rate = attr->rate_limit;

			if (ucmd->burst_info.max_burst_sz) {
				if (attr->rate_limit &&
				    MLX5_CAP_QOS(dev->mdev, packet_pacing_burst_bound)) {
					raw_qp_param.rl.max_burst_sz =
						ucmd->burst_info.max_burst_sz;
				} else {
					err = -EINVAL;
					goto out;
				}
			}

			if (ucmd->burst_info.typical_pkt_sz) {
				if (attr->rate_limit &&
				    MLX5_CAP_QOS(dev->mdev, packet_pacing_typical_size)) {
					raw_qp_param.rl.typical_pkt_sz =
						ucmd->burst_info.typical_pkt_sz;
				} else {
					err = -EINVAL;
					goto out;
				}
			}

			raw_qp_param.set_mask |= MLX5_RAW_QP_RATE_LIMIT;
		}

		err = modify_raw_packet_qp(dev, qp, &raw_qp_param, tx_affinity);
	} else {
		err = mlx5_core_qp_modify(dev->mdev, op, optpar, context,
					  &base->mqp);
	}

	if (err)
		goto out;

	qp->state = new_state;

	if (attr_mask & IB_QP_ACCESS_FLAGS)
		qp->trans_qp.atomic_rd_en = attr->qp_access_flags;
	if (attr_mask & IB_QP_MAX_DEST_RD_ATOMIC)
		qp->trans_qp.resp_depth = attr->max_dest_rd_atomic;
	if (attr_mask & IB_QP_PORT)
		qp->port = attr->port_num;
	if (attr_mask & IB_QP_ALT_PATH)
		qp->trans_qp.alt_port = attr->alt_port_num;

	/*
	 * If we moved a kernel QP to RESET, clean up all old CQ
	 * entries and reinitialize the QP.
	 */
	if (new_state == IB_QPS_RESET &&
	    !ibqp->uobject && ibqp->qp_type != IB_QPT_XRC_TGT) {
		mlx5_ib_cq_clean(recv_cq, base->mqp.qpn,
				 ibqp->srq ? to_msrq(ibqp->srq) : NULL);
		if (send_cq != recv_cq)
			mlx5_ib_cq_clean(send_cq, base->mqp.qpn, NULL);

		qp->rq.head = 0;
		qp->rq.tail = 0;
		qp->sq.head = 0;
		qp->sq.tail = 0;
		qp->sq.cur_post = 0;
		if (qp->sq.wqe_cnt)
			qp->sq.cur_edge = get_sq_edge(&qp->sq, 0);
		qp->sq.last_poll = 0;
		qp->db.db[MLX5_RCV_DBR] = 0;
		qp->db.db[MLX5_SND_DBR] = 0;
	}

	if ((new_state == IB_QPS_RTS) && qp->counter_pending) {
		err = __mlx5_ib_qp_set_counter(ibqp, ibqp->counter);
		if (!err)
			qp->counter_pending = 0;
	}

out:
	kfree(context);
	return err;
}

static inline bool is_valid_mask(int mask, int req, int opt)
{
	if ((mask & req) != req)
		return false;

	if (mask & ~(req | opt))
		return false;

	return true;
}

/* check valid transition for driver QP types
 * for now the only QP type that this function supports is DCI
 */
static bool modify_dci_qp_is_ok(enum ib_qp_state cur_state, enum ib_qp_state new_state,
				enum ib_qp_attr_mask attr_mask)
{
	int req = IB_QP_STATE;
	int opt = 0;

	if (new_state == IB_QPS_RESET) {
		return is_valid_mask(attr_mask, req, opt);
	} else if (cur_state == IB_QPS_RESET && new_state == IB_QPS_INIT) {
		req |= IB_QP_PKEY_INDEX | IB_QP_PORT;
		return is_valid_mask(attr_mask, req, opt);
	} else if (cur_state == IB_QPS_INIT && new_state == IB_QPS_INIT) {
		opt = IB_QP_PKEY_INDEX | IB_QP_PORT;
		return is_valid_mask(attr_mask, req, opt);
	} else if (cur_state == IB_QPS_INIT && new_state == IB_QPS_RTR) {
		req |= IB_QP_PATH_MTU;
		opt = IB_QP_PKEY_INDEX | IB_QP_AV;
		return is_valid_mask(attr_mask, req, opt);
	} else if (cur_state == IB_QPS_RTR && new_state == IB_QPS_RTS) {
		req |= IB_QP_TIMEOUT | IB_QP_RETRY_CNT | IB_QP_RNR_RETRY |
		       IB_QP_MAX_QP_RD_ATOMIC | IB_QP_SQ_PSN;
		opt = IB_QP_MIN_RNR_TIMER;
		return is_valid_mask(attr_mask, req, opt);
	} else if (cur_state == IB_QPS_RTS && new_state == IB_QPS_RTS) {
		opt = IB_QP_MIN_RNR_TIMER;
		return is_valid_mask(attr_mask, req, opt);
	} else if (cur_state != IB_QPS_RESET && new_state == IB_QPS_ERR) {
		return is_valid_mask(attr_mask, req, opt);
	}
	return false;
}

/* mlx5_ib_modify_dct: modify a DCT QP
 * valid transitions are:
 * RESET to INIT: must set access_flags, pkey_index and port
 * INIT  to RTR : must set min_rnr_timer, tclass, flow_label,
 *			   mtu, gid_index and hop_limit
 * Other transitions and attributes are illegal
 */
static int mlx5_ib_modify_dct(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			      int attr_mask, struct ib_udata *udata)
{
	struct mlx5_ib_qp *qp = to_mqp(ibqp);
	struct mlx5_ib_dev *dev = to_mdev(ibqp->device);
	enum ib_qp_state cur_state, new_state;
	int err = 0;
	int required = IB_QP_STATE;
	void *dctc;

	if (!(attr_mask & IB_QP_STATE))
		return -EINVAL;

	cur_state = qp->state;
	new_state = attr->qp_state;

	dctc = MLX5_ADDR_OF(create_dct_in, qp->dct.in, dct_context_entry);
	if (cur_state == IB_QPS_RESET && new_state == IB_QPS_INIT) {
		u16 set_id;

		required |= IB_QP_ACCESS_FLAGS | IB_QP_PKEY_INDEX | IB_QP_PORT;
		if (!is_valid_mask(attr_mask, required, 0))
			return -EINVAL;

		if (attr->port_num == 0 ||
		    attr->port_num > MLX5_CAP_GEN(dev->mdev, num_ports)) {
			mlx5_ib_dbg(dev, "invalid port number %d. number of ports is %d\n",
				    attr->port_num, dev->num_ports);
			return -EINVAL;
		}
		if (attr->qp_access_flags & IB_ACCESS_REMOTE_READ)
			MLX5_SET(dctc, dctc, rre, 1);
		if (attr->qp_access_flags & IB_ACCESS_REMOTE_WRITE)
			MLX5_SET(dctc, dctc, rwe, 1);
		if (attr->qp_access_flags & IB_ACCESS_REMOTE_ATOMIC) {
			int atomic_mode;

			atomic_mode = get_atomic_mode(dev, MLX5_IB_QPT_DCT);
			if (atomic_mode < 0)
				return -EOPNOTSUPP;

			MLX5_SET(dctc, dctc, atomic_mode, atomic_mode);
			MLX5_SET(dctc, dctc, rae, 1);
		}
		MLX5_SET(dctc, dctc, pkey_index, attr->pkey_index);
		MLX5_SET(dctc, dctc, port, attr->port_num);

		set_id = mlx5_ib_get_counters_id(dev, attr->port_num - 1);
		MLX5_SET(dctc, dctc, counter_set_id, set_id);

	} else if (cur_state == IB_QPS_INIT && new_state == IB_QPS_RTR) {
		struct mlx5_ib_modify_qp_resp resp = {};
		u32 out[MLX5_ST_SZ_DW(create_dct_out)] = {0};
		u32 min_resp_len = offsetof(typeof(resp), dctn) +
				   sizeof(resp.dctn);

		if (udata->outlen < min_resp_len)
			return -EINVAL;
		resp.response_length = min_resp_len;

		required |= IB_QP_MIN_RNR_TIMER | IB_QP_AV | IB_QP_PATH_MTU;
		if (!is_valid_mask(attr_mask, required, 0))
			return -EINVAL;
		MLX5_SET(dctc, dctc, min_rnr_nak, attr->min_rnr_timer);
		MLX5_SET(dctc, dctc, tclass, attr->ah_attr.grh.traffic_class);
		MLX5_SET(dctc, dctc, flow_label, attr->ah_attr.grh.flow_label);
		MLX5_SET(dctc, dctc, mtu, attr->path_mtu);
		MLX5_SET(dctc, dctc, my_addr_index, attr->ah_attr.grh.sgid_index);
		MLX5_SET(dctc, dctc, hop_limit, attr->ah_attr.grh.hop_limit);

		err = mlx5_core_create_dct(dev->mdev, &qp->dct.mdct, qp->dct.in,
					   MLX5_ST_SZ_BYTES(create_dct_in), out,
					   sizeof(out));
		if (err)
			return err;
		resp.dctn = qp->dct.mdct.mqp.qpn;
		err = ib_copy_to_udata(udata, &resp, resp.response_length);
		if (err) {
			mlx5_core_destroy_dct(dev->mdev, &qp->dct.mdct);
			return err;
		}
	} else {
		mlx5_ib_warn(dev, "Modify DCT: Invalid transition from %d to %d\n", cur_state, new_state);
		return -EINVAL;
	}
	if (err)
		qp->state = IB_QPS_ERR;
	else
		qp->state = new_state;
	return err;
}

int mlx5_ib_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
		      int attr_mask, struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev = to_mdev(ibqp->device);
	struct mlx5_ib_qp *qp = to_mqp(ibqp);
	struct mlx5_ib_modify_qp ucmd = {};
	enum ib_qp_type qp_type;
	enum ib_qp_state cur_state, new_state;
	size_t required_cmd_sz;
	int err = -EINVAL;
	int port;

	if (ibqp->rwq_ind_tbl)
		return -ENOSYS;

	if (udata && udata->inlen) {
		required_cmd_sz = offsetof(typeof(ucmd), reserved) +
			sizeof(ucmd.reserved);
		if (udata->inlen < required_cmd_sz)
			return -EINVAL;

		if (udata->inlen > sizeof(ucmd) &&
		    !ib_is_udata_cleared(udata, sizeof(ucmd),
					 udata->inlen - sizeof(ucmd)))
			return -EOPNOTSUPP;

		if (ib_copy_from_udata(&ucmd, udata,
				       min(udata->inlen, sizeof(ucmd))))
			return -EFAULT;

		if (ucmd.comp_mask ||
		    memchr_inv(&ucmd.reserved, 0, sizeof(ucmd.reserved)) ||
		    memchr_inv(&ucmd.burst_info.reserved, 0,
			       sizeof(ucmd.burst_info.reserved)))
			return -EOPNOTSUPP;
	}

	if (unlikely(ibqp->qp_type == IB_QPT_GSI))
		return mlx5_ib_gsi_modify_qp(ibqp, attr, attr_mask);

	if (ibqp->qp_type == IB_QPT_DRIVER)
		qp_type = qp->qp_sub_type;
	else
		qp_type = (unlikely(ibqp->qp_type == MLX5_IB_QPT_HW_GSI)) ?
			IB_QPT_GSI : ibqp->qp_type;

	if (qp_type == MLX5_IB_QPT_DCT)
		return mlx5_ib_modify_dct(ibqp, attr, attr_mask, udata);

	mutex_lock(&qp->mutex);

	cur_state = attr_mask & IB_QP_CUR_STATE ? attr->cur_qp_state : qp->state;
	new_state = attr_mask & IB_QP_STATE ? attr->qp_state : cur_state;

	if (!(cur_state == new_state && cur_state == IB_QPS_RESET)) {
		port = attr_mask & IB_QP_PORT ? attr->port_num : qp->port;
	}

	if (qp->flags & MLX5_IB_QP_UNDERLAY) {
		if (attr_mask & ~(IB_QP_STATE | IB_QP_CUR_STATE)) {
			mlx5_ib_dbg(dev, "invalid attr_mask 0x%x when underlay QP is used\n",
				    attr_mask);
			goto out;
		}
	} else if (qp_type != MLX5_IB_QPT_REG_UMR &&
		   qp_type != MLX5_IB_QPT_DCI &&
		   !ib_modify_qp_is_ok(cur_state, new_state, qp_type,
				       attr_mask)) {
		mlx5_ib_dbg(dev, "invalid QP state transition from %d to %d, qp_type %d, attr_mask 0x%x\n",
			    cur_state, new_state, ibqp->qp_type, attr_mask);
		goto out;
	} else if (qp_type == MLX5_IB_QPT_DCI &&
		   !modify_dci_qp_is_ok(cur_state, new_state, attr_mask)) {
		mlx5_ib_dbg(dev, "invalid QP state transition from %d to %d, qp_type %d, attr_mask 0x%x\n",
			    cur_state, new_state, qp_type, attr_mask);
		goto out;
	}

	if ((attr_mask & IB_QP_PORT) &&
	    (attr->port_num == 0 ||
	     attr->port_num > dev->num_ports)) {
		mlx5_ib_dbg(dev, "invalid port number %d. number of ports is %d\n",
			    attr->port_num, dev->num_ports);
		goto out;
	}

	if (attr_mask & IB_QP_PKEY_INDEX) {
		port = attr_mask & IB_QP_PORT ? attr->port_num : qp->port;
		if (attr->pkey_index >=
		    dev->mdev->port_caps[port - 1].pkey_table_len) {
			mlx5_ib_dbg(dev, "invalid pkey index %d\n",
				    attr->pkey_index);
			goto out;
		}
	}

	if (attr_mask & IB_QP_MAX_QP_RD_ATOMIC &&
	    attr->max_rd_atomic >
	    (1 << MLX5_CAP_GEN(dev->mdev, log_max_ra_res_qp))) {
		mlx5_ib_dbg(dev, "invalid max_rd_atomic value %d\n",
			    attr->max_rd_atomic);
		goto out;
	}

	if (attr_mask & IB_QP_MAX_DEST_RD_ATOMIC &&
	    attr->max_dest_rd_atomic >
	    (1 << MLX5_CAP_GEN(dev->mdev, log_max_ra_req_qp))) {
		mlx5_ib_dbg(dev, "invalid max_dest_rd_atomic value %d\n",
			    attr->max_dest_rd_atomic);
		goto out;
	}

	if (cur_state == new_state && cur_state == IB_QPS_RESET) {
		err = 0;
		goto out;
	}

	err = __mlx5_ib_modify_qp(ibqp, attr, attr_mask, cur_state,
				  new_state, &ucmd, udata);

out:
	mutex_unlock(&qp->mutex);
	return err;
}

static void _handle_post_send_edge(struct mlx5_ib_wq *sq, void **seg,
				   u32 wqe_sz, void **cur_edge)
{
	u32 idx;

	idx = (sq->cur_post + (wqe_sz >> 2)) & (sq->wqe_cnt - 1);
	*cur_edge = get_sq_edge(sq, idx);

	*seg = mlx5_frag_buf_get_wqe(&sq->fbc, idx);
}

/* handle_post_send_edge - Check if we get to SQ edge. If yes, update to the
 * next nearby edge and get new address translation for current WQE position.
 * @sq - SQ buffer.
 * @seg: Current WQE position (16B aligned).
 * @wqe_sz: Total current WQE size [16B].
 * @cur_edge: Updated current edge.
 */
static inline void handle_post_send_edge(struct mlx5_ib_wq *sq, void **seg,
					 u32 wqe_sz, void **cur_edge)
{
	if (likely(*seg != *cur_edge))
		return;

	_handle_post_send_edge(sq, seg, wqe_sz, cur_edge);
}

/* memcpy_send_wqe - copy data from src to WQE and update the relevant WQ's
 * pointers. At the end @seg is aligned to 16B regardless the copied size.
 * @sq - SQ buffer.
 * @cur_edge: Updated current edge.
 * @seg: Current WQE position (16B aligned).
 * @wqe_sz: Total current WQE size [16B].
 * @src: Pointer to copy from.
 * @n: Number of bytes to copy.
 */
static inline void memcpy_send_wqe(struct mlx5_ib_wq *sq, void **cur_edge,
				   void **seg, u32 *wqe_sz, const void *src,
				   size_t n)
{
	while (likely(n)) {
		size_t leftlen = *cur_edge - *seg;
		size_t copysz = min_t(size_t, leftlen, n);
		size_t stride;

		memcpy(*seg, src, copysz);

		n -= copysz;
		src += copysz;
		stride = !n ? ALIGN(copysz, 16) : copysz;
		*seg += stride;
		*wqe_sz += stride >> 4;
		handle_post_send_edge(sq, seg, *wqe_sz, cur_edge);
	}
}

static int mlx5_wq_overflow(struct mlx5_ib_wq *wq, int nreq, struct ib_cq *ib_cq)
{
	struct mlx5_ib_cq *cq;
	unsigned cur;

	cur = wq->head - wq->tail;
	if (likely(cur + nreq < wq->max_post))
		return 0;

	cq = to_mcq(ib_cq);
	spin_lock(&cq->lock);
	cur = wq->head - wq->tail;
	spin_unlock(&cq->lock);

	return cur + nreq >= wq->max_post;
}

static __always_inline void set_raddr_seg(struct mlx5_wqe_raddr_seg *rseg,
					  u64 remote_addr, u32 rkey)
{
	rseg->raddr    = cpu_to_be64(remote_addr);
	rseg->rkey     = cpu_to_be32(rkey);
	rseg->reserved = 0;
}

static void set_eth_seg(const struct ib_send_wr *wr, struct mlx5_ib_qp *qp,
			void **seg, int *size, void **cur_edge)
{
	struct mlx5_wqe_eth_seg *eseg = *seg;

	memset(eseg, 0, sizeof(struct mlx5_wqe_eth_seg));

	if (wr->send_flags & IB_SEND_IP_CSUM)
		eseg->cs_flags = MLX5_ETH_WQE_L3_CSUM |
				 MLX5_ETH_WQE_L4_CSUM;

	if (wr->opcode == IB_WR_LSO) {
		struct ib_ud_wr *ud_wr = container_of(wr, struct ib_ud_wr, wr);
		size_t left, copysz;
		void *pdata = ud_wr->header;
		size_t stride;

		left = ud_wr->hlen;
		eseg->mss = cpu_to_be16(ud_wr->mss);
		eseg->inline_hdr.sz = cpu_to_be16(left);

		/* memcpy_send_wqe should get a 16B align address. Hence, we
		 * first copy up to the current edge and then, if needed,
		 * fall-through to memcpy_send_wqe.
		 */
		copysz = min_t(u64, *cur_edge - (void *)eseg->inline_hdr.start,
			       left);
		memcpy(eseg->inline_hdr.start, pdata, copysz);
		stride = ALIGN(sizeof(struct mlx5_wqe_eth_seg) -
			       sizeof(eseg->inline_hdr.start) + copysz, 16);
		*size += stride / 16;
		*seg += stride;

		if (copysz < left) {
			handle_post_send_edge(&qp->sq, seg, *size, cur_edge);
			left -= copysz;
			pdata += copysz;
			memcpy_send_wqe(&qp->sq, cur_edge, seg, size, pdata,
					left);
		}

		return;
	}

	*seg += sizeof(struct mlx5_wqe_eth_seg);
	*size += sizeof(struct mlx5_wqe_eth_seg) / 16;
}

static void set_datagram_seg(struct mlx5_wqe_datagram_seg *dseg,
			     const struct ib_send_wr *wr)
{
	memcpy(&dseg->av, &to_mah(ud_wr(wr)->ah)->av, sizeof(struct mlx5_av));
	dseg->av.dqp_dct = cpu_to_be32(ud_wr(wr)->remote_qpn | MLX5_EXTENDED_UD_AV);
	dseg->av.key.qkey.qkey = cpu_to_be32(ud_wr(wr)->remote_qkey);
}

static void set_data_ptr_seg(struct mlx5_wqe_data_seg *dseg, struct ib_sge *sg)
{
	dseg->byte_count = cpu_to_be32(sg->length);
	dseg->lkey       = cpu_to_be32(sg->lkey);
	dseg->addr       = cpu_to_be64(sg->addr);
}

static u64 get_xlt_octo(u64 bytes)
{
	return ALIGN(bytes, MLX5_IB_UMR_XLT_ALIGNMENT) /
	       MLX5_IB_UMR_OCTOWORD;
}

static __be64 frwr_mkey_mask(bool atomic)
{
	u64 result;

	result = MLX5_MKEY_MASK_LEN		|
		MLX5_MKEY_MASK_PAGE_SIZE	|
		MLX5_MKEY_MASK_START_ADDR	|
		MLX5_MKEY_MASK_EN_RINVAL	|
		MLX5_MKEY_MASK_KEY		|
		MLX5_MKEY_MASK_LR		|
		MLX5_MKEY_MASK_LW		|
		MLX5_MKEY_MASK_RR		|
		MLX5_MKEY_MASK_RW		|
		MLX5_MKEY_MASK_SMALL_FENCE	|
		MLX5_MKEY_MASK_FREE;

	if (atomic)
		result |= MLX5_MKEY_MASK_A;

	return cpu_to_be64(result);
}

static __be64 sig_mkey_mask(void)
{
	u64 result;

	result = MLX5_MKEY_MASK_LEN		|
		MLX5_MKEY_MASK_PAGE_SIZE	|
		MLX5_MKEY_MASK_START_ADDR	|
		MLX5_MKEY_MASK_EN_SIGERR	|
		MLX5_MKEY_MASK_EN_RINVAL	|
		MLX5_MKEY_MASK_KEY		|
		MLX5_MKEY_MASK_LR		|
		MLX5_MKEY_MASK_LW		|
		MLX5_MKEY_MASK_RR		|
		MLX5_MKEY_MASK_RW		|
		MLX5_MKEY_MASK_SMALL_FENCE	|
		MLX5_MKEY_MASK_FREE		|
		MLX5_MKEY_MASK_BSF_EN;

	return cpu_to_be64(result);
}

static void set_reg_umr_seg(struct mlx5_wqe_umr_ctrl_seg *umr,
			    struct mlx5_ib_mr *mr, u8 flags, bool atomic)
{
	int size = (mr->ndescs + mr->meta_ndescs) * mr->desc_size;

	memset(umr, 0, sizeof(*umr));

	umr->flags = flags;
	umr->xlt_octowords = cpu_to_be16(get_xlt_octo(size));
	umr->mkey_mask = frwr_mkey_mask(atomic);
}

static void set_linv_umr_seg(struct mlx5_wqe_umr_ctrl_seg *umr)
{
	memset(umr, 0, sizeof(*umr));
	umr->mkey_mask = cpu_to_be64(MLX5_MKEY_MASK_FREE);
	umr->flags = MLX5_UMR_INLINE;
}

static __be64 get_umr_enable_mr_mask(void)
{
	u64 result;

	result = MLX5_MKEY_MASK_KEY |
		 MLX5_MKEY_MASK_FREE;

	return cpu_to_be64(result);
}

static __be64 get_umr_disable_mr_mask(void)
{
	u64 result;

	result = MLX5_MKEY_MASK_FREE;

	return cpu_to_be64(result);
}

static __be64 get_umr_update_translation_mask(void)
{
	u64 result;

	result = MLX5_MKEY_MASK_LEN |
		 MLX5_MKEY_MASK_PAGE_SIZE |
		 MLX5_MKEY_MASK_START_ADDR;

	return cpu_to_be64(result);
}

static __be64 get_umr_update_access_mask(int atomic)
{
	u64 result;

	result = MLX5_MKEY_MASK_LR |
		 MLX5_MKEY_MASK_LW |
		 MLX5_MKEY_MASK_RR |
		 MLX5_MKEY_MASK_RW;

	if (atomic)
		result |= MLX5_MKEY_MASK_A;

	return cpu_to_be64(result);
}

static __be64 get_umr_update_pd_mask(void)
{
	u64 result;

	result = MLX5_MKEY_MASK_PD;

	return cpu_to_be64(result);
}

static int umr_check_mkey_mask(struct mlx5_ib_dev *dev, u64 mask)
{
	if ((mask & MLX5_MKEY_MASK_PAGE_SIZE &&
	     MLX5_CAP_GEN(dev->mdev, umr_modify_entity_size_disabled)) ||
	    (mask & MLX5_MKEY_MASK_A &&
	     MLX5_CAP_GEN(dev->mdev, umr_modify_atomic_disabled)))
		return -EPERM;
	return 0;
}

static int set_reg_umr_segment(struct mlx5_ib_dev *dev,
			       struct mlx5_wqe_umr_ctrl_seg *umr,
			       const struct ib_send_wr *wr, int atomic)
{
	const struct mlx5_umr_wr *umrwr = umr_wr(wr);

	memset(umr, 0, sizeof(*umr));

	if (!umrwr->ignore_free_state) {
		if (wr->send_flags & MLX5_IB_SEND_UMR_FAIL_IF_FREE)
			 /* fail if free */
			umr->flags = MLX5_UMR_CHECK_FREE;
		else
			/* fail if not free */
			umr->flags = MLX5_UMR_CHECK_NOT_FREE;
	}

	umr->xlt_octowords = cpu_to_be16(get_xlt_octo(umrwr->xlt_size));
	if (wr->send_flags & MLX5_IB_SEND_UMR_UPDATE_XLT) {
		u64 offset = get_xlt_octo(umrwr->offset);

		umr->xlt_offset = cpu_to_be16(offset & 0xffff);
		umr->xlt_offset_47_16 = cpu_to_be32(offset >> 16);
		umr->flags |= MLX5_UMR_TRANSLATION_OFFSET_EN;
	}
	if (wr->send_flags & MLX5_IB_SEND_UMR_UPDATE_TRANSLATION)
		umr->mkey_mask |= get_umr_update_translation_mask();
	if (wr->send_flags & MLX5_IB_SEND_UMR_UPDATE_PD_ACCESS) {
		umr->mkey_mask |= get_umr_update_access_mask(atomic);
		umr->mkey_mask |= get_umr_update_pd_mask();
	}
	if (wr->send_flags & MLX5_IB_SEND_UMR_ENABLE_MR)
		umr->mkey_mask |= get_umr_enable_mr_mask();
	if (wr->send_flags & MLX5_IB_SEND_UMR_DISABLE_MR)
		umr->mkey_mask |= get_umr_disable_mr_mask();

	if (!wr->num_sge)
		umr->flags |= MLX5_UMR_INLINE;

	return umr_check_mkey_mask(dev, be64_to_cpu(umr->mkey_mask));
}

static u8 get_umr_flags(int acc)
{
	return (acc & IB_ACCESS_REMOTE_ATOMIC ? MLX5_PERM_ATOMIC       : 0) |
	       (acc & IB_ACCESS_REMOTE_WRITE  ? MLX5_PERM_REMOTE_WRITE : 0) |
	       (acc & IB_ACCESS_REMOTE_READ   ? MLX5_PERM_REMOTE_READ  : 0) |
	       (acc & IB_ACCESS_LOCAL_WRITE   ? MLX5_PERM_LOCAL_WRITE  : 0) |
		MLX5_PERM_LOCAL_READ | MLX5_PERM_UMR_EN;
}

static void set_reg_mkey_seg(struct mlx5_mkey_seg *seg,
			     struct mlx5_ib_mr *mr,
			     u32 key, int access)
{
	int ndescs = ALIGN(mr->ndescs + mr->meta_ndescs, 8) >> 1;

	memset(seg, 0, sizeof(*seg));

	if (mr->access_mode == MLX5_MKC_ACCESS_MODE_MTT)
		seg->log2_page_size = ilog2(mr->ibmr.page_size);
	else if (mr->access_mode == MLX5_MKC_ACCESS_MODE_KLMS)
		/* KLMs take twice the size of MTTs */
		ndescs *= 2;

	seg->flags = get_umr_flags(access) | mr->access_mode;
	seg->qpn_mkey7_0 = cpu_to_be32((key & 0xff) | 0xffffff00);
	seg->flags_pd = cpu_to_be32(MLX5_MKEY_REMOTE_INVAL);
	seg->start_addr = cpu_to_be64(mr->ibmr.iova);
	seg->len = cpu_to_be64(mr->ibmr.length);
	seg->xlt_oct_size = cpu_to_be32(ndescs);
}

static void set_linv_mkey_seg(struct mlx5_mkey_seg *seg)
{
	memset(seg, 0, sizeof(*seg));
	seg->status = MLX5_MKEY_STATUS_FREE;
}

static void set_reg_mkey_segment(struct mlx5_mkey_seg *seg,
				 const struct ib_send_wr *wr)
{
	const struct mlx5_umr_wr *umrwr = umr_wr(wr);

	memset(seg, 0, sizeof(*seg));
	if (wr->send_flags & MLX5_IB_SEND_UMR_DISABLE_MR)
		seg->status = MLX5_MKEY_STATUS_FREE;

	seg->flags = convert_access(umrwr->access_flags);
	if (umrwr->pd)
		seg->flags_pd = cpu_to_be32(to_mpd(umrwr->pd)->pdn);
	if (wr->send_flags & MLX5_IB_SEND_UMR_UPDATE_TRANSLATION &&
	    !umrwr->length)
		seg->flags_pd |= cpu_to_be32(MLX5_MKEY_LEN64);

	seg->start_addr = cpu_to_be64(umrwr->virt_addr);
	seg->len = cpu_to_be64(umrwr->length);
	seg->log2_page_size = umrwr->page_shift;
	seg->qpn_mkey7_0 = cpu_to_be32(0xffffff00 |
				       mlx5_mkey_variant(umrwr->mkey));
}

static void set_reg_data_seg(struct mlx5_wqe_data_seg *dseg,
			     struct mlx5_ib_mr *mr,
			     struct mlx5_ib_pd *pd)
{
	int bcount = mr->desc_size * (mr->ndescs + mr->meta_ndescs);

	dseg->addr = cpu_to_be64(mr->desc_map);
	dseg->byte_count = cpu_to_be32(ALIGN(bcount, 64));
	dseg->lkey = cpu_to_be32(pd->ibpd.local_dma_lkey);
}

static __be32 send_ieth(const struct ib_send_wr *wr)
{
	switch (wr->opcode) {
	case IB_WR_SEND_WITH_IMM:
	case IB_WR_RDMA_WRITE_WITH_IMM:
		return wr->ex.imm_data;

	case IB_WR_SEND_WITH_INV:
		return cpu_to_be32(wr->ex.invalidate_rkey);

	default:
		return 0;
	}
}

static u8 calc_sig(void *wqe, int size)
{
	u8 *p = wqe;
	u8 res = 0;
	int i;

	for (i = 0; i < size; i++)
		res ^= p[i];

	return ~res;
}

static u8 wq_sig(void *wqe)
{
	return calc_sig(wqe, (*((u8 *)wqe + 8) & 0x3f) << 4);
}

static int set_data_inl_seg(struct mlx5_ib_qp *qp, const struct ib_send_wr *wr,
			    void **wqe, int *wqe_sz, void **cur_edge)
{
	struct mlx5_wqe_inline_seg *seg;
	size_t offset;
	int inl = 0;
	int i;

	seg = *wqe;
	*wqe += sizeof(*seg);
	offset = sizeof(*seg);

	for (i = 0; i < wr->num_sge; i++) {
		size_t len  = wr->sg_list[i].length;
		void *addr = (void *)(unsigned long)(wr->sg_list[i].addr);

		inl += len;

		if (unlikely(inl > qp->max_inline_data))
			return -ENOMEM;

		while (likely(len)) {
			size_t leftlen;
			size_t copysz;

			handle_post_send_edge(&qp->sq, wqe,
					      *wqe_sz + (offset >> 4),
					      cur_edge);

			leftlen = *cur_edge - *wqe;
			copysz = min_t(size_t, leftlen, len);

			memcpy(*wqe, addr, copysz);
			len -= copysz;
			addr += copysz;
			*wqe += copysz;
			offset += copysz;
		}
	}

	seg->byte_count = cpu_to_be32(inl | MLX5_INLINE_SEG);

	*wqe_sz +=  ALIGN(inl + sizeof(seg->byte_count), 16) / 16;

	return 0;
}

static u16 prot_field_size(enum ib_signature_type type)
{
	switch (type) {
	case IB_SIG_TYPE_T10_DIF:
		return MLX5_DIF_SIZE;
	default:
		return 0;
	}
}

static u8 bs_selector(int block_size)
{
	switch (block_size) {
	case 512:	    return 0x1;
	case 520:	    return 0x2;
	case 4096:	    return 0x3;
	case 4160:	    return 0x4;
	case 1073741824:    return 0x5;
	default:	    return 0;
	}
}

static void mlx5_fill_inl_bsf(struct ib_sig_domain *domain,
			      struct mlx5_bsf_inl *inl)
{
	/* Valid inline section and allow BSF refresh */
	inl->vld_refresh = cpu_to_be16(MLX5_BSF_INL_VALID |
				       MLX5_BSF_REFRESH_DIF);
	inl->dif_apptag = cpu_to_be16(domain->sig.dif.app_tag);
	inl->dif_reftag = cpu_to_be32(domain->sig.dif.ref_tag);
	/* repeating block */
	inl->rp_inv_seed = MLX5_BSF_REPEAT_BLOCK;
	inl->sig_type = domain->sig.dif.bg_type == IB_T10DIF_CRC ?
			MLX5_DIF_CRC : MLX5_DIF_IPCS;

	if (domain->sig.dif.ref_remap)
		inl->dif_inc_ref_guard_check |= MLX5_BSF_INC_REFTAG;

	if (domain->sig.dif.app_escape) {
		if (domain->sig.dif.ref_escape)
			inl->dif_inc_ref_guard_check |= MLX5_BSF_APPREF_ESCAPE;
		else
			inl->dif_inc_ref_guard_check |= MLX5_BSF_APPTAG_ESCAPE;
	}

	inl->dif_app_bitmask_check =
		cpu_to_be16(domain->sig.dif.apptag_check_mask);
}

static int mlx5_set_bsf(struct ib_mr *sig_mr,
			struct ib_sig_attrs *sig_attrs,
			struct mlx5_bsf *bsf, u32 data_size)
{
	struct mlx5_core_sig_ctx *msig = to_mmr(sig_mr)->sig;
	struct mlx5_bsf_basic *basic = &bsf->basic;
	struct ib_sig_domain *mem = &sig_attrs->mem;
	struct ib_sig_domain *wire = &sig_attrs->wire;

	memset(bsf, 0, sizeof(*bsf));

	/* Basic + Extended + Inline */
	basic->bsf_size_sbs = 1 << 7;
	/* Input domain check byte mask */
	basic->check_byte_mask = sig_attrs->check_mask;
	basic->raw_data_size = cpu_to_be32(data_size);

	/* Memory domain */
	switch (sig_attrs->mem.sig_type) {
	case IB_SIG_TYPE_NONE:
		break;
	case IB_SIG_TYPE_T10_DIF:
		basic->mem.bs_selector = bs_selector(mem->sig.dif.pi_interval);
		basic->m_bfs_psv = cpu_to_be32(msig->psv_memory.psv_idx);
		mlx5_fill_inl_bsf(mem, &bsf->m_inl);
		break;
	default:
		return -EINVAL;
	}

	/* Wire domain */
	switch (sig_attrs->wire.sig_type) {
	case IB_SIG_TYPE_NONE:
		break;
	case IB_SIG_TYPE_T10_DIF:
		if (mem->sig.dif.pi_interval == wire->sig.dif.pi_interval &&
		    mem->sig_type == wire->sig_type) {
			/* Same block structure */
			basic->bsf_size_sbs |= 1 << 4;
			if (mem->sig.dif.bg_type == wire->sig.dif.bg_type)
				basic->wire.copy_byte_mask |= MLX5_CPY_GRD_MASK;
			if (mem->sig.dif.app_tag == wire->sig.dif.app_tag)
				basic->wire.copy_byte_mask |= MLX5_CPY_APP_MASK;
			if (mem->sig.dif.ref_tag == wire->sig.dif.ref_tag)
				basic->wire.copy_byte_mask |= MLX5_CPY_REF_MASK;
		} else
			basic->wire.bs_selector = bs_selector(wire->sig.dif.pi_interval);

		basic->w_bfs_psv = cpu_to_be32(msig->psv_wire.psv_idx);
		mlx5_fill_inl_bsf(wire, &bsf->w_inl);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int set_sig_data_segment(const struct ib_send_wr *send_wr,
				struct ib_mr *sig_mr,
				struct ib_sig_attrs *sig_attrs,
				struct mlx5_ib_qp *qp, void **seg, int *size,
				void **cur_edge)
{
	struct mlx5_bsf *bsf;
	u32 data_len;
	u32 data_key;
	u64 data_va;
	u32 prot_len = 0;
	u32 prot_key = 0;
	u64 prot_va = 0;
	bool prot = false;
	int ret;
	int wqe_size;
	struct mlx5_ib_mr *mr = to_mmr(sig_mr);
	struct mlx5_ib_mr *pi_mr = mr->pi_mr;

	data_len = pi_mr->data_length;
	data_key = pi_mr->ibmr.lkey;
	data_va = pi_mr->data_iova;
	if (pi_mr->meta_ndescs) {
		prot_len = pi_mr->meta_length;
		prot_key = pi_mr->ibmr.lkey;
		prot_va = pi_mr->pi_iova;
		prot = true;
	}

	if (!prot || (data_key == prot_key && data_va == prot_va &&
		      data_len == prot_len)) {
		/**
		 * Source domain doesn't contain signature information
		 * or data and protection are interleaved in memory.
		 * So need construct:
		 *                  ------------------
		 *                 |     data_klm     |
		 *                  ------------------
		 *                 |       BSF        |
		 *                  ------------------
		 **/
		struct mlx5_klm *data_klm = *seg;

		data_klm->bcount = cpu_to_be32(data_len);
		data_klm->key = cpu_to_be32(data_key);
		data_klm->va = cpu_to_be64(data_va);
		wqe_size = ALIGN(sizeof(*data_klm), 64);
	} else {
		/**
		 * Source domain contains signature information
		 * So need construct a strided block format:
		 *               ---------------------------
		 *              |     stride_block_ctrl     |
		 *               ---------------------------
		 *              |          data_klm         |
		 *               ---------------------------
		 *              |          prot_klm         |
		 *               ---------------------------
		 *              |             BSF           |
		 *               ---------------------------
		 **/
		struct mlx5_stride_block_ctrl_seg *sblock_ctrl;
		struct mlx5_stride_block_entry *data_sentry;
		struct mlx5_stride_block_entry *prot_sentry;
		u16 block_size = sig_attrs->mem.sig.dif.pi_interval;
		int prot_size;

		sblock_ctrl = *seg;
		data_sentry = (void *)sblock_ctrl + sizeof(*sblock_ctrl);
		prot_sentry = (void *)data_sentry + sizeof(*data_sentry);

		prot_size = prot_field_size(sig_attrs->mem.sig_type);
		if (!prot_size) {
			pr_err("Bad block size given: %u\n", block_size);
			return -EINVAL;
		}
		sblock_ctrl->bcount_per_cycle = cpu_to_be32(block_size +
							    prot_size);
		sblock_ctrl->op = cpu_to_be32(MLX5_STRIDE_BLOCK_OP);
		sblock_ctrl->repeat_count = cpu_to_be32(data_len / block_size);
		sblock_ctrl->num_entries = cpu_to_be16(2);

		data_sentry->bcount = cpu_to_be16(block_size);
		data_sentry->key = cpu_to_be32(data_key);
		data_sentry->va = cpu_to_be64(data_va);
		data_sentry->stride = cpu_to_be16(block_size);

		prot_sentry->bcount = cpu_to_be16(prot_size);
		prot_sentry->key = cpu_to_be32(prot_key);
		prot_sentry->va = cpu_to_be64(prot_va);
		prot_sentry->stride = cpu_to_be16(prot_size);

		wqe_size = ALIGN(sizeof(*sblock_ctrl) + sizeof(*data_sentry) +
				 sizeof(*prot_sentry), 64);
	}

	*seg += wqe_size;
	*size += wqe_size / 16;
	handle_post_send_edge(&qp->sq, seg, *size, cur_edge);

	bsf = *seg;
	ret = mlx5_set_bsf(sig_mr, sig_attrs, bsf, data_len);
	if (ret)
		return -EINVAL;

	*seg += sizeof(*bsf);
	*size += sizeof(*bsf) / 16;
	handle_post_send_edge(&qp->sq, seg, *size, cur_edge);

	return 0;
}

static void set_sig_mkey_segment(struct mlx5_mkey_seg *seg,
				 struct ib_mr *sig_mr, int access_flags,
				 u32 size, u32 length, u32 pdn)
{
	u32 sig_key = sig_mr->rkey;
	u8 sigerr = to_mmr(sig_mr)->sig->sigerr_count & 1;

	memset(seg, 0, sizeof(*seg));

	seg->flags = get_umr_flags(access_flags) | MLX5_MKC_ACCESS_MODE_KLMS;
	seg->qpn_mkey7_0 = cpu_to_be32((sig_key & 0xff) | 0xffffff00);
	seg->flags_pd = cpu_to_be32(MLX5_MKEY_REMOTE_INVAL | sigerr << 26 |
				    MLX5_MKEY_BSF_EN | pdn);
	seg->len = cpu_to_be64(length);
	seg->xlt_oct_size = cpu_to_be32(get_xlt_octo(size));
	seg->bsfs_octo_size = cpu_to_be32(MLX5_MKEY_BSF_OCTO_SIZE);
}

static void set_sig_umr_segment(struct mlx5_wqe_umr_ctrl_seg *umr,
				u32 size)
{
	memset(umr, 0, sizeof(*umr));

	umr->flags = MLX5_FLAGS_INLINE | MLX5_FLAGS_CHECK_FREE;
	umr->xlt_octowords = cpu_to_be16(get_xlt_octo(size));
	umr->bsf_octowords = cpu_to_be16(MLX5_MKEY_BSF_OCTO_SIZE);
	umr->mkey_mask = sig_mkey_mask();
}

static int set_pi_umr_wr(const struct ib_send_wr *send_wr,
			 struct mlx5_ib_qp *qp, void **seg, int *size,
			 void **cur_edge)
{
	const struct ib_reg_wr *wr = reg_wr(send_wr);
	struct mlx5_ib_mr *sig_mr = to_mmr(wr->mr);
	struct mlx5_ib_mr *pi_mr = sig_mr->pi_mr;
	struct ib_sig_attrs *sig_attrs = sig_mr->ibmr.sig_attrs;
	u32 pdn = get_pd(qp)->pdn;
	u32 xlt_size;
	int region_len, ret;

	if (unlikely(send_wr->num_sge != 0) ||
	    unlikely(wr->access & IB_ACCESS_REMOTE_ATOMIC) ||
	    unlikely(!sig_mr->sig) || unlikely(!qp->ibqp.integrity_en) ||
	    unlikely(!sig_mr->sig->sig_status_checked))
		return -EINVAL;

	/* length of the protected region, data + protection */
	region_len = pi_mr->ibmr.length;

	/**
	 * KLM octoword size - if protection was provided
	 * then we use strided block format (3 octowords),
	 * else we use single KLM (1 octoword)
	 **/
	if (sig_attrs->mem.sig_type != IB_SIG_TYPE_NONE)
		xlt_size = 0x30;
	else
		xlt_size = sizeof(struct mlx5_klm);

	set_sig_umr_segment(*seg, xlt_size);
	*seg += sizeof(struct mlx5_wqe_umr_ctrl_seg);
	*size += sizeof(struct mlx5_wqe_umr_ctrl_seg) / 16;
	handle_post_send_edge(&qp->sq, seg, *size, cur_edge);

	set_sig_mkey_segment(*seg, wr->mr, wr->access, xlt_size, region_len,
			     pdn);
	*seg += sizeof(struct mlx5_mkey_seg);
	*size += sizeof(struct mlx5_mkey_seg) / 16;
	handle_post_send_edge(&qp->sq, seg, *size, cur_edge);

	ret = set_sig_data_segment(send_wr, wr->mr, sig_attrs, qp, seg, size,
				   cur_edge);
	if (ret)
		return ret;

	sig_mr->sig->sig_status_checked = false;
	return 0;
}

static int set_psv_wr(struct ib_sig_domain *domain,
		      u32 psv_idx, void **seg, int *size)
{
	struct mlx5_seg_set_psv *psv_seg = *seg;

	memset(psv_seg, 0, sizeof(*psv_seg));
	psv_seg->psv_num = cpu_to_be32(psv_idx);
	switch (domain->sig_type) {
	case IB_SIG_TYPE_NONE:
		break;
	case IB_SIG_TYPE_T10_DIF:
		psv_seg->transient_sig = cpu_to_be32(domain->sig.dif.bg << 16 |
						     domain->sig.dif.app_tag);
		psv_seg->ref_tag = cpu_to_be32(domain->sig.dif.ref_tag);
		break;
	default:
		pr_err("Bad signature type (%d) is given.\n",
		       domain->sig_type);
		return -EINVAL;
	}

	*seg += sizeof(*psv_seg);
	*size += sizeof(*psv_seg) / 16;

	return 0;
}

static int set_reg_wr(struct mlx5_ib_qp *qp,
		      const struct ib_reg_wr *wr,
		      void **seg, int *size, void **cur_edge,
		      bool check_not_free)
{
	struct mlx5_ib_mr *mr = to_mmr(wr->mr);
	struct mlx5_ib_pd *pd = to_mpd(qp->ibqp.pd);
	struct mlx5_ib_dev *dev = to_mdev(pd->ibpd.device);
	int mr_list_size = (mr->ndescs + mr->meta_ndescs) * mr->desc_size;
	bool umr_inline = mr_list_size <= MLX5_IB_SQ_UMR_INLINE_THRESHOLD;
	bool atomic = wr->access & IB_ACCESS_REMOTE_ATOMIC;
	u8 flags = 0;

	if (!mlx5_ib_can_use_umr(dev, atomic)) {
		mlx5_ib_warn(to_mdev(qp->ibqp.device),
			     "Fast update of %s for MR is disabled\n",
			     (MLX5_CAP_GEN(dev->mdev,
					   umr_modify_entity_size_disabled)) ?
				     "entity size" :
				     "atomic access");
		return -EINVAL;
	}

	if (unlikely(wr->wr.send_flags & IB_SEND_INLINE)) {
		mlx5_ib_warn(to_mdev(qp->ibqp.device),
			     "Invalid IB_SEND_INLINE send flag\n");
		return -EINVAL;
	}

	if (check_not_free)
		flags |= MLX5_UMR_CHECK_NOT_FREE;
	if (umr_inline)
		flags |= MLX5_UMR_INLINE;

	set_reg_umr_seg(*seg, mr, flags, atomic);
	*seg += sizeof(struct mlx5_wqe_umr_ctrl_seg);
	*size += sizeof(struct mlx5_wqe_umr_ctrl_seg) / 16;
	handle_post_send_edge(&qp->sq, seg, *size, cur_edge);

	set_reg_mkey_seg(*seg, mr, wr->key, wr->access);
	*seg += sizeof(struct mlx5_mkey_seg);
	*size += sizeof(struct mlx5_mkey_seg) / 16;
	handle_post_send_edge(&qp->sq, seg, *size, cur_edge);

	if (umr_inline) {
		memcpy_send_wqe(&qp->sq, cur_edge, seg, size, mr->descs,
				mr_list_size);
		*size = ALIGN(*size, MLX5_SEND_WQE_BB >> 4);
	} else {
		set_reg_data_seg(*seg, mr, pd);
		*seg += sizeof(struct mlx5_wqe_data_seg);
		*size += (sizeof(struct mlx5_wqe_data_seg) / 16);
	}
	return 0;
}

static void set_linv_wr(struct mlx5_ib_qp *qp, void **seg, int *size,
			void **cur_edge)
{
	set_linv_umr_seg(*seg);
	*seg += sizeof(struct mlx5_wqe_umr_ctrl_seg);
	*size += sizeof(struct mlx5_wqe_umr_ctrl_seg) / 16;
	handle_post_send_edge(&qp->sq, seg, *size, cur_edge);
	set_linv_mkey_seg(*seg);
	*seg += sizeof(struct mlx5_mkey_seg);
	*size += sizeof(struct mlx5_mkey_seg) / 16;
	handle_post_send_edge(&qp->sq, seg, *size, cur_edge);
}

static void dump_wqe(struct mlx5_ib_qp *qp, u32 idx, int size_16)
{
	__be32 *p = NULL;
	int i, j;

	pr_debug("dump WQE index %u:\n", idx);
	for (i = 0, j = 0; i < size_16 * 4; i += 4, j += 4) {
		if ((i & 0xf) == 0) {
			p = mlx5_frag_buf_get_wqe(&qp->sq.fbc, idx);
			pr_debug("WQBB at %p:\n", (void *)p);
			j = 0;
			idx = (idx + 1) & (qp->sq.wqe_cnt - 1);
		}
		pr_debug("%08x %08x %08x %08x\n", be32_to_cpu(p[j]),
			 be32_to_cpu(p[j + 1]), be32_to_cpu(p[j + 2]),
			 be32_to_cpu(p[j + 3]));
	}
}

static int __begin_wqe(struct mlx5_ib_qp *qp, void **seg,
		       struct mlx5_wqe_ctrl_seg **ctrl,
		       const struct ib_send_wr *wr, unsigned int *idx,
		       int *size, void **cur_edge, int nreq,
		       bool send_signaled, bool solicited)
{
	if (unlikely(mlx5_wq_overflow(&qp->sq, nreq, qp->ibqp.send_cq)))
		return -ENOMEM;

	*idx = qp->sq.cur_post & (qp->sq.wqe_cnt - 1);
	*seg = mlx5_frag_buf_get_wqe(&qp->sq.fbc, *idx);
	*ctrl = *seg;
	*(uint32_t *)(*seg + 8) = 0;
	(*ctrl)->imm = send_ieth(wr);
	(*ctrl)->fm_ce_se = qp->sq_signal_bits |
		(send_signaled ? MLX5_WQE_CTRL_CQ_UPDATE : 0) |
		(solicited ? MLX5_WQE_CTRL_SOLICITED : 0);

	*seg += sizeof(**ctrl);
	*size = sizeof(**ctrl) / 16;
	*cur_edge = qp->sq.cur_edge;

	return 0;
}

static int begin_wqe(struct mlx5_ib_qp *qp, void **seg,
		     struct mlx5_wqe_ctrl_seg **ctrl,
		     const struct ib_send_wr *wr, unsigned *idx,
		     int *size, void **cur_edge, int nreq)
{
	return __begin_wqe(qp, seg, ctrl, wr, idx, size, cur_edge, nreq,
			   wr->send_flags & IB_SEND_SIGNALED,
			   wr->send_flags & IB_SEND_SOLICITED);
}

static void finish_wqe(struct mlx5_ib_qp *qp,
		       struct mlx5_wqe_ctrl_seg *ctrl,
		       void *seg, u8 size, void *cur_edge,
		       unsigned int idx, u64 wr_id, int nreq, u8 fence,
		       u32 mlx5_opcode)
{
	u8 opmod = 0;

	ctrl->opmod_idx_opcode = cpu_to_be32(((u32)(qp->sq.cur_post) << 8) |
					     mlx5_opcode | ((u32)opmod << 24));
	ctrl->qpn_ds = cpu_to_be32(size | (qp->trans_qp.base.mqp.qpn << 8));
	ctrl->fm_ce_se |= fence;
	if (unlikely(qp->wq_sig))
		ctrl->signature = wq_sig(ctrl);

	qp->sq.wrid[idx] = wr_id;
	qp->sq.w_list[idx].opcode = mlx5_opcode;
	qp->sq.wqe_head[idx] = qp->sq.head + nreq;
	qp->sq.cur_post += DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB);
	qp->sq.w_list[idx].next = qp->sq.cur_post;

	/* We save the edge which was possibly updated during the WQE
	 * construction, into SQ's cache.
	 */
	seg = PTR_ALIGN(seg, MLX5_SEND_WQE_BB);
	qp->sq.cur_edge = (unlikely(seg == cur_edge)) ?
			  get_sq_edge(&qp->sq, qp->sq.cur_post &
				      (qp->sq.wqe_cnt - 1)) :
			  cur_edge;
}

static int _mlx5_ib_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
			      const struct ib_send_wr **bad_wr, bool drain)
{
	struct mlx5_wqe_ctrl_seg *ctrl = NULL;  /* compiler warning */
	struct mlx5_ib_dev *dev = to_mdev(ibqp->device);
	struct mlx5_core_dev *mdev = dev->mdev;
	struct ib_reg_wr reg_pi_wr;
	struct mlx5_ib_qp *qp;
	struct mlx5_ib_mr *mr;
	struct mlx5_ib_mr *pi_mr;
	struct mlx5_ib_mr pa_pi_mr;
	struct ib_sig_attrs *sig_attrs;
	struct mlx5_wqe_xrc_seg *xrc;
	struct mlx5_bf *bf;
	void *cur_edge;
	int uninitialized_var(size);
	unsigned long flags;
	unsigned idx;
	int err = 0;
	int num_sge;
	void *seg;
	int nreq;
	int i;
	u8 next_fence = 0;
	u8 fence;

	if (unlikely(mdev->state == MLX5_DEVICE_STATE_INTERNAL_ERROR &&
		     !drain)) {
		*bad_wr = wr;
		return -EIO;
	}

	if (unlikely(ibqp->qp_type == IB_QPT_GSI))
		return mlx5_ib_gsi_post_send(ibqp, wr, bad_wr);

	qp = to_mqp(ibqp);
	bf = &qp->bf;

	spin_lock_irqsave(&qp->sq.lock, flags);

	for (nreq = 0; wr; nreq++, wr = wr->next) {
		if (unlikely(wr->opcode >= ARRAY_SIZE(mlx5_ib_opcode))) {
			mlx5_ib_warn(dev, "\n");
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		num_sge = wr->num_sge;
		if (unlikely(num_sge > qp->sq.max_gs)) {
			mlx5_ib_warn(dev, "\n");
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		err = begin_wqe(qp, &seg, &ctrl, wr, &idx, &size, &cur_edge,
				nreq);
		if (err) {
			mlx5_ib_warn(dev, "\n");
			err = -ENOMEM;
			*bad_wr = wr;
			goto out;
		}

		if (wr->opcode == IB_WR_REG_MR ||
		    wr->opcode == IB_WR_REG_MR_INTEGRITY) {
			fence = dev->umr_fence;
			next_fence = MLX5_FENCE_MODE_INITIATOR_SMALL;
		} else  {
			if (wr->send_flags & IB_SEND_FENCE) {
				if (qp->next_fence)
					fence = MLX5_FENCE_MODE_SMALL_AND_FENCE;
				else
					fence = MLX5_FENCE_MODE_FENCE;
			} else {
				fence = qp->next_fence;
			}
		}

		switch (ibqp->qp_type) {
		case IB_QPT_XRC_INI:
			xrc = seg;
			seg += sizeof(*xrc);
			size += sizeof(*xrc) / 16;
			/* fall through */
		case IB_QPT_RC:
			switch (wr->opcode) {
			case IB_WR_RDMA_READ:
			case IB_WR_RDMA_WRITE:
			case IB_WR_RDMA_WRITE_WITH_IMM:
				set_raddr_seg(seg, rdma_wr(wr)->remote_addr,
					      rdma_wr(wr)->rkey);
				seg += sizeof(struct mlx5_wqe_raddr_seg);
				size += sizeof(struct mlx5_wqe_raddr_seg) / 16;
				break;

			case IB_WR_ATOMIC_CMP_AND_SWP:
			case IB_WR_ATOMIC_FETCH_AND_ADD:
			case IB_WR_MASKED_ATOMIC_CMP_AND_SWP:
				mlx5_ib_warn(dev, "Atomic operations are not supported yet\n");
				err = -ENOSYS;
				*bad_wr = wr;
				goto out;

			case IB_WR_LOCAL_INV:
				qp->sq.wr_data[idx] = IB_WR_LOCAL_INV;
				ctrl->imm = cpu_to_be32(wr->ex.invalidate_rkey);
				set_linv_wr(qp, &seg, &size, &cur_edge);
				num_sge = 0;
				break;

			case IB_WR_REG_MR:
				qp->sq.wr_data[idx] = IB_WR_REG_MR;
				ctrl->imm = cpu_to_be32(reg_wr(wr)->key);
				err = set_reg_wr(qp, reg_wr(wr), &seg, &size,
						 &cur_edge, true);
				if (err) {
					*bad_wr = wr;
					goto out;
				}
				num_sge = 0;
				break;

			case IB_WR_REG_MR_INTEGRITY:
				qp->sq.wr_data[idx] = IB_WR_REG_MR_INTEGRITY;

				mr = to_mmr(reg_wr(wr)->mr);
				pi_mr = mr->pi_mr;

				if (pi_mr) {
					memset(&reg_pi_wr, 0,
					       sizeof(struct ib_reg_wr));

					reg_pi_wr.mr = &pi_mr->ibmr;
					reg_pi_wr.access = reg_wr(wr)->access;
					reg_pi_wr.key = pi_mr->ibmr.rkey;

					ctrl->imm = cpu_to_be32(reg_pi_wr.key);
					/* UMR for data + prot registration */
					err = set_reg_wr(qp, &reg_pi_wr, &seg,
							 &size, &cur_edge,
							 false);
					if (err) {
						*bad_wr = wr;
						goto out;
					}
					finish_wqe(qp, ctrl, seg, size,
						   cur_edge, idx, wr->wr_id,
						   nreq, fence,
						   MLX5_OPCODE_UMR);

					err = begin_wqe(qp, &seg, &ctrl, wr,
							&idx, &size, &cur_edge,
							nreq);
					if (err) {
						mlx5_ib_warn(dev, "\n");
						err = -ENOMEM;
						*bad_wr = wr;
						goto out;
					}
				} else {
					memset(&pa_pi_mr, 0,
					       sizeof(struct mlx5_ib_mr));
					/* No UMR, use local_dma_lkey */
					pa_pi_mr.ibmr.lkey =
						mr->ibmr.pd->local_dma_lkey;

					pa_pi_mr.ndescs = mr->ndescs;
					pa_pi_mr.data_length = mr->data_length;
					pa_pi_mr.data_iova = mr->data_iova;
					if (mr->meta_ndescs) {
						pa_pi_mr.meta_ndescs =
							mr->meta_ndescs;
						pa_pi_mr.meta_length =
							mr->meta_length;
						pa_pi_mr.pi_iova = mr->pi_iova;
					}

					pa_pi_mr.ibmr.length = mr->ibmr.length;
					mr->pi_mr = &pa_pi_mr;
				}
				ctrl->imm = cpu_to_be32(mr->ibmr.rkey);
				/* UMR for sig MR */
				err = set_pi_umr_wr(wr, qp, &seg, &size,
						    &cur_edge);
				if (err) {
					mlx5_ib_warn(dev, "\n");
					*bad_wr = wr;
					goto out;
				}
				finish_wqe(qp, ctrl, seg, size, cur_edge, idx,
					   wr->wr_id, nreq, fence,
					   MLX5_OPCODE_UMR);

				/*
				 * SET_PSV WQEs are not signaled and solicited
				 * on error
				 */
				sig_attrs = mr->ibmr.sig_attrs;
				err = __begin_wqe(qp, &seg, &ctrl, wr, &idx,
						  &size, &cur_edge, nreq, false,
						  true);
				if (err) {
					mlx5_ib_warn(dev, "\n");
					err = -ENOMEM;
					*bad_wr = wr;
					goto out;
				}
				err = set_psv_wr(&sig_attrs->mem,
						 mr->sig->psv_memory.psv_idx,
						 &seg, &size);
				if (err) {
					mlx5_ib_warn(dev, "\n");
					*bad_wr = wr;
					goto out;
				}
				finish_wqe(qp, ctrl, seg, size, cur_edge, idx,
					   wr->wr_id, nreq, next_fence,
					   MLX5_OPCODE_SET_PSV);

				err = __begin_wqe(qp, &seg, &ctrl, wr, &idx,
						  &size, &cur_edge, nreq, false,
						  true);
				if (err) {
					mlx5_ib_warn(dev, "\n");
					err = -ENOMEM;
					*bad_wr = wr;
					goto out;
				}
				err = set_psv_wr(&sig_attrs->wire,
						 mr->sig->psv_wire.psv_idx,
						 &seg, &size);
				if (err) {
					mlx5_ib_warn(dev, "\n");
					*bad_wr = wr;
					goto out;
				}
				finish_wqe(qp, ctrl, seg, size, cur_edge, idx,
					   wr->wr_id, nreq, next_fence,
					   MLX5_OPCODE_SET_PSV);

				qp->next_fence =
					MLX5_FENCE_MODE_INITIATOR_SMALL;
				num_sge = 0;
				goto skip_psv;

			default:
				break;
			}
			break;

		case IB_QPT_UC:
			switch (wr->opcode) {
			case IB_WR_RDMA_WRITE:
			case IB_WR_RDMA_WRITE_WITH_IMM:
				set_raddr_seg(seg, rdma_wr(wr)->remote_addr,
					      rdma_wr(wr)->rkey);
				seg  += sizeof(struct mlx5_wqe_raddr_seg);
				size += sizeof(struct mlx5_wqe_raddr_seg) / 16;
				break;

			default:
				break;
			}
			break;

		case IB_QPT_SMI:
			if (unlikely(!mdev->port_caps[qp->port - 1].has_smi)) {
				mlx5_ib_warn(dev, "Send SMP MADs is not allowed\n");
				err = -EPERM;
				*bad_wr = wr;
				goto out;
			}
			/* fall through */
		case MLX5_IB_QPT_HW_GSI:
			set_datagram_seg(seg, wr);
			seg += sizeof(struct mlx5_wqe_datagram_seg);
			size += sizeof(struct mlx5_wqe_datagram_seg) / 16;
			handle_post_send_edge(&qp->sq, &seg, size, &cur_edge);

			break;
		case IB_QPT_UD:
			set_datagram_seg(seg, wr);
			seg += sizeof(struct mlx5_wqe_datagram_seg);
			size += sizeof(struct mlx5_wqe_datagram_seg) / 16;
			handle_post_send_edge(&qp->sq, &seg, size, &cur_edge);

			/* handle qp that supports ud offload */
			if (qp->flags & IB_QP_CREATE_IPOIB_UD_LSO) {
				struct mlx5_wqe_eth_pad *pad;

				pad = seg;
				memset(pad, 0, sizeof(struct mlx5_wqe_eth_pad));
				seg += sizeof(struct mlx5_wqe_eth_pad);
				size += sizeof(struct mlx5_wqe_eth_pad) / 16;
				set_eth_seg(wr, qp, &seg, &size, &cur_edge);
				handle_post_send_edge(&qp->sq, &seg, size,
						      &cur_edge);
			}
			break;
		case MLX5_IB_QPT_REG_UMR:
			if (wr->opcode != MLX5_IB_WR_UMR) {
				err = -EINVAL;
				mlx5_ib_warn(dev, "bad opcode\n");
				goto out;
			}
			qp->sq.wr_data[idx] = MLX5_IB_WR_UMR;
			ctrl->imm = cpu_to_be32(umr_wr(wr)->mkey);
			err = set_reg_umr_segment(dev, seg, wr, !!(MLX5_CAP_GEN(mdev, atomic)));
			if (unlikely(err))
				goto out;
			seg += sizeof(struct mlx5_wqe_umr_ctrl_seg);
			size += sizeof(struct mlx5_wqe_umr_ctrl_seg) / 16;
			handle_post_send_edge(&qp->sq, &seg, size, &cur_edge);
			set_reg_mkey_segment(seg, wr);
			seg += sizeof(struct mlx5_mkey_seg);
			size += sizeof(struct mlx5_mkey_seg) / 16;
			handle_post_send_edge(&qp->sq, &seg, size, &cur_edge);
			break;

		default:
			break;
		}

		if (wr->send_flags & IB_SEND_INLINE && num_sge) {
			err = set_data_inl_seg(qp, wr, &seg, &size, &cur_edge);
			if (unlikely(err)) {
				mlx5_ib_warn(dev, "\n");
				*bad_wr = wr;
				goto out;
			}
		} else {
			for (i = 0; i < num_sge; i++) {
				handle_post_send_edge(&qp->sq, &seg, size,
						      &cur_edge);
				if (likely(wr->sg_list[i].length)) {
					set_data_ptr_seg
					((struct mlx5_wqe_data_seg *)seg,
					 wr->sg_list + i);
					size += sizeof(struct mlx5_wqe_data_seg) / 16;
					seg += sizeof(struct mlx5_wqe_data_seg);
				}
			}
		}

		qp->next_fence = next_fence;
		finish_wqe(qp, ctrl, seg, size, cur_edge, idx, wr->wr_id, nreq,
			   fence, mlx5_ib_opcode[wr->opcode]);
skip_psv:
		if (0)
			dump_wqe(qp, idx, size);
	}

out:
	if (likely(nreq)) {
		qp->sq.head += nreq;

		/* Make sure that descriptors are written before
		 * updating doorbell record and ringing the doorbell
		 */
		wmb();

		qp->db.db[MLX5_SND_DBR] = cpu_to_be32(qp->sq.cur_post);

		/* Make sure doorbell record is visible to the HCA before
		 * we hit doorbell */
		wmb();

		/* currently we support only regular doorbells */
		mlx5_write64((__be32 *)ctrl, bf->bfreg->map + bf->offset);
		/* Make sure doorbells don't leak out of SQ spinlock
		 * and reach the HCA out of order.
		 */
		bf->offset ^= bf->buf_size;
	}

	spin_unlock_irqrestore(&qp->sq.lock, flags);

	return err;
}

int mlx5_ib_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
		      const struct ib_send_wr **bad_wr)
{
	return _mlx5_ib_post_send(ibqp, wr, bad_wr, false);
}

static void set_sig_seg(struct mlx5_rwqe_sig *sig, int size)
{
	sig->signature = calc_sig(sig, size);
}

static int _mlx5_ib_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
		      const struct ib_recv_wr **bad_wr, bool drain)
{
	struct mlx5_ib_qp *qp = to_mqp(ibqp);
	struct mlx5_wqe_data_seg *scat;
	struct mlx5_rwqe_sig *sig;
	struct mlx5_ib_dev *dev = to_mdev(ibqp->device);
	struct mlx5_core_dev *mdev = dev->mdev;
	unsigned long flags;
	int err = 0;
	int nreq;
	int ind;
	int i;

	if (unlikely(mdev->state == MLX5_DEVICE_STATE_INTERNAL_ERROR &&
		     !drain)) {
		*bad_wr = wr;
		return -EIO;
	}

	if (unlikely(ibqp->qp_type == IB_QPT_GSI))
		return mlx5_ib_gsi_post_recv(ibqp, wr, bad_wr);

	spin_lock_irqsave(&qp->rq.lock, flags);

	ind = qp->rq.head & (qp->rq.wqe_cnt - 1);

	for (nreq = 0; wr; nreq++, wr = wr->next) {
		if (mlx5_wq_overflow(&qp->rq, nreq, qp->ibqp.recv_cq)) {
			err = -ENOMEM;
			*bad_wr = wr;
			goto out;
		}

		if (unlikely(wr->num_sge > qp->rq.max_gs)) {
			err = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		scat = mlx5_frag_buf_get_wqe(&qp->rq.fbc, ind);
		if (qp->wq_sig)
			scat++;

		for (i = 0; i < wr->num_sge; i++)
			set_data_ptr_seg(scat + i, wr->sg_list + i);

		if (i < qp->rq.max_gs) {
			scat[i].byte_count = 0;
			scat[i].lkey       = cpu_to_be32(MLX5_INVALID_LKEY);
			scat[i].addr       = 0;
		}

		if (qp->wq_sig) {
			sig = (struct mlx5_rwqe_sig *)scat;
			set_sig_seg(sig, (qp->rq.max_gs + 1) << 2);
		}

		qp->rq.wrid[ind] = wr->wr_id;

		ind = (ind + 1) & (qp->rq.wqe_cnt - 1);
	}

out:
	if (likely(nreq)) {
		qp->rq.head += nreq;

		/* Make sure that descriptors are written before
		 * doorbell record.
		 */
		wmb();

		*qp->db.db = cpu_to_be32(qp->rq.head & 0xffff);
	}

	spin_unlock_irqrestore(&qp->rq.lock, flags);

	return err;
}

int mlx5_ib_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
		      const struct ib_recv_wr **bad_wr)
{
	return _mlx5_ib_post_recv(ibqp, wr, bad_wr, false);
}

static inline enum ib_qp_state to_ib_qp_state(enum mlx5_qp_state mlx5_state)
{
	switch (mlx5_state) {
	case MLX5_QP_STATE_RST:      return IB_QPS_RESET;
	case MLX5_QP_STATE_INIT:     return IB_QPS_INIT;
	case MLX5_QP_STATE_RTR:      return IB_QPS_RTR;
	case MLX5_QP_STATE_RTS:      return IB_QPS_RTS;
	case MLX5_QP_STATE_SQ_DRAINING:
	case MLX5_QP_STATE_SQD:      return IB_QPS_SQD;
	case MLX5_QP_STATE_SQER:     return IB_QPS_SQE;
	case MLX5_QP_STATE_ERR:      return IB_QPS_ERR;
	default:		     return -1;
	}
}

static inline enum ib_mig_state to_ib_mig_state(int mlx5_mig_state)
{
	switch (mlx5_mig_state) {
	case MLX5_QP_PM_ARMED:		return IB_MIG_ARMED;
	case MLX5_QP_PM_REARM:		return IB_MIG_REARM;
	case MLX5_QP_PM_MIGRATED:	return IB_MIG_MIGRATED;
	default: return -1;
	}
}

static int to_ib_qp_access_flags(int mlx5_flags)
{
	int ib_flags = 0;

	if (mlx5_flags & MLX5_QP_BIT_RRE)
		ib_flags |= IB_ACCESS_REMOTE_READ;
	if (mlx5_flags & MLX5_QP_BIT_RWE)
		ib_flags |= IB_ACCESS_REMOTE_WRITE;
	if (mlx5_flags & MLX5_QP_BIT_RAE)
		ib_flags |= IB_ACCESS_REMOTE_ATOMIC;

	return ib_flags;
}

static void to_rdma_ah_attr(struct mlx5_ib_dev *ibdev,
			    struct rdma_ah_attr *ah_attr,
			    struct mlx5_qp_path *path)
{

	memset(ah_attr, 0, sizeof(*ah_attr));

	if (!path->port || path->port > ibdev->num_ports)
		return;

	ah_attr->type = rdma_ah_find_type(&ibdev->ib_dev, path->port);

	rdma_ah_set_port_num(ah_attr, path->port);
	rdma_ah_set_sl(ah_attr, path->dci_cfi_prio_sl & 0xf);

	rdma_ah_set_dlid(ah_attr, be16_to_cpu(path->rlid));
	rdma_ah_set_path_bits(ah_attr, path->grh_mlid & 0x7f);
	rdma_ah_set_static_rate(ah_attr,
				path->static_rate ? path->static_rate - 5 : 0);

	if (path->grh_mlid & (1 << 7) ||
	    ah_attr->type == RDMA_AH_ATTR_TYPE_ROCE) {
		u32 tc_fl = be32_to_cpu(path->tclass_flowlabel);

		rdma_ah_set_grh(ah_attr, NULL,
				tc_fl & 0xfffff,
				path->mgid_index,
				path->hop_limit,
				(tc_fl >> 20) & 0xff);
		rdma_ah_set_dgid_raw(ah_attr, path->rgid);
	}
}

static int query_raw_packet_qp_sq_state(struct mlx5_ib_dev *dev,
					struct mlx5_ib_sq *sq,
					u8 *sq_state)
{
	int err;

	err = mlx5_core_query_sq_state(dev->mdev, sq->base.mqp.qpn, sq_state);
	if (err)
		goto out;
	sq->state = *sq_state;

out:
	return err;
}

static int query_raw_packet_qp_rq_state(struct mlx5_ib_dev *dev,
					struct mlx5_ib_rq *rq,
					u8 *rq_state)
{
	void *out;
	void *rqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(query_rq_out);
	out = kvzalloc(inlen, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	err = mlx5_core_query_rq(dev->mdev, rq->base.mqp.qpn, out);
	if (err)
		goto out;

	rqc = MLX5_ADDR_OF(query_rq_out, out, rq_context);
	*rq_state = MLX5_GET(rqc, rqc, state);
	rq->state = *rq_state;

out:
	kvfree(out);
	return err;
}

static int sqrq_state_to_qp_state(u8 sq_state, u8 rq_state,
				  struct mlx5_ib_qp *qp, u8 *qp_state)
{
	static const u8 sqrq_trans[MLX5_RQ_NUM_STATE][MLX5_SQ_NUM_STATE] = {
		[MLX5_RQC_STATE_RST] = {
			[MLX5_SQC_STATE_RST]	= IB_QPS_RESET,
			[MLX5_SQC_STATE_RDY]	= MLX5_QP_STATE_BAD,
			[MLX5_SQC_STATE_ERR]	= MLX5_QP_STATE_BAD,
			[MLX5_SQ_STATE_NA]	= IB_QPS_RESET,
		},
		[MLX5_RQC_STATE_RDY] = {
			[MLX5_SQC_STATE_RST]	= MLX5_QP_STATE_BAD,
			[MLX5_SQC_STATE_RDY]	= MLX5_QP_STATE,
			[MLX5_SQC_STATE_ERR]	= IB_QPS_SQE,
			[MLX5_SQ_STATE_NA]	= MLX5_QP_STATE,
		},
		[MLX5_RQC_STATE_ERR] = {
			[MLX5_SQC_STATE_RST]    = MLX5_QP_STATE_BAD,
			[MLX5_SQC_STATE_RDY]	= MLX5_QP_STATE_BAD,
			[MLX5_SQC_STATE_ERR]	= IB_QPS_ERR,
			[MLX5_SQ_STATE_NA]	= IB_QPS_ERR,
		},
		[MLX5_RQ_STATE_NA] = {
			[MLX5_SQC_STATE_RST]    = IB_QPS_RESET,
			[MLX5_SQC_STATE_RDY]	= MLX5_QP_STATE,
			[MLX5_SQC_STATE_ERR]	= MLX5_QP_STATE,
			[MLX5_SQ_STATE_NA]	= MLX5_QP_STATE_BAD,
		},
	};

	*qp_state = sqrq_trans[rq_state][sq_state];

	if (*qp_state == MLX5_QP_STATE_BAD) {
		WARN(1, "Buggy Raw Packet QP state, SQ 0x%x state: 0x%x, RQ 0x%x state: 0x%x",
		     qp->raw_packet_qp.sq.base.mqp.qpn, sq_state,
		     qp->raw_packet_qp.rq.base.mqp.qpn, rq_state);
		return -EINVAL;
	}

	if (*qp_state == MLX5_QP_STATE)
		*qp_state = qp->state;

	return 0;
}

static int query_raw_packet_qp_state(struct mlx5_ib_dev *dev,
				     struct mlx5_ib_qp *qp,
				     u8 *raw_packet_qp_state)
{
	struct mlx5_ib_raw_packet_qp *raw_packet_qp = &qp->raw_packet_qp;
	struct mlx5_ib_sq *sq = &raw_packet_qp->sq;
	struct mlx5_ib_rq *rq = &raw_packet_qp->rq;
	int err;
	u8 sq_state = MLX5_SQ_STATE_NA;
	u8 rq_state = MLX5_RQ_STATE_NA;

	if (qp->sq.wqe_cnt) {
		err = query_raw_packet_qp_sq_state(dev, sq, &sq_state);
		if (err)
			return err;
	}

	if (qp->rq.wqe_cnt) {
		err = query_raw_packet_qp_rq_state(dev, rq, &rq_state);
		if (err)
			return err;
	}

	return sqrq_state_to_qp_state(sq_state, rq_state, qp,
				      raw_packet_qp_state);
}

static int query_qp_attr(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *qp,
			 struct ib_qp_attr *qp_attr)
{
	int outlen = MLX5_ST_SZ_BYTES(query_qp_out);
	struct mlx5_qp_context *context;
	int mlx5_state;
	u32 *outb;
	int err = 0;

	outb = kzalloc(outlen, GFP_KERNEL);
	if (!outb)
		return -ENOMEM;

	err = mlx5_core_qp_query(dev->mdev, &qp->trans_qp.base.mqp, outb,
				 outlen);
	if (err)
		goto out;

	/* FIXME: use MLX5_GET rather than mlx5_qp_context manual struct */
	context = (struct mlx5_qp_context *)MLX5_ADDR_OF(query_qp_out, outb, qpc);

	mlx5_state = be32_to_cpu(context->flags) >> 28;

	qp->state		     = to_ib_qp_state(mlx5_state);
	qp_attr->path_mtu	     = context->mtu_msgmax >> 5;
	qp_attr->path_mig_state	     =
		to_ib_mig_state((be32_to_cpu(context->flags) >> 11) & 0x3);
	qp_attr->qkey		     = be32_to_cpu(context->qkey);
	qp_attr->rq_psn		     = be32_to_cpu(context->rnr_nextrecvpsn) & 0xffffff;
	qp_attr->sq_psn		     = be32_to_cpu(context->next_send_psn) & 0xffffff;
	qp_attr->dest_qp_num	     = be32_to_cpu(context->log_pg_sz_remote_qpn) & 0xffffff;
	qp_attr->qp_access_flags     =
		to_ib_qp_access_flags(be32_to_cpu(context->params2));

	if (qp->ibqp.qp_type == IB_QPT_RC || qp->ibqp.qp_type == IB_QPT_UC) {
		to_rdma_ah_attr(dev, &qp_attr->ah_attr, &context->pri_path);
		to_rdma_ah_attr(dev, &qp_attr->alt_ah_attr, &context->alt_path);
		qp_attr->alt_pkey_index =
			be16_to_cpu(context->alt_path.pkey_index);
		qp_attr->alt_port_num	=
			rdma_ah_get_port_num(&qp_attr->alt_ah_attr);
	}

	qp_attr->pkey_index = be16_to_cpu(context->pri_path.pkey_index);
	qp_attr->port_num = context->pri_path.port;

	/* qp_attr->en_sqd_async_notify is only applicable in modify qp */
	qp_attr->sq_draining = mlx5_state == MLX5_QP_STATE_SQ_DRAINING;

	qp_attr->max_rd_atomic = 1 << ((be32_to_cpu(context->params1) >> 21) & 0x7);

	qp_attr->max_dest_rd_atomic =
		1 << ((be32_to_cpu(context->params2) >> 21) & 0x7);
	qp_attr->min_rnr_timer	    =
		(be32_to_cpu(context->rnr_nextrecvpsn) >> 24) & 0x1f;
	qp_attr->timeout	    = context->pri_path.ackto_lt >> 3;
	qp_attr->retry_cnt	    = (be32_to_cpu(context->params1) >> 16) & 0x7;
	qp_attr->rnr_retry	    = (be32_to_cpu(context->params1) >> 13) & 0x7;
	qp_attr->alt_timeout	    = context->alt_path.ackto_lt >> 3;

out:
	kfree(outb);
	return err;
}

static int mlx5_ib_dct_query_qp(struct mlx5_ib_dev *dev, struct mlx5_ib_qp *mqp,
				struct ib_qp_attr *qp_attr, int qp_attr_mask,
				struct ib_qp_init_attr *qp_init_attr)
{
	struct mlx5_core_dct	*dct = &mqp->dct.mdct;
	u32 *out;
	u32 access_flags = 0;
	int outlen = MLX5_ST_SZ_BYTES(query_dct_out);
	void *dctc;
	int err;
	int supported_mask = IB_QP_STATE |
			     IB_QP_ACCESS_FLAGS |
			     IB_QP_PORT |
			     IB_QP_MIN_RNR_TIMER |
			     IB_QP_AV |
			     IB_QP_PATH_MTU |
			     IB_QP_PKEY_INDEX;

	if (qp_attr_mask & ~supported_mask)
		return -EINVAL;
	if (mqp->state != IB_QPS_RTR)
		return -EINVAL;

	out = kzalloc(outlen, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	err = mlx5_core_dct_query(dev->mdev, dct, out, outlen);
	if (err)
		goto out;

	dctc = MLX5_ADDR_OF(query_dct_out, out, dct_context_entry);

	if (qp_attr_mask & IB_QP_STATE)
		qp_attr->qp_state = IB_QPS_RTR;

	if (qp_attr_mask & IB_QP_ACCESS_FLAGS) {
		if (MLX5_GET(dctc, dctc, rre))
			access_flags |= IB_ACCESS_REMOTE_READ;
		if (MLX5_GET(dctc, dctc, rwe))
			access_flags |= IB_ACCESS_REMOTE_WRITE;
		if (MLX5_GET(dctc, dctc, rae))
			access_flags |= IB_ACCESS_REMOTE_ATOMIC;
		qp_attr->qp_access_flags = access_flags;
	}

	if (qp_attr_mask & IB_QP_PORT)
		qp_attr->port_num = MLX5_GET(dctc, dctc, port);
	if (qp_attr_mask & IB_QP_MIN_RNR_TIMER)
		qp_attr->min_rnr_timer = MLX5_GET(dctc, dctc, min_rnr_nak);
	if (qp_attr_mask & IB_QP_AV) {
		qp_attr->ah_attr.grh.traffic_class = MLX5_GET(dctc, dctc, tclass);
		qp_attr->ah_attr.grh.flow_label = MLX5_GET(dctc, dctc, flow_label);
		qp_attr->ah_attr.grh.sgid_index = MLX5_GET(dctc, dctc, my_addr_index);
		qp_attr->ah_attr.grh.hop_limit = MLX5_GET(dctc, dctc, hop_limit);
	}
	if (qp_attr_mask & IB_QP_PATH_MTU)
		qp_attr->path_mtu = MLX5_GET(dctc, dctc, mtu);
	if (qp_attr_mask & IB_QP_PKEY_INDEX)
		qp_attr->pkey_index = MLX5_GET(dctc, dctc, pkey_index);
out:
	kfree(out);
	return err;
}

int mlx5_ib_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *qp_attr,
		     int qp_attr_mask, struct ib_qp_init_attr *qp_init_attr)
{
	struct mlx5_ib_dev *dev = to_mdev(ibqp->device);
	struct mlx5_ib_qp *qp = to_mqp(ibqp);
	int err = 0;
	u8 raw_packet_qp_state;

	if (ibqp->rwq_ind_tbl)
		return -ENOSYS;

	if (unlikely(ibqp->qp_type == IB_QPT_GSI))
		return mlx5_ib_gsi_query_qp(ibqp, qp_attr, qp_attr_mask,
					    qp_init_attr);

	/* Not all of output fields are applicable, make sure to zero them */
	memset(qp_init_attr, 0, sizeof(*qp_init_attr));
	memset(qp_attr, 0, sizeof(*qp_attr));

	if (unlikely(qp->qp_sub_type == MLX5_IB_QPT_DCT))
		return mlx5_ib_dct_query_qp(dev, qp, qp_attr,
					    qp_attr_mask, qp_init_attr);

	mutex_lock(&qp->mutex);

	if (qp->ibqp.qp_type == IB_QPT_RAW_PACKET ||
	    qp->flags & MLX5_IB_QP_UNDERLAY) {
		err = query_raw_packet_qp_state(dev, qp, &raw_packet_qp_state);
		if (err)
			goto out;
		qp->state = raw_packet_qp_state;
		qp_attr->port_num = 1;
	} else {
		err = query_qp_attr(dev, qp, qp_attr);
		if (err)
			goto out;
	}

	qp_attr->qp_state	     = qp->state;
	qp_attr->cur_qp_state	     = qp_attr->qp_state;
	qp_attr->cap.max_recv_wr     = qp->rq.wqe_cnt;
	qp_attr->cap.max_recv_sge    = qp->rq.max_gs;

	if (!ibqp->uobject) {
		qp_attr->cap.max_send_wr  = qp->sq.max_post;
		qp_attr->cap.max_send_sge = qp->sq.max_gs;
		qp_init_attr->qp_context = ibqp->qp_context;
	} else {
		qp_attr->cap.max_send_wr  = 0;
		qp_attr->cap.max_send_sge = 0;
	}

	qp_init_attr->qp_type = ibqp->qp_type;
	qp_init_attr->recv_cq = ibqp->recv_cq;
	qp_init_attr->send_cq = ibqp->send_cq;
	qp_init_attr->srq = ibqp->srq;
	qp_attr->cap.max_inline_data = qp->max_inline_data;

	qp_init_attr->cap	     = qp_attr->cap;

	qp_init_attr->create_flags = 0;
	if (qp->flags & MLX5_IB_QP_BLOCK_MULTICAST_LOOPBACK)
		qp_init_attr->create_flags |= IB_QP_CREATE_BLOCK_MULTICAST_LOOPBACK;

	if (qp->flags & MLX5_IB_QP_CROSS_CHANNEL)
		qp_init_attr->create_flags |= IB_QP_CREATE_CROSS_CHANNEL;
	if (qp->flags & MLX5_IB_QP_MANAGED_SEND)
		qp_init_attr->create_flags |= IB_QP_CREATE_MANAGED_SEND;
	if (qp->flags & MLX5_IB_QP_MANAGED_RECV)
		qp_init_attr->create_flags |= IB_QP_CREATE_MANAGED_RECV;
	if (qp->flags & MLX5_IB_QP_SQPN_QP1)
		qp_init_attr->create_flags |= mlx5_ib_create_qp_sqpn_qp1();

	qp_init_attr->sq_sig_type = qp->sq_signal_bits & MLX5_WQE_CTRL_CQ_UPDATE ?
		IB_SIGNAL_ALL_WR : IB_SIGNAL_REQ_WR;

out:
	mutex_unlock(&qp->mutex);
	return err;
}

struct ib_xrcd *mlx5_ib_alloc_xrcd(struct ib_device *ibdev,
				   struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev = to_mdev(ibdev);
	struct mlx5_ib_xrcd *xrcd;
	int err;

	if (!MLX5_CAP_GEN(dev->mdev, xrc))
		return ERR_PTR(-ENOSYS);

	xrcd = kmalloc(sizeof(*xrcd), GFP_KERNEL);
	if (!xrcd)
		return ERR_PTR(-ENOMEM);

	err = mlx5_cmd_xrcd_alloc(dev->mdev, &xrcd->xrcdn, 0);
	if (err) {
		kfree(xrcd);
		return ERR_PTR(-ENOMEM);
	}

	return &xrcd->ibxrcd;
}

int mlx5_ib_dealloc_xrcd(struct ib_xrcd *xrcd, struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev = to_mdev(xrcd->device);
	u32 xrcdn = to_mxrcd(xrcd)->xrcdn;
	int err;

	err = mlx5_cmd_xrcd_dealloc(dev->mdev, xrcdn, 0);
	if (err)
		mlx5_ib_warn(dev, "failed to dealloc xrcdn 0x%x\n", xrcdn);

	kfree(xrcd);
	return 0;
}

static void mlx5_ib_wq_event(struct mlx5_core_qp *core_qp, int type)
{
	struct mlx5_ib_rwq *rwq = to_mibrwq(core_qp);
	struct mlx5_ib_dev *dev = to_mdev(rwq->ibwq.device);
	struct ib_event event;

	if (rwq->ibwq.event_handler) {
		event.device     = rwq->ibwq.device;
		event.element.wq = &rwq->ibwq;
		switch (type) {
		case MLX5_EVENT_TYPE_WQ_CATAS_ERROR:
			event.event = IB_EVENT_WQ_FATAL;
			break;
		default:
			mlx5_ib_warn(dev, "Unexpected event type %d on WQ %06x\n", type, core_qp->qpn);
			return;
		}

		rwq->ibwq.event_handler(&event, rwq->ibwq.wq_context);
	}
}

static int set_delay_drop(struct mlx5_ib_dev *dev)
{
	int err = 0;

	mutex_lock(&dev->delay_drop.lock);
	if (dev->delay_drop.activate)
		goto out;

	err = mlx5_core_set_delay_drop(dev->mdev, dev->delay_drop.timeout);
	if (err)
		goto out;

	dev->delay_drop.activate = true;
out:
	mutex_unlock(&dev->delay_drop.lock);

	if (!err)
		atomic_inc(&dev->delay_drop.rqs_cnt);
	return err;
}

static int  create_rq(struct mlx5_ib_rwq *rwq, struct ib_pd *pd,
		      struct ib_wq_init_attr *init_attr)
{
	struct mlx5_ib_dev *dev;
	int has_net_offloads;
	__be64 *rq_pas0;
	void *in;
	void *rqc;
	void *wq;
	int inlen;
	int err;

	dev = to_mdev(pd->device);

	inlen = MLX5_ST_SZ_BYTES(create_rq_in) + sizeof(u64) * rwq->rq_num_pas;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	MLX5_SET(create_rq_in, in, uid, to_mpd(pd)->uid);
	rqc = MLX5_ADDR_OF(create_rq_in, in, ctx);
	MLX5_SET(rqc,  rqc, mem_rq_type,
		 MLX5_RQC_MEM_RQ_TYPE_MEMORY_RQ_INLINE);
	MLX5_SET(rqc, rqc, user_index, rwq->user_index);
	MLX5_SET(rqc,  rqc, cqn, to_mcq(init_attr->cq)->mcq.cqn);
	MLX5_SET(rqc,  rqc, state, MLX5_RQC_STATE_RST);
	MLX5_SET(rqc,  rqc, flush_in_error_en, 1);
	wq = MLX5_ADDR_OF(rqc, rqc, wq);
	MLX5_SET(wq, wq, wq_type,
		 rwq->create_flags & MLX5_IB_WQ_FLAGS_STRIDING_RQ ?
		 MLX5_WQ_TYPE_CYCLIC_STRIDING_RQ : MLX5_WQ_TYPE_CYCLIC);
	if (init_attr->create_flags & IB_WQ_FLAGS_PCI_WRITE_END_PADDING) {
		if (!MLX5_CAP_GEN(dev->mdev, end_pad)) {
			mlx5_ib_dbg(dev, "Scatter end padding is not supported\n");
			err = -EOPNOTSUPP;
			goto out;
		} else {
			MLX5_SET(wq, wq, end_padding_mode, MLX5_WQ_END_PAD_MODE_ALIGN);
		}
	}
	MLX5_SET(wq, wq, log_wq_stride, rwq->log_rq_stride);
	if (rwq->create_flags & MLX5_IB_WQ_FLAGS_STRIDING_RQ) {
		MLX5_SET(wq, wq, two_byte_shift_en, rwq->two_byte_shift_en);
		MLX5_SET(wq, wq, log_wqe_stride_size,
			 rwq->single_stride_log_num_of_bytes -
			 MLX5_MIN_SINGLE_STRIDE_LOG_NUM_BYTES);
		MLX5_SET(wq, wq, log_wqe_num_of_strides, rwq->log_num_strides -
			 MLX5_MIN_SINGLE_WQE_LOG_NUM_STRIDES);
	}
	MLX5_SET(wq, wq, log_wq_sz, rwq->log_rq_size);
	MLX5_SET(wq, wq, pd, to_mpd(pd)->pdn);
	MLX5_SET(wq, wq, page_offset, rwq->rq_page_offset);
	MLX5_SET(wq, wq, log_wq_pg_sz, rwq->log_page_size);
	MLX5_SET(wq, wq, wq_signature, rwq->wq_sig);
	MLX5_SET64(wq, wq, dbr_addr, rwq->db.dma);
	has_net_offloads = MLX5_CAP_GEN(dev->mdev, eth_net_offloads);
	if (init_attr->create_flags & IB_WQ_FLAGS_CVLAN_STRIPPING) {
		if (!(has_net_offloads && MLX5_CAP_ETH(dev->mdev, vlan_cap))) {
			mlx5_ib_dbg(dev, "VLAN offloads are not supported\n");
			err = -EOPNOTSUPP;
			goto out;
		}
	} else {
		MLX5_SET(rqc, rqc, vsd, 1);
	}
	if (init_attr->create_flags & IB_WQ_FLAGS_SCATTER_FCS) {
		if (!(has_net_offloads && MLX5_CAP_ETH(dev->mdev, scatter_fcs))) {
			mlx5_ib_dbg(dev, "Scatter FCS is not supported\n");
			err = -EOPNOTSUPP;
			goto out;
		}
		MLX5_SET(rqc, rqc, scatter_fcs, 1);
	}
	if (init_attr->create_flags & IB_WQ_FLAGS_DELAY_DROP) {
		if (!(dev->ib_dev.attrs.raw_packet_caps &
		      IB_RAW_PACKET_CAP_DELAY_DROP)) {
			mlx5_ib_dbg(dev, "Delay drop is not supported\n");
			err = -EOPNOTSUPP;
			goto out;
		}
		MLX5_SET(rqc, rqc, delay_drop_en, 1);
	}
	rq_pas0 = (__be64 *)MLX5_ADDR_OF(wq, wq, pas);
	mlx5_ib_populate_pas(dev, rwq->umem, rwq->page_shift, rq_pas0, 0);
	err = mlx5_core_create_rq_tracked(dev->mdev, in, inlen, &rwq->core_qp);
	if (!err && init_attr->create_flags & IB_WQ_FLAGS_DELAY_DROP) {
		err = set_delay_drop(dev);
		if (err) {
			mlx5_ib_warn(dev, "Failed to enable delay drop err=%d\n",
				     err);
			mlx5_core_destroy_rq_tracked(dev->mdev, &rwq->core_qp);
		} else {
			rwq->create_flags |= MLX5_IB_WQ_FLAGS_DELAY_DROP;
		}
	}
out:
	kvfree(in);
	return err;
}

static int set_user_rq_size(struct mlx5_ib_dev *dev,
			    struct ib_wq_init_attr *wq_init_attr,
			    struct mlx5_ib_create_wq *ucmd,
			    struct mlx5_ib_rwq *rwq)
{
	/* Sanity check RQ size before proceeding */
	if (wq_init_attr->max_wr > (1 << MLX5_CAP_GEN(dev->mdev, log_max_wq_sz)))
		return -EINVAL;

	if (!ucmd->rq_wqe_count)
		return -EINVAL;

	rwq->wqe_count = ucmd->rq_wqe_count;
	rwq->wqe_shift = ucmd->rq_wqe_shift;
	if (check_shl_overflow(rwq->wqe_count, rwq->wqe_shift, &rwq->buf_size))
		return -EINVAL;

	rwq->log_rq_stride = rwq->wqe_shift;
	rwq->log_rq_size = ilog2(rwq->wqe_count);
	return 0;
}

static int prepare_user_rq(struct ib_pd *pd,
			   struct ib_wq_init_attr *init_attr,
			   struct ib_udata *udata,
			   struct mlx5_ib_rwq *rwq)
{
	struct mlx5_ib_dev *dev = to_mdev(pd->device);
	struct mlx5_ib_create_wq ucmd = {};
	int err;
	size_t required_cmd_sz;

	required_cmd_sz = offsetof(typeof(ucmd), single_stride_log_num_of_bytes)
		+ sizeof(ucmd.single_stride_log_num_of_bytes);
	if (udata->inlen < required_cmd_sz) {
		mlx5_ib_dbg(dev, "invalid inlen\n");
		return -EINVAL;
	}

	if (udata->inlen > sizeof(ucmd) &&
	    !ib_is_udata_cleared(udata, sizeof(ucmd),
				 udata->inlen - sizeof(ucmd))) {
		mlx5_ib_dbg(dev, "inlen is not supported\n");
		return -EOPNOTSUPP;
	}

	if (ib_copy_from_udata(&ucmd, udata, min(sizeof(ucmd), udata->inlen))) {
		mlx5_ib_dbg(dev, "copy failed\n");
		return -EFAULT;
	}

	if (ucmd.comp_mask & (~MLX5_IB_CREATE_WQ_STRIDING_RQ)) {
		mlx5_ib_dbg(dev, "invalid comp mask\n");
		return -EOPNOTSUPP;
	} else if (ucmd.comp_mask & MLX5_IB_CREATE_WQ_STRIDING_RQ) {
		if (!MLX5_CAP_GEN(dev->mdev, striding_rq)) {
			mlx5_ib_dbg(dev, "Striding RQ is not supported\n");
			return -EOPNOTSUPP;
		}
		if ((ucmd.single_stride_log_num_of_bytes <
		    MLX5_MIN_SINGLE_STRIDE_LOG_NUM_BYTES) ||
		    (ucmd.single_stride_log_num_of_bytes >
		     MLX5_MAX_SINGLE_STRIDE_LOG_NUM_BYTES)) {
			mlx5_ib_dbg(dev, "Invalid log stride size (%u. Range is %u - %u)\n",
				    ucmd.single_stride_log_num_of_bytes,
				    MLX5_MIN_SINGLE_STRIDE_LOG_NUM_BYTES,
				    MLX5_MAX_SINGLE_STRIDE_LOG_NUM_BYTES);
			return -EINVAL;
		}
		if ((ucmd.single_wqe_log_num_of_strides >
		    MLX5_MAX_SINGLE_WQE_LOG_NUM_STRIDES) ||
		     (ucmd.single_wqe_log_num_of_strides <
			MLX5_MIN_SINGLE_WQE_LOG_NUM_STRIDES)) {
			mlx5_ib_dbg(dev, "Invalid log num strides (%u. Range is %u - %u)\n",
				    ucmd.single_wqe_log_num_of_strides,
				    MLX5_MIN_SINGLE_WQE_LOG_NUM_STRIDES,
				    MLX5_MAX_SINGLE_WQE_LOG_NUM_STRIDES);
			return -EINVAL;
		}
		rwq->single_stride_log_num_of_bytes =
			ucmd.single_stride_log_num_of_bytes;
		rwq->log_num_strides = ucmd.single_wqe_log_num_of_strides;
		rwq->two_byte_shift_en = !!ucmd.two_byte_shift_en;
		rwq->create_flags |= MLX5_IB_WQ_FLAGS_STRIDING_RQ;
	}

	err = set_user_rq_size(dev, init_attr, &ucmd, rwq);
	if (err) {
		mlx5_ib_dbg(dev, "err %d\n", err);
		return err;
	}

	err = create_user_rq(dev, pd, udata, rwq, &ucmd);
	if (err) {
		mlx5_ib_dbg(dev, "err %d\n", err);
		return err;
	}

	rwq->user_index = ucmd.user_index;
	return 0;
}

struct ib_wq *mlx5_ib_create_wq(struct ib_pd *pd,
				struct ib_wq_init_attr *init_attr,
				struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev;
	struct mlx5_ib_rwq *rwq;
	struct mlx5_ib_create_wq_resp resp = {};
	size_t min_resp_len;
	int err;

	if (!udata)
		return ERR_PTR(-ENOSYS);

	min_resp_len = offsetof(typeof(resp), reserved) + sizeof(resp.reserved);
	if (udata->outlen && udata->outlen < min_resp_len)
		return ERR_PTR(-EINVAL);

	if (!capable(CAP_SYS_RAWIO) &&
	    init_attr->create_flags & IB_WQ_FLAGS_DELAY_DROP)
		return ERR_PTR(-EPERM);

	dev = to_mdev(pd->device);
	switch (init_attr->wq_type) {
	case IB_WQT_RQ:
		rwq = kzalloc(sizeof(*rwq), GFP_KERNEL);
		if (!rwq)
			return ERR_PTR(-ENOMEM);
		err = prepare_user_rq(pd, init_attr, udata, rwq);
		if (err)
			goto err;
		err = create_rq(rwq, pd, init_attr);
		if (err)
			goto err_user_rq;
		break;
	default:
		mlx5_ib_dbg(dev, "unsupported wq type %d\n",
			    init_attr->wq_type);
		return ERR_PTR(-EINVAL);
	}

	rwq->ibwq.wq_num = rwq->core_qp.qpn;
	rwq->ibwq.state = IB_WQS_RESET;
	if (udata->outlen) {
		resp.response_length = offsetof(typeof(resp), response_length) +
				sizeof(resp.response_length);
		err = ib_copy_to_udata(udata, &resp, resp.response_length);
		if (err)
			goto err_copy;
	}

	rwq->core_qp.event = mlx5_ib_wq_event;
	rwq->ibwq.event_handler = init_attr->event_handler;
	return &rwq->ibwq;

err_copy:
	mlx5_core_destroy_rq_tracked(dev->mdev, &rwq->core_qp);
err_user_rq:
	destroy_user_rq(dev, pd, rwq, udata);
err:
	kfree(rwq);
	return ERR_PTR(err);
}

void mlx5_ib_destroy_wq(struct ib_wq *wq, struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev = to_mdev(wq->device);
	struct mlx5_ib_rwq *rwq = to_mrwq(wq);

	mlx5_core_destroy_rq_tracked(dev->mdev, &rwq->core_qp);
	destroy_user_rq(dev, wq->pd, rwq, udata);
	kfree(rwq);
}

struct ib_rwq_ind_table *mlx5_ib_create_rwq_ind_table(struct ib_device *device,
						      struct ib_rwq_ind_table_init_attr *init_attr,
						      struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev = to_mdev(device);
	struct mlx5_ib_rwq_ind_table *rwq_ind_tbl;
	int sz = 1 << init_attr->log_ind_tbl_size;
	struct mlx5_ib_create_rwq_ind_tbl_resp resp = {};
	size_t min_resp_len;
	int inlen;
	int err;
	int i;
	u32 *in;
	void *rqtc;

	if (udata->inlen > 0 &&
	    !ib_is_udata_cleared(udata, 0,
				 udata->inlen))
		return ERR_PTR(-EOPNOTSUPP);

	if (init_attr->log_ind_tbl_size >
	    MLX5_CAP_GEN(dev->mdev, log_max_rqt_size)) {
		mlx5_ib_dbg(dev, "log_ind_tbl_size = %d is bigger than supported = %d\n",
			    init_attr->log_ind_tbl_size,
			    MLX5_CAP_GEN(dev->mdev, log_max_rqt_size));
		return ERR_PTR(-EINVAL);
	}

	min_resp_len = offsetof(typeof(resp), reserved) + sizeof(resp.reserved);
	if (udata->outlen && udata->outlen < min_resp_len)
		return ERR_PTR(-EINVAL);

	rwq_ind_tbl = kzalloc(sizeof(*rwq_ind_tbl), GFP_KERNEL);
	if (!rwq_ind_tbl)
		return ERR_PTR(-ENOMEM);

	inlen = MLX5_ST_SZ_BYTES(create_rqt_in) + sizeof(u32) * sz;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in) {
		err = -ENOMEM;
		goto err;
	}

	rqtc = MLX5_ADDR_OF(create_rqt_in, in, rqt_context);

	MLX5_SET(rqtc, rqtc, rqt_actual_size, sz);
	MLX5_SET(rqtc, rqtc, rqt_max_size, sz);

	for (i = 0; i < sz; i++)
		MLX5_SET(rqtc, rqtc, rq_num[i], init_attr->ind_tbl[i]->wq_num);

	rwq_ind_tbl->uid = to_mpd(init_attr->ind_tbl[0]->pd)->uid;
	MLX5_SET(create_rqt_in, in, uid, rwq_ind_tbl->uid);

	err = mlx5_core_create_rqt(dev->mdev, in, inlen, &rwq_ind_tbl->rqtn);
	kvfree(in);

	if (err)
		goto err;

	rwq_ind_tbl->ib_rwq_ind_tbl.ind_tbl_num = rwq_ind_tbl->rqtn;
	if (udata->outlen) {
		resp.response_length = offsetof(typeof(resp), response_length) +
					sizeof(resp.response_length);
		err = ib_copy_to_udata(udata, &resp, resp.response_length);
		if (err)
			goto err_copy;
	}

	return &rwq_ind_tbl->ib_rwq_ind_tbl;

err_copy:
	mlx5_cmd_destroy_rqt(dev->mdev, rwq_ind_tbl->rqtn, rwq_ind_tbl->uid);
err:
	kfree(rwq_ind_tbl);
	return ERR_PTR(err);
}

int mlx5_ib_destroy_rwq_ind_table(struct ib_rwq_ind_table *ib_rwq_ind_tbl)
{
	struct mlx5_ib_rwq_ind_table *rwq_ind_tbl = to_mrwq_ind_table(ib_rwq_ind_tbl);
	struct mlx5_ib_dev *dev = to_mdev(ib_rwq_ind_tbl->device);

	mlx5_cmd_destroy_rqt(dev->mdev, rwq_ind_tbl->rqtn, rwq_ind_tbl->uid);

	kfree(rwq_ind_tbl);
	return 0;
}

int mlx5_ib_modify_wq(struct ib_wq *wq, struct ib_wq_attr *wq_attr,
		      u32 wq_attr_mask, struct ib_udata *udata)
{
	struct mlx5_ib_dev *dev = to_mdev(wq->device);
	struct mlx5_ib_rwq *rwq = to_mrwq(wq);
	struct mlx5_ib_modify_wq ucmd = {};
	size_t required_cmd_sz;
	int curr_wq_state;
	int wq_state;
	int inlen;
	int err;
	void *rqc;
	void *in;

	required_cmd_sz = offsetof(typeof(ucmd), reserved) + sizeof(ucmd.reserved);
	if (udata->inlen < required_cmd_sz)
		return -EINVAL;

	if (udata->inlen > sizeof(ucmd) &&
	    !ib_is_udata_cleared(udata, sizeof(ucmd),
				 udata->inlen - sizeof(ucmd)))
		return -EOPNOTSUPP;

	if (ib_copy_from_udata(&ucmd, udata, min(sizeof(ucmd), udata->inlen)))
		return -EFAULT;

	if (ucmd.comp_mask || ucmd.reserved)
		return -EOPNOTSUPP;

	inlen = MLX5_ST_SZ_BYTES(modify_rq_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);

	curr_wq_state = (wq_attr_mask & IB_WQ_CUR_STATE) ?
		wq_attr->curr_wq_state : wq->state;
	wq_state = (wq_attr_mask & IB_WQ_STATE) ?
		wq_attr->wq_state : curr_wq_state;
	if (curr_wq_state == IB_WQS_ERR)
		curr_wq_state = MLX5_RQC_STATE_ERR;
	if (wq_state == IB_WQS_ERR)
		wq_state = MLX5_RQC_STATE_ERR;
	MLX5_SET(modify_rq_in, in, rq_state, curr_wq_state);
	MLX5_SET(modify_rq_in, in, uid, to_mpd(wq->pd)->uid);
	MLX5_SET(rqc, rqc, state, wq_state);

	if (wq_attr_mask & IB_WQ_FLAGS) {
		if (wq_attr->flags_mask & IB_WQ_FLAGS_CVLAN_STRIPPING) {
			if (!(MLX5_CAP_GEN(dev->mdev, eth_net_offloads) &&
			      MLX5_CAP_ETH(dev->mdev, vlan_cap))) {
				mlx5_ib_dbg(dev, "VLAN offloads are not "
					    "supported\n");
				err = -EOPNOTSUPP;
				goto out;
			}
			MLX5_SET64(modify_rq_in, in, modify_bitmask,
				   MLX5_MODIFY_RQ_IN_MODIFY_BITMASK_VSD);
			MLX5_SET(rqc, rqc, vsd,
				 (wq_attr->flags & IB_WQ_FLAGS_CVLAN_STRIPPING) ? 0 : 1);
		}

		if (wq_attr->flags_mask & IB_WQ_FLAGS_PCI_WRITE_END_PADDING) {
			mlx5_ib_dbg(dev, "Modifying scatter end padding is not supported\n");
			err = -EOPNOTSUPP;
			goto out;
		}
	}

	if (curr_wq_state == IB_WQS_RESET && wq_state == IB_WQS_RDY) {
		u16 set_id;

		set_id = mlx5_ib_get_counters_id(dev, 0);
		if (MLX5_CAP_GEN(dev->mdev, modify_rq_counter_set_id)) {
			MLX5_SET64(modify_rq_in, in, modify_bitmask,
				   MLX5_MODIFY_RQ_IN_MODIFY_BITMASK_RQ_COUNTER_SET_ID);
			MLX5_SET(rqc, rqc, counter_set_id, set_id);
		} else
			dev_info_once(
				&dev->ib_dev.dev,
				"Receive WQ counters are not supported on current FW\n");
	}

	err = mlx5_core_modify_rq(dev->mdev, rwq->core_qp.qpn, in, inlen);
	if (!err)
		rwq->ibwq.state = (wq_state == MLX5_RQC_STATE_ERR) ? IB_WQS_ERR : wq_state;

out:
	kvfree(in);
	return err;
}

struct mlx5_ib_drain_cqe {
	struct ib_cqe cqe;
	struct completion done;
};

static void mlx5_ib_drain_qp_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct mlx5_ib_drain_cqe *cqe = container_of(wc->wr_cqe,
						     struct mlx5_ib_drain_cqe,
						     cqe);

	complete(&cqe->done);
}

/* This function returns only once the drained WR was completed */
static void handle_drain_completion(struct ib_cq *cq,
				    struct mlx5_ib_drain_cqe *sdrain,
				    struct mlx5_ib_dev *dev)
{
	struct mlx5_core_dev *mdev = dev->mdev;

	if (cq->poll_ctx == IB_POLL_DIRECT) {
		while (wait_for_completion_timeout(&sdrain->done, HZ / 10) <= 0)
			ib_process_cq_direct(cq, -1);
		return;
	}

	if (mdev->state == MLX5_DEVICE_STATE_INTERNAL_ERROR) {
		struct mlx5_ib_cq *mcq = to_mcq(cq);
		bool triggered = false;
		unsigned long flags;

		spin_lock_irqsave(&dev->reset_flow_resource_lock, flags);
		/* Make sure that the CQ handler won't run if wasn't run yet */
		if (!mcq->mcq.reset_notify_added)
			mcq->mcq.reset_notify_added = 1;
		else
			triggered = true;
		spin_unlock_irqrestore(&dev->reset_flow_resource_lock, flags);

		if (triggered) {
			/* Wait for any scheduled/running task to be ended */
			switch (cq->poll_ctx) {
			case IB_POLL_SOFTIRQ:
				irq_poll_disable(&cq->iop);
				irq_poll_enable(&cq->iop);
				break;
			case IB_POLL_WORKQUEUE:
				cancel_work_sync(&cq->work);
				break;
			default:
				WARN_ON_ONCE(1);
			}
		}

		/* Run the CQ handler - this makes sure that the drain WR will
		 * be processed if wasn't processed yet.
		 */
		mcq->mcq.comp(&mcq->mcq, NULL);
	}

	wait_for_completion(&sdrain->done);
}

void mlx5_ib_drain_sq(struct ib_qp *qp)
{
	struct ib_cq *cq = qp->send_cq;
	struct ib_qp_attr attr = { .qp_state = IB_QPS_ERR };
	struct mlx5_ib_drain_cqe sdrain;
	const struct ib_send_wr *bad_swr;
	struct ib_rdma_wr swr = {
		.wr = {
			.next = NULL,
			{ .wr_cqe	= &sdrain.cqe, },
			.opcode	= IB_WR_RDMA_WRITE,
		},
	};
	int ret;
	struct mlx5_ib_dev *dev = to_mdev(qp->device);
	struct mlx5_core_dev *mdev = dev->mdev;

	ret = ib_modify_qp(qp, &attr, IB_QP_STATE);
	if (ret && mdev->state != MLX5_DEVICE_STATE_INTERNAL_ERROR) {
		WARN_ONCE(ret, "failed to drain send queue: %d\n", ret);
		return;
	}

	sdrain.cqe.done = mlx5_ib_drain_qp_done;
	init_completion(&sdrain.done);

	ret = _mlx5_ib_post_send(qp, &swr.wr, &bad_swr, true);
	if (ret) {
		WARN_ONCE(ret, "failed to drain send queue: %d\n", ret);
		return;
	}

	handle_drain_completion(cq, &sdrain, dev);
}

void mlx5_ib_drain_rq(struct ib_qp *qp)
{
	struct ib_cq *cq = qp->recv_cq;
	struct ib_qp_attr attr = { .qp_state = IB_QPS_ERR };
	struct mlx5_ib_drain_cqe rdrain;
	struct ib_recv_wr rwr = {};
	const struct ib_recv_wr *bad_rwr;
	int ret;
	struct mlx5_ib_dev *dev = to_mdev(qp->device);
	struct mlx5_core_dev *mdev = dev->mdev;

	ret = ib_modify_qp(qp, &attr, IB_QP_STATE);
	if (ret && mdev->state != MLX5_DEVICE_STATE_INTERNAL_ERROR) {
		WARN_ONCE(ret, "failed to drain recv queue: %d\n", ret);
		return;
	}

	rwr.wr_cqe = &rdrain.cqe;
	rdrain.cqe.done = mlx5_ib_drain_qp_done;
	init_completion(&rdrain.done);

	ret = _mlx5_ib_post_recv(qp, &rwr, &bad_rwr, true);
	if (ret) {
		WARN_ONCE(ret, "failed to drain recv queue: %d\n", ret);
		return;
	}

	handle_drain_completion(cq, &rdrain, dev);
}

/**
 * Bind a qp to a counter. If @counter is NULL then bind the qp to
 * the default counter
 */
int mlx5_ib_qp_set_counter(struct ib_qp *qp, struct rdma_counter *counter)
{
	struct mlx5_ib_dev *dev = to_mdev(qp->device);
	struct mlx5_ib_qp *mqp = to_mqp(qp);
	int err = 0;

	mutex_lock(&mqp->mutex);
	if (mqp->state == IB_QPS_RESET) {
		qp->counter = counter;
		goto out;
	}

	if (!MLX5_CAP_GEN(dev->mdev, rts2rts_qp_counters_set_id)) {
		err = -EOPNOTSUPP;
		goto out;
	}

	if (mqp->state == IB_QPS_RTS) {
		err = __mlx5_ib_qp_set_counter(qp, counter);
		if (!err)
			qp->counter = counter;

		goto out;
	}

	mqp->counter_pending = 1;
	qp->counter = counter;

out:
	mutex_unlock(&mqp->mutex);
	return err;
}
