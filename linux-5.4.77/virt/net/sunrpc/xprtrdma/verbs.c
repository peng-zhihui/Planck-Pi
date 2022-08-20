// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (c) 2014-2017 Oracle.  All rights reserved.
 * Copyright (c) 2003-2007 Network Appliance, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of the Network Appliance, Inc. nor the names of
 *      its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * verbs.c
 *
 * Encapsulates the major functions managing:
 *  o adapters
 *  o endpoints
 *  o connections
 *  o buffer memory
 */

#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/sunrpc/addr.h>
#include <linux/sunrpc/svc_rdma.h>
#include <linux/log2.h>

#include <asm-generic/barrier.h>
#include <asm/bitops.h>

#include <rdma/ib_cm.h>

#include "xprt_rdma.h"
#include <trace/events/rpcrdma.h>

/*
 * Globals/Macros
 */

#if IS_ENABLED(CONFIG_SUNRPC_DEBUG)
# define RPCDBG_FACILITY	RPCDBG_TRANS
#endif

/*
 * internal functions
 */
static void rpcrdma_sendctx_put_locked(struct rpcrdma_sendctx *sc);
static void rpcrdma_reqs_reset(struct rpcrdma_xprt *r_xprt);
static void rpcrdma_reps_unmap(struct rpcrdma_xprt *r_xprt);
static void rpcrdma_mrs_create(struct rpcrdma_xprt *r_xprt);
static void rpcrdma_mrs_destroy(struct rpcrdma_buffer *buf);
static struct rpcrdma_regbuf *
rpcrdma_regbuf_alloc(size_t size, enum dma_data_direction direction,
		     gfp_t flags);
static void rpcrdma_regbuf_dma_unmap(struct rpcrdma_regbuf *rb);
static void rpcrdma_regbuf_free(struct rpcrdma_regbuf *rb);

/* Wait for outstanding transport work to finish. ib_drain_qp
 * handles the drains in the wrong order for us, so open code
 * them here.
 */
static void rpcrdma_xprt_drain(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_ia *ia = &r_xprt->rx_ia;

	/* Flush Receives, then wait for deferred Reply work
	 * to complete.
	 */
	ib_drain_rq(ia->ri_id->qp);

	/* Deferred Reply processing might have scheduled
	 * local invalidations.
	 */
	ib_drain_sq(ia->ri_id->qp);
}

/**
 * rpcrdma_qp_event_handler - Handle one QP event (error notification)
 * @event: details of the event
 * @context: ep that owns QP where event occurred
 *
 * Called from the RDMA provider (device driver) possibly in an interrupt
 * context.
 */
static void
rpcrdma_qp_event_handler(struct ib_event *event, void *context)
{
	struct rpcrdma_ep *ep = context;
	struct rpcrdma_xprt *r_xprt = container_of(ep, struct rpcrdma_xprt,
						   rx_ep);

	trace_xprtrdma_qp_event(r_xprt, event);
}

/**
 * rpcrdma_wc_send - Invoked by RDMA provider for each polled Send WC
 * @cq:	completion queue (ignored)
 * @wc:	completed WR
 *
 */
static void
rpcrdma_wc_send(struct ib_cq *cq, struct ib_wc *wc)
{
	struct ib_cqe *cqe = wc->wr_cqe;
	struct rpcrdma_sendctx *sc =
		container_of(cqe, struct rpcrdma_sendctx, sc_cqe);

	/* WARNING: Only wr_cqe and status are reliable at this point */
	trace_xprtrdma_wc_send(sc, wc);
	rpcrdma_sendctx_put_locked(sc);
}

/**
 * rpcrdma_wc_receive - Invoked by RDMA provider for each polled Receive WC
 * @cq:	completion queue (ignored)
 * @wc:	completed WR
 *
 */
static void
rpcrdma_wc_receive(struct ib_cq *cq, struct ib_wc *wc)
{
	struct ib_cqe *cqe = wc->wr_cqe;
	struct rpcrdma_rep *rep = container_of(cqe, struct rpcrdma_rep,
					       rr_cqe);
	struct rpcrdma_xprt *r_xprt = rep->rr_rxprt;

	/* WARNING: Only wr_cqe and status are reliable at this point */
	trace_xprtrdma_wc_receive(wc);
	--r_xprt->rx_ep.rep_receive_count;
	if (wc->status != IB_WC_SUCCESS)
		goto out_flushed;

	/* status == SUCCESS means all fields in wc are trustworthy */
	rpcrdma_set_xdrlen(&rep->rr_hdrbuf, wc->byte_len);
	rep->rr_wc_flags = wc->wc_flags;
	rep->rr_inv_rkey = wc->ex.invalidate_rkey;

	ib_dma_sync_single_for_cpu(rdmab_device(rep->rr_rdmabuf),
				   rdmab_addr(rep->rr_rdmabuf),
				   wc->byte_len, DMA_FROM_DEVICE);

	rpcrdma_reply_handler(rep);
	return;

out_flushed:
	rpcrdma_recv_buffer_put(rep);
}

static void
rpcrdma_update_connect_private(struct rpcrdma_xprt *r_xprt,
			       struct rdma_conn_param *param)
{
	const struct rpcrdma_connect_private *pmsg = param->private_data;
	unsigned int rsize, wsize;

	/* Default settings for RPC-over-RDMA Version One */
	r_xprt->rx_ia.ri_implicit_roundup = xprt_rdma_pad_optimize;
	rsize = RPCRDMA_V1_DEF_INLINE_SIZE;
	wsize = RPCRDMA_V1_DEF_INLINE_SIZE;

	if (pmsg &&
	    pmsg->cp_magic == rpcrdma_cmp_magic &&
	    pmsg->cp_version == RPCRDMA_CMP_VERSION) {
		r_xprt->rx_ia.ri_implicit_roundup = true;
		rsize = rpcrdma_decode_buffer_size(pmsg->cp_send_size);
		wsize = rpcrdma_decode_buffer_size(pmsg->cp_recv_size);
	}

	if (rsize < r_xprt->rx_ep.rep_inline_recv)
		r_xprt->rx_ep.rep_inline_recv = rsize;
	if (wsize < r_xprt->rx_ep.rep_inline_send)
		r_xprt->rx_ep.rep_inline_send = wsize;
	dprintk("RPC:       %s: max send %u, max recv %u\n", __func__,
		r_xprt->rx_ep.rep_inline_send,
		r_xprt->rx_ep.rep_inline_recv);
	rpcrdma_set_max_header_sizes(r_xprt);
}

/**
 * rpcrdma_cm_event_handler - Handle RDMA CM events
 * @id: rdma_cm_id on which an event has occurred
 * @event: details of the event
 *
 * Called with @id's mutex held. Returns 1 if caller should
 * destroy @id, otherwise 0.
 */
static int
rpcrdma_cm_event_handler(struct rdma_cm_id *id, struct rdma_cm_event *event)
{
	struct rpcrdma_xprt *r_xprt = id->context;
	struct rpcrdma_ia *ia = &r_xprt->rx_ia;
	struct rpcrdma_ep *ep = &r_xprt->rx_ep;
	struct rpc_xprt *xprt = &r_xprt->rx_xprt;

	might_sleep();

	trace_xprtrdma_cm_event(r_xprt, event);
	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		ia->ri_async_rc = 0;
		complete(&ia->ri_done);
		return 0;
	case RDMA_CM_EVENT_ADDR_ERROR:
		ia->ri_async_rc = -EPROTO;
		complete(&ia->ri_done);
		return 0;
	case RDMA_CM_EVENT_ROUTE_ERROR:
		ia->ri_async_rc = -ENETUNREACH;
		complete(&ia->ri_done);
		return 0;
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
#if IS_ENABLED(CONFIG_SUNRPC_DEBUG)
		pr_info("rpcrdma: removing device %s for %s:%s\n",
			ia->ri_id->device->name,
			rpcrdma_addrstr(r_xprt), rpcrdma_portstr(r_xprt));
#endif
		init_completion(&ia->ri_remove_done);
		set_bit(RPCRDMA_IAF_REMOVING, &ia->ri_flags);
		ep->rep_connected = -ENODEV;
		xprt_force_disconnect(xprt);
		wait_for_completion(&ia->ri_remove_done);

		ia->ri_id = NULL;
		/* Return 1 to ensure the core destroys the id. */
		return 1;
	case RDMA_CM_EVENT_ESTABLISHED:
		++xprt->connect_cookie;
		ep->rep_connected = 1;
		rpcrdma_update_connect_private(r_xprt, &event->param.conn);
		wake_up_all(&ep->rep_connect_wait);
		break;
	case RDMA_CM_EVENT_CONNECT_ERROR:
		ep->rep_connected = -ENOTCONN;
		goto disconnected;
	case RDMA_CM_EVENT_UNREACHABLE:
		ep->rep_connected = -ENETUNREACH;
		goto disconnected;
	case RDMA_CM_EVENT_REJECTED:
		dprintk("rpcrdma: connection to %s:%s rejected: %s\n",
			rpcrdma_addrstr(r_xprt), rpcrdma_portstr(r_xprt),
			rdma_reject_msg(id, event->status));
		ep->rep_connected = -ECONNREFUSED;
		if (event->status == IB_CM_REJ_STALE_CONN)
			ep->rep_connected = -EAGAIN;
		goto disconnected;
	case RDMA_CM_EVENT_DISCONNECTED:
		ep->rep_connected = -ECONNABORTED;
disconnected:
		xprt_force_disconnect(xprt);
		wake_up_all(&ep->rep_connect_wait);
		break;
	default:
		break;
	}

	dprintk("RPC:       %s: %s:%s on %s/frwr: %s\n", __func__,
		rpcrdma_addrstr(r_xprt), rpcrdma_portstr(r_xprt),
		ia->ri_id->device->name, rdma_event_msg(event->event));
	return 0;
}

static struct rdma_cm_id *
rpcrdma_create_id(struct rpcrdma_xprt *xprt, struct rpcrdma_ia *ia)
{
	unsigned long wtimeout = msecs_to_jiffies(RDMA_RESOLVE_TIMEOUT) + 1;
	struct rdma_cm_id *id;
	int rc;

	trace_xprtrdma_conn_start(xprt);

	init_completion(&ia->ri_done);

	id = rdma_create_id(xprt->rx_xprt.xprt_net, rpcrdma_cm_event_handler,
			    xprt, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(id))
		return id;

	ia->ri_async_rc = -ETIMEDOUT;
	rc = rdma_resolve_addr(id, NULL,
			       (struct sockaddr *)&xprt->rx_xprt.addr,
			       RDMA_RESOLVE_TIMEOUT);
	if (rc)
		goto out;
	rc = wait_for_completion_interruptible_timeout(&ia->ri_done, wtimeout);
	if (rc < 0) {
		trace_xprtrdma_conn_tout(xprt);
		goto out;
	}

	rc = ia->ri_async_rc;
	if (rc)
		goto out;

	ia->ri_async_rc = -ETIMEDOUT;
	rc = rdma_resolve_route(id, RDMA_RESOLVE_TIMEOUT);
	if (rc)
		goto out;
	rc = wait_for_completion_interruptible_timeout(&ia->ri_done, wtimeout);
	if (rc < 0) {
		trace_xprtrdma_conn_tout(xprt);
		goto out;
	}
	rc = ia->ri_async_rc;
	if (rc)
		goto out;

	return id;

out:
	rdma_destroy_id(id);
	return ERR_PTR(rc);
}

/*
 * Exported functions.
 */

/**
 * rpcrdma_ia_open - Open and initialize an Interface Adapter.
 * @xprt: transport with IA to (re)initialize
 *
 * Returns 0 on success, negative errno if an appropriate
 * Interface Adapter could not be found and opened.
 */
int
rpcrdma_ia_open(struct rpcrdma_xprt *xprt)
{
	struct rpcrdma_ia *ia = &xprt->rx_ia;
	int rc;

	ia->ri_id = rpcrdma_create_id(xprt, ia);
	if (IS_ERR(ia->ri_id)) {
		rc = PTR_ERR(ia->ri_id);
		goto out_err;
	}

	ia->ri_pd = ib_alloc_pd(ia->ri_id->device, 0);
	if (IS_ERR(ia->ri_pd)) {
		rc = PTR_ERR(ia->ri_pd);
		pr_err("rpcrdma: ib_alloc_pd() returned %d\n", rc);
		goto out_err;
	}

	switch (xprt_rdma_memreg_strategy) {
	case RPCRDMA_FRWR:
		if (frwr_is_supported(ia->ri_id->device))
			break;
		/*FALLTHROUGH*/
	default:
		pr_err("rpcrdma: Device %s does not support memreg mode %d\n",
		       ia->ri_id->device->name, xprt_rdma_memreg_strategy);
		rc = -EINVAL;
		goto out_err;
	}

	return 0;

out_err:
	rpcrdma_ia_close(ia);
	return rc;
}

/**
 * rpcrdma_ia_remove - Handle device driver unload
 * @ia: interface adapter being removed
 *
 * Divest transport H/W resources associated with this adapter,
 * but allow it to be restored later.
 */
void
rpcrdma_ia_remove(struct rpcrdma_ia *ia)
{
	struct rpcrdma_xprt *r_xprt = container_of(ia, struct rpcrdma_xprt,
						   rx_ia);
	struct rpcrdma_ep *ep = &r_xprt->rx_ep;
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_req *req;

	cancel_work_sync(&buf->rb_refresh_worker);

	/* This is similar to rpcrdma_ep_destroy, but:
	 * - Don't cancel the connect worker.
	 * - Don't call rpcrdma_ep_disconnect, which waits
	 *   for another conn upcall, which will deadlock.
	 * - rdma_disconnect is unneeded, the underlying
	 *   connection is already gone.
	 */
	if (ia->ri_id->qp) {
		rpcrdma_xprt_drain(r_xprt);
		rdma_destroy_qp(ia->ri_id);
		ia->ri_id->qp = NULL;
	}
	ib_free_cq(ep->rep_attr.recv_cq);
	ep->rep_attr.recv_cq = NULL;
	ib_free_cq(ep->rep_attr.send_cq);
	ep->rep_attr.send_cq = NULL;

	/* The ULP is responsible for ensuring all DMA
	 * mappings and MRs are gone.
	 */
	rpcrdma_reps_unmap(r_xprt);
	list_for_each_entry(req, &buf->rb_allreqs, rl_all) {
		rpcrdma_regbuf_dma_unmap(req->rl_rdmabuf);
		rpcrdma_regbuf_dma_unmap(req->rl_sendbuf);
		rpcrdma_regbuf_dma_unmap(req->rl_recvbuf);
	}
	rpcrdma_mrs_destroy(buf);
	ib_dealloc_pd(ia->ri_pd);
	ia->ri_pd = NULL;

	/* Allow waiters to continue */
	complete(&ia->ri_remove_done);

	trace_xprtrdma_remove(r_xprt);
}

/**
 * rpcrdma_ia_close - Clean up/close an IA.
 * @ia: interface adapter to close
 *
 */
void
rpcrdma_ia_close(struct rpcrdma_ia *ia)
{
	if (ia->ri_id != NULL && !IS_ERR(ia->ri_id)) {
		if (ia->ri_id->qp)
			rdma_destroy_qp(ia->ri_id);
		rdma_destroy_id(ia->ri_id);
	}
	ia->ri_id = NULL;

	/* If the pd is still busy, xprtrdma missed freeing a resource */
	if (ia->ri_pd && !IS_ERR(ia->ri_pd))
		ib_dealloc_pd(ia->ri_pd);
	ia->ri_pd = NULL;
}

/**
 * rpcrdma_ep_create - Create unconnected endpoint
 * @r_xprt: transport to instantiate
 *
 * Returns zero on success, or a negative errno.
 */
int rpcrdma_ep_create(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_ep *ep = &r_xprt->rx_ep;
	struct rpcrdma_ia *ia = &r_xprt->rx_ia;
	struct rpcrdma_connect_private *pmsg = &ep->rep_cm_private;
	struct ib_cq *sendcq, *recvcq;
	unsigned int max_sge;
	int rc;

	ep->rep_max_requests = xprt_rdma_slot_table_entries;
	ep->rep_inline_send = xprt_rdma_max_inline_write;
	ep->rep_inline_recv = xprt_rdma_max_inline_read;

	max_sge = min_t(unsigned int, ia->ri_id->device->attrs.max_send_sge,
			RPCRDMA_MAX_SEND_SGES);
	if (max_sge < RPCRDMA_MIN_SEND_SGES) {
		pr_warn("rpcrdma: HCA provides only %d send SGEs\n", max_sge);
		return -ENOMEM;
	}
	ia->ri_max_send_sges = max_sge;

	rc = frwr_open(ia, ep);
	if (rc)
		return rc;

	ep->rep_attr.event_handler = rpcrdma_qp_event_handler;
	ep->rep_attr.qp_context = ep;
	ep->rep_attr.srq = NULL;
	ep->rep_attr.cap.max_send_sge = max_sge;
	ep->rep_attr.cap.max_recv_sge = 1;
	ep->rep_attr.cap.max_inline_data = 0;
	ep->rep_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	ep->rep_attr.qp_type = IB_QPT_RC;
	ep->rep_attr.port_num = ~0;

	dprintk("RPC:       %s: requested max: dtos: send %d recv %d; "
		"iovs: send %d recv %d\n",
		__func__,
		ep->rep_attr.cap.max_send_wr,
		ep->rep_attr.cap.max_recv_wr,
		ep->rep_attr.cap.max_send_sge,
		ep->rep_attr.cap.max_recv_sge);

	ep->rep_send_batch = ep->rep_max_requests >> 3;
	ep->rep_send_count = ep->rep_send_batch;
	init_waitqueue_head(&ep->rep_connect_wait);
	ep->rep_receive_count = 0;

	sendcq = ib_alloc_cq_any(ia->ri_id->device, NULL,
				 ep->rep_attr.cap.max_send_wr + 1,
				 IB_POLL_WORKQUEUE);
	if (IS_ERR(sendcq)) {
		rc = PTR_ERR(sendcq);
		goto out1;
	}

	recvcq = ib_alloc_cq_any(ia->ri_id->device, NULL,
				 ep->rep_attr.cap.max_recv_wr + 1,
				 IB_POLL_WORKQUEUE);
	if (IS_ERR(recvcq)) {
		rc = PTR_ERR(recvcq);
		goto out2;
	}

	ep->rep_attr.send_cq = sendcq;
	ep->rep_attr.recv_cq = recvcq;

	/* Initialize cma parameters */
	memset(&ep->rep_remote_cma, 0, sizeof(ep->rep_remote_cma));

	/* Prepare RDMA-CM private message */
	pmsg->cp_magic = rpcrdma_cmp_magic;
	pmsg->cp_version = RPCRDMA_CMP_VERSION;
	pmsg->cp_flags |= RPCRDMA_CMP_F_SND_W_INV_OK;
	pmsg->cp_send_size = rpcrdma_encode_buffer_size(ep->rep_inline_send);
	pmsg->cp_recv_size = rpcrdma_encode_buffer_size(ep->rep_inline_recv);
	ep->rep_remote_cma.private_data = pmsg;
	ep->rep_remote_cma.private_data_len = sizeof(*pmsg);

	/* Client offers RDMA Read but does not initiate */
	ep->rep_remote_cma.initiator_depth = 0;
	ep->rep_remote_cma.responder_resources =
		min_t(int, U8_MAX, ia->ri_id->device->attrs.max_qp_rd_atom);

	/* Limit transport retries so client can detect server
	 * GID changes quickly. RPC layer handles re-establishing
	 * transport connection and retransmission.
	 */
	ep->rep_remote_cma.retry_count = 6;

	/* RPC-over-RDMA handles its own flow control. In addition,
	 * make all RNR NAKs visible so we know that RPC-over-RDMA
	 * flow control is working correctly (no NAKs should be seen).
	 */
	ep->rep_remote_cma.flow_control = 0;
	ep->rep_remote_cma.rnr_retry_count = 0;

	return 0;

out2:
	ib_free_cq(sendcq);
out1:
	return rc;
}

/**
 * rpcrdma_ep_destroy - Disconnect and destroy endpoint.
 * @r_xprt: transport instance to shut down
 *
 */
void rpcrdma_ep_destroy(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_ep *ep = &r_xprt->rx_ep;
	struct rpcrdma_ia *ia = &r_xprt->rx_ia;

	if (ia->ri_id && ia->ri_id->qp) {
		rpcrdma_ep_disconnect(ep, ia);
		rdma_destroy_qp(ia->ri_id);
		ia->ri_id->qp = NULL;
	}

	if (ep->rep_attr.recv_cq)
		ib_free_cq(ep->rep_attr.recv_cq);
	if (ep->rep_attr.send_cq)
		ib_free_cq(ep->rep_attr.send_cq);
}

/* Re-establish a connection after a device removal event.
 * Unlike a normal reconnection, a fresh PD and a new set
 * of MRs and buffers is needed.
 */
static int rpcrdma_ep_recreate_xprt(struct rpcrdma_xprt *r_xprt,
				    struct ib_qp_init_attr *qp_init_attr)
{
	struct rpcrdma_ia *ia = &r_xprt->rx_ia;
	struct rpcrdma_ep *ep = &r_xprt->rx_ep;
	int rc, err;

	trace_xprtrdma_reinsert(r_xprt);

	rc = -EHOSTUNREACH;
	if (rpcrdma_ia_open(r_xprt))
		goto out1;

	rc = -ENOMEM;
	err = rpcrdma_ep_create(r_xprt);
	if (err) {
		pr_err("rpcrdma: rpcrdma_ep_create returned %d\n", err);
		goto out2;
	}
	memcpy(qp_init_attr, &ep->rep_attr, sizeof(*qp_init_attr));

	rc = -ENETUNREACH;
	err = rdma_create_qp(ia->ri_id, ia->ri_pd, qp_init_attr);
	if (err) {
		pr_err("rpcrdma: rdma_create_qp returned %d\n", err);
		goto out3;
	}

	rpcrdma_mrs_create(r_xprt);
	return 0;

out3:
	rpcrdma_ep_destroy(r_xprt);
out2:
	rpcrdma_ia_close(ia);
out1:
	return rc;
}

static int rpcrdma_ep_reconnect(struct rpcrdma_xprt *r_xprt,
				struct ib_qp_init_attr *qp_init_attr)
{
	struct rpcrdma_ia *ia = &r_xprt->rx_ia;
	struct rdma_cm_id *id, *old;
	int err, rc;

	trace_xprtrdma_reconnect(r_xprt);

	rpcrdma_ep_disconnect(&r_xprt->rx_ep, ia);

	rc = -EHOSTUNREACH;
	id = rpcrdma_create_id(r_xprt, ia);
	if (IS_ERR(id))
		goto out;

	/* As long as the new ID points to the same device as the
	 * old ID, we can reuse the transport's existing PD and all
	 * previously allocated MRs. Also, the same device means
	 * the transport's previous DMA mappings are still valid.
	 *
	 * This is a sanity check only. There should be no way these
	 * point to two different devices here.
	 */
	old = id;
	rc = -ENETUNREACH;
	if (ia->ri_id->device != id->device) {
		pr_err("rpcrdma: can't reconnect on different device!\n");
		goto out_destroy;
	}

	err = rdma_create_qp(id, ia->ri_pd, qp_init_attr);
	if (err)
		goto out_destroy;

	/* Atomically replace the transport's ID and QP. */
	rc = 0;
	old = ia->ri_id;
	ia->ri_id = id;
	rdma_destroy_qp(old);

out_destroy:
	rdma_destroy_id(old);
out:
	return rc;
}

/*
 * Connect unconnected endpoint.
 */
int
rpcrdma_ep_connect(struct rpcrdma_ep *ep, struct rpcrdma_ia *ia)
{
	struct rpcrdma_xprt *r_xprt = container_of(ia, struct rpcrdma_xprt,
						   rx_ia);
	struct rpc_xprt *xprt = &r_xprt->rx_xprt;
	struct ib_qp_init_attr qp_init_attr;
	int rc;

retry:
	memcpy(&qp_init_attr, &ep->rep_attr, sizeof(qp_init_attr));
	switch (ep->rep_connected) {
	case 0:
		dprintk("RPC:       %s: connecting...\n", __func__);
		rc = rdma_create_qp(ia->ri_id, ia->ri_pd, &qp_init_attr);
		if (rc) {
			rc = -ENETUNREACH;
			goto out_noupdate;
		}
		break;
	case -ENODEV:
		rc = rpcrdma_ep_recreate_xprt(r_xprt, &qp_init_attr);
		if (rc)
			goto out_noupdate;
		break;
	default:
		rc = rpcrdma_ep_reconnect(r_xprt, &qp_init_attr);
		if (rc)
			goto out;
	}

	ep->rep_connected = 0;
	xprt_clear_connected(xprt);

	rpcrdma_post_recvs(r_xprt, true);

	rc = rdma_connect(ia->ri_id, &ep->rep_remote_cma);
	if (rc)
		goto out;

	if (xprt->reestablish_timeout < RPCRDMA_INIT_REEST_TO)
		xprt->reestablish_timeout = RPCRDMA_INIT_REEST_TO;
	wait_event_interruptible(ep->rep_connect_wait, ep->rep_connected != 0);
	if (ep->rep_connected <= 0) {
		if (ep->rep_connected == -EAGAIN)
			goto retry;
		rc = ep->rep_connected;
		goto out;
	}

	dprintk("RPC:       %s: connected\n", __func__);

out:
	if (rc)
		ep->rep_connected = rc;

out_noupdate:
	return rc;
}

/**
 * rpcrdma_ep_disconnect - Disconnect underlying transport
 * @ep: endpoint to disconnect
 * @ia: associated interface adapter
 *
 * This is separate from destroy to facilitate the ability
 * to reconnect without recreating the endpoint.
 *
 * This call is not reentrant, and must not be made in parallel
 * on the same endpoint.
 */
void
rpcrdma_ep_disconnect(struct rpcrdma_ep *ep, struct rpcrdma_ia *ia)
{
	struct rpcrdma_xprt *r_xprt = container_of(ep, struct rpcrdma_xprt,
						   rx_ep);
	int rc;

	/* returns without wait if ID is not connected */
	rc = rdma_disconnect(ia->ri_id);
	if (!rc)
		wait_event_interruptible(ep->rep_connect_wait,
							ep->rep_connected != 1);
	else
		ep->rep_connected = rc;
	trace_xprtrdma_disconnect(r_xprt, rc);

	rpcrdma_xprt_drain(r_xprt);
	rpcrdma_reqs_reset(r_xprt);
}

/* Fixed-size circular FIFO queue. This implementation is wait-free and
 * lock-free.
 *
 * Consumer is the code path that posts Sends. This path dequeues a
 * sendctx for use by a Send operation. Multiple consumer threads
 * are serialized by the RPC transport lock, which allows only one
 * ->send_request call at a time.
 *
 * Producer is the code path that handles Send completions. This path
 * enqueues a sendctx that has been completed. Multiple producer
 * threads are serialized by the ib_poll_cq() function.
 */

/* rpcrdma_sendctxs_destroy() assumes caller has already quiesced
 * queue activity, and rpcrdma_xprt_drain has flushed all remaining
 * Send requests.
 */
static void rpcrdma_sendctxs_destroy(struct rpcrdma_buffer *buf)
{
	unsigned long i;

	for (i = 0; i <= buf->rb_sc_last; i++)
		kfree(buf->rb_sc_ctxs[i]);
	kfree(buf->rb_sc_ctxs);
}

static struct rpcrdma_sendctx *rpcrdma_sendctx_create(struct rpcrdma_ia *ia)
{
	struct rpcrdma_sendctx *sc;

	sc = kzalloc(struct_size(sc, sc_sges, ia->ri_max_send_sges),
		     GFP_KERNEL);
	if (!sc)
		return NULL;

	sc->sc_wr.wr_cqe = &sc->sc_cqe;
	sc->sc_wr.sg_list = sc->sc_sges;
	sc->sc_wr.opcode = IB_WR_SEND;
	sc->sc_cqe.done = rpcrdma_wc_send;
	return sc;
}

static int rpcrdma_sendctxs_create(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_sendctx *sc;
	unsigned long i;

	/* Maximum number of concurrent outstanding Send WRs. Capping
	 * the circular queue size stops Send Queue overflow by causing
	 * the ->send_request call to fail temporarily before too many
	 * Sends are posted.
	 */
	i = buf->rb_max_requests + RPCRDMA_MAX_BC_REQUESTS;
	dprintk("RPC:       %s: allocating %lu send_ctxs\n", __func__, i);
	buf->rb_sc_ctxs = kcalloc(i, sizeof(sc), GFP_KERNEL);
	if (!buf->rb_sc_ctxs)
		return -ENOMEM;

	buf->rb_sc_last = i - 1;
	for (i = 0; i <= buf->rb_sc_last; i++) {
		sc = rpcrdma_sendctx_create(&r_xprt->rx_ia);
		if (!sc)
			return -ENOMEM;

		sc->sc_xprt = r_xprt;
		buf->rb_sc_ctxs[i] = sc;
	}

	return 0;
}

/* The sendctx queue is not guaranteed to have a size that is a
 * power of two, thus the helpers in circ_buf.h cannot be used.
 * The other option is to use modulus (%), which can be expensive.
 */
static unsigned long rpcrdma_sendctx_next(struct rpcrdma_buffer *buf,
					  unsigned long item)
{
	return likely(item < buf->rb_sc_last) ? item + 1 : 0;
}

/**
 * rpcrdma_sendctx_get_locked - Acquire a send context
 * @r_xprt: controlling transport instance
 *
 * Returns pointer to a free send completion context; or NULL if
 * the queue is empty.
 *
 * Usage: Called to acquire an SGE array before preparing a Send WR.
 *
 * The caller serializes calls to this function (per transport), and
 * provides an effective memory barrier that flushes the new value
 * of rb_sc_head.
 */
struct rpcrdma_sendctx *rpcrdma_sendctx_get_locked(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_sendctx *sc;
	unsigned long next_head;

	next_head = rpcrdma_sendctx_next(buf, buf->rb_sc_head);

	if (next_head == READ_ONCE(buf->rb_sc_tail))
		goto out_emptyq;

	/* ORDER: item must be accessed _before_ head is updated */
	sc = buf->rb_sc_ctxs[next_head];

	/* Releasing the lock in the caller acts as a memory
	 * barrier that flushes rb_sc_head.
	 */
	buf->rb_sc_head = next_head;

	return sc;

out_emptyq:
	/* The queue is "empty" if there have not been enough Send
	 * completions recently. This is a sign the Send Queue is
	 * backing up. Cause the caller to pause and try again.
	 */
	xprt_wait_for_buffer_space(&r_xprt->rx_xprt);
	r_xprt->rx_stats.empty_sendctx_q++;
	return NULL;
}

/**
 * rpcrdma_sendctx_put_locked - Release a send context
 * @sc: send context to release
 *
 * Usage: Called from Send completion to return a sendctxt
 * to the queue.
 *
 * The caller serializes calls to this function (per transport).
 */
static void
rpcrdma_sendctx_put_locked(struct rpcrdma_sendctx *sc)
{
	struct rpcrdma_buffer *buf = &sc->sc_xprt->rx_buf;
	unsigned long next_tail;

	/* Unmap SGEs of previously completed but unsignaled
	 * Sends by walking up the queue until @sc is found.
	 */
	next_tail = buf->rb_sc_tail;
	do {
		next_tail = rpcrdma_sendctx_next(buf, next_tail);

		/* ORDER: item must be accessed _before_ tail is updated */
		rpcrdma_sendctx_unmap(buf->rb_sc_ctxs[next_tail]);

	} while (buf->rb_sc_ctxs[next_tail] != sc);

	/* Paired with READ_ONCE */
	smp_store_release(&buf->rb_sc_tail, next_tail);

	xprt_write_space(&sc->sc_xprt->rx_xprt);
}

static void
rpcrdma_mrs_create(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_ia *ia = &r_xprt->rx_ia;
	unsigned int count;

	for (count = 0; count < ia->ri_max_segs; count++) {
		struct rpcrdma_mr *mr;
		int rc;

		mr = kzalloc(sizeof(*mr), GFP_NOFS);
		if (!mr)
			break;

		rc = frwr_init_mr(ia, mr);
		if (rc) {
			kfree(mr);
			break;
		}

		mr->mr_xprt = r_xprt;

		spin_lock(&buf->rb_lock);
		rpcrdma_mr_push(mr, &buf->rb_mrs);
		list_add(&mr->mr_all, &buf->rb_all_mrs);
		spin_unlock(&buf->rb_lock);
	}

	r_xprt->rx_stats.mrs_allocated += count;
	trace_xprtrdma_createmrs(r_xprt, count);
}

static void
rpcrdma_mr_refresh_worker(struct work_struct *work)
{
	struct rpcrdma_buffer *buf = container_of(work, struct rpcrdma_buffer,
						  rb_refresh_worker);
	struct rpcrdma_xprt *r_xprt = container_of(buf, struct rpcrdma_xprt,
						   rx_buf);

	rpcrdma_mrs_create(r_xprt);
	xprt_write_space(&r_xprt->rx_xprt);
}

/**
 * rpcrdma_req_create - Allocate an rpcrdma_req object
 * @r_xprt: controlling r_xprt
 * @size: initial size, in bytes, of send and receive buffers
 * @flags: GFP flags passed to memory allocators
 *
 * Returns an allocated and fully initialized rpcrdma_req or NULL.
 */
struct rpcrdma_req *rpcrdma_req_create(struct rpcrdma_xprt *r_xprt, size_t size,
				       gfp_t flags)
{
	struct rpcrdma_buffer *buffer = &r_xprt->rx_buf;
	struct rpcrdma_regbuf *rb;
	struct rpcrdma_req *req;
	size_t maxhdrsize;

	req = kzalloc(sizeof(*req), flags);
	if (req == NULL)
		goto out1;

	/* Compute maximum header buffer size in bytes */
	maxhdrsize = rpcrdma_fixed_maxsz + 3 +
		     r_xprt->rx_ia.ri_max_segs * rpcrdma_readchunk_maxsz;
	maxhdrsize *= sizeof(__be32);
	rb = rpcrdma_regbuf_alloc(__roundup_pow_of_two(maxhdrsize),
				  DMA_TO_DEVICE, flags);
	if (!rb)
		goto out2;
	req->rl_rdmabuf = rb;
	xdr_buf_init(&req->rl_hdrbuf, rdmab_data(rb), rdmab_length(rb));

	req->rl_sendbuf = rpcrdma_regbuf_alloc(size, DMA_TO_DEVICE, flags);
	if (!req->rl_sendbuf)
		goto out3;

	req->rl_recvbuf = rpcrdma_regbuf_alloc(size, DMA_NONE, flags);
	if (!req->rl_recvbuf)
		goto out4;

	INIT_LIST_HEAD(&req->rl_free_mrs);
	INIT_LIST_HEAD(&req->rl_registered);
	spin_lock(&buffer->rb_lock);
	list_add(&req->rl_all, &buffer->rb_allreqs);
	spin_unlock(&buffer->rb_lock);
	return req;

out4:
	kfree(req->rl_sendbuf);
out3:
	kfree(req->rl_rdmabuf);
out2:
	kfree(req);
out1:
	return NULL;
}

/**
 * rpcrdma_reqs_reset - Reset all reqs owned by a transport
 * @r_xprt: controlling transport instance
 *
 * ASSUMPTION: the rb_allreqs list is stable for the duration,
 * and thus can be walked without holding rb_lock. Eg. the
 * caller is holding the transport send lock to exclude
 * device removal or disconnection.
 */
static void rpcrdma_reqs_reset(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_req *req;

	list_for_each_entry(req, &buf->rb_allreqs, rl_all) {
		/* Credits are valid only for one connection */
		req->rl_slot.rq_cong = 0;
	}
}

static struct rpcrdma_rep *rpcrdma_rep_create(struct rpcrdma_xprt *r_xprt,
					      bool temp)
{
	struct rpcrdma_rep *rep;

	rep = kzalloc(sizeof(*rep), GFP_KERNEL);
	if (rep == NULL)
		goto out;

	rep->rr_rdmabuf = rpcrdma_regbuf_alloc(r_xprt->rx_ep.rep_inline_recv,
					       DMA_FROM_DEVICE, GFP_KERNEL);
	if (!rep->rr_rdmabuf)
		goto out_free;

	xdr_buf_init(&rep->rr_hdrbuf, rdmab_data(rep->rr_rdmabuf),
		     rdmab_length(rep->rr_rdmabuf));
	rep->rr_cqe.done = rpcrdma_wc_receive;
	rep->rr_rxprt = r_xprt;
	rep->rr_recv_wr.next = NULL;
	rep->rr_recv_wr.wr_cqe = &rep->rr_cqe;
	rep->rr_recv_wr.sg_list = &rep->rr_rdmabuf->rg_iov;
	rep->rr_recv_wr.num_sge = 1;
	rep->rr_temp = temp;
	list_add(&rep->rr_all, &r_xprt->rx_buf.rb_all_reps);
	return rep;

out_free:
	kfree(rep);
out:
	return NULL;
}

static void rpcrdma_rep_destroy(struct rpcrdma_rep *rep)
{
	list_del(&rep->rr_all);
	rpcrdma_regbuf_free(rep->rr_rdmabuf);
	kfree(rep);
}

static struct rpcrdma_rep *rpcrdma_rep_get_locked(struct rpcrdma_buffer *buf)
{
	struct llist_node *node;

	/* Calls to llist_del_first are required to be serialized */
	node = llist_del_first(&buf->rb_free_reps);
	if (!node)
		return NULL;
	return llist_entry(node, struct rpcrdma_rep, rr_node);
}

static void rpcrdma_rep_put(struct rpcrdma_buffer *buf,
			    struct rpcrdma_rep *rep)
{
	llist_add(&rep->rr_node, &buf->rb_free_reps);
}

static void rpcrdma_reps_unmap(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_rep *rep;

	list_for_each_entry(rep, &buf->rb_all_reps, rr_all)
		rpcrdma_regbuf_dma_unmap(rep->rr_rdmabuf);
}

static void rpcrdma_reps_destroy(struct rpcrdma_buffer *buf)
{
	struct rpcrdma_rep *rep;

	while ((rep = rpcrdma_rep_get_locked(buf)) != NULL)
		rpcrdma_rep_destroy(rep);
}

/**
 * rpcrdma_buffer_create - Create initial set of req/rep objects
 * @r_xprt: transport instance to (re)initialize
 *
 * Returns zero on success, otherwise a negative errno.
 */
int rpcrdma_buffer_create(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	int i, rc;

	buf->rb_max_requests = r_xprt->rx_ep.rep_max_requests;
	buf->rb_bc_srv_max_requests = 0;
	spin_lock_init(&buf->rb_lock);
	INIT_LIST_HEAD(&buf->rb_mrs);
	INIT_LIST_HEAD(&buf->rb_all_mrs);
	INIT_WORK(&buf->rb_refresh_worker, rpcrdma_mr_refresh_worker);

	rpcrdma_mrs_create(r_xprt);

	INIT_LIST_HEAD(&buf->rb_send_bufs);
	INIT_LIST_HEAD(&buf->rb_allreqs);
	INIT_LIST_HEAD(&buf->rb_all_reps);

	rc = -ENOMEM;
	for (i = 0; i < buf->rb_max_requests; i++) {
		struct rpcrdma_req *req;

		req = rpcrdma_req_create(r_xprt, RPCRDMA_V1_DEF_INLINE_SIZE,
					 GFP_KERNEL);
		if (!req)
			goto out;
		list_add(&req->rl_list, &buf->rb_send_bufs);
	}

	buf->rb_credits = 1;
	init_llist_head(&buf->rb_free_reps);

	rc = rpcrdma_sendctxs_create(r_xprt);
	if (rc)
		goto out;

	return 0;
out:
	rpcrdma_buffer_destroy(buf);
	return rc;
}

/**
 * rpcrdma_req_destroy - Destroy an rpcrdma_req object
 * @req: unused object to be destroyed
 *
 * This function assumes that the caller prevents concurrent device
 * unload and transport tear-down.
 */
void rpcrdma_req_destroy(struct rpcrdma_req *req)
{
	struct rpcrdma_mr *mr;

	list_del(&req->rl_all);

	while ((mr = rpcrdma_mr_pop(&req->rl_free_mrs))) {
		struct rpcrdma_buffer *buf = &mr->mr_xprt->rx_buf;

		spin_lock(&buf->rb_lock);
		list_del(&mr->mr_all);
		spin_unlock(&buf->rb_lock);

		frwr_release_mr(mr);
	}

	rpcrdma_regbuf_free(req->rl_recvbuf);
	rpcrdma_regbuf_free(req->rl_sendbuf);
	rpcrdma_regbuf_free(req->rl_rdmabuf);
	kfree(req);
}

/**
 * rpcrdma_mrs_destroy - Release all of a transport's MRs
 * @buf: controlling buffer instance
 *
 * Relies on caller holding the transport send lock to protect
 * removing mr->mr_list from req->rl_free_mrs safely.
 */
static void rpcrdma_mrs_destroy(struct rpcrdma_buffer *buf)
{
	struct rpcrdma_xprt *r_xprt = container_of(buf, struct rpcrdma_xprt,
						   rx_buf);
	struct rpcrdma_mr *mr;

	spin_lock(&buf->rb_lock);
	while ((mr = list_first_entry_or_null(&buf->rb_all_mrs,
					      struct rpcrdma_mr,
					      mr_all)) != NULL) {
		list_del(&mr->mr_list);
		list_del(&mr->mr_all);
		spin_unlock(&buf->rb_lock);

		frwr_release_mr(mr);
		spin_lock(&buf->rb_lock);
	}
	spin_unlock(&buf->rb_lock);
	r_xprt->rx_stats.mrs_allocated = 0;
}

/**
 * rpcrdma_buffer_destroy - Release all hw resources
 * @buf: root control block for resources
 *
 * ORDERING: relies on a prior rpcrdma_xprt_drain :
 * - No more Send or Receive completions can occur
 * - All MRs, reps, and reqs are returned to their free lists
 */
void
rpcrdma_buffer_destroy(struct rpcrdma_buffer *buf)
{
	cancel_work_sync(&buf->rb_refresh_worker);

	rpcrdma_sendctxs_destroy(buf);
	rpcrdma_reps_destroy(buf);

	while (!list_empty(&buf->rb_send_bufs)) {
		struct rpcrdma_req *req;

		req = list_first_entry(&buf->rb_send_bufs,
				       struct rpcrdma_req, rl_list);
		list_del(&req->rl_list);
		rpcrdma_req_destroy(req);
	}

	rpcrdma_mrs_destroy(buf);
}

/**
 * rpcrdma_mr_get - Allocate an rpcrdma_mr object
 * @r_xprt: controlling transport
 *
 * Returns an initialized rpcrdma_mr or NULL if no free
 * rpcrdma_mr objects are available.
 */
struct rpcrdma_mr *
rpcrdma_mr_get(struct rpcrdma_xprt *r_xprt)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_mr *mr;

	spin_lock(&buf->rb_lock);
	mr = rpcrdma_mr_pop(&buf->rb_mrs);
	spin_unlock(&buf->rb_lock);
	return mr;
}

/**
 * rpcrdma_mr_put - DMA unmap an MR and release it
 * @mr: MR to release
 *
 */
void rpcrdma_mr_put(struct rpcrdma_mr *mr)
{
	struct rpcrdma_xprt *r_xprt = mr->mr_xprt;

	if (mr->mr_dir != DMA_NONE) {
		trace_xprtrdma_mr_unmap(mr);
		ib_dma_unmap_sg(r_xprt->rx_ia.ri_id->device,
				mr->mr_sg, mr->mr_nents, mr->mr_dir);
		mr->mr_dir = DMA_NONE;
	}

	rpcrdma_mr_push(mr, &mr->mr_req->rl_free_mrs);
}

/**
 * rpcrdma_buffer_get - Get a request buffer
 * @buffers: Buffer pool from which to obtain a buffer
 *
 * Returns a fresh rpcrdma_req, or NULL if none are available.
 */
struct rpcrdma_req *
rpcrdma_buffer_get(struct rpcrdma_buffer *buffers)
{
	struct rpcrdma_req *req;

	spin_lock(&buffers->rb_lock);
	req = list_first_entry_or_null(&buffers->rb_send_bufs,
				       struct rpcrdma_req, rl_list);
	if (req)
		list_del_init(&req->rl_list);
	spin_unlock(&buffers->rb_lock);
	return req;
}

/**
 * rpcrdma_buffer_put - Put request/reply buffers back into pool
 * @buffers: buffer pool
 * @req: object to return
 *
 */
void rpcrdma_buffer_put(struct rpcrdma_buffer *buffers, struct rpcrdma_req *req)
{
	if (req->rl_reply)
		rpcrdma_rep_put(buffers, req->rl_reply);
	req->rl_reply = NULL;

	spin_lock(&buffers->rb_lock);
	list_add(&req->rl_list, &buffers->rb_send_bufs);
	spin_unlock(&buffers->rb_lock);
}

/**
 * rpcrdma_recv_buffer_put - Release rpcrdma_rep back to free list
 * @rep: rep to release
 *
 * Used after error conditions.
 */
void rpcrdma_recv_buffer_put(struct rpcrdma_rep *rep)
{
	rpcrdma_rep_put(&rep->rr_rxprt->rx_buf, rep);
}

/* Returns a pointer to a rpcrdma_regbuf object, or NULL.
 *
 * xprtrdma uses a regbuf for posting an outgoing RDMA SEND, or for
 * receiving the payload of RDMA RECV operations. During Long Calls
 * or Replies they may be registered externally via frwr_map.
 */
static struct rpcrdma_regbuf *
rpcrdma_regbuf_alloc(size_t size, enum dma_data_direction direction,
		     gfp_t flags)
{
	struct rpcrdma_regbuf *rb;

	rb = kmalloc(sizeof(*rb), flags);
	if (!rb)
		return NULL;
	rb->rg_data = kmalloc(size, flags);
	if (!rb->rg_data) {
		kfree(rb);
		return NULL;
	}

	rb->rg_device = NULL;
	rb->rg_direction = direction;
	rb->rg_iov.length = size;
	return rb;
}

/**
 * rpcrdma_regbuf_realloc - re-allocate a SEND/RECV buffer
 * @rb: regbuf to reallocate
 * @size: size of buffer to be allocated, in bytes
 * @flags: GFP flags
 *
 * Returns true if reallocation was successful. If false is
 * returned, @rb is left untouched.
 */
bool rpcrdma_regbuf_realloc(struct rpcrdma_regbuf *rb, size_t size, gfp_t flags)
{
	void *buf;

	buf = kmalloc(size, flags);
	if (!buf)
		return false;

	rpcrdma_regbuf_dma_unmap(rb);
	kfree(rb->rg_data);

	rb->rg_data = buf;
	rb->rg_iov.length = size;
	return true;
}

/**
 * __rpcrdma_regbuf_dma_map - DMA-map a regbuf
 * @r_xprt: controlling transport instance
 * @rb: regbuf to be mapped
 *
 * Returns true if the buffer is now DMA mapped to @r_xprt's device
 */
bool __rpcrdma_regbuf_dma_map(struct rpcrdma_xprt *r_xprt,
			      struct rpcrdma_regbuf *rb)
{
	struct ib_device *device = r_xprt->rx_ia.ri_id->device;

	if (rb->rg_direction == DMA_NONE)
		return false;

	rb->rg_iov.addr = ib_dma_map_single(device, rdmab_data(rb),
					    rdmab_length(rb), rb->rg_direction);
	if (ib_dma_mapping_error(device, rdmab_addr(rb))) {
		trace_xprtrdma_dma_maperr(rdmab_addr(rb));
		return false;
	}

	rb->rg_device = device;
	rb->rg_iov.lkey = r_xprt->rx_ia.ri_pd->local_dma_lkey;
	return true;
}

static void rpcrdma_regbuf_dma_unmap(struct rpcrdma_regbuf *rb)
{
	if (!rb)
		return;

	if (!rpcrdma_regbuf_is_mapped(rb))
		return;

	ib_dma_unmap_single(rb->rg_device, rdmab_addr(rb), rdmab_length(rb),
			    rb->rg_direction);
	rb->rg_device = NULL;
}

static void rpcrdma_regbuf_free(struct rpcrdma_regbuf *rb)
{
	rpcrdma_regbuf_dma_unmap(rb);
	if (rb)
		kfree(rb->rg_data);
	kfree(rb);
}

/**
 * rpcrdma_ep_post - Post WRs to a transport's Send Queue
 * @ia: transport's device information
 * @ep: transport's RDMA endpoint information
 * @req: rpcrdma_req containing the Send WR to post
 *
 * Returns 0 if the post was successful, otherwise -ENOTCONN
 * is returned.
 */
int
rpcrdma_ep_post(struct rpcrdma_ia *ia,
		struct rpcrdma_ep *ep,
		struct rpcrdma_req *req)
{
	struct ib_send_wr *send_wr = &req->rl_sendctx->sc_wr;
	int rc;

	if (!ep->rep_send_count || kref_read(&req->rl_kref) > 1) {
		send_wr->send_flags |= IB_SEND_SIGNALED;
		ep->rep_send_count = ep->rep_send_batch;
	} else {
		send_wr->send_flags &= ~IB_SEND_SIGNALED;
		--ep->rep_send_count;
	}

	rc = frwr_send(ia, req);
	trace_xprtrdma_post_send(req, rc);
	if (rc)
		return -ENOTCONN;
	return 0;
}

/**
 * rpcrdma_post_recvs - Refill the Receive Queue
 * @r_xprt: controlling transport instance
 * @temp: mark Receive buffers to be deleted after use
 *
 */
void rpcrdma_post_recvs(struct rpcrdma_xprt *r_xprt, bool temp)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_ep *ep = &r_xprt->rx_ep;
	struct ib_recv_wr *i, *wr, *bad_wr;
	struct rpcrdma_rep *rep;
	int needed, count, rc;

	rc = 0;
	count = 0;

	needed = buf->rb_credits + (buf->rb_bc_srv_max_requests << 1);
	if (likely(ep->rep_receive_count > needed))
		goto out;
	needed -= ep->rep_receive_count;
	if (!temp)
		needed += RPCRDMA_MAX_RECV_BATCH;

	/* fast path: all needed reps can be found on the free list */
	wr = NULL;
	while (needed) {
		rep = rpcrdma_rep_get_locked(buf);
		if (rep && rep->rr_temp) {
			rpcrdma_rep_destroy(rep);
			continue;
		}
		if (!rep)
			rep = rpcrdma_rep_create(r_xprt, temp);
		if (!rep)
			break;

		rep->rr_recv_wr.next = wr;
		wr = &rep->rr_recv_wr;
		--needed;
	}
	if (!wr)
		goto out;

	for (i = wr; i; i = i->next) {
		rep = container_of(i, struct rpcrdma_rep, rr_recv_wr);

		if (!rpcrdma_regbuf_dma_map(r_xprt, rep->rr_rdmabuf))
			goto release_wrs;

		trace_xprtrdma_post_recv(rep);
		++count;
	}

	rc = ib_post_recv(r_xprt->rx_ia.ri_id->qp, wr,
			  (const struct ib_recv_wr **)&bad_wr);
out:
	trace_xprtrdma_post_recvs(r_xprt, count, rc);
	if (rc) {
		for (wr = bad_wr; wr;) {
			struct rpcrdma_rep *rep;

			rep = container_of(wr, struct rpcrdma_rep, rr_recv_wr);
			wr = wr->next;
			rpcrdma_recv_buffer_put(rep);
			--count;
		}
	}
	ep->rep_receive_count += count;
	return;

release_wrs:
	for (i = wr; i;) {
		rep = container_of(i, struct rpcrdma_rep, rr_recv_wr);
		i = i->next;
		rpcrdma_recv_buffer_put(rep);
	}
}
