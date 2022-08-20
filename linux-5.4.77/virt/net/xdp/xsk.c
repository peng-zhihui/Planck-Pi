// SPDX-License-Identifier: GPL-2.0
/* XDP sockets
 *
 * AF_XDP sockets allows a channel between XDP programs and userspace
 * applications.
 * Copyright(c) 2018 Intel Corporation.
 *
 * Author(s): Björn Töpel <bjorn.topel@intel.com>
 *	      Magnus Karlsson <magnus.karlsson@intel.com>
 */

#define pr_fmt(fmt) "AF_XDP: %s: " fmt, __func__

#include <linux/if_xdp.h>
#include <linux/init.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/socket.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/rculist.h>
#include <net/xdp_sock.h>
#include <net/xdp.h>

#include "xsk_queue.h"
#include "xdp_umem.h"
#include "xsk.h"

#define TX_BATCH_SIZE 16

bool xsk_is_setup_for_bpf_map(struct xdp_sock *xs)
{
	return READ_ONCE(xs->rx) &&  READ_ONCE(xs->umem) &&
		READ_ONCE(xs->umem->fq);
}

bool xsk_umem_has_addrs(struct xdp_umem *umem, u32 cnt)
{
	return xskq_has_addrs(umem->fq, cnt);
}
EXPORT_SYMBOL(xsk_umem_has_addrs);

u64 *xsk_umem_peek_addr(struct xdp_umem *umem, u64 *addr)
{
	return xskq_peek_addr(umem->fq, addr, umem);
}
EXPORT_SYMBOL(xsk_umem_peek_addr);

void xsk_umem_discard_addr(struct xdp_umem *umem)
{
	xskq_discard_addr(umem->fq);
}
EXPORT_SYMBOL(xsk_umem_discard_addr);

void xsk_set_rx_need_wakeup(struct xdp_umem *umem)
{
	if (umem->need_wakeup & XDP_WAKEUP_RX)
		return;

	umem->fq->ring->flags |= XDP_RING_NEED_WAKEUP;
	umem->need_wakeup |= XDP_WAKEUP_RX;
}
EXPORT_SYMBOL(xsk_set_rx_need_wakeup);

void xsk_set_tx_need_wakeup(struct xdp_umem *umem)
{
	struct xdp_sock *xs;

	if (umem->need_wakeup & XDP_WAKEUP_TX)
		return;

	rcu_read_lock();
	list_for_each_entry_rcu(xs, &umem->xsk_list, list) {
		xs->tx->ring->flags |= XDP_RING_NEED_WAKEUP;
	}
	rcu_read_unlock();

	umem->need_wakeup |= XDP_WAKEUP_TX;
}
EXPORT_SYMBOL(xsk_set_tx_need_wakeup);

void xsk_clear_rx_need_wakeup(struct xdp_umem *umem)
{
	if (!(umem->need_wakeup & XDP_WAKEUP_RX))
		return;

	umem->fq->ring->flags &= ~XDP_RING_NEED_WAKEUP;
	umem->need_wakeup &= ~XDP_WAKEUP_RX;
}
EXPORT_SYMBOL(xsk_clear_rx_need_wakeup);

void xsk_clear_tx_need_wakeup(struct xdp_umem *umem)
{
	struct xdp_sock *xs;

	if (!(umem->need_wakeup & XDP_WAKEUP_TX))
		return;

	rcu_read_lock();
	list_for_each_entry_rcu(xs, &umem->xsk_list, list) {
		xs->tx->ring->flags &= ~XDP_RING_NEED_WAKEUP;
	}
	rcu_read_unlock();

	umem->need_wakeup &= ~XDP_WAKEUP_TX;
}
EXPORT_SYMBOL(xsk_clear_tx_need_wakeup);

bool xsk_umem_uses_need_wakeup(struct xdp_umem *umem)
{
	return umem->flags & XDP_UMEM_USES_NEED_WAKEUP;
}
EXPORT_SYMBOL(xsk_umem_uses_need_wakeup);

/* If a buffer crosses a page boundary, we need to do 2 memcpy's, one for
 * each page. This is only required in copy mode.
 */
static void __xsk_rcv_memcpy(struct xdp_umem *umem, u64 addr, void *from_buf,
			     u32 len, u32 metalen)
{
	void *to_buf = xdp_umem_get_data(umem, addr);

	addr = xsk_umem_add_offset_to_addr(addr);
	if (xskq_crosses_non_contig_pg(umem, addr, len + metalen)) {
		void *next_pg_addr = umem->pages[(addr >> PAGE_SHIFT) + 1].addr;
		u64 page_start = addr & ~(PAGE_SIZE - 1);
		u64 first_len = PAGE_SIZE - (addr - page_start);

		memcpy(to_buf, from_buf, first_len);
		memcpy(next_pg_addr, from_buf + first_len,
		       len + metalen - first_len);

		return;
	}

	memcpy(to_buf, from_buf, len + metalen);
}

static int __xsk_rcv(struct xdp_sock *xs, struct xdp_buff *xdp, u32 len)
{
	u64 offset = xs->umem->headroom;
	u64 addr, memcpy_addr;
	void *from_buf;
	u32 metalen;
	int err;

	if (!xskq_peek_addr(xs->umem->fq, &addr, xs->umem) ||
	    len > xs->umem->chunk_size_nohr - XDP_PACKET_HEADROOM) {
		xs->rx_dropped++;
		return -ENOSPC;
	}

	if (unlikely(xdp_data_meta_unsupported(xdp))) {
		from_buf = xdp->data;
		metalen = 0;
	} else {
		from_buf = xdp->data_meta;
		metalen = xdp->data - xdp->data_meta;
	}

	memcpy_addr = xsk_umem_adjust_offset(xs->umem, addr, offset);
	__xsk_rcv_memcpy(xs->umem, memcpy_addr, from_buf, len, metalen);

	offset += metalen;
	addr = xsk_umem_adjust_offset(xs->umem, addr, offset);
	err = xskq_produce_batch_desc(xs->rx, addr, len);
	if (!err) {
		xskq_discard_addr(xs->umem->fq);
		xdp_return_buff(xdp);
		return 0;
	}

	xs->rx_dropped++;
	return err;
}

static int __xsk_rcv_zc(struct xdp_sock *xs, struct xdp_buff *xdp, u32 len)
{
	int err = xskq_produce_batch_desc(xs->rx, (u64)xdp->handle, len);

	if (err)
		xs->rx_dropped++;

	return err;
}

static bool xsk_is_bound(struct xdp_sock *xs)
{
	if (READ_ONCE(xs->state) == XSK_BOUND) {
		/* Matches smp_wmb() in bind(). */
		smp_rmb();
		return true;
	}
	return false;
}

int xsk_rcv(struct xdp_sock *xs, struct xdp_buff *xdp)
{
	u32 len;

	if (!xsk_is_bound(xs))
		return -EINVAL;

	if (xs->dev != xdp->rxq->dev || xs->queue_id != xdp->rxq->queue_index)
		return -EINVAL;

	len = xdp->data_end - xdp->data;

	return (xdp->rxq->mem.type == MEM_TYPE_ZERO_COPY) ?
		__xsk_rcv_zc(xs, xdp, len) : __xsk_rcv(xs, xdp, len);
}

void xsk_flush(struct xdp_sock *xs)
{
	xskq_produce_flush_desc(xs->rx);
	xs->sk.sk_data_ready(&xs->sk);
}

int xsk_generic_rcv(struct xdp_sock *xs, struct xdp_buff *xdp)
{
	u32 metalen = xdp->data - xdp->data_meta;
	u32 len = xdp->data_end - xdp->data;
	u64 offset = xs->umem->headroom;
	void *buffer;
	u64 addr;
	int err;

	spin_lock_bh(&xs->rx_lock);

	if (xs->dev != xdp->rxq->dev || xs->queue_id != xdp->rxq->queue_index) {
		err = -EINVAL;
		goto out_unlock;
	}

	if (!xskq_peek_addr(xs->umem->fq, &addr, xs->umem) ||
	    len > xs->umem->chunk_size_nohr - XDP_PACKET_HEADROOM) {
		err = -ENOSPC;
		goto out_drop;
	}

	addr = xsk_umem_adjust_offset(xs->umem, addr, offset);
	buffer = xdp_umem_get_data(xs->umem, addr);
	memcpy(buffer, xdp->data_meta, len + metalen);

	addr = xsk_umem_adjust_offset(xs->umem, addr, metalen);
	err = xskq_produce_batch_desc(xs->rx, addr, len);
	if (err)
		goto out_drop;

	xskq_discard_addr(xs->umem->fq);
	xskq_produce_flush_desc(xs->rx);

	spin_unlock_bh(&xs->rx_lock);

	xs->sk.sk_data_ready(&xs->sk);
	return 0;

out_drop:
	xs->rx_dropped++;
out_unlock:
	spin_unlock_bh(&xs->rx_lock);
	return err;
}

void xsk_umem_complete_tx(struct xdp_umem *umem, u32 nb_entries)
{
	xskq_produce_flush_addr_n(umem->cq, nb_entries);
}
EXPORT_SYMBOL(xsk_umem_complete_tx);

void xsk_umem_consume_tx_done(struct xdp_umem *umem)
{
	struct xdp_sock *xs;

	rcu_read_lock();
	list_for_each_entry_rcu(xs, &umem->xsk_list, list) {
		xs->sk.sk_write_space(&xs->sk);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(xsk_umem_consume_tx_done);

bool xsk_umem_consume_tx(struct xdp_umem *umem, struct xdp_desc *desc)
{
	struct xdp_sock *xs;

	rcu_read_lock();
	list_for_each_entry_rcu(xs, &umem->xsk_list, list) {
		if (!xskq_peek_desc(xs->tx, desc, umem))
			continue;

		if (xskq_produce_addr_lazy(umem->cq, desc->addr))
			goto out;

		xskq_discard_desc(xs->tx);
		rcu_read_unlock();
		return true;
	}

out:
	rcu_read_unlock();
	return false;
}
EXPORT_SYMBOL(xsk_umem_consume_tx);

static int xsk_wakeup(struct xdp_sock *xs, u8 flags)
{
	struct net_device *dev = xs->dev;
	int err;

	rcu_read_lock();
	err = dev->netdev_ops->ndo_xsk_wakeup(dev, xs->queue_id, flags);
	rcu_read_unlock();

	return err;
}

static int xsk_zc_xmit(struct xdp_sock *xs)
{
	return xsk_wakeup(xs, XDP_WAKEUP_TX);
}

static void xsk_destruct_skb(struct sk_buff *skb)
{
	u64 addr = (u64)(long)skb_shinfo(skb)->destructor_arg;
	struct xdp_sock *xs = xdp_sk(skb->sk);
	unsigned long flags;

	spin_lock_irqsave(&xs->tx_completion_lock, flags);
	WARN_ON_ONCE(xskq_produce_addr(xs->umem->cq, addr));
	spin_unlock_irqrestore(&xs->tx_completion_lock, flags);

	sock_wfree(skb);
}

static int xsk_generic_xmit(struct sock *sk)
{
	struct xdp_sock *xs = xdp_sk(sk);
	u32 max_batch = TX_BATCH_SIZE;
	bool sent_frame = false;
	struct xdp_desc desc;
	struct sk_buff *skb;
	int err = 0;

	mutex_lock(&xs->mutex);

	if (xs->queue_id >= xs->dev->real_num_tx_queues)
		goto out;

	while (xskq_peek_desc(xs->tx, &desc, xs->umem)) {
		char *buffer;
		u64 addr;
		u32 len;

		if (max_batch-- == 0) {
			err = -EAGAIN;
			goto out;
		}

		len = desc.len;
		skb = sock_alloc_send_skb(sk, len, 1, &err);
		if (unlikely(!skb))
			goto out;

		skb_put(skb, len);
		addr = desc.addr;
		buffer = xdp_umem_get_data(xs->umem, addr);
		err = skb_store_bits(skb, 0, buffer, len);
		if (unlikely(err) || xskq_reserve_addr(xs->umem->cq)) {
			kfree_skb(skb);
			goto out;
		}

		skb->dev = xs->dev;
		skb->priority = sk->sk_priority;
		skb->mark = sk->sk_mark;
		skb_shinfo(skb)->destructor_arg = (void *)(long)desc.addr;
		skb->destructor = xsk_destruct_skb;

		err = dev_direct_xmit(skb, xs->queue_id);
		xskq_discard_desc(xs->tx);
		/* Ignore NET_XMIT_CN as packet might have been sent */
		if (err == NET_XMIT_DROP || err == NETDEV_TX_BUSY) {
			/* SKB completed but not sent */
			err = -EBUSY;
			goto out;
		}

		sent_frame = true;
	}

out:
	if (sent_frame)
		sk->sk_write_space(sk);

	mutex_unlock(&xs->mutex);
	return err;
}

static int __xsk_sendmsg(struct sock *sk)
{
	struct xdp_sock *xs = xdp_sk(sk);

	if (unlikely(!(xs->dev->flags & IFF_UP)))
		return -ENETDOWN;
	if (unlikely(!xs->tx))
		return -ENOBUFS;

	return xs->zc ? xsk_zc_xmit(xs) : xsk_generic_xmit(sk);
}

static int xsk_sendmsg(struct socket *sock, struct msghdr *m, size_t total_len)
{
	bool need_wait = !(m->msg_flags & MSG_DONTWAIT);
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);

	if (unlikely(!xsk_is_bound(xs)))
		return -ENXIO;
	if (unlikely(need_wait))
		return -EOPNOTSUPP;

	return __xsk_sendmsg(sk);
}

static unsigned int xsk_poll(struct file *file, struct socket *sock,
			     struct poll_table_struct *wait)
{
	unsigned int mask = datagram_poll(file, sock, wait);
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	struct xdp_umem *umem;

	if (unlikely(!xsk_is_bound(xs)))
		return mask;

	umem = xs->umem;

	if (umem->need_wakeup) {
		if (xs->zc)
			xsk_wakeup(xs, umem->need_wakeup);
		else
			/* Poll needs to drive Tx also in copy mode */
			__xsk_sendmsg(sk);
	}

	if (xs->rx && !xskq_empty_desc(xs->rx))
		mask |= POLLIN | POLLRDNORM;
	if (xs->tx && !xskq_full_desc(xs->tx))
		mask |= POLLOUT | POLLWRNORM;

	return mask;
}

static int xsk_init_queue(u32 entries, struct xsk_queue **queue,
			  bool umem_queue)
{
	struct xsk_queue *q;

	if (entries == 0 || *queue || !is_power_of_2(entries))
		return -EINVAL;

	q = xskq_create(entries, umem_queue);
	if (!q)
		return -ENOMEM;

	/* Make sure queue is ready before it can be seen by others */
	smp_wmb();
	WRITE_ONCE(*queue, q);
	return 0;
}

static void xsk_unbind_dev(struct xdp_sock *xs)
{
	struct net_device *dev = xs->dev;

	if (xs->state != XSK_BOUND)
		return;
	WRITE_ONCE(xs->state, XSK_UNBOUND);

	/* Wait for driver to stop using the xdp socket. */
	xdp_del_sk_umem(xs->umem, xs);
	xs->dev = NULL;
	synchronize_net();
	dev_put(dev);
}

static struct xsk_map *xsk_get_map_list_entry(struct xdp_sock *xs,
					      struct xdp_sock ***map_entry)
{
	struct xsk_map *map = NULL;
	struct xsk_map_node *node;

	*map_entry = NULL;

	spin_lock_bh(&xs->map_list_lock);
	node = list_first_entry_or_null(&xs->map_list, struct xsk_map_node,
					node);
	if (node) {
		WARN_ON(xsk_map_inc(node->map));
		map = node->map;
		*map_entry = node->map_entry;
	}
	spin_unlock_bh(&xs->map_list_lock);
	return map;
}

static void xsk_delete_from_maps(struct xdp_sock *xs)
{
	/* This function removes the current XDP socket from all the
	 * maps it resides in. We need to take extra care here, due to
	 * the two locks involved. Each map has a lock synchronizing
	 * updates to the entries, and each socket has a lock that
	 * synchronizes access to the list of maps (map_list). For
	 * deadlock avoidance the locks need to be taken in the order
	 * "map lock"->"socket map list lock". We start off by
	 * accessing the socket map list, and take a reference to the
	 * map to guarantee existence between the
	 * xsk_get_map_list_entry() and xsk_map_try_sock_delete()
	 * calls. Then we ask the map to remove the socket, which
	 * tries to remove the socket from the map. Note that there
	 * might be updates to the map between
	 * xsk_get_map_list_entry() and xsk_map_try_sock_delete().
	 */
	struct xdp_sock **map_entry = NULL;
	struct xsk_map *map;

	while ((map = xsk_get_map_list_entry(xs, &map_entry))) {
		xsk_map_try_sock_delete(map, xs, map_entry);
		xsk_map_put(map);
	}
}

static int xsk_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	struct net *net;

	if (!sk)
		return 0;

	net = sock_net(sk);

	mutex_lock(&net->xdp.lock);
	sk_del_node_init_rcu(sk);
	mutex_unlock(&net->xdp.lock);

	local_bh_disable();
	sock_prot_inuse_add(net, sk->sk_prot, -1);
	local_bh_enable();

	xsk_delete_from_maps(xs);
	mutex_lock(&xs->mutex);
	xsk_unbind_dev(xs);
	mutex_unlock(&xs->mutex);

	xskq_destroy(xs->rx);
	xskq_destroy(xs->tx);

	sock_orphan(sk);
	sock->sk = NULL;

	sk_refcnt_debug_release(sk);
	sock_put(sk);

	return 0;
}

static struct socket *xsk_lookup_xsk_from_fd(int fd)
{
	struct socket *sock;
	int err;

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		return ERR_PTR(-ENOTSOCK);

	if (sock->sk->sk_family != PF_XDP) {
		sockfd_put(sock);
		return ERR_PTR(-ENOPROTOOPT);
	}

	return sock;
}

/* Check if umem pages are contiguous.
 * If zero-copy mode, use the DMA address to do the page contiguity check
 * For all other modes we use addr (kernel virtual address)
 * Store the result in the low bits of addr.
 */
static void xsk_check_page_contiguity(struct xdp_umem *umem, u32 flags)
{
	struct xdp_umem_page *pgs = umem->pages;
	int i, is_contig;

	for (i = 0; i < umem->npgs - 1; i++) {
		is_contig = (flags & XDP_ZEROCOPY) ?
			(pgs[i].dma + PAGE_SIZE == pgs[i + 1].dma) :
			(pgs[i].addr + PAGE_SIZE == pgs[i + 1].addr);
		pgs[i].addr += is_contig << XSK_NEXT_PG_CONTIG_SHIFT;
	}
}

static int xsk_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	struct sockaddr_xdp *sxdp = (struct sockaddr_xdp *)addr;
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	struct net_device *dev;
	u32 flags, qid;
	int err = 0;

	if (addr_len < sizeof(struct sockaddr_xdp))
		return -EINVAL;
	if (sxdp->sxdp_family != AF_XDP)
		return -EINVAL;

	flags = sxdp->sxdp_flags;
	if (flags & ~(XDP_SHARED_UMEM | XDP_COPY | XDP_ZEROCOPY |
		      XDP_USE_NEED_WAKEUP))
		return -EINVAL;

	rtnl_lock();
	mutex_lock(&xs->mutex);
	if (xs->state != XSK_READY) {
		err = -EBUSY;
		goto out_release;
	}

	dev = dev_get_by_index(sock_net(sk), sxdp->sxdp_ifindex);
	if (!dev) {
		err = -ENODEV;
		goto out_release;
	}

	if (!xs->rx && !xs->tx) {
		err = -EINVAL;
		goto out_unlock;
	}

	qid = sxdp->sxdp_queue_id;

	if (flags & XDP_SHARED_UMEM) {
		struct xdp_sock *umem_xs;
		struct socket *sock;

		if ((flags & XDP_COPY) || (flags & XDP_ZEROCOPY) ||
		    (flags & XDP_USE_NEED_WAKEUP)) {
			/* Cannot specify flags for shared sockets. */
			err = -EINVAL;
			goto out_unlock;
		}

		if (xs->umem) {
			/* We have already our own. */
			err = -EINVAL;
			goto out_unlock;
		}

		sock = xsk_lookup_xsk_from_fd(sxdp->sxdp_shared_umem_fd);
		if (IS_ERR(sock)) {
			err = PTR_ERR(sock);
			goto out_unlock;
		}

		umem_xs = xdp_sk(sock->sk);
		if (!xsk_is_bound(umem_xs)) {
			err = -EBADF;
			sockfd_put(sock);
			goto out_unlock;
		}
		if (umem_xs->dev != dev || umem_xs->queue_id != qid) {
			err = -EINVAL;
			sockfd_put(sock);
			goto out_unlock;
		}

		xdp_get_umem(umem_xs->umem);
		WRITE_ONCE(xs->umem, umem_xs->umem);
		sockfd_put(sock);
	} else if (!xs->umem || !xdp_umem_validate_queues(xs->umem)) {
		err = -EINVAL;
		goto out_unlock;
	} else {
		/* This xsk has its own umem. */
		xskq_set_umem(xs->umem->fq, xs->umem->size,
			      xs->umem->chunk_mask);
		xskq_set_umem(xs->umem->cq, xs->umem->size,
			      xs->umem->chunk_mask);

		err = xdp_umem_assign_dev(xs->umem, dev, qid, flags);
		if (err)
			goto out_unlock;

		xsk_check_page_contiguity(xs->umem, flags);
	}

	xs->dev = dev;
	xs->zc = xs->umem->zc;
	xs->queue_id = qid;
	xskq_set_umem(xs->rx, xs->umem->size, xs->umem->chunk_mask);
	xskq_set_umem(xs->tx, xs->umem->size, xs->umem->chunk_mask);
	xdp_add_sk_umem(xs->umem, xs);

out_unlock:
	if (err) {
		dev_put(dev);
	} else {
		/* Matches smp_rmb() in bind() for shared umem
		 * sockets, and xsk_is_bound().
		 */
		smp_wmb();
		WRITE_ONCE(xs->state, XSK_BOUND);
	}
out_release:
	mutex_unlock(&xs->mutex);
	rtnl_unlock();
	return err;
}

struct xdp_umem_reg_v1 {
	__u64 addr; /* Start of packet data area */
	__u64 len; /* Length of packet data area */
	__u32 chunk_size;
	__u32 headroom;
};

static int xsk_setsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	int err;

	if (level != SOL_XDP)
		return -ENOPROTOOPT;

	switch (optname) {
	case XDP_RX_RING:
	case XDP_TX_RING:
	{
		struct xsk_queue **q;
		int entries;

		if (optlen < sizeof(entries))
			return -EINVAL;
		if (copy_from_user(&entries, optval, sizeof(entries)))
			return -EFAULT;

		mutex_lock(&xs->mutex);
		if (xs->state != XSK_READY) {
			mutex_unlock(&xs->mutex);
			return -EBUSY;
		}
		q = (optname == XDP_TX_RING) ? &xs->tx : &xs->rx;
		err = xsk_init_queue(entries, q, false);
		if (!err && optname == XDP_TX_RING)
			/* Tx needs to be explicitly woken up the first time */
			xs->tx->ring->flags |= XDP_RING_NEED_WAKEUP;
		mutex_unlock(&xs->mutex);
		return err;
	}
	case XDP_UMEM_REG:
	{
		size_t mr_size = sizeof(struct xdp_umem_reg);
		struct xdp_umem_reg mr = {};
		struct xdp_umem *umem;

		if (optlen < sizeof(struct xdp_umem_reg_v1))
			return -EINVAL;
		else if (optlen < sizeof(mr))
			mr_size = sizeof(struct xdp_umem_reg_v1);

		if (copy_from_user(&mr, optval, mr_size))
			return -EFAULT;

		mutex_lock(&xs->mutex);
		if (xs->state != XSK_READY || xs->umem) {
			mutex_unlock(&xs->mutex);
			return -EBUSY;
		}

		umem = xdp_umem_create(&mr);
		if (IS_ERR(umem)) {
			mutex_unlock(&xs->mutex);
			return PTR_ERR(umem);
		}

		/* Make sure umem is ready before it can be seen by others */
		smp_wmb();
		WRITE_ONCE(xs->umem, umem);
		mutex_unlock(&xs->mutex);
		return 0;
	}
	case XDP_UMEM_FILL_RING:
	case XDP_UMEM_COMPLETION_RING:
	{
		struct xsk_queue **q;
		int entries;

		if (copy_from_user(&entries, optval, sizeof(entries)))
			return -EFAULT;

		mutex_lock(&xs->mutex);
		if (xs->state != XSK_READY) {
			mutex_unlock(&xs->mutex);
			return -EBUSY;
		}
		if (!xs->umem) {
			mutex_unlock(&xs->mutex);
			return -EINVAL;
		}

		q = (optname == XDP_UMEM_FILL_RING) ? &xs->umem->fq :
			&xs->umem->cq;
		err = xsk_init_queue(entries, q, true);
		mutex_unlock(&xs->mutex);
		return err;
	}
	default:
		break;
	}

	return -ENOPROTOOPT;
}

static void xsk_enter_rxtx_offsets(struct xdp_ring_offset_v1 *ring)
{
	ring->producer = offsetof(struct xdp_rxtx_ring, ptrs.producer);
	ring->consumer = offsetof(struct xdp_rxtx_ring, ptrs.consumer);
	ring->desc = offsetof(struct xdp_rxtx_ring, desc);
}

static void xsk_enter_umem_offsets(struct xdp_ring_offset_v1 *ring)
{
	ring->producer = offsetof(struct xdp_umem_ring, ptrs.producer);
	ring->consumer = offsetof(struct xdp_umem_ring, ptrs.consumer);
	ring->desc = offsetof(struct xdp_umem_ring, desc);
}

static int xsk_getsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	int len;

	if (level != SOL_XDP)
		return -ENOPROTOOPT;

	if (get_user(len, optlen))
		return -EFAULT;
	if (len < 0)
		return -EINVAL;

	switch (optname) {
	case XDP_STATISTICS:
	{
		struct xdp_statistics stats;

		if (len < sizeof(stats))
			return -EINVAL;

		mutex_lock(&xs->mutex);
		stats.rx_dropped = xs->rx_dropped;
		stats.rx_invalid_descs = xskq_nb_invalid_descs(xs->rx);
		stats.tx_invalid_descs = xskq_nb_invalid_descs(xs->tx);
		mutex_unlock(&xs->mutex);

		if (copy_to_user(optval, &stats, sizeof(stats)))
			return -EFAULT;
		if (put_user(sizeof(stats), optlen))
			return -EFAULT;

		return 0;
	}
	case XDP_MMAP_OFFSETS:
	{
		struct xdp_mmap_offsets off;
		struct xdp_mmap_offsets_v1 off_v1;
		bool flags_supported = true;
		void *to_copy;

		if (len < sizeof(off_v1))
			return -EINVAL;
		else if (len < sizeof(off))
			flags_supported = false;

		if (flags_supported) {
			/* xdp_ring_offset is identical to xdp_ring_offset_v1
			 * except for the flags field added to the end.
			 */
			xsk_enter_rxtx_offsets((struct xdp_ring_offset_v1 *)
					       &off.rx);
			xsk_enter_rxtx_offsets((struct xdp_ring_offset_v1 *)
					       &off.tx);
			xsk_enter_umem_offsets((struct xdp_ring_offset_v1 *)
					       &off.fr);
			xsk_enter_umem_offsets((struct xdp_ring_offset_v1 *)
					       &off.cr);
			off.rx.flags = offsetof(struct xdp_rxtx_ring,
						ptrs.flags);
			off.tx.flags = offsetof(struct xdp_rxtx_ring,
						ptrs.flags);
			off.fr.flags = offsetof(struct xdp_umem_ring,
						ptrs.flags);
			off.cr.flags = offsetof(struct xdp_umem_ring,
						ptrs.flags);

			len = sizeof(off);
			to_copy = &off;
		} else {
			xsk_enter_rxtx_offsets(&off_v1.rx);
			xsk_enter_rxtx_offsets(&off_v1.tx);
			xsk_enter_umem_offsets(&off_v1.fr);
			xsk_enter_umem_offsets(&off_v1.cr);

			len = sizeof(off_v1);
			to_copy = &off_v1;
		}

		if (copy_to_user(optval, to_copy, len))
			return -EFAULT;
		if (put_user(len, optlen))
			return -EFAULT;

		return 0;
	}
	case XDP_OPTIONS:
	{
		struct xdp_options opts = {};

		if (len < sizeof(opts))
			return -EINVAL;

		mutex_lock(&xs->mutex);
		if (xs->zc)
			opts.flags |= XDP_OPTIONS_ZEROCOPY;
		mutex_unlock(&xs->mutex);

		len = sizeof(opts);
		if (copy_to_user(optval, &opts, len))
			return -EFAULT;
		if (put_user(len, optlen))
			return -EFAULT;

		return 0;
	}
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int xsk_mmap(struct file *file, struct socket *sock,
		    struct vm_area_struct *vma)
{
	loff_t offset = (loff_t)vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct xdp_sock *xs = xdp_sk(sock->sk);
	struct xsk_queue *q = NULL;
	struct xdp_umem *umem;
	unsigned long pfn;
	struct page *qpg;

	if (READ_ONCE(xs->state) != XSK_READY)
		return -EBUSY;

	if (offset == XDP_PGOFF_RX_RING) {
		q = READ_ONCE(xs->rx);
	} else if (offset == XDP_PGOFF_TX_RING) {
		q = READ_ONCE(xs->tx);
	} else {
		umem = READ_ONCE(xs->umem);
		if (!umem)
			return -EINVAL;

		/* Matches the smp_wmb() in XDP_UMEM_REG */
		smp_rmb();
		if (offset == XDP_UMEM_PGOFF_FILL_RING)
			q = READ_ONCE(umem->fq);
		else if (offset == XDP_UMEM_PGOFF_COMPLETION_RING)
			q = READ_ONCE(umem->cq);
	}

	if (!q)
		return -EINVAL;

	/* Matches the smp_wmb() in xsk_init_queue */
	smp_rmb();
	qpg = virt_to_head_page(q->ring);
	if (size > page_size(qpg))
		return -EINVAL;

	pfn = virt_to_phys(q->ring) >> PAGE_SHIFT;
	return remap_pfn_range(vma, vma->vm_start, pfn,
			       size, vma->vm_page_prot);
}

static int xsk_notifier(struct notifier_block *this,
			unsigned long msg, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct net *net = dev_net(dev);
	struct sock *sk;

	switch (msg) {
	case NETDEV_UNREGISTER:
		mutex_lock(&net->xdp.lock);
		sk_for_each(sk, &net->xdp.list) {
			struct xdp_sock *xs = xdp_sk(sk);

			mutex_lock(&xs->mutex);
			if (xs->dev == dev) {
				sk->sk_err = ENETDOWN;
				if (!sock_flag(sk, SOCK_DEAD))
					sk->sk_error_report(sk);

				xsk_unbind_dev(xs);

				/* Clear device references in umem. */
				xdp_umem_clear_dev(xs->umem);
			}
			mutex_unlock(&xs->mutex);
		}
		mutex_unlock(&net->xdp.lock);
		break;
	}
	return NOTIFY_DONE;
}

static struct proto xsk_proto = {
	.name =		"XDP",
	.owner =	THIS_MODULE,
	.obj_size =	sizeof(struct xdp_sock),
};

static const struct proto_ops xsk_proto_ops = {
	.family		= PF_XDP,
	.owner		= THIS_MODULE,
	.release	= xsk_release,
	.bind		= xsk_bind,
	.connect	= sock_no_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= sock_no_getname,
	.poll		= xsk_poll,
	.ioctl		= sock_no_ioctl,
	.listen		= sock_no_listen,
	.shutdown	= sock_no_shutdown,
	.setsockopt	= xsk_setsockopt,
	.getsockopt	= xsk_getsockopt,
	.sendmsg	= xsk_sendmsg,
	.recvmsg	= sock_no_recvmsg,
	.mmap		= xsk_mmap,
	.sendpage	= sock_no_sendpage,
};

static void xsk_destruct(struct sock *sk)
{
	struct xdp_sock *xs = xdp_sk(sk);

	if (!sock_flag(sk, SOCK_DEAD))
		return;

	xdp_put_umem(xs->umem);

	sk_refcnt_debug_dec(sk);
}

static int xsk_create(struct net *net, struct socket *sock, int protocol,
		      int kern)
{
	struct sock *sk;
	struct xdp_sock *xs;

	if (!ns_capable(net->user_ns, CAP_NET_RAW))
		return -EPERM;
	if (sock->type != SOCK_RAW)
		return -ESOCKTNOSUPPORT;

	if (protocol)
		return -EPROTONOSUPPORT;

	sock->state = SS_UNCONNECTED;

	sk = sk_alloc(net, PF_XDP, GFP_KERNEL, &xsk_proto, kern);
	if (!sk)
		return -ENOBUFS;

	sock->ops = &xsk_proto_ops;

	sock_init_data(sock, sk);

	sk->sk_family = PF_XDP;

	sk->sk_destruct = xsk_destruct;
	sk_refcnt_debug_inc(sk);

	sock_set_flag(sk, SOCK_RCU_FREE);

	xs = xdp_sk(sk);
	xs->state = XSK_READY;
	mutex_init(&xs->mutex);
	spin_lock_init(&xs->rx_lock);
	spin_lock_init(&xs->tx_completion_lock);

	INIT_LIST_HEAD(&xs->map_list);
	spin_lock_init(&xs->map_list_lock);

	mutex_lock(&net->xdp.lock);
	sk_add_node_rcu(sk, &net->xdp.list);
	mutex_unlock(&net->xdp.lock);

	local_bh_disable();
	sock_prot_inuse_add(net, &xsk_proto, 1);
	local_bh_enable();

	return 0;
}

static const struct net_proto_family xsk_family_ops = {
	.family = PF_XDP,
	.create = xsk_create,
	.owner	= THIS_MODULE,
};

static struct notifier_block xsk_netdev_notifier = {
	.notifier_call	= xsk_notifier,
};

static int __net_init xsk_net_init(struct net *net)
{
	mutex_init(&net->xdp.lock);
	INIT_HLIST_HEAD(&net->xdp.list);
	return 0;
}

static void __net_exit xsk_net_exit(struct net *net)
{
	WARN_ON_ONCE(!hlist_empty(&net->xdp.list));
}

static struct pernet_operations xsk_net_ops = {
	.init = xsk_net_init,
	.exit = xsk_net_exit,
};

static int __init xsk_init(void)
{
	int err;

	err = proto_register(&xsk_proto, 0 /* no slab */);
	if (err)
		goto out;

	err = sock_register(&xsk_family_ops);
	if (err)
		goto out_proto;

	err = register_pernet_subsys(&xsk_net_ops);
	if (err)
		goto out_sk;

	err = register_netdevice_notifier(&xsk_netdev_notifier);
	if (err)
		goto out_pernet;

	return 0;

out_pernet:
	unregister_pernet_subsys(&xsk_net_ops);
out_sk:
	sock_unregister(PF_XDP);
out_proto:
	proto_unregister(&xsk_proto);
out:
	return err;
}

fs_initcall(xsk_init);
