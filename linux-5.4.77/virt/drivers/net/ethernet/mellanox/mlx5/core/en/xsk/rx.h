/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2019 Mellanox Technologies. */

#ifndef __MLX5_EN_XSK_RX_H__
#define __MLX5_EN_XSK_RX_H__

#include "en.h"
#include <net/xdp_sock.h>

/* RX data path */

bool mlx5e_xsk_pages_enough_umem(struct mlx5e_rq *rq, int count);
int mlx5e_xsk_page_alloc_umem(struct mlx5e_rq *rq,
			      struct mlx5e_dma_info *dma_info);
void mlx5e_xsk_page_release(struct mlx5e_rq *rq,
			    struct mlx5e_dma_info *dma_info);
void mlx5e_xsk_zca_free(struct zero_copy_allocator *zca, unsigned long handle);
struct sk_buff *mlx5e_xsk_skb_from_cqe_mpwrq_linear(struct mlx5e_rq *rq,
						    struct mlx5e_mpw_info *wi,
						    u16 cqe_bcnt,
						    u32 head_offset,
						    u32 page_idx);
struct sk_buff *mlx5e_xsk_skb_from_cqe_linear(struct mlx5e_rq *rq,
					      struct mlx5_cqe64 *cqe,
					      struct mlx5e_wqe_frag_info *wi,
					      u32 cqe_bcnt);

static inline bool mlx5e_xsk_update_rx_wakeup(struct mlx5e_rq *rq, bool alloc_err)
{
	if (!xsk_umem_uses_need_wakeup(rq->umem))
		return alloc_err;

	if (unlikely(alloc_err))
		xsk_set_rx_need_wakeup(rq->umem);
	else
		xsk_clear_rx_need_wakeup(rq->umem);

	return false;
}

#endif /* __MLX5_EN_XSK_RX_H__ */
