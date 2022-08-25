// SPDX-License-Identifier: GPL-2.0
/*
 * Shared application/kernel submission and completion ring pairs, for
 * supporting fast/efficient IO.
 *
 * A note on the read/write ordering memory barriers that are matched between
 * the application and kernel side.
 *
 * After the application reads the CQ ring tail, it must use an
 * appropriate smp_rmb() to pair with the smp_wmb() the kernel uses
 * before writing the tail (using smp_load_acquire to read the tail will
 * do). It also needs a smp_mb() before updating CQ head (ordering the
 * entry load(s) with the head store), pairing with an implicit barrier
 * through a control-dependency in io_get_cqring (smp_store_release to
 * store head will do). Failure to do so could lead to reading invalid
 * CQ entries.
 *
 * Likewise, the application must use an appropriate smp_wmb() before
 * writing the SQ tail (ordering SQ entry stores with the tail store),
 * which pairs with smp_load_acquire in io_get_sqring (smp_store_release
 * to store the tail will do). And it needs a barrier ordering the SQ
 * head load before writing new SQ entries (smp_load_acquire to read
 * head will do).
 *
 * When using the SQ poll thread (IORING_SETUP_SQPOLL), the application
 * needs to check the SQ flags for IORING_SQ_NEED_WAKEUP *after*
 * updating the SQ tail; a full memory barrier smp_mb() is needed
 * between.
 *
 * Also see the examples in the liburing library:
 *
 *	git://git.kernel.dk/liburing
 *
 * io_uring also uses READ/WRITE_ONCE() for _any_ store or load that happens
 * from data shared between the kernel and application. This is done both
 * for ordering purposes, but also to ensure that once a value is loaded from
 * data that the application could potentially modify, it remains stable.
 *
 * Copyright (C) 2018-2019 Jens Axboe
 * Copyright (c) 2018-2019 Christoph Hellwig
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/syscalls.h>
#include <linux/compat.h>
#include <linux/refcount.h>
#include <linux/uio.h>

#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmu_context.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/blkdev.h>
#include <linux/bvec.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/af_unix.h>
#include <net/scm.h>
#include <linux/anon_inodes.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/nospec.h>
#include <linux/sizes.h>
#include <linux/hugetlb.h>
#include <linux/highmem.h>
#include <linux/fs_struct.h>

#include <uapi/linux/io_uring.h>

#include "internal.h"

#define IORING_MAX_ENTRIES	32768
#define IORING_MAX_FIXED_FILES	1024

struct io_uring {
	u32 head ____cacheline_aligned_in_smp;
	u32 tail ____cacheline_aligned_in_smp;
};

/*
 * This data is shared with the application through the mmap at offsets
 * IORING_OFF_SQ_RING and IORING_OFF_CQ_RING.
 *
 * The offsets to the member fields are published through struct
 * io_sqring_offsets when calling io_uring_setup.
 */
struct io_rings {
	/*
	 * Head and tail offsets into the ring; the offsets need to be
	 * masked to get valid indices.
	 *
	 * The kernel controls head of the sq ring and the tail of the cq ring,
	 * and the application controls tail of the sq ring and the head of the
	 * cq ring.
	 */
	struct io_uring		sq, cq;
	/*
	 * Bitmasks to apply to head and tail offsets (constant, equals
	 * ring_entries - 1)
	 */
	u32			sq_ring_mask, cq_ring_mask;
	/* Ring sizes (constant, power of 2) */
	u32			sq_ring_entries, cq_ring_entries;
	/*
	 * Number of invalid entries dropped by the kernel due to
	 * invalid index stored in array
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application (i.e. get number of "new events" by comparing to
	 * cached value).
	 *
	 * After a new SQ head value was read by the application this
	 * counter includes all submissions that were dropped reaching
	 * the new SQ head (and possibly more).
	 */
	u32			sq_dropped;
	/*
	 * Runtime flags
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application.
	 *
	 * The application needs a full memory barrier before checking
	 * for IORING_SQ_NEED_WAKEUP after updating the sq tail.
	 */
	u32			sq_flags;
	/*
	 * Number of completion events lost because the queue was full;
	 * this should be avoided by the application by making sure
	 * there are not more requests pending thatn there is space in
	 * the completion queue.
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application (i.e. get number of "new events" by comparing to
	 * cached value).
	 *
	 * As completion events come in out of order this counter is not
	 * ordered with any other data.
	 */
	u32			cq_overflow;
	/*
	 * Ring buffer of completion events.
	 *
	 * The kernel writes completion events fresh every time they are
	 * produced, so the application is allowed to modify pending
	 * entries.
	 */
	struct io_uring_cqe	cqes[] ____cacheline_aligned_in_smp;
};

struct io_mapped_ubuf {
	u64		ubuf;
	size_t		len;
	struct		bio_vec *bvec;
	unsigned int	nr_bvecs;
};

struct async_list {
	spinlock_t		lock;
	atomic_t		cnt;
	struct list_head	list;

	struct file		*file;
	off_t			io_start;
	size_t			io_len;
};

struct io_ring_ctx {
	struct {
		struct percpu_ref	refs;
	} ____cacheline_aligned_in_smp;

	struct {
		unsigned int		flags;
		bool			compat;
		bool			account_mem;

		/*
		 * Ring buffer of indices into array of io_uring_sqe, which is
		 * mmapped by the application using the IORING_OFF_SQES offset.
		 *
		 * This indirection could e.g. be used to assign fixed
		 * io_uring_sqe entries to operations and only submit them to
		 * the queue when needed.
		 *
		 * The kernel modifies neither the indices array nor the entries
		 * array.
		 */
		u32			*sq_array;
		unsigned		cached_sq_head;
		unsigned		sq_entries;
		unsigned		sq_mask;
		unsigned		sq_thread_idle;
		unsigned		cached_sq_dropped;
		struct io_uring_sqe	*sq_sqes;

		struct list_head	defer_list;
		struct list_head	timeout_list;
	} ____cacheline_aligned_in_smp;

	/* IO offload */
	struct workqueue_struct	*sqo_wq[2];
	struct task_struct	*sqo_thread;	/* if using sq thread polling */
	struct mm_struct	*sqo_mm;
	wait_queue_head_t	sqo_wait;
	struct completion	sqo_thread_started;

	struct {
		unsigned		cached_cq_tail;
		atomic_t		cached_cq_overflow;
		unsigned		cq_entries;
		unsigned		cq_mask;
		struct wait_queue_head	cq_wait;
		struct fasync_struct	*cq_fasync;
		struct eventfd_ctx	*cq_ev_fd;
		atomic_t		cq_timeouts;
	} ____cacheline_aligned_in_smp;

	struct io_rings	*rings;

	/*
	 * If used, fixed file set. Writers must ensure that ->refs is dead,
	 * readers must ensure that ->refs is alive as long as the file* is
	 * used. Only updated through io_uring_register(2).
	 */
	struct file		**user_files;
	unsigned		nr_user_files;

	/* if used, fixed mapped user buffers */
	unsigned		nr_user_bufs;
	struct io_mapped_ubuf	*user_bufs;

	struct user_struct	*user;

	const struct cred	*creds;

	struct completion	ctx_done;

	struct {
		struct mutex		uring_lock;
		wait_queue_head_t	wait;
	} ____cacheline_aligned_in_smp;

	struct {
		spinlock_t		completion_lock;
		bool			poll_multi_file;
		/*
		 * ->poll_list is protected by the ctx->uring_lock for
		 * io_uring instances that don't use IORING_SETUP_SQPOLL.
		 * For SQPOLL, only the single threaded io_sq_thread() will
		 * manipulate the list, hence no extra locking is needed there.
		 */
		struct list_head	poll_list;
		struct list_head	cancel_list;
	} ____cacheline_aligned_in_smp;

	struct async_list	pending_async[2];

#if defined(CONFIG_UNIX)
	struct socket		*ring_sock;
#endif

	struct list_head	task_list;
	spinlock_t		task_lock;
};

struct sqe_submit {
	const struct io_uring_sqe	*sqe;
	unsigned short			index;
	u32				sequence;
	bool				has_user;
	bool				needs_lock;
	bool				needs_fixed_file;
	u8				opcode;
};

/*
 * First field must be the file pointer in all the
 * iocb unions! See also 'struct kiocb' in <linux/fs.h>
 */
struct io_poll_iocb {
	struct file			*file;
	struct wait_queue_head		*head;
	__poll_t			events;
	bool				done;
	bool				canceled;
	struct wait_queue_entry		wait;
};

struct io_timeout {
	struct file			*file;
	struct hrtimer			timer;
};

/*
 * NOTE! Each of the iocb union members has the file pointer
 * as the first entry in their struct definition. So you can
 * access the file pointer through any of the sub-structs,
 * or directly as just 'ki_filp' in this struct.
 */
struct io_kiocb {
	union {
		struct file		*file;
		struct kiocb		rw;
		struct io_poll_iocb	poll;
		struct io_timeout	timeout;
	};

	struct sqe_submit	submit;

	struct io_ring_ctx	*ctx;
	struct list_head	list;
	struct list_head	link_list;
	unsigned int		flags;
	refcount_t		refs;
#define REQ_F_NOWAIT		1	/* must not punt to workers */
#define REQ_F_IOPOLL_COMPLETED	2	/* polled IO has completed */
#define REQ_F_FIXED_FILE	4	/* ctx owns file */
#define REQ_F_SEQ_PREV		8	/* sequential with previous */
#define REQ_F_IO_DRAIN		16	/* drain existing IO first */
#define REQ_F_IO_DRAINED	32	/* drain done */
#define REQ_F_LINK		64	/* linked sqes */
#define REQ_F_LINK_DONE		128	/* linked sqes done */
#define REQ_F_FAIL_LINK		256	/* fail rest of links */
#define REQ_F_SHADOW_DRAIN	512	/* link-drain shadow req */
#define REQ_F_TIMEOUT		1024	/* timeout request */
#define REQ_F_ISREG		2048	/* regular file */
#define REQ_F_MUST_PUNT		4096	/* must be punted even for NONBLOCK */
#define REQ_F_TIMEOUT_NOSEQ	8192	/* no timeout sequence */
#define REQ_F_CANCEL		16384	/* cancel request */
	unsigned long		fsize;
	u64			user_data;
	u32			result;
	u32			sequence;
	struct files_struct	*files;

	struct fs_struct	*fs;

	struct work_struct	work;
	struct task_struct	*work_task;
	struct list_head	task_list;
};

#define IO_PLUG_THRESHOLD		2
#define IO_IOPOLL_BATCH			8

struct io_submit_state {
	struct blk_plug		plug;

	/*
	 * io_kiocb alloc cache
	 */
	void			*reqs[IO_IOPOLL_BATCH];
	unsigned		int free_reqs;
	unsigned		int cur_req;

	/*
	 * File reference cache
	 */
	struct file		*file;
	unsigned int		fd;
	unsigned int		has_refs;
	unsigned int		used_refs;
	unsigned int		ios_left;
};

static void io_sq_wq_submit_work(struct work_struct *work);
static void io_cqring_fill_event(struct io_ring_ctx *ctx, u64 ki_user_data,
				 long res);
static void __io_free_req(struct io_kiocb *req);

static struct kmem_cache *req_cachep;

static const struct file_operations io_uring_fops;

struct sock *io_uring_get_socket(struct file *file)
{
#if defined(CONFIG_UNIX)
	if (file->f_op == &io_uring_fops) {
		struct io_ring_ctx *ctx = file->private_data;

		return ctx->ring_sock->sk;
	}
#endif
	return NULL;
}
EXPORT_SYMBOL(io_uring_get_socket);

static void io_ring_ctx_ref_free(struct percpu_ref *ref)
{
	struct io_ring_ctx *ctx = container_of(ref, struct io_ring_ctx, refs);

	complete(&ctx->ctx_done);
}

static struct io_ring_ctx *io_ring_ctx_alloc(struct io_uring_params *p)
{
	struct io_ring_ctx *ctx;
	int i;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	if (percpu_ref_init(&ctx->refs, io_ring_ctx_ref_free,
			    PERCPU_REF_ALLOW_REINIT, GFP_KERNEL)) {
		kfree(ctx);
		return NULL;
	}

	ctx->flags = p->flags;
	init_waitqueue_head(&ctx->sqo_wait);
	init_waitqueue_head(&ctx->cq_wait);
	init_completion(&ctx->ctx_done);
	init_completion(&ctx->sqo_thread_started);
	mutex_init(&ctx->uring_lock);
	init_waitqueue_head(&ctx->wait);
	for (i = 0; i < ARRAY_SIZE(ctx->pending_async); i++) {
		spin_lock_init(&ctx->pending_async[i].lock);
		INIT_LIST_HEAD(&ctx->pending_async[i].list);
		atomic_set(&ctx->pending_async[i].cnt, 0);
	}
	spin_lock_init(&ctx->completion_lock);
	INIT_LIST_HEAD(&ctx->poll_list);
	INIT_LIST_HEAD(&ctx->cancel_list);
	INIT_LIST_HEAD(&ctx->defer_list);
	INIT_LIST_HEAD(&ctx->timeout_list);
	INIT_LIST_HEAD(&ctx->task_list);
	spin_lock_init(&ctx->task_lock);
	return ctx;
}

static inline bool __io_sequence_defer(struct io_ring_ctx *ctx,
				       struct io_kiocb *req)
{
	return req->sequence != ctx->cached_cq_tail + ctx->cached_sq_dropped
					+ atomic_read(&ctx->cached_cq_overflow);
}

static inline bool io_sequence_defer(struct io_ring_ctx *ctx,
				     struct io_kiocb *req)
{
	if ((req->flags & (REQ_F_IO_DRAIN|REQ_F_IO_DRAINED)) != REQ_F_IO_DRAIN)
		return false;

	return __io_sequence_defer(ctx, req);
}

static struct io_kiocb *io_get_deferred_req(struct io_ring_ctx *ctx)
{
	struct io_kiocb *req;

	req = list_first_entry_or_null(&ctx->defer_list, struct io_kiocb, list);
	if (req && !io_sequence_defer(ctx, req)) {
		list_del_init(&req->list);
		return req;
	}

	return NULL;
}

static struct io_kiocb *io_get_timeout_req(struct io_ring_ctx *ctx)
{
	struct io_kiocb *req;

	req = list_first_entry_or_null(&ctx->timeout_list, struct io_kiocb, list);
	if (req) {
		if (req->flags & REQ_F_TIMEOUT_NOSEQ)
			return NULL;
		if (!__io_sequence_defer(ctx, req)) {
			list_del_init(&req->list);
			return req;
		}
	}

	return NULL;
}

static void __io_commit_cqring(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;

	if (ctx->cached_cq_tail != READ_ONCE(rings->cq.tail)) {
		/* order cqe stores with ring update */
		smp_store_release(&rings->cq.tail, ctx->cached_cq_tail);

		if (wq_has_sleeper(&ctx->cq_wait)) {
			wake_up_interruptible(&ctx->cq_wait);
			kill_fasync(&ctx->cq_fasync, SIGIO, POLL_IN);
		}
	}
}

static inline void io_queue_async_work(struct io_ring_ctx *ctx,
				       struct io_kiocb *req)
{
	unsigned long flags;
	int rw = 0;

	if (req->submit.sqe) {
		switch (req->submit.opcode) {
		case IORING_OP_WRITEV:
		case IORING_OP_WRITE_FIXED:
			rw = !(req->rw.ki_flags & IOCB_DIRECT);
			break;
		}
	}

	if (req->work.func == io_sq_wq_submit_work) {
		req->files = current->files;

		spin_lock_irqsave(&ctx->task_lock, flags);
		list_add(&req->task_list, &ctx->task_list);
		req->work_task = NULL;
		spin_unlock_irqrestore(&ctx->task_lock, flags);
	}

	queue_work(ctx->sqo_wq[rw], &req->work);
}

static void io_kill_timeout(struct io_kiocb *req)
{
	int ret;

	ret = hrtimer_try_to_cancel(&req->timeout.timer);
	if (ret != -1) {
		atomic_inc(&req->ctx->cq_timeouts);
		list_del(&req->list);
		io_cqring_fill_event(req->ctx, req->user_data, 0);
		__io_free_req(req);
	}
}

static void io_kill_timeouts(struct io_ring_ctx *ctx)
{
	struct io_kiocb *req, *tmp;

	spin_lock_irq(&ctx->completion_lock);
	list_for_each_entry_safe(req, tmp, &ctx->timeout_list, list)
		io_kill_timeout(req);
	spin_unlock_irq(&ctx->completion_lock);
}

static void io_commit_cqring(struct io_ring_ctx *ctx)
{
	struct io_kiocb *req;

	while ((req = io_get_timeout_req(ctx)) != NULL)
		io_kill_timeout(req);

	__io_commit_cqring(ctx);

	while ((req = io_get_deferred_req(ctx)) != NULL) {
		if (req->flags & REQ_F_SHADOW_DRAIN) {
			/* Just for drain, free it. */
			__io_free_req(req);
			continue;
		}
		req->flags |= REQ_F_IO_DRAINED;
		io_queue_async_work(ctx, req);
	}
}

static struct io_uring_cqe *io_get_cqring(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;
	unsigned tail;

	tail = ctx->cached_cq_tail;
	/*
	 * writes to the cq entry need to come after reading head; the
	 * control dependency is enough as we're using WRITE_ONCE to
	 * fill the cq entry
	 */
	if (tail - READ_ONCE(rings->cq.head) == rings->cq_ring_entries)
		return NULL;

	ctx->cached_cq_tail++;
	return &rings->cqes[tail & ctx->cq_mask];
}

static void io_cqring_fill_event(struct io_ring_ctx *ctx, u64 ki_user_data,
				 long res)
{
	struct io_uring_cqe *cqe;

	/*
	 * If we can't get a cq entry, userspace overflowed the
	 * submission (by quite a lot). Increment the overflow count in
	 * the ring.
	 */
	cqe = io_get_cqring(ctx);
	if (cqe) {
		WRITE_ONCE(cqe->user_data, ki_user_data);
		WRITE_ONCE(cqe->res, res);
		WRITE_ONCE(cqe->flags, 0);
	} else {
		WRITE_ONCE(ctx->rings->cq_overflow,
				atomic_inc_return(&ctx->cached_cq_overflow));
	}
}

static void io_cqring_ev_posted(struct io_ring_ctx *ctx)
{
	if (waitqueue_active(&ctx->wait))
		wake_up(&ctx->wait);
	if (waitqueue_active(&ctx->sqo_wait))
		wake_up(&ctx->sqo_wait);
	if (ctx->cq_ev_fd)
		eventfd_signal(ctx->cq_ev_fd, 1);
}

static void io_cqring_add_event(struct io_ring_ctx *ctx, u64 user_data,
				long res)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->completion_lock, flags);
	io_cqring_fill_event(ctx, user_data, res);
	io_commit_cqring(ctx);
	spin_unlock_irqrestore(&ctx->completion_lock, flags);

	io_cqring_ev_posted(ctx);
}

static struct io_kiocb *io_get_req(struct io_ring_ctx *ctx,
				   struct io_submit_state *state)
{
	gfp_t gfp = GFP_KERNEL | __GFP_NOWARN;
	struct io_kiocb *req;

	if (!percpu_ref_tryget(&ctx->refs))
		return NULL;

	if (!state) {
		req = kmem_cache_alloc(req_cachep, gfp);
		if (unlikely(!req))
			goto out;
	} else if (!state->free_reqs) {
		size_t sz;
		int ret;

		sz = min_t(size_t, state->ios_left, ARRAY_SIZE(state->reqs));
		ret = kmem_cache_alloc_bulk(req_cachep, gfp, sz, state->reqs);

		/*
		 * Bulk alloc is all-or-nothing. If we fail to get a batch,
		 * retry single alloc to be on the safe side.
		 */
		if (unlikely(ret <= 0)) {
			state->reqs[0] = kmem_cache_alloc(req_cachep, gfp);
			if (!state->reqs[0])
				goto out;
			ret = 1;
		}
		state->free_reqs = ret - 1;
		state->cur_req = 1;
		req = state->reqs[0];
	} else {
		req = state->reqs[state->cur_req];
		state->free_reqs--;
		state->cur_req++;
	}

	INIT_LIST_HEAD(&req->task_list);
	req->file = NULL;
	req->ctx = ctx;
	req->flags = 0;
	/* one is dropped after submission, the other at completion */
	refcount_set(&req->refs, 2);
	req->result = 0;
	req->fs = NULL;
	return req;
out:
	percpu_ref_put(&ctx->refs);
	return NULL;
}

static void io_free_req_many(struct io_ring_ctx *ctx, void **reqs, int *nr)
{
	if (*nr) {
		kmem_cache_free_bulk(req_cachep, *nr, reqs);
		percpu_ref_put_many(&ctx->refs, *nr);
		*nr = 0;
	}
}

static void __io_free_req(struct io_kiocb *req)
{
	if (req->file && !(req->flags & REQ_F_FIXED_FILE))
		fput(req->file);
	percpu_ref_put(&req->ctx->refs);
	kmem_cache_free(req_cachep, req);
}

static void io_req_link_next(struct io_kiocb *req)
{
	struct io_kiocb *nxt;

	/*
	 * The list should never be empty when we are called here. But could
	 * potentially happen if the chain is messed up, check to be on the
	 * safe side.
	 */
	nxt = list_first_entry_or_null(&req->link_list, struct io_kiocb, list);
	if (nxt) {
		list_del(&nxt->list);
		if (!list_empty(&req->link_list)) {
			INIT_LIST_HEAD(&nxt->link_list);
			list_splice(&req->link_list, &nxt->link_list);
			nxt->flags |= REQ_F_LINK;
		}

		nxt->flags |= REQ_F_LINK_DONE;
		INIT_WORK(&nxt->work, io_sq_wq_submit_work);
		io_queue_async_work(req->ctx, nxt);
	}
}

/*
 * Called if REQ_F_LINK is set, and we fail the head request
 */
static void io_fail_links(struct io_kiocb *req)
{
	struct io_kiocb *link;

	while (!list_empty(&req->link_list)) {
		link = list_first_entry(&req->link_list, struct io_kiocb, list);
		list_del(&link->list);

		io_cqring_add_event(req->ctx, link->user_data, -ECANCELED);
		__io_free_req(link);
	}
}

static void io_free_req(struct io_kiocb *req)
{
	/*
	 * If LINK is set, we have dependent requests in this chain. If we
	 * didn't fail this request, queue the first one up, moving any other
	 * dependencies to the next request. In case of failure, fail the rest
	 * of the chain.
	 */
	if (req->flags & REQ_F_LINK) {
		if (req->flags & REQ_F_FAIL_LINK)
			io_fail_links(req);
		else
			io_req_link_next(req);
	}

	__io_free_req(req);
}

static void io_put_req(struct io_kiocb *req)
{
	if (refcount_dec_and_test(&req->refs))
		io_free_req(req);
}

static unsigned io_cqring_events(struct io_rings *rings)
{
	/* See comment at the top of this file */
	smp_rmb();
	return READ_ONCE(rings->cq.tail) - READ_ONCE(rings->cq.head);
}

static inline unsigned int io_sqring_entries(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;

	/* make sure SQ entry isn't read before tail */
	return smp_load_acquire(&rings->sq.tail) - ctx->cached_sq_head;
}

/*
 * Find and free completed poll iocbs
 */
static void io_iopoll_complete(struct io_ring_ctx *ctx, unsigned int *nr_events,
			       struct list_head *done)
{
	void *reqs[IO_IOPOLL_BATCH];
	struct io_kiocb *req;
	int to_free;

	to_free = 0;
	while (!list_empty(done)) {
		req = list_first_entry(done, struct io_kiocb, list);
		list_del(&req->list);

		io_cqring_fill_event(ctx, req->user_data, req->result);
		(*nr_events)++;

		if (refcount_dec_and_test(&req->refs)) {
			/* If we're not using fixed files, we have to pair the
			 * completion part with the file put. Use regular
			 * completions for those, only batch free for fixed
			 * file and non-linked commands.
			 */
			if ((req->flags & (REQ_F_FIXED_FILE|REQ_F_LINK)) ==
			    REQ_F_FIXED_FILE) {
				reqs[to_free++] = req;
				if (to_free == ARRAY_SIZE(reqs))
					io_free_req_many(ctx, reqs, &to_free);
			} else {
				io_free_req(req);
			}
		}
	}

	io_commit_cqring(ctx);
	io_free_req_many(ctx, reqs, &to_free);
}

static int io_do_iopoll(struct io_ring_ctx *ctx, unsigned int *nr_events,
			long min)
{
	struct io_kiocb *req, *tmp;
	LIST_HEAD(done);
	bool spin;
	int ret;

	/*
	 * Only spin for completions if we don't have multiple devices hanging
	 * off our complete list, and we're under the requested amount.
	 */
	spin = !ctx->poll_multi_file && *nr_events < min;

	ret = 0;
	list_for_each_entry_safe(req, tmp, &ctx->poll_list, list) {
		struct kiocb *kiocb = &req->rw;

		/*
		 * Move completed entries to our local list. If we find a
		 * request that requires polling, break out and complete
		 * the done list first, if we have entries there.
		 */
		if (req->flags & REQ_F_IOPOLL_COMPLETED) {
			list_move_tail(&req->list, &done);
			continue;
		}
		if (!list_empty(&done))
			break;

		ret = kiocb->ki_filp->f_op->iopoll(kiocb, spin);
		if (ret < 0)
			break;

		if (ret && spin)
			spin = false;
		ret = 0;
	}

	if (!list_empty(&done))
		io_iopoll_complete(ctx, nr_events, &done);

	return ret;
}

/*
 * Poll for a mininum of 'min' events. Note that if min == 0 we consider that a
 * non-spinning poll check - we'll still enter the driver poll loop, but only
 * as a non-spinning completion check.
 */
static int io_iopoll_getevents(struct io_ring_ctx *ctx, unsigned int *nr_events,
				long min)
{
	while (!list_empty(&ctx->poll_list) && !need_resched()) {
		int ret;

		ret = io_do_iopoll(ctx, nr_events, min);
		if (ret < 0)
			return ret;
		if (!min || *nr_events >= min)
			return 0;
	}

	return 1;
}

/*
 * We can't just wait for polled events to come to us, we have to actively
 * find and complete them.
 */
static void io_iopoll_reap_events(struct io_ring_ctx *ctx)
{
	if (!(ctx->flags & IORING_SETUP_IOPOLL))
		return;

	mutex_lock(&ctx->uring_lock);
	while (!list_empty(&ctx->poll_list)) {
		unsigned int nr_events = 0;

		io_iopoll_getevents(ctx, &nr_events, 1);

		/*
		 * Ensure we allow local-to-the-cpu processing to take place,
		 * in this case we need to ensure that we reap all events.
		 */
		cond_resched();
	}
	mutex_unlock(&ctx->uring_lock);
}

static int io_iopoll_check(struct io_ring_ctx *ctx, unsigned *nr_events,
			   long min)
{
	int iters = 0, ret = 0;

	/*
	 * We disallow the app entering submit/complete with polling, but we
	 * still need to lock the ring to prevent racing with polled issue
	 * that got punted to a workqueue.
	 */
	mutex_lock(&ctx->uring_lock);
	do {
		int tmin = 0;

		/*
		 * Don't enter poll loop if we already have events pending.
		 * If we do, we can potentially be spinning for commands that
		 * already triggered a CQE (eg in error).
		 */
		if (io_cqring_events(ctx->rings))
			break;

		/*
		 * If a submit got punted to a workqueue, we can have the
		 * application entering polling for a command before it gets
		 * issued. That app will hold the uring_lock for the duration
		 * of the poll right here, so we need to take a breather every
		 * now and then to ensure that the issue has a chance to add
		 * the poll to the issued list. Otherwise we can spin here
		 * forever, while the workqueue is stuck trying to acquire the
		 * very same mutex.
		 */
		if (!(++iters & 7)) {
			mutex_unlock(&ctx->uring_lock);
			mutex_lock(&ctx->uring_lock);
		}

		if (*nr_events < min)
			tmin = min - *nr_events;

		ret = io_iopoll_getevents(ctx, nr_events, tmin);
		if (ret <= 0)
			break;
		ret = 0;
	} while (min && !*nr_events && !need_resched());

	mutex_unlock(&ctx->uring_lock);
	return ret;
}

static void kiocb_end_write(struct io_kiocb *req)
{
	/*
	 * Tell lockdep we inherited freeze protection from submission
	 * thread.
	 */
	if (req->flags & REQ_F_ISREG) {
		struct inode *inode = file_inode(req->file);

		__sb_writers_acquired(inode->i_sb, SB_FREEZE_WRITE);
	}
	file_end_write(req->file);
}

static void io_complete_rw(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *req = container_of(kiocb, struct io_kiocb, rw);

	if (kiocb->ki_flags & IOCB_WRITE)
		kiocb_end_write(req);

	if ((req->flags & REQ_F_LINK) && res != req->result)
		req->flags |= REQ_F_FAIL_LINK;
	io_cqring_add_event(req->ctx, req->user_data, res);
	io_put_req(req);
}

static void io_complete_rw_iopoll(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *req = container_of(kiocb, struct io_kiocb, rw);

	if (kiocb->ki_flags & IOCB_WRITE)
		kiocb_end_write(req);

	if ((req->flags & REQ_F_LINK) && res != req->result)
		req->flags |= REQ_F_FAIL_LINK;
	req->result = res;
	if (res != -EAGAIN)
		req->flags |= REQ_F_IOPOLL_COMPLETED;
}

/*
 * After the iocb has been issued, it's safe to be found on the poll list.
 * Adding the kiocb to the list AFTER submission ensures that we don't
 * find it from a io_iopoll_getevents() thread before the issuer is done
 * accessing the kiocb cookie.
 */
static void io_iopoll_req_issued(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;

	/*
	 * Track whether we have multiple files in our lists. This will impact
	 * how we do polling eventually, not spinning if we're on potentially
	 * different devices.
	 */
	if (list_empty(&ctx->poll_list)) {
		ctx->poll_multi_file = false;
	} else if (!ctx->poll_multi_file) {
		struct io_kiocb *list_req;

		list_req = list_first_entry(&ctx->poll_list, struct io_kiocb,
						list);
		if (list_req->rw.ki_filp != req->rw.ki_filp)
			ctx->poll_multi_file = true;
	}

	/*
	 * For fast devices, IO may have already completed. If it has, add
	 * it to the front so we find it first.
	 */
	if (req->flags & REQ_F_IOPOLL_COMPLETED)
		list_add(&req->list, &ctx->poll_list);
	else
		list_add_tail(&req->list, &ctx->poll_list);
}

static void io_file_put(struct io_submit_state *state)
{
	if (state->file) {
		int diff = state->has_refs - state->used_refs;

		if (diff)
			fput_many(state->file, diff);
		state->file = NULL;
	}
}

/*
 * Get as many references to a file as we have IOs left in this submission,
 * assuming most submissions are for one file, or at least that each file
 * has more than one submission.
 */
static struct file *io_file_get(struct io_submit_state *state, int fd)
{
	if (!state)
		return fget(fd);

	if (state->file) {
		if (state->fd == fd) {
			state->used_refs++;
			state->ios_left--;
			return state->file;
		}
		io_file_put(state);
	}
	state->file = fget_many(fd, state->ios_left);
	if (!state->file)
		return NULL;

	state->fd = fd;
	state->has_refs = state->ios_left;
	state->used_refs = 1;
	state->ios_left--;
	return state->file;
}

/*
 * If we tracked the file through the SCM inflight mechanism, we could support
 * any file. For now, just ensure that anything potentially problematic is done
 * inline.
 */
static bool io_file_supports_async(struct file *file)
{
	umode_t mode = file_inode(file)->i_mode;

	if (S_ISBLK(mode) || S_ISCHR(mode))
		return true;
	if (S_ISREG(mode) && file->f_op != &io_uring_fops)
		return true;

	return false;
}

static int io_prep_rw(struct io_kiocb *req, const struct sqe_submit *s,
		      bool force_nonblock)
{
	const struct io_uring_sqe *sqe = s->sqe;
	struct io_ring_ctx *ctx = req->ctx;
	struct kiocb *kiocb = &req->rw;
	unsigned ioprio;
	int ret;

	if (!req->file)
		return -EBADF;

	if (S_ISREG(file_inode(req->file)->i_mode))
		req->flags |= REQ_F_ISREG;

	if (force_nonblock)
		req->fsize = rlimit(RLIMIT_FSIZE);

	/*
	 * If the file doesn't support async, mark it as REQ_F_MUST_PUNT so
	 * we know to async punt it even if it was opened O_NONBLOCK
	 */
	if (force_nonblock && !io_file_supports_async(req->file)) {
		req->flags |= REQ_F_MUST_PUNT;
		return -EAGAIN;
	}

	kiocb->ki_pos = READ_ONCE(sqe->off);
	kiocb->ki_flags = iocb_flags(kiocb->ki_filp);
	kiocb->ki_hint = ki_hint_validate(file_write_hint(kiocb->ki_filp));

	ioprio = READ_ONCE(sqe->ioprio);
	if (ioprio) {
		ret = ioprio_check_cap(ioprio);
		if (ret)
			return ret;

		kiocb->ki_ioprio = ioprio;
	} else
		kiocb->ki_ioprio = get_current_ioprio();

	ret = kiocb_set_rw_flags(kiocb, READ_ONCE(sqe->rw_flags));
	if (unlikely(ret))
		return ret;

	/* don't allow async punt if RWF_NOWAIT was requested */
	if ((kiocb->ki_flags & IOCB_NOWAIT) ||
	    (req->file->f_flags & O_NONBLOCK))
		req->flags |= REQ_F_NOWAIT;

	if (force_nonblock)
		kiocb->ki_flags |= IOCB_NOWAIT;

	if (ctx->flags & IORING_SETUP_IOPOLL) {
		if (!(kiocb->ki_flags & IOCB_DIRECT) ||
		    !kiocb->ki_filp->f_op->iopoll)
			return -EOPNOTSUPP;

		kiocb->ki_flags |= IOCB_HIPRI;
		kiocb->ki_complete = io_complete_rw_iopoll;
		req->result = 0;
	} else {
		if (kiocb->ki_flags & IOCB_HIPRI)
			return -EINVAL;
		kiocb->ki_complete = io_complete_rw;
	}
	return 0;
}

static inline void io_rw_done(struct kiocb *kiocb, ssize_t ret)
{
	switch (ret) {
	case -EIOCBQUEUED:
		break;
	case -ERESTARTSYS:
	case -ERESTARTNOINTR:
	case -ERESTARTNOHAND:
	case -ERESTART_RESTARTBLOCK:
		/*
		 * We can't just restart the syscall, since previously
		 * submitted sqes may already be in progress. Just fail this
		 * IO with EINTR.
		 */
		ret = -EINTR;
		/* fall through */
	default:
		kiocb->ki_complete(kiocb, ret, 0);
	}
}

static int io_import_fixed(struct io_ring_ctx *ctx, int rw,
			   const struct io_uring_sqe *sqe,
			   struct iov_iter *iter)
{
	size_t len = READ_ONCE(sqe->len);
	struct io_mapped_ubuf *imu;
	unsigned index, buf_index;
	size_t offset;
	u64 buf_addr;

	/* attempt to use fixed buffers without having provided iovecs */
	if (unlikely(!ctx->user_bufs))
		return -EFAULT;

	buf_index = READ_ONCE(sqe->buf_index);
	if (unlikely(buf_index >= ctx->nr_user_bufs))
		return -EFAULT;

	index = array_index_nospec(buf_index, ctx->nr_user_bufs);
	imu = &ctx->user_bufs[index];
	buf_addr = READ_ONCE(sqe->addr);

	/* overflow */
	if (buf_addr + len < buf_addr)
		return -EFAULT;
	/* not inside the mapped region */
	if (buf_addr < imu->ubuf || buf_addr + len > imu->ubuf + imu->len)
		return -EFAULT;

	/*
	 * May not be a start of buffer, set size appropriately
	 * and advance us to the beginning.
	 */
	offset = buf_addr - imu->ubuf;
	iov_iter_bvec(iter, rw, imu->bvec, imu->nr_bvecs, offset + len);

	if (offset) {
		/*
		 * Don't use iov_iter_advance() here, as it's really slow for
		 * using the latter parts of a big fixed buffer - it iterates
		 * over each segment manually. We can cheat a bit here, because
		 * we know that:
		 *
		 * 1) it's a BVEC iter, we set it up
		 * 2) all bvecs are PAGE_SIZE in size, except potentially the
		 *    first and last bvec
		 *
		 * So just find our index, and adjust the iterator afterwards.
		 * If the offset is within the first bvec (or the whole first
		 * bvec, just use iov_iter_advance(). This makes it easier
		 * since we can just skip the first segment, which may not
		 * be PAGE_SIZE aligned.
		 */
		const struct bio_vec *bvec = imu->bvec;

		if (offset <= bvec->bv_len) {
			iov_iter_advance(iter, offset);
		} else {
			unsigned long seg_skip;

			/* skip first vec */
			offset -= bvec->bv_len;
			seg_skip = 1 + (offset >> PAGE_SHIFT);

			iter->bvec = bvec + seg_skip;
			iter->nr_segs -= seg_skip;
			iter->count -= bvec->bv_len + offset;
			iter->iov_offset = offset & ~PAGE_MASK;
		}
	}

	return len;
}

static ssize_t io_import_iovec(struct io_ring_ctx *ctx, int rw,
			       struct io_kiocb *req, struct iovec **iovec,
			       struct iov_iter *iter)
{
	const struct io_uring_sqe *sqe = req->submit.sqe;
	void __user *buf = u64_to_user_ptr(READ_ONCE(sqe->addr));
	size_t sqe_len = READ_ONCE(sqe->len);
	u8 opcode;

	opcode = req->submit.opcode;
	if (opcode == IORING_OP_READ_FIXED ||
	    opcode == IORING_OP_WRITE_FIXED) {
		ssize_t ret = io_import_fixed(ctx, rw, sqe, iter);
		*iovec = NULL;
		return ret;
	}

	if (!req->submit.has_user)
		return -EFAULT;

#ifdef CONFIG_COMPAT
	if (ctx->compat)
		return compat_import_iovec(rw, buf, sqe_len, UIO_FASTIOV,
						iovec, iter);
#endif

	return import_iovec(rw, buf, sqe_len, UIO_FASTIOV, iovec, iter);
}

static inline bool io_should_merge(struct async_list *al, struct kiocb *kiocb)
{
	if (al->file == kiocb->ki_filp) {
		off_t start, end;

		/*
		 * Allow merging if we're anywhere in the range of the same
		 * page. Generally this happens for sub-page reads or writes,
		 * and it's beneficial to allow the first worker to bring the
		 * page in and the piggy backed work can then work on the
		 * cached page.
		 */
		start = al->io_start & PAGE_MASK;
		end = (al->io_start + al->io_len + PAGE_SIZE - 1) & PAGE_MASK;
		if (kiocb->ki_pos >= start && kiocb->ki_pos <= end)
			return true;
	}

	al->file = NULL;
	return false;
}

/*
 * Make a note of the last file/offset/direction we punted to async
 * context. We'll use this information to see if we can piggy back a
 * sequential request onto the previous one, if it's still hasn't been
 * completed by the async worker.
 */
static void io_async_list_note(int rw, struct io_kiocb *req, size_t len)
{
	struct async_list *async_list = &req->ctx->pending_async[rw];
	struct kiocb *kiocb = &req->rw;
	struct file *filp = kiocb->ki_filp;

	if (io_should_merge(async_list, kiocb)) {
		unsigned long max_bytes;

		/* Use 8x RA size as a decent limiter for both reads/writes */
		max_bytes = filp->f_ra.ra_pages << (PAGE_SHIFT + 3);
		if (!max_bytes)
			max_bytes = VM_READAHEAD_PAGES << (PAGE_SHIFT + 3);

		/* If max len are exceeded, reset the state */
		if (async_list->io_len + len <= max_bytes) {
			req->flags |= REQ_F_SEQ_PREV;
			async_list->io_len += len;
		} else {
			async_list->file = NULL;
		}
	}

	/* New file? Reset state. */
	if (async_list->file != filp) {
		async_list->io_start = kiocb->ki_pos;
		async_list->io_len = len;
		async_list->file = filp;
	}
}

/*
 * For files that don't have ->read_iter() and ->write_iter(), handle them
 * by looping over ->read() or ->write() manually.
 */
static ssize_t loop_rw_iter(int rw, struct file *file, struct kiocb *kiocb,
			   struct iov_iter *iter)
{
	ssize_t ret = 0;

	/*
	 * Don't support polled IO through this interface, and we can't
	 * support non-blocking either. For the latter, this just causes
	 * the kiocb to be handled from an async context.
	 */
	if (kiocb->ki_flags & IOCB_HIPRI)
		return -EOPNOTSUPP;
	if (kiocb->ki_flags & IOCB_NOWAIT)
		return -EAGAIN;

	while (iov_iter_count(iter)) {
		struct iovec iovec;
		ssize_t nr;

		if (!iov_iter_is_bvec(iter)) {
			iovec = iov_iter_iovec(iter);
		} else {
			/* fixed buffers import bvec */
			iovec.iov_base = kmap(iter->bvec->bv_page)
						+ iter->iov_offset;
			iovec.iov_len = min(iter->count,
					iter->bvec->bv_len - iter->iov_offset);
		}

		if (rw == READ) {
			nr = file->f_op->read(file, iovec.iov_base,
					      iovec.iov_len, &kiocb->ki_pos);
		} else {
			nr = file->f_op->write(file, iovec.iov_base,
					       iovec.iov_len, &kiocb->ki_pos);
		}

		if (iov_iter_is_bvec(iter))
			kunmap(iter->bvec->bv_page);

		if (nr < 0) {
			if (!ret)
				ret = nr;
			break;
		}
		ret += nr;
		if (nr != iovec.iov_len)
			break;
		iov_iter_advance(iter, nr);
	}

	return ret;
}

static int io_read(struct io_kiocb *req, const struct sqe_submit *s,
		   bool force_nonblock)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	struct kiocb *kiocb = &req->rw;
	struct iov_iter iter;
	struct file *file;
	size_t iov_count;
	ssize_t read_size, ret;

	ret = io_prep_rw(req, s, force_nonblock);
	if (ret)
		return ret;
	file = kiocb->ki_filp;

	if (unlikely(!(file->f_mode & FMODE_READ)))
		return -EBADF;

	ret = io_import_iovec(req->ctx, READ, req, &iovec, &iter);
	if (ret < 0)
		return ret;

	read_size = ret;
	if (req->flags & REQ_F_LINK)
		req->result = read_size;

	iov_count = iov_iter_count(&iter);
	ret = rw_verify_area(READ, file, &kiocb->ki_pos, iov_count);
	if (!ret) {
		ssize_t ret2;

		if (file->f_op->read_iter)
			ret2 = call_read_iter(file, kiocb, &iter);
		else if (req->file->f_op->read)
			ret2 = loop_rw_iter(READ, file, kiocb, &iter);
		else
			ret2 = -EINVAL;

		/*
		 * In case of a short read, punt to async. This can happen
		 * if we have data partially cached. Alternatively we can
		 * return the short read, in which case the application will
		 * need to issue another SQE and wait for it. That SQE will
		 * need async punt anyway, so it's more efficient to do it
		 * here.
		 */
		if (force_nonblock && !(req->flags & REQ_F_NOWAIT) &&
		    (req->flags & REQ_F_ISREG) &&
		    ret2 > 0 && ret2 < read_size)
			ret2 = -EAGAIN;
		/* Catch -EAGAIN return for forced non-blocking submission */
		if (!force_nonblock || ret2 != -EAGAIN) {
			io_rw_done(kiocb, ret2);
		} else {
			/*
			 * If ->needs_lock is true, we're already in async
			 * context.
			 */
			if (!s->needs_lock)
				io_async_list_note(READ, req, iov_count);
			ret = -EAGAIN;
		}
	}
	kfree(iovec);
	return ret;
}

static int io_write(struct io_kiocb *req, const struct sqe_submit *s,
		    bool force_nonblock)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	struct kiocb *kiocb = &req->rw;
	struct iov_iter iter;
	struct file *file;
	size_t iov_count;
	ssize_t ret;

	ret = io_prep_rw(req, s, force_nonblock);
	if (ret)
		return ret;

	file = kiocb->ki_filp;
	if (unlikely(!(file->f_mode & FMODE_WRITE)))
		return -EBADF;

	ret = io_import_iovec(req->ctx, WRITE, req, &iovec, &iter);
	if (ret < 0)
		return ret;

	if (req->flags & REQ_F_LINK)
		req->result = ret;

	iov_count = iov_iter_count(&iter);

	ret = -EAGAIN;
	if (force_nonblock && !(kiocb->ki_flags & IOCB_DIRECT)) {
		/* If ->needs_lock is true, we're already in async context. */
		if (!s->needs_lock)
			io_async_list_note(WRITE, req, iov_count);
		goto out_free;
	}

	ret = rw_verify_area(WRITE, file, &kiocb->ki_pos, iov_count);
	if (!ret) {
		ssize_t ret2;

		/*
		 * Open-code file_start_write here to grab freeze protection,
		 * which will be released by another thread in
		 * io_complete_rw().  Fool lockdep by telling it the lock got
		 * released so that it doesn't complain about the held lock when
		 * we return to userspace.
		 */
		if (req->flags & REQ_F_ISREG) {
			__sb_start_write(file_inode(file)->i_sb,
						SB_FREEZE_WRITE, true);
			__sb_writers_release(file_inode(file)->i_sb,
						SB_FREEZE_WRITE);
		}
		kiocb->ki_flags |= IOCB_WRITE;

		if (!force_nonblock)
			current->signal->rlim[RLIMIT_FSIZE].rlim_cur = req->fsize;

		if (file->f_op->write_iter)
			ret2 = call_write_iter(file, kiocb, &iter);
		else if (req->file->f_op->write)
			ret2 = loop_rw_iter(WRITE, file, kiocb, &iter);
		else
			ret2 = -EINVAL;

		if (!force_nonblock)
			current->signal->rlim[RLIMIT_FSIZE].rlim_cur = RLIM_INFINITY;

		if (!force_nonblock || ret2 != -EAGAIN) {
			io_rw_done(kiocb, ret2);
		} else {
			/*
			 * If ->needs_lock is true, we're already in async
			 * context.
			 */
			if (!s->needs_lock)
				io_async_list_note(WRITE, req, iov_count);
			ret = -EAGAIN;
		}
	}
out_free:
	kfree(iovec);
	return ret;
}

/*
 * IORING_OP_NOP just posts a completion event, nothing else.
 */
static int io_nop(struct io_kiocb *req, u64 user_data)
{
	struct io_ring_ctx *ctx = req->ctx;
	long err = 0;

	if (unlikely(ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;

	io_cqring_add_event(ctx, user_data, err);
	io_put_req(req);
	return 0;
}

static int io_prep_fsync(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_ring_ctx *ctx = req->ctx;

	if (!req->file)
		return -EBADF;

	if (unlikely(ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;
	if (unlikely(sqe->addr || sqe->ioprio || sqe->buf_index))
		return -EINVAL;

	return 0;
}

static int io_fsync(struct io_kiocb *req, const struct io_uring_sqe *sqe,
		    bool force_nonblock)
{
	loff_t sqe_off = READ_ONCE(sqe->off);
	loff_t sqe_len = READ_ONCE(sqe->len);
	loff_t end = sqe_off + sqe_len;
	unsigned fsync_flags;
	int ret;

	fsync_flags = READ_ONCE(sqe->fsync_flags);
	if (unlikely(fsync_flags & ~IORING_FSYNC_DATASYNC))
		return -EINVAL;

	ret = io_prep_fsync(req, sqe);
	if (ret)
		return ret;

	/* fsync always requires a blocking context */
	if (force_nonblock)
		return -EAGAIN;

	ret = vfs_fsync_range(req->rw.ki_filp, sqe_off,
				end > 0 ? end : LLONG_MAX,
				fsync_flags & IORING_FSYNC_DATASYNC);

	if (ret < 0 && (req->flags & REQ_F_LINK))
		req->flags |= REQ_F_FAIL_LINK;
	io_cqring_add_event(req->ctx, sqe->user_data, ret);
	io_put_req(req);
	return 0;
}

static int io_prep_sfr(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_ring_ctx *ctx = req->ctx;
	int ret = 0;

	if (!req->file)
		return -EBADF;

	if (unlikely(ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;
	if (unlikely(sqe->addr || sqe->ioprio || sqe->buf_index))
		return -EINVAL;

	return ret;
}

static int io_sync_file_range(struct io_kiocb *req,
			      const struct io_uring_sqe *sqe,
			      bool force_nonblock)
{
	loff_t sqe_off;
	loff_t sqe_len;
	unsigned flags;
	int ret;

	ret = io_prep_sfr(req, sqe);
	if (ret)
		return ret;

	/* sync_file_range always requires a blocking context */
	if (force_nonblock)
		return -EAGAIN;

	sqe_off = READ_ONCE(sqe->off);
	sqe_len = READ_ONCE(sqe->len);
	flags = READ_ONCE(sqe->sync_range_flags);

	ret = sync_file_range(req->rw.ki_filp, sqe_off, sqe_len, flags);

	if (ret < 0 && (req->flags & REQ_F_LINK))
		req->flags |= REQ_F_FAIL_LINK;
	io_cqring_add_event(req->ctx, sqe->user_data, ret);
	io_put_req(req);
	return 0;
}

#if defined(CONFIG_NET)
static int io_send_recvmsg(struct io_kiocb *req, const struct io_uring_sqe *sqe,
			   bool force_nonblock,
		   long (*fn)(struct socket *, struct user_msghdr __user *,
				unsigned int))
{
	struct socket *sock;
	int ret;

	if (unlikely(req->ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;

	sock = sock_from_file(req->file, &ret);
	if (sock) {
		struct user_msghdr __user *msg;
		unsigned flags;

		flags = READ_ONCE(sqe->msg_flags);
		if (flags & MSG_DONTWAIT)
			req->flags |= REQ_F_NOWAIT;
		else if (force_nonblock)
			flags |= MSG_DONTWAIT;

#ifdef CONFIG_COMPAT
		if (req->ctx->compat)
			flags |= MSG_CMSG_COMPAT;
#endif

		msg = (struct user_msghdr __user *) (unsigned long)
			READ_ONCE(sqe->addr);

		ret = fn(sock, msg, flags);
		if (force_nonblock && ret == -EAGAIN)
			return ret;
		if (ret == -ERESTARTSYS)
			ret = -EINTR;
	}

	if (req->fs) {
		struct fs_struct *fs = req->fs;

		spin_lock(&req->fs->lock);
		if (--fs->users)
			fs = NULL;
		spin_unlock(&req->fs->lock);
		if (fs)
			free_fs_struct(fs);
	}
	io_cqring_add_event(req->ctx, sqe->user_data, ret);
	io_put_req(req);
	return 0;
}
#endif

static int io_sendmsg(struct io_kiocb *req, const struct io_uring_sqe *sqe,
		      bool force_nonblock)
{
#if defined(CONFIG_NET)
	return io_send_recvmsg(req, sqe, force_nonblock, __sys_sendmsg_sock);
#else
	return -EOPNOTSUPP;
#endif
}

static int io_recvmsg(struct io_kiocb *req, const struct io_uring_sqe *sqe,
		      bool force_nonblock)
{
#if defined(CONFIG_NET)
	return io_send_recvmsg(req, sqe, force_nonblock, __sys_recvmsg_sock);
#else
	return -EOPNOTSUPP;
#endif
}

static void io_poll_remove_one(struct io_kiocb *req)
{
	struct io_poll_iocb *poll = &req->poll;

	spin_lock(&poll->head->lock);
	WRITE_ONCE(poll->canceled, true);
	if (!list_empty(&poll->wait.entry)) {
		list_del_init(&poll->wait.entry);
		io_queue_async_work(req->ctx, req);
	}
	spin_unlock(&poll->head->lock);

	list_del_init(&req->list);
}

static void io_poll_remove_all(struct io_ring_ctx *ctx)
{
	struct io_kiocb *req;

	spin_lock_irq(&ctx->completion_lock);
	while (!list_empty(&ctx->cancel_list)) {
		req = list_first_entry(&ctx->cancel_list, struct io_kiocb,list);
		io_poll_remove_one(req);
	}
	spin_unlock_irq(&ctx->completion_lock);
}

/*
 * Find a running poll command that matches one specified in sqe->addr,
 * and remove it if found.
 */
static int io_poll_remove(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_kiocb *poll_req, *next;
	int ret = -ENOENT;

	if (unlikely(req->ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;
	if (sqe->ioprio || sqe->off || sqe->len || sqe->buf_index ||
	    sqe->poll_events)
		return -EINVAL;

	spin_lock_irq(&ctx->completion_lock);
	list_for_each_entry_safe(poll_req, next, &ctx->cancel_list, list) {
		if (READ_ONCE(sqe->addr) == poll_req->user_data) {
			io_poll_remove_one(poll_req);
			ret = 0;
			break;
		}
	}
	spin_unlock_irq(&ctx->completion_lock);

	io_cqring_add_event(req->ctx, sqe->user_data, ret);
	io_put_req(req);
	return 0;
}

static void io_poll_complete(struct io_ring_ctx *ctx, struct io_kiocb *req,
			     __poll_t mask)
{
	req->poll.done = true;
	io_cqring_fill_event(ctx, req->user_data, mangle_poll(mask));
	io_commit_cqring(ctx);
}

static void io_poll_complete_work(struct work_struct *work)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work);
	struct io_poll_iocb *poll = &req->poll;
	struct poll_table_struct pt = { ._key = poll->events };
	struct io_ring_ctx *ctx = req->ctx;
	const struct cred *old_cred;
	__poll_t mask = 0;

	old_cred = override_creds(ctx->creds);

	if (!READ_ONCE(poll->canceled))
		mask = vfs_poll(poll->file, &pt) & poll->events;

	/*
	 * Note that ->ki_cancel callers also delete iocb from active_reqs after
	 * calling ->ki_cancel.  We need the ctx_lock roundtrip here to
	 * synchronize with them.  In the cancellation case the list_del_init
	 * itself is not actually needed, but harmless so we keep it in to
	 * avoid further branches in the fast path.
	 */
	spin_lock_irq(&ctx->completion_lock);
	if (!mask && !READ_ONCE(poll->canceled)) {
		add_wait_queue(poll->head, &poll->wait);
		spin_unlock_irq(&ctx->completion_lock);
		goto out;
	}
	list_del_init(&req->list);
	io_poll_complete(ctx, req, mask);
	spin_unlock_irq(&ctx->completion_lock);

	io_cqring_ev_posted(ctx);
	io_put_req(req);
out:
	revert_creds(old_cred);
}

static int io_poll_wake(struct wait_queue_entry *wait, unsigned mode, int sync,
			void *key)
{
	struct io_poll_iocb *poll = container_of(wait, struct io_poll_iocb,
							wait);
	struct io_kiocb *req = container_of(poll, struct io_kiocb, poll);
	struct io_ring_ctx *ctx = req->ctx;
	__poll_t mask = key_to_poll(key);
	unsigned long flags;

	/* for instances that support it check for an event match first: */
	if (mask && !(mask & poll->events))
		return 0;

	list_del_init(&poll->wait.entry);

	if (mask && spin_trylock_irqsave(&ctx->completion_lock, flags)) {
		list_del(&req->list);
		io_poll_complete(ctx, req, mask);
		spin_unlock_irqrestore(&ctx->completion_lock, flags);

		io_cqring_ev_posted(ctx);
		io_put_req(req);
	} else {
		io_queue_async_work(ctx, req);
	}

	return 1;
}

struct io_poll_table {
	struct poll_table_struct pt;
	struct io_kiocb *req;
	int error;
};

static void io_poll_queue_proc(struct file *file, struct wait_queue_head *head,
			       struct poll_table_struct *p)
{
	struct io_poll_table *pt = container_of(p, struct io_poll_table, pt);

	if (unlikely(pt->req->poll.head)) {
		pt->error = -EINVAL;
		return;
	}

	pt->error = 0;
	pt->req->poll.head = head;
	add_wait_queue(head, &pt->req->poll.wait);
}

static int io_poll_add(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_poll_iocb *poll = &req->poll;
	struct io_ring_ctx *ctx = req->ctx;
	struct io_poll_table ipt;
	bool cancel = false;
	__poll_t mask;
	u16 events;

	if (unlikely(req->ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;
	if (sqe->addr || sqe->ioprio || sqe->off || sqe->len || sqe->buf_index)
		return -EINVAL;
	if (!poll->file)
		return -EBADF;

	req->submit.sqe = NULL;
	INIT_WORK(&req->work, io_poll_complete_work);
	events = READ_ONCE(sqe->poll_events);
	poll->events = demangle_poll(events) | EPOLLERR | EPOLLHUP;

	poll->head = NULL;
	poll->done = false;
	poll->canceled = false;

	ipt.pt._qproc = io_poll_queue_proc;
	ipt.pt._key = poll->events;
	ipt.req = req;
	ipt.error = -EINVAL; /* same as no support for IOCB_CMD_POLL */

	/* initialized the list so that we can do list_empty checks */
	INIT_LIST_HEAD(&poll->wait.entry);
	init_waitqueue_func_entry(&poll->wait, io_poll_wake);

	INIT_LIST_HEAD(&req->list);

	mask = vfs_poll(poll->file, &ipt.pt) & poll->events;

	spin_lock_irq(&ctx->completion_lock);
	if (likely(poll->head)) {
		spin_lock(&poll->head->lock);
		if (unlikely(list_empty(&poll->wait.entry))) {
			if (ipt.error)
				cancel = true;
			ipt.error = 0;
			mask = 0;
		}
		if (mask || ipt.error)
			list_del_init(&poll->wait.entry);
		else if (cancel)
			WRITE_ONCE(poll->canceled, true);
		else if (!poll->done) /* actually waiting for an event */
			list_add_tail(&req->list, &ctx->cancel_list);
		spin_unlock(&poll->head->lock);
	}
	if (mask) { /* no async, we'd stolen it */
		ipt.error = 0;
		io_poll_complete(ctx, req, mask);
	}
	spin_unlock_irq(&ctx->completion_lock);

	if (mask) {
		io_cqring_ev_posted(ctx);
		io_put_req(req);
	}
	return ipt.error;
}

static enum hrtimer_restart io_timeout_fn(struct hrtimer *timer)
{
	struct io_ring_ctx *ctx;
	struct io_kiocb *req, *prev;
	unsigned long flags;

	req = container_of(timer, struct io_kiocb, timeout.timer);
	ctx = req->ctx;
	atomic_inc(&ctx->cq_timeouts);

	spin_lock_irqsave(&ctx->completion_lock, flags);
	/*
	 * Adjust the reqs sequence before the current one because it
	 * will consume a slot in the cq_ring and the the cq_tail pointer
	 * will be increased, otherwise other timeout reqs may return in
	 * advance without waiting for enough wait_nr.
	 */
	prev = req;
	list_for_each_entry_continue_reverse(prev, &ctx->timeout_list, list)
		prev->sequence++;
	list_del(&req->list);

	io_cqring_fill_event(ctx, req->user_data, -ETIME);
	io_commit_cqring(ctx);
	spin_unlock_irqrestore(&ctx->completion_lock, flags);

	io_cqring_ev_posted(ctx);

	io_put_req(req);
	return HRTIMER_NORESTART;
}

static int io_timeout(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	unsigned count;
	struct io_ring_ctx *ctx = req->ctx;
	struct list_head *entry;
	struct timespec64 ts;
	unsigned span = 0;

	if (unlikely(ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;
	if (sqe->flags || sqe->ioprio || sqe->buf_index || sqe->timeout_flags ||
	    sqe->len != 1)
		return -EINVAL;

	if (get_timespec64(&ts, u64_to_user_ptr(sqe->addr)))
		return -EFAULT;

	req->flags |= REQ_F_TIMEOUT;

	/*
	 * sqe->off holds how many events that need to occur for this
	 * timeout event to be satisfied. If it isn't set, then this is
	 * a pure timeout request, sequence isn't used.
	 */
	count = READ_ONCE(sqe->off);
	if (!count) {
		req->flags |= REQ_F_TIMEOUT_NOSEQ;
		spin_lock_irq(&ctx->completion_lock);
		entry = ctx->timeout_list.prev;
		goto add;
	}

	req->sequence = ctx->cached_sq_head + count - 1;
	/* reuse it to store the count */
	req->submit.sequence = count;

	/*
	 * Insertion sort, ensuring the first entry in the list is always
	 * the one we need first.
	 */
	spin_lock_irq(&ctx->completion_lock);
	list_for_each_prev(entry, &ctx->timeout_list) {
		struct io_kiocb *nxt = list_entry(entry, struct io_kiocb, list);
		unsigned nxt_sq_head;
		long long tmp, tmp_nxt;

		if (nxt->flags & REQ_F_TIMEOUT_NOSEQ)
			continue;

		/*
		 * Since cached_sq_head + count - 1 can overflow, use type long
		 * long to store it.
		 */
		tmp = (long long)ctx->cached_sq_head + count - 1;
		nxt_sq_head = nxt->sequence - nxt->submit.sequence + 1;
		tmp_nxt = (long long)nxt_sq_head + nxt->submit.sequence - 1;

		/*
		 * cached_sq_head may overflow, and it will never overflow twice
		 * once there is some timeout req still be valid.
		 */
		if (ctx->cached_sq_head < nxt_sq_head)
			tmp += UINT_MAX;

		if (tmp > tmp_nxt)
			break;

		/*
		 * Sequence of reqs after the insert one and itself should
		 * be adjusted because each timeout req consumes a slot.
		 */
		span++;
		nxt->sequence++;
	}
	req->sequence -= span;
add:
	list_add(&req->list, entry);
	spin_unlock_irq(&ctx->completion_lock);

	hrtimer_init(&req->timeout.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	req->timeout.timer.function = io_timeout_fn;
	hrtimer_start(&req->timeout.timer, timespec64_to_ktime(ts),
			HRTIMER_MODE_REL);
	return 0;
}

static int io_req_defer(struct io_ring_ctx *ctx, struct io_kiocb *req,
			struct sqe_submit *s)
{
	struct io_uring_sqe *sqe_copy;

	if (!io_sequence_defer(ctx, req) && list_empty(&ctx->defer_list))
		return 0;

	sqe_copy = kmalloc(sizeof(*sqe_copy), GFP_KERNEL);
	if (!sqe_copy)
		return -EAGAIN;

	spin_lock_irq(&ctx->completion_lock);
	if (!io_sequence_defer(ctx, req) && list_empty(&ctx->defer_list)) {
		spin_unlock_irq(&ctx->completion_lock);
		kfree(sqe_copy);
		return 0;
	}

	memcpy(&req->submit, s, sizeof(*s));
	memcpy(sqe_copy, s->sqe, sizeof(*sqe_copy));
	req->submit.sqe = sqe_copy;

	INIT_WORK(&req->work, io_sq_wq_submit_work);
	list_add_tail(&req->list, &ctx->defer_list);
	spin_unlock_irq(&ctx->completion_lock);
	return -EIOCBQUEUED;
}

static int __io_submit_sqe(struct io_ring_ctx *ctx, struct io_kiocb *req,
			   const struct sqe_submit *s, bool force_nonblock)
{
	int ret;

	req->user_data = READ_ONCE(s->sqe->user_data);

	if (unlikely(s->index >= ctx->sq_entries))
		return -EINVAL;

	switch (req->submit.opcode) {
	case IORING_OP_NOP:
		ret = io_nop(req, req->user_data);
		break;
	case IORING_OP_READV:
		if (unlikely(s->sqe->buf_index))
			return -EINVAL;
		ret = io_read(req, s, force_nonblock);
		break;
	case IORING_OP_WRITEV:
		if (unlikely(s->sqe->buf_index))
			return -EINVAL;
		ret = io_write(req, s, force_nonblock);
		break;
	case IORING_OP_READ_FIXED:
		ret = io_read(req, s, force_nonblock);
		break;
	case IORING_OP_WRITE_FIXED:
		ret = io_write(req, s, force_nonblock);
		break;
	case IORING_OP_FSYNC:
		ret = io_fsync(req, s->sqe, force_nonblock);
		break;
	case IORING_OP_POLL_ADD:
		ret = io_poll_add(req, s->sqe);
		break;
	case IORING_OP_POLL_REMOVE:
		ret = io_poll_remove(req, s->sqe);
		break;
	case IORING_OP_SYNC_FILE_RANGE:
		ret = io_sync_file_range(req, s->sqe, force_nonblock);
		break;
	case IORING_OP_SENDMSG:
		ret = io_sendmsg(req, s->sqe, force_nonblock);
		break;
	case IORING_OP_RECVMSG:
		ret = io_recvmsg(req, s->sqe, force_nonblock);
		break;
	case IORING_OP_TIMEOUT:
		ret = io_timeout(req, s->sqe);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	if (ctx->flags & IORING_SETUP_IOPOLL) {
		if (req->result == -EAGAIN)
			return -EAGAIN;

		/* workqueue context doesn't hold uring_lock, grab it now */
		if (s->needs_lock)
			mutex_lock(&ctx->uring_lock);
		io_iopoll_req_issued(req);
		if (s->needs_lock)
			mutex_unlock(&ctx->uring_lock);
	}

	return 0;
}

static struct async_list *io_async_list_from_req(struct io_ring_ctx *ctx,
						 struct io_kiocb *req)
{
	switch (req->submit.opcode) {
	case IORING_OP_READV:
	case IORING_OP_READ_FIXED:
		return &ctx->pending_async[READ];
	case IORING_OP_WRITEV:
	case IORING_OP_WRITE_FIXED:
		return &ctx->pending_async[WRITE];
	default:
		return NULL;
	}
}

static inline bool io_req_needs_user(struct io_kiocb *req)
{
	return !(req->submit.opcode == IORING_OP_READ_FIXED ||
		req->submit.opcode == IORING_OP_WRITE_FIXED);
}

static void io_sq_wq_submit_work(struct work_struct *work)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work);
	struct fs_struct *old_fs_struct = current->fs;
	struct io_ring_ctx *ctx = req->ctx;
	struct mm_struct *cur_mm = NULL;
	struct async_list *async_list;
	const struct cred *old_cred;
	LIST_HEAD(req_list);
	mm_segment_t old_fs;
	int ret;

	old_cred = override_creds(ctx->creds);
	async_list = io_async_list_from_req(ctx, req);

	allow_kernel_signal(SIGINT);
restart:
	do {
		struct sqe_submit *s = &req->submit;
		const struct io_uring_sqe *sqe = s->sqe;
		unsigned int flags = req->flags;

		/* Ensure we clear previously set non-block flag */
		req->rw.ki_flags &= ~IOCB_NOWAIT;

		if (req->fs != current->fs && current->fs != old_fs_struct) {
			task_lock(current);
			if (req->fs)
				current->fs = req->fs;
			else
				current->fs = old_fs_struct;
			task_unlock(current);
		}

		ret = 0;
		if (io_req_needs_user(req) && !cur_mm) {
			if (!mmget_not_zero(ctx->sqo_mm)) {
				ret = -EFAULT;
				goto end_req;
			} else {
				cur_mm = ctx->sqo_mm;
				use_mm(cur_mm);
				old_fs = get_fs();
				set_fs(USER_DS);
			}
		}

		if (!ret) {
			req->work_task = current;

			/*
			 * Pairs with the smp_store_mb() (B) in
			 * io_cancel_async_work().
			 */
			smp_mb(); /* A */
			if (req->flags & REQ_F_CANCEL) {
				ret = -ECANCELED;
				goto end_req;
			}

			s->has_user = cur_mm != NULL;
			s->needs_lock = true;
			do {
				ret = __io_submit_sqe(ctx, req, s, false);
				/*
				 * We can get EAGAIN for polled IO even though
				 * we're forcing a sync submission from here,
				 * since we can't wait for request slots on the
				 * block side.
				 */
				if (ret != -EAGAIN)
					break;
				cond_resched();
			} while (1);
		}
end_req:
		spin_lock_irq(&ctx->task_lock);
		list_del_init(&req->task_list);
		spin_unlock_irq(&ctx->task_lock);

		/* drop submission reference */
		io_put_req(req);

		if (ret) {
			io_cqring_add_event(ctx, sqe->user_data, ret);
			io_put_req(req);
		}

		/* async context always use a copy of the sqe */
		kfree(sqe);

		/* req from defer and link list needn't decrease async cnt */
		if (flags & (REQ_F_IO_DRAINED | REQ_F_LINK_DONE))
			goto out;

		if (!async_list)
			break;
		if (!list_empty(&req_list)) {
			req = list_first_entry(&req_list, struct io_kiocb,
						list);
			list_del(&req->list);
			continue;
		}
		if (list_empty(&async_list->list))
			break;

		req = NULL;
		spin_lock(&async_list->lock);
		if (list_empty(&async_list->list)) {
			spin_unlock(&async_list->lock);
			break;
		}
		list_splice_init(&async_list->list, &req_list);
		spin_unlock(&async_list->lock);

		req = list_first_entry(&req_list, struct io_kiocb, list);
		list_del(&req->list);
	} while (req);

	/*
	 * Rare case of racing with a submitter. If we find the count has
	 * dropped to zero AND we have pending work items, then restart
	 * the processing. This is a tiny race window.
	 */
	if (async_list) {
		ret = atomic_dec_return(&async_list->cnt);
		while (!ret && !list_empty(&async_list->list)) {
			spin_lock(&async_list->lock);
			atomic_inc(&async_list->cnt);
			list_splice_init(&async_list->list, &req_list);
			spin_unlock(&async_list->lock);

			if (!list_empty(&req_list)) {
				req = list_first_entry(&req_list,
							struct io_kiocb, list);
				list_del(&req->list);
				goto restart;
			}
			ret = atomic_dec_return(&async_list->cnt);
		}
	}

out:
	disallow_signal(SIGINT);
	if (cur_mm) {
		set_fs(old_fs);
		unuse_mm(cur_mm);
		mmput(cur_mm);
	}
	revert_creds(old_cred);
	if (old_fs_struct) {
		task_lock(current);
		current->fs = old_fs_struct;
		task_unlock(current);
	}
}

/*
 * See if we can piggy back onto previously submitted work, that is still
 * running. We currently only allow this if the new request is sequential
 * to the previous one we punted.
 */
static bool io_add_to_prev_work(struct async_list *list, struct io_kiocb *req)
{
	bool ret;

	if (!list)
		return false;
	if (!(req->flags & REQ_F_SEQ_PREV))
		return false;
	if (!atomic_read(&list->cnt))
		return false;

	ret = true;
	spin_lock(&list->lock);
	list_add_tail(&req->list, &list->list);
	/*
	 * Ensure we see a simultaneous modification from io_sq_wq_submit_work()
	 */
	smp_mb();
	if (!atomic_read(&list->cnt)) {
		list_del_init(&req->list);
		ret = false;
	}

	if (ret) {
		struct io_ring_ctx *ctx = req->ctx;

		req->files = current->files;

		spin_lock_irq(&ctx->task_lock);
		list_add(&req->task_list, &ctx->task_list);
		req->work_task = NULL;
		spin_unlock_irq(&ctx->task_lock);
	}
	spin_unlock(&list->lock);
	return ret;
}

static bool io_op_needs_file(struct io_kiocb *req)
{
	switch (req->submit.opcode) {
	case IORING_OP_NOP:
	case IORING_OP_POLL_REMOVE:
	case IORING_OP_TIMEOUT:
		return false;
	default:
		return true;
	}
}

static int io_req_set_file(struct io_ring_ctx *ctx, const struct sqe_submit *s,
			   struct io_submit_state *state, struct io_kiocb *req)
{
	unsigned flags;
	int fd;

	flags = READ_ONCE(s->sqe->flags);
	fd = READ_ONCE(s->sqe->fd);

	if (flags & IOSQE_IO_DRAIN)
		req->flags |= REQ_F_IO_DRAIN;
	/*
	 * All io need record the previous position, if LINK vs DARIN,
	 * it can be used to mark the position of the first IO in the
	 * link list.
	 */
	req->sequence = s->sequence;

	if (!io_op_needs_file(req))
		return 0;

	if (flags & IOSQE_FIXED_FILE) {
		if (unlikely(!ctx->user_files ||
		    (unsigned) fd >= ctx->nr_user_files))
			return -EBADF;
		req->file = ctx->user_files[fd];
		req->flags |= REQ_F_FIXED_FILE;
	} else {
		if (s->needs_fixed_file)
			return -EBADF;
		req->file = io_file_get(state, fd);
		if (unlikely(!req->file))
			return -EBADF;
	}

	return 0;
}

static int __io_queue_sqe(struct io_ring_ctx *ctx, struct io_kiocb *req,
			struct sqe_submit *s)
{
	int ret;

	ret = __io_submit_sqe(ctx, req, s, true);

	/*
	 * We async punt it if the file wasn't marked NOWAIT, or if the file
	 * doesn't support non-blocking read/write attempts
	 */
	if (ret == -EAGAIN && (!(req->flags & REQ_F_NOWAIT) ||
	    (req->flags & REQ_F_MUST_PUNT))) {
		struct io_uring_sqe *sqe_copy;

		sqe_copy = kmemdup(s->sqe, sizeof(*sqe_copy), GFP_KERNEL);
		if (sqe_copy) {
			struct async_list *list;

			s->sqe = sqe_copy;
			memcpy(&req->submit, s, sizeof(*s));
			list = io_async_list_from_req(ctx, req);
			if (!io_add_to_prev_work(list, req)) {
				if (list)
					atomic_inc(&list->cnt);
				INIT_WORK(&req->work, io_sq_wq_submit_work);
				io_queue_async_work(ctx, req);
			}

			/*
			 * Queued up for async execution, worker will release
			 * submit reference when the iocb is actually submitted.
			 */
			return 0;
		}
	}

	/* drop submission reference */
	io_put_req(req);

	/* and drop final reference, if we failed */
	if (ret) {
		io_cqring_add_event(ctx, req->user_data, ret);
		if (req->flags & REQ_F_LINK)
			req->flags |= REQ_F_FAIL_LINK;
		io_put_req(req);
	}

	return ret;
}

static int io_queue_sqe(struct io_ring_ctx *ctx, struct io_kiocb *req,
			struct sqe_submit *s)
{
	int ret;

	ret = io_req_defer(ctx, req, s);
	if (ret) {
		if (ret != -EIOCBQUEUED) {
			io_free_req(req);
			io_cqring_add_event(ctx, s->sqe->user_data, ret);
		}
		return 0;
	}

	return __io_queue_sqe(ctx, req, s);
}

static int io_queue_link_head(struct io_ring_ctx *ctx, struct io_kiocb *req,
			      struct sqe_submit *s, struct io_kiocb *shadow)
{
	int ret;
	int need_submit = false;

	if (!shadow)
		return io_queue_sqe(ctx, req, s);

	/*
	 * Mark the first IO in link list as DRAIN, let all the following
	 * IOs enter the defer list. all IO needs to be completed before link
	 * list.
	 */
	req->flags |= REQ_F_IO_DRAIN;
	ret = io_req_defer(ctx, req, s);
	if (ret) {
		if (ret != -EIOCBQUEUED) {
			io_free_req(req);
			__io_free_req(shadow);
			io_cqring_add_event(ctx, s->sqe->user_data, ret);
			return 0;
		}
	} else {
		/*
		 * If ret == 0 means that all IOs in front of link io are
		 * running done. let's queue link head.
		 */
		need_submit = true;
	}

	/* Insert shadow req to defer_list, blocking next IOs */
	spin_lock_irq(&ctx->completion_lock);
	list_add_tail(&shadow->list, &ctx->defer_list);
	spin_unlock_irq(&ctx->completion_lock);

	if (need_submit)
		return __io_queue_sqe(ctx, req, s);

	return 0;
}

#define SQE_VALID_FLAGS	(IOSQE_FIXED_FILE|IOSQE_IO_DRAIN|IOSQE_IO_LINK)

static void io_submit_sqe(struct io_ring_ctx *ctx, struct sqe_submit *s,
			  struct io_submit_state *state, struct io_kiocb **link)
{
	struct io_uring_sqe *sqe_copy;
	struct io_kiocb *req;
	int ret;

	/* enforce forwards compatibility on users */
	if (unlikely(s->sqe->flags & ~SQE_VALID_FLAGS)) {
		ret = -EINVAL;
		goto err;
	}

	req = io_get_req(ctx, state);
	if (unlikely(!req)) {
		ret = -EAGAIN;
		goto err;
	}

	memcpy(&req->submit, s, sizeof(*s));
	ret = io_req_set_file(ctx, s, state, req);
	if (unlikely(ret)) {
err_req:
		io_free_req(req);
err:
		io_cqring_add_event(ctx, s->sqe->user_data, ret);
		return;
	}

	req->user_data = s->sqe->user_data;

#if defined(CONFIG_NET)
	switch (req->submit.opcode) {
	case IORING_OP_SENDMSG:
	case IORING_OP_RECVMSG:
		spin_lock(&current->fs->lock);
		if (!current->fs->in_exec) {
			req->fs = current->fs;
			req->fs->users++;
		}
		spin_unlock(&current->fs->lock);
		if (!req->fs) {
			ret = -EAGAIN;
			goto err_req;
		}
	}
#endif

	/*
	 * If we already have a head request, queue this one for async
	 * submittal once the head completes. If we don't have a head but
	 * IOSQE_IO_LINK is set in the sqe, start a new head. This one will be
	 * submitted sync once the chain is complete. If none of those
	 * conditions are true (normal request), then just queue it.
	 */
	if (*link) {
		struct io_kiocb *prev = *link;

		sqe_copy = kmemdup(s->sqe, sizeof(*sqe_copy), GFP_KERNEL);
		if (!sqe_copy) {
			ret = -EAGAIN;
			goto err_req;
		}

		s->sqe = sqe_copy;
		memcpy(&req->submit, s, sizeof(*s));
		list_add_tail(&req->list, &prev->link_list);
	} else if (s->sqe->flags & IOSQE_IO_LINK) {
		req->flags |= REQ_F_LINK;

		memcpy(&req->submit, s, sizeof(*s));
		INIT_LIST_HEAD(&req->link_list);
		*link = req;
	} else {
		io_queue_sqe(ctx, req, s);
	}
}

/*
 * Batched submission is done, ensure local IO is flushed out.
 */
static void io_submit_state_end(struct io_submit_state *state)
{
	blk_finish_plug(&state->plug);
	io_file_put(state);
	if (state->free_reqs)
		kmem_cache_free_bulk(req_cachep, state->free_reqs,
					&state->reqs[state->cur_req]);
}

/*
 * Start submission side cache.
 */
static void io_submit_state_start(struct io_submit_state *state,
				  struct io_ring_ctx *ctx, unsigned max_ios)
{
	blk_start_plug(&state->plug);
	state->free_reqs = 0;
	state->file = NULL;
	state->ios_left = max_ios;
}

static void io_commit_sqring(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;

	if (ctx->cached_sq_head != READ_ONCE(rings->sq.head)) {
		/*
		 * Ensure any loads from the SQEs are done at this point,
		 * since once we write the new head, the application could
		 * write new data to them.
		 */
		smp_store_release(&rings->sq.head, ctx->cached_sq_head);
	}
}

/*
 * Fetch an sqe, if one is available. Note that s->sqe will point to memory
 * that is mapped by userspace. This means that care needs to be taken to
 * ensure that reads are stable, as we cannot rely on userspace always
 * being a good citizen. If members of the sqe are validated and then later
 * used, it's important that those reads are done through READ_ONCE() to
 * prevent a re-load down the line.
 */
static bool io_get_sqring(struct io_ring_ctx *ctx, struct sqe_submit *s)
{
	struct io_rings *rings = ctx->rings;
	u32 *sq_array = ctx->sq_array;
	unsigned head;

	/*
	 * The cached sq head (or cq tail) serves two purposes:
	 *
	 * 1) allows us to batch the cost of updating the user visible
	 *    head updates.
	 * 2) allows the kernel side to track the head on its own, even
	 *    though the application is the one updating it.
	 */
	head = ctx->cached_sq_head;
	/* make sure SQ entry isn't read before tail */
	if (head == smp_load_acquire(&rings->sq.tail))
		return false;

	head = READ_ONCE(sq_array[head & ctx->sq_mask]);
	if (head < ctx->sq_entries) {
		s->index = head;
		s->sqe = &ctx->sq_sqes[head];
		s->opcode = READ_ONCE(s->sqe->opcode);
		s->sequence = ctx->cached_sq_head;
		ctx->cached_sq_head++;
		return true;
	}

	/* drop invalid entries */
	ctx->cached_sq_head++;
	ctx->cached_sq_dropped++;
	WRITE_ONCE(rings->sq_dropped, ctx->cached_sq_dropped);
	return false;
}

static int io_submit_sqes(struct io_ring_ctx *ctx, unsigned int nr,
			  bool has_user, bool mm_fault)
{
	struct io_submit_state state, *statep = NULL;
	struct io_kiocb *link = NULL;
	struct io_kiocb *shadow_req = NULL;
	bool prev_was_link = false;
	int i, submitted = 0;

	if (nr > IO_PLUG_THRESHOLD) {
		io_submit_state_start(&state, ctx, nr);
		statep = &state;
	}

	for (i = 0; i < nr; i++) {
		struct sqe_submit s;

		if (!io_get_sqring(ctx, &s))
			break;

		/*
		 * If previous wasn't linked and we have a linked command,
		 * that's the end of the chain. Submit the previous link.
		 */
		if (!prev_was_link && link) {
			io_queue_link_head(ctx, link, &link->submit, shadow_req);
			link = NULL;
			shadow_req = NULL;
		}
		prev_was_link = (s.sqe->flags & IOSQE_IO_LINK) != 0;

		if (link && (s.sqe->flags & IOSQE_IO_DRAIN)) {
			if (!shadow_req) {
				shadow_req = io_get_req(ctx, NULL);
				if (unlikely(!shadow_req))
					goto out;
				shadow_req->flags |= (REQ_F_IO_DRAIN | REQ_F_SHADOW_DRAIN);
				refcount_dec(&shadow_req->refs);
			}
			shadow_req->sequence = s.sequence;
		}

out:
		if (unlikely(mm_fault)) {
			io_cqring_add_event(ctx, s.sqe->user_data,
						-EFAULT);
		} else {
			s.has_user = has_user;
			s.needs_lock = true;
			s.needs_fixed_file = true;
			io_submit_sqe(ctx, &s, statep, &link);
			submitted++;
		}
	}

	if (link)
		io_queue_link_head(ctx, link, &link->submit, shadow_req);
	if (statep)
		io_submit_state_end(&state);

	return submitted;
}

static int io_sq_thread(void *data)
{
	struct io_ring_ctx *ctx = data;
	struct mm_struct *cur_mm = NULL;
	const struct cred *old_cred;
	mm_segment_t old_fs;
	DEFINE_WAIT(wait);
	unsigned inflight;
	unsigned long timeout;

	complete(&ctx->sqo_thread_started);

	old_fs = get_fs();
	set_fs(USER_DS);
	old_cred = override_creds(ctx->creds);

	timeout = inflight = 0;
	while (!kthread_should_park()) {
		bool mm_fault = false;
		unsigned int to_submit;

		if (inflight) {
			unsigned nr_events = 0;

			if (ctx->flags & IORING_SETUP_IOPOLL) {
				/*
				 * inflight is the count of the maximum possible
				 * entries we submitted, but it can be smaller
				 * if we dropped some of them. If we don't have
				 * poll entries available, then we know that we
				 * have nothing left to poll for. Reset the
				 * inflight count to zero in that case.
				 */
				mutex_lock(&ctx->uring_lock);
				if (!list_empty(&ctx->poll_list))
					io_iopoll_getevents(ctx, &nr_events, 0);
				else
					inflight = 0;
				mutex_unlock(&ctx->uring_lock);
			} else {
				/*
				 * Normal IO, just pretend everything completed.
				 * We don't have to poll completions for that.
				 */
				nr_events = inflight;
			}

			inflight -= nr_events;
			if (!inflight)
				timeout = jiffies + ctx->sq_thread_idle;
		}

		to_submit = io_sqring_entries(ctx);
		if (!to_submit) {
			/*
			 * Drop cur_mm before scheduling, we can't hold it for
			 * long periods (or over schedule()). Do this before
			 * adding ourselves to the waitqueue, as the unuse/drop
			 * may sleep.
			 */
			if (cur_mm) {
				unuse_mm(cur_mm);
				mmput(cur_mm);
				cur_mm = NULL;
			}

			/*
			 * We're polling. If we're within the defined idle
			 * period, then let us spin without work before going
			 * to sleep.
			 */
			if (inflight || !time_after(jiffies, timeout)) {
				cond_resched();
				continue;
			}

			prepare_to_wait(&ctx->sqo_wait, &wait,
						TASK_INTERRUPTIBLE);

			/* Tell userspace we may need a wakeup call */
			ctx->rings->sq_flags |= IORING_SQ_NEED_WAKEUP;
			/* make sure to read SQ tail after writing flags */
			smp_mb();

			to_submit = io_sqring_entries(ctx);
			if (!to_submit) {
				if (kthread_should_park()) {
					finish_wait(&ctx->sqo_wait, &wait);
					break;
				}
				if (signal_pending(current))
					flush_signals(current);
				schedule();
				finish_wait(&ctx->sqo_wait, &wait);

				ctx->rings->sq_flags &= ~IORING_SQ_NEED_WAKEUP;
				continue;
			}
			finish_wait(&ctx->sqo_wait, &wait);

			ctx->rings->sq_flags &= ~IORING_SQ_NEED_WAKEUP;
		}

		/* Unless all new commands are FIXED regions, grab mm */
		if (!cur_mm) {
			mm_fault = !mmget_not_zero(ctx->sqo_mm);
			if (!mm_fault) {
				use_mm(ctx->sqo_mm);
				cur_mm = ctx->sqo_mm;
			}
		}

		to_submit = min(to_submit, ctx->sq_entries);
		inflight += io_submit_sqes(ctx, to_submit, cur_mm != NULL,
					   mm_fault);

		/* Commit SQ ring head once we've consumed all SQEs */
		io_commit_sqring(ctx);
	}

	set_fs(old_fs);
	if (cur_mm) {
		unuse_mm(cur_mm);
		mmput(cur_mm);
	}
	revert_creds(old_cred);

	kthread_parkme();

	return 0;
}

static int io_ring_submit(struct io_ring_ctx *ctx, unsigned int to_submit)
{
	struct io_submit_state state, *statep = NULL;
	struct io_kiocb *link = NULL;
	struct io_kiocb *shadow_req = NULL;
	bool prev_was_link = false;
	int i, submit = 0;

	if (to_submit > IO_PLUG_THRESHOLD) {
		io_submit_state_start(&state, ctx, to_submit);
		statep = &state;
	}

	for (i = 0; i < to_submit; i++) {
		struct sqe_submit s;

		if (!io_get_sqring(ctx, &s))
			break;

		/*
		 * If previous wasn't linked and we have a linked command,
		 * that's the end of the chain. Submit the previous link.
		 */
		if (!prev_was_link && link) {
			io_queue_link_head(ctx, link, &link->submit, shadow_req);
			link = NULL;
			shadow_req = NULL;
		}
		prev_was_link = (s.sqe->flags & IOSQE_IO_LINK) != 0;

		if (link && (s.sqe->flags & IOSQE_IO_DRAIN)) {
			if (!shadow_req) {
				shadow_req = io_get_req(ctx, NULL);
				if (unlikely(!shadow_req))
					goto out;
				shadow_req->flags |= (REQ_F_IO_DRAIN | REQ_F_SHADOW_DRAIN);
				refcount_dec(&shadow_req->refs);
			}
			shadow_req->sequence = s.sequence;
		}

out:
		s.has_user = true;
		s.needs_lock = false;
		s.needs_fixed_file = false;
		submit++;
		io_submit_sqe(ctx, &s, statep, &link);
	}

	if (link)
		io_queue_link_head(ctx, link, &link->submit, shadow_req);
	if (statep)
		io_submit_state_end(statep);

	io_commit_sqring(ctx);

	return submit;
}

struct io_wait_queue {
	struct wait_queue_entry wq;
	struct io_ring_ctx *ctx;
	unsigned to_wait;
	unsigned nr_timeouts;
};

static inline bool io_should_wake(struct io_wait_queue *iowq)
{
	struct io_ring_ctx *ctx = iowq->ctx;

	/*
	 * Wake up if we have enough events, or if a timeout occured since we
	 * started waiting. For timeouts, we always want to return to userspace,
	 * regardless of event count.
	 */
	return io_cqring_events(ctx->rings) >= iowq->to_wait ||
			atomic_read(&ctx->cq_timeouts) != iowq->nr_timeouts;
}

static int io_wake_function(struct wait_queue_entry *curr, unsigned int mode,
			    int wake_flags, void *key)
{
	struct io_wait_queue *iowq = container_of(curr, struct io_wait_queue,
							wq);

	if (!io_should_wake(iowq))
		return -1;

	return autoremove_wake_function(curr, mode, wake_flags, key);
}

/*
 * Wait until events become available, if we don't already have some. The
 * application must reap them itself, as they reside on the shared cq ring.
 */
static int io_cqring_wait(struct io_ring_ctx *ctx, int min_events,
			  const sigset_t __user *sig, size_t sigsz)
{
	struct io_wait_queue iowq = {
		.wq = {
			.private	= current,
			.func		= io_wake_function,
			.entry		= LIST_HEAD_INIT(iowq.wq.entry),
		},
		.ctx		= ctx,
		.to_wait	= min_events,
	};
	struct io_rings *rings = ctx->rings;
	int ret;

	if (io_cqring_events(rings) >= min_events)
		return 0;

	if (sig) {
#ifdef CONFIG_COMPAT
		if (in_compat_syscall())
			ret = set_compat_user_sigmask((const compat_sigset_t __user *)sig,
						      sigsz);
		else
#endif
			ret = set_user_sigmask(sig, sigsz);

		if (ret)
			return ret;
	}

	ret = 0;
	iowq.nr_timeouts = atomic_read(&ctx->cq_timeouts);
	do {
		prepare_to_wait_exclusive(&ctx->wait, &iowq.wq,
						TASK_INTERRUPTIBLE);
		if (io_should_wake(&iowq))
			break;
		schedule();
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
	} while (1);
	finish_wait(&ctx->wait, &iowq.wq);

	restore_saved_sigmask_unless(ret == -ERESTARTSYS);
	if (ret == -ERESTARTSYS)
		ret = -EINTR;

	return READ_ONCE(rings->cq.head) == READ_ONCE(rings->cq.tail) ? ret : 0;
}

static void __io_sqe_files_unregister(struct io_ring_ctx *ctx)
{
#if defined(CONFIG_UNIX)
	if (ctx->ring_sock) {
		struct sock *sock = ctx->ring_sock->sk;
		struct sk_buff *skb;

		while ((skb = skb_dequeue(&sock->sk_receive_queue)) != NULL)
			kfree_skb(skb);
	}
#else
	int i;

	for (i = 0; i < ctx->nr_user_files; i++)
		fput(ctx->user_files[i]);
#endif
}

static int io_sqe_files_unregister(struct io_ring_ctx *ctx)
{
	if (!ctx->user_files)
		return -ENXIO;

	__io_sqe_files_unregister(ctx);
	kfree(ctx->user_files);
	ctx->user_files = NULL;
	ctx->nr_user_files = 0;
	return 0;
}

static void io_sq_thread_stop(struct io_ring_ctx *ctx)
{
	if (ctx->sqo_thread) {
		wait_for_completion(&ctx->sqo_thread_started);
		/*
		 * The park is a bit of a work-around, without it we get
		 * warning spews on shutdown with SQPOLL set and affinity
		 * set to a single CPU.
		 */
		kthread_park(ctx->sqo_thread);
		kthread_stop(ctx->sqo_thread);
		ctx->sqo_thread = NULL;
	}
}

static void io_finish_async(struct io_ring_ctx *ctx)
{
	int i;

	io_sq_thread_stop(ctx);

	for (i = 0; i < ARRAY_SIZE(ctx->sqo_wq); i++) {
		if (ctx->sqo_wq[i]) {
			destroy_workqueue(ctx->sqo_wq[i]);
			ctx->sqo_wq[i] = NULL;
		}
	}
}

#if defined(CONFIG_UNIX)
static void io_destruct_skb(struct sk_buff *skb)
{
	struct io_ring_ctx *ctx = skb->sk->sk_user_data;
	int i;

	for (i = 0; i < ARRAY_SIZE(ctx->sqo_wq); i++)
		if (ctx->sqo_wq[i])
			flush_workqueue(ctx->sqo_wq[i]);

	unix_destruct_scm(skb);
}

/*
 * Ensure the UNIX gc is aware of our file set, so we are certain that
 * the io_uring can be safely unregistered on process exit, even if we have
 * loops in the file referencing.
 */
static int __io_sqe_files_scm(struct io_ring_ctx *ctx, int nr, int offset)
{
	struct sock *sk = ctx->ring_sock->sk;
	struct scm_fp_list *fpl;
	struct sk_buff *skb;
	int i;

	fpl = kzalloc(sizeof(*fpl), GFP_KERNEL);
	if (!fpl)
		return -ENOMEM;

	skb = alloc_skb(0, GFP_KERNEL);
	if (!skb) {
		kfree(fpl);
		return -ENOMEM;
	}

	skb->sk = sk;
	skb->destructor = io_destruct_skb;

	fpl->user = get_uid(ctx->user);
	for (i = 0; i < nr; i++) {
		fpl->fp[i] = get_file(ctx->user_files[i + offset]);
		unix_inflight(fpl->user, fpl->fp[i]);
	}

	fpl->max = fpl->count = nr;
	UNIXCB(skb).fp = fpl;
	refcount_add(skb->truesize, &sk->sk_wmem_alloc);
	skb_queue_head(&sk->sk_receive_queue, skb);

	for (i = 0; i < nr; i++)
		fput(fpl->fp[i]);

	return 0;
}

/*
 * If UNIX sockets are enabled, fd passing can cause a reference cycle which
 * causes regular reference counting to break down. We rely on the UNIX
 * garbage collection to take care of this problem for us.
 */
static int io_sqe_files_scm(struct io_ring_ctx *ctx)
{
	unsigned left, total;
	int ret = 0;

	total = 0;
	left = ctx->nr_user_files;
	while (left) {
		unsigned this_files = min_t(unsigned, left, SCM_MAX_FD);

		ret = __io_sqe_files_scm(ctx, this_files, total);
		if (ret)
			break;
		left -= this_files;
		total += this_files;
	}

	if (!ret)
		return 0;

	while (total < ctx->nr_user_files) {
		fput(ctx->user_files[total]);
		total++;
	}

	return ret;
}
#else
static int io_sqe_files_scm(struct io_ring_ctx *ctx)
{
	return 0;
}
#endif

static int io_sqe_files_register(struct io_ring_ctx *ctx, void __user *arg,
				 unsigned nr_args)
{
	__s32 __user *fds = (__s32 __user *) arg;
	int fd, ret = 0;
	unsigned i;

	if (ctx->user_files)
		return -EBUSY;
	if (!nr_args)
		return -EINVAL;
	if (nr_args > IORING_MAX_FIXED_FILES)
		return -EMFILE;

	ctx->user_files = kcalloc(nr_args, sizeof(struct file *), GFP_KERNEL);
	if (!ctx->user_files)
		return -ENOMEM;

	for (i = 0; i < nr_args; i++) {
		ret = -EFAULT;
		if (copy_from_user(&fd, &fds[i], sizeof(fd)))
			break;

		ctx->user_files[i] = fget(fd);

		ret = -EBADF;
		if (!ctx->user_files[i])
			break;
		/*
		 * Don't allow io_uring instances to be registered. If UNIX
		 * isn't enabled, then this causes a reference cycle and this
		 * instance can never get freed. If UNIX is enabled we'll
		 * handle it just fine, but there's still no point in allowing
		 * a ring fd as it doesn't support regular read/write anyway.
		 */
		if (ctx->user_files[i]->f_op == &io_uring_fops) {
			fput(ctx->user_files[i]);
			break;
		}
		ctx->nr_user_files++;
		ret = 0;
	}

	if (ret) {
		for (i = 0; i < ctx->nr_user_files; i++)
			fput(ctx->user_files[i]);

		kfree(ctx->user_files);
		ctx->user_files = NULL;
		ctx->nr_user_files = 0;
		return ret;
	}

	ret = io_sqe_files_scm(ctx);
	if (ret)
		io_sqe_files_unregister(ctx);

	return ret;
}

static int io_sq_offload_start(struct io_ring_ctx *ctx,
			       struct io_uring_params *p)
{
	int ret;

	mmgrab(current->mm);
	ctx->sqo_mm = current->mm;

	if (ctx->flags & IORING_SETUP_SQPOLL) {
		ret = -EPERM;
		if (!capable(CAP_SYS_ADMIN))
			goto err;

		ctx->sq_thread_idle = msecs_to_jiffies(p->sq_thread_idle);
		if (!ctx->sq_thread_idle)
			ctx->sq_thread_idle = HZ;

		if (p->flags & IORING_SETUP_SQ_AFF) {
			int cpu = p->sq_thread_cpu;

			ret = -EINVAL;
			if (cpu >= nr_cpu_ids)
				goto err;
			if (!cpu_online(cpu))
				goto err;

			ctx->sqo_thread = kthread_create_on_cpu(io_sq_thread,
							ctx, cpu,
							"io_uring-sq");
		} else {
			ctx->sqo_thread = kthread_create(io_sq_thread, ctx,
							"io_uring-sq");
		}
		if (IS_ERR(ctx->sqo_thread)) {
			ret = PTR_ERR(ctx->sqo_thread);
			ctx->sqo_thread = NULL;
			goto err;
		}
		wake_up_process(ctx->sqo_thread);
	} else if (p->flags & IORING_SETUP_SQ_AFF) {
		/* Can't have SQ_AFF without SQPOLL */
		ret = -EINVAL;
		goto err;
	}

	/* Do QD, or 2 * CPUS, whatever is smallest */
	ctx->sqo_wq[0] = alloc_workqueue("io_ring-wq",
			WQ_UNBOUND | WQ_FREEZABLE,
			min(ctx->sq_entries - 1, 2 * num_online_cpus()));
	if (!ctx->sqo_wq[0]) {
		ret = -ENOMEM;
		goto err;
	}

	/*
	 * This is for buffered writes, where we want to limit the parallelism
	 * due to file locking in file systems. As "normal" buffered writes
	 * should parellelize on writeout quite nicely, limit us to having 2
	 * pending. This avoids massive contention on the inode when doing
	 * buffered async writes.
	 */
	ctx->sqo_wq[1] = alloc_workqueue("io_ring-write-wq",
						WQ_UNBOUND | WQ_FREEZABLE, 2);
	if (!ctx->sqo_wq[1]) {
		ret = -ENOMEM;
		goto err;
	}

	return 0;
err:
	io_finish_async(ctx);
	mmdrop(ctx->sqo_mm);
	ctx->sqo_mm = NULL;
	return ret;
}

static void io_unaccount_mem(struct user_struct *user, unsigned long nr_pages)
{
	atomic_long_sub(nr_pages, &user->locked_vm);
}

static int io_account_mem(struct user_struct *user, unsigned long nr_pages)
{
	unsigned long page_limit, cur_pages, new_pages;

	/* Don't allow more pages than we can safely lock */
	page_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	do {
		cur_pages = atomic_long_read(&user->locked_vm);
		new_pages = cur_pages + nr_pages;
		if (new_pages > page_limit)
			return -ENOMEM;
	} while (atomic_long_cmpxchg(&user->locked_vm, cur_pages,
					new_pages) != cur_pages);

	return 0;
}

static void io_mem_free(void *ptr)
{
	struct page *page;

	if (!ptr)
		return;

	page = virt_to_head_page(ptr);
	if (put_page_testzero(page))
		free_compound_page(page);
}

static void *io_mem_alloc(size_t size)
{
	gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN | __GFP_COMP |
				__GFP_NORETRY;

	return (void *) __get_free_pages(gfp_flags, get_order(size));
}

static unsigned long rings_size(unsigned sq_entries, unsigned cq_entries,
				size_t *sq_offset)
{
	struct io_rings *rings;
	size_t off, sq_array_size;

	off = struct_size(rings, cqes, cq_entries);
	if (off == SIZE_MAX)
		return SIZE_MAX;

#ifdef CONFIG_SMP
	off = ALIGN(off, SMP_CACHE_BYTES);
	if (off == 0)
		return SIZE_MAX;
#endif

	if (sq_offset)
		*sq_offset = off;

	sq_array_size = array_size(sizeof(u32), sq_entries);
	if (sq_array_size == SIZE_MAX)
		return SIZE_MAX;

	if (check_add_overflow(off, sq_array_size, &off))
		return SIZE_MAX;

	return off;
}

static unsigned long ring_pages(unsigned sq_entries, unsigned cq_entries)
{
	size_t pages;

	pages = (size_t)1 << get_order(
		rings_size(sq_entries, cq_entries, NULL));
	pages += (size_t)1 << get_order(
		array_size(sizeof(struct io_uring_sqe), sq_entries));

	return pages;
}

static int io_sqe_buffer_unregister(struct io_ring_ctx *ctx)
{
	int i, j;

	if (!ctx->user_bufs)
		return -ENXIO;

	for (i = 0; i < ctx->nr_user_bufs; i++) {
		struct io_mapped_ubuf *imu = &ctx->user_bufs[i];

		for (j = 0; j < imu->nr_bvecs; j++)
			put_user_page(imu->bvec[j].bv_page);

		if (ctx->account_mem)
			io_unaccount_mem(ctx->user, imu->nr_bvecs);
		kvfree(imu->bvec);
		imu->nr_bvecs = 0;
	}

	kfree(ctx->user_bufs);
	ctx->user_bufs = NULL;
	ctx->nr_user_bufs = 0;
	return 0;
}

static int io_copy_iov(struct io_ring_ctx *ctx, struct iovec *dst,
		       void __user *arg, unsigned index)
{
	struct iovec __user *src;

#ifdef CONFIG_COMPAT
	if (ctx->compat) {
		struct compat_iovec __user *ciovs;
		struct compat_iovec ciov;

		ciovs = (struct compat_iovec __user *) arg;
		if (copy_from_user(&ciov, &ciovs[index], sizeof(ciov)))
			return -EFAULT;

		dst->iov_base = (void __user *) (unsigned long) ciov.iov_base;
		dst->iov_len = ciov.iov_len;
		return 0;
	}
#endif
	src = (struct iovec __user *) arg;
	if (copy_from_user(dst, &src[index], sizeof(*dst)))
		return -EFAULT;
	return 0;
}

static int io_sqe_buffer_register(struct io_ring_ctx *ctx, void __user *arg,
				  unsigned nr_args)
{
	struct vm_area_struct **vmas = NULL;
	struct page **pages = NULL;
	int i, j, got_pages = 0;
	int ret = -EINVAL;

	if (ctx->user_bufs)
		return -EBUSY;
	if (!nr_args || nr_args > UIO_MAXIOV)
		return -EINVAL;

	ctx->user_bufs = kcalloc(nr_args, sizeof(struct io_mapped_ubuf),
					GFP_KERNEL);
	if (!ctx->user_bufs)
		return -ENOMEM;

	for (i = 0; i < nr_args; i++) {
		struct io_mapped_ubuf *imu = &ctx->user_bufs[i];
		unsigned long off, start, end, ubuf;
		int pret, nr_pages;
		struct iovec iov;
		size_t size;

		ret = io_copy_iov(ctx, &iov, arg, i);
		if (ret)
			goto err;

		/*
		 * Don't impose further limits on the size and buffer
		 * constraints here, we'll -EINVAL later when IO is
		 * submitted if they are wrong.
		 */
		ret = -EFAULT;
		if (!iov.iov_base || !iov.iov_len)
			goto err;

		/* arbitrary limit, but we need something */
		if (iov.iov_len > SZ_1G)
			goto err;

		ubuf = (unsigned long) iov.iov_base;
		end = (ubuf + iov.iov_len + PAGE_SIZE - 1) >> PAGE_SHIFT;
		start = ubuf >> PAGE_SHIFT;
		nr_pages = end - start;

		if (ctx->account_mem) {
			ret = io_account_mem(ctx->user, nr_pages);
			if (ret)
				goto err;
		}

		ret = 0;
		if (!pages || nr_pages > got_pages) {
			kvfree(vmas);
			kvfree(pages);
			pages = kvmalloc_array(nr_pages, sizeof(struct page *),
						GFP_KERNEL);
			vmas = kvmalloc_array(nr_pages,
					sizeof(struct vm_area_struct *),
					GFP_KERNEL);
			if (!pages || !vmas) {
				ret = -ENOMEM;
				if (ctx->account_mem)
					io_unaccount_mem(ctx->user, nr_pages);
				goto err;
			}
			got_pages = nr_pages;
		}

		imu->bvec = kvmalloc_array(nr_pages, sizeof(struct bio_vec),
						GFP_KERNEL);
		ret = -ENOMEM;
		if (!imu->bvec) {
			if (ctx->account_mem)
				io_unaccount_mem(ctx->user, nr_pages);
			goto err;
		}

		ret = 0;
		down_read(&current->mm->mmap_sem);
		pret = get_user_pages(ubuf, nr_pages,
				      FOLL_WRITE | FOLL_LONGTERM,
				      pages, vmas);
		if (pret == nr_pages) {
			/* don't support file backed memory */
			for (j = 0; j < nr_pages; j++) {
				struct vm_area_struct *vma = vmas[j];

				if (vma->vm_file &&
				    !is_file_hugepages(vma->vm_file)) {
					ret = -EOPNOTSUPP;
					break;
				}
			}
		} else {
			ret = pret < 0 ? pret : -EFAULT;
		}
		up_read(&current->mm->mmap_sem);
		if (ret) {
			/*
			 * if we did partial map, or found file backed vmas,
			 * release any pages we did get
			 */
			if (pret > 0)
				put_user_pages(pages, pret);
			if (ctx->account_mem)
				io_unaccount_mem(ctx->user, nr_pages);
			kvfree(imu->bvec);
			goto err;
		}

		off = ubuf & ~PAGE_MASK;
		size = iov.iov_len;
		for (j = 0; j < nr_pages; j++) {
			size_t vec_len;

			vec_len = min_t(size_t, size, PAGE_SIZE - off);
			imu->bvec[j].bv_page = pages[j];
			imu->bvec[j].bv_len = vec_len;
			imu->bvec[j].bv_offset = off;
			off = 0;
			size -= vec_len;
		}
		/* store original address for later verification */
		imu->ubuf = ubuf;
		imu->len = iov.iov_len;
		imu->nr_bvecs = nr_pages;

		ctx->nr_user_bufs++;
	}
	kvfree(pages);
	kvfree(vmas);
	return 0;
err:
	kvfree(pages);
	kvfree(vmas);
	io_sqe_buffer_unregister(ctx);
	return ret;
}

static int io_eventfd_register(struct io_ring_ctx *ctx, void __user *arg)
{
	__s32 __user *fds = arg;
	int fd;

	if (ctx->cq_ev_fd)
		return -EBUSY;

	if (copy_from_user(&fd, fds, sizeof(*fds)))
		return -EFAULT;

	ctx->cq_ev_fd = eventfd_ctx_fdget(fd);
	if (IS_ERR(ctx->cq_ev_fd)) {
		int ret = PTR_ERR(ctx->cq_ev_fd);
		ctx->cq_ev_fd = NULL;
		return ret;
	}

	return 0;
}

static int io_eventfd_unregister(struct io_ring_ctx *ctx)
{
	if (ctx->cq_ev_fd) {
		eventfd_ctx_put(ctx->cq_ev_fd);
		ctx->cq_ev_fd = NULL;
		return 0;
	}

	return -ENXIO;
}

static void io_ring_ctx_free(struct io_ring_ctx *ctx)
{
	io_finish_async(ctx);
	if (ctx->sqo_mm)
		mmdrop(ctx->sqo_mm);

	io_iopoll_reap_events(ctx);
	io_sqe_buffer_unregister(ctx);
	io_sqe_files_unregister(ctx);
	io_eventfd_unregister(ctx);

#if defined(CONFIG_UNIX)
	if (ctx->ring_sock) {
		ctx->ring_sock->file = NULL; /* so that iput() is called */
		sock_release(ctx->ring_sock);
	}
#endif

	io_mem_free(ctx->rings);
	io_mem_free(ctx->sq_sqes);

	percpu_ref_exit(&ctx->refs);
	if (ctx->account_mem)
		io_unaccount_mem(ctx->user,
				ring_pages(ctx->sq_entries, ctx->cq_entries));
	free_uid(ctx->user);
	if (ctx->creds)
		put_cred(ctx->creds);
	kfree(ctx);
}

static __poll_t io_uring_poll(struct file *file, poll_table *wait)
{
	struct io_ring_ctx *ctx = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &ctx->cq_wait, wait);
	/*
	 * synchronizes with barrier from wq_has_sleeper call in
	 * io_commit_cqring
	 */
	smp_rmb();
	if (READ_ONCE(ctx->rings->sq.tail) - ctx->cached_sq_head !=
	    ctx->rings->sq_ring_entries)
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (READ_ONCE(ctx->rings->cq.head) != ctx->cached_cq_tail)
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static int io_uring_fasync(int fd, struct file *file, int on)
{
	struct io_ring_ctx *ctx = file->private_data;

	return fasync_helper(fd, file, on, &ctx->cq_fasync);
}

static void io_cancel_async_work(struct io_ring_ctx *ctx,
				 struct files_struct *files)
{
	struct io_kiocb *req;

	if (list_empty(&ctx->task_list))
		return;

	spin_lock_irq(&ctx->task_lock);

	list_for_each_entry(req, &ctx->task_list, task_list) {
		if (files && req->files != files)
			continue;

		/*
		 * The below executes an smp_mb(), which matches with the
		 * smp_mb() (A) in io_sq_wq_submit_work() such that either
		 * we store REQ_F_CANCEL flag to req->flags or we see the
		 * req->work_task setted in io_sq_wq_submit_work().
		 */
		smp_store_mb(req->flags, req->flags | REQ_F_CANCEL); /* B */

		if (req->work_task)
			send_sig(SIGINT, req->work_task, 1);
	}
	spin_unlock_irq(&ctx->task_lock);
}

static void io_ring_ctx_wait_and_kill(struct io_ring_ctx *ctx)
{
	mutex_lock(&ctx->uring_lock);
	percpu_ref_kill(&ctx->refs);
	mutex_unlock(&ctx->uring_lock);

	io_cancel_async_work(ctx, NULL);
	io_kill_timeouts(ctx);
	io_poll_remove_all(ctx);
	io_iopoll_reap_events(ctx);
	wait_for_completion(&ctx->ctx_done);
	io_ring_ctx_free(ctx);
}

static int io_uring_flush(struct file *file, void *data)
{
	struct io_ring_ctx *ctx = file->private_data;

	if (fatal_signal_pending(current) || (current->flags & PF_EXITING))
		io_cancel_async_work(ctx, data);

	return 0;
}

static int io_uring_release(struct inode *inode, struct file *file)
{
	struct io_ring_ctx *ctx = file->private_data;

	file->private_data = NULL;
	io_ring_ctx_wait_and_kill(ctx);
	return 0;
}

static int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	loff_t offset = (loff_t) vma->vm_pgoff << PAGE_SHIFT;
	unsigned long sz = vma->vm_end - vma->vm_start;
	struct io_ring_ctx *ctx = file->private_data;
	unsigned long pfn;
	struct page *page;
	void *ptr;

	switch (offset) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		ptr = ctx->rings;
		break;
	case IORING_OFF_SQES:
		ptr = ctx->sq_sqes;
		break;
	default:
		return -EINVAL;
	}

	page = virt_to_head_page(ptr);
	if (sz > page_size(page))
		return -EINVAL;

	pfn = virt_to_phys(ptr) >> PAGE_SHIFT;
	return remap_pfn_range(vma, vma->vm_start, pfn, sz, vma->vm_page_prot);
}

SYSCALL_DEFINE6(io_uring_enter, unsigned int, fd, u32, to_submit,
		u32, min_complete, u32, flags, const sigset_t __user *, sig,
		size_t, sigsz)
{
	struct io_ring_ctx *ctx;
	long ret = -EBADF;
	int submitted = 0;
	struct fd f;

	if (flags & ~(IORING_ENTER_GETEVENTS | IORING_ENTER_SQ_WAKEUP))
		return -EINVAL;

	f = fdget(fd);
	if (!f.file)
		return -EBADF;

	ret = -EOPNOTSUPP;
	if (f.file->f_op != &io_uring_fops)
		goto out_fput;

	ret = -ENXIO;
	ctx = f.file->private_data;
	if (!percpu_ref_tryget(&ctx->refs))
		goto out_fput;

	/*
	 * For SQ polling, the thread will do all submissions and completions.
	 * Just return the requested submit count, and wake the thread if
	 * we were asked to.
	 */
	ret = 0;
	if (ctx->flags & IORING_SETUP_SQPOLL) {
		if (flags & IORING_ENTER_SQ_WAKEUP)
			wake_up(&ctx->sqo_wait);
		submitted = to_submit;
	} else if (to_submit) {
		to_submit = min(to_submit, ctx->sq_entries);

		mutex_lock(&ctx->uring_lock);
		submitted = io_ring_submit(ctx, to_submit);
		mutex_unlock(&ctx->uring_lock);

		if (submitted != to_submit)
			goto out;
	}
	if (flags & IORING_ENTER_GETEVENTS) {
		unsigned nr_events = 0;

		min_complete = min(min_complete, ctx->cq_entries);

		if (ctx->flags & IORING_SETUP_IOPOLL) {
			ret = io_iopoll_check(ctx, &nr_events, min_complete);
		} else {
			ret = io_cqring_wait(ctx, min_complete, sig, sigsz);
		}
	}

out:
	percpu_ref_put(&ctx->refs);
out_fput:
	fdput(f);
	return submitted ? submitted : ret;
}

static const struct file_operations io_uring_fops = {
	.release	= io_uring_release,
	.flush		= io_uring_flush,
	.mmap		= io_uring_mmap,
	.poll		= io_uring_poll,
	.fasync		= io_uring_fasync,
};

static int io_allocate_scq_urings(struct io_ring_ctx *ctx,
				  struct io_uring_params *p)
{
	struct io_rings *rings;
	size_t size, sq_array_offset;

	/* make sure these are sane, as we already accounted them */
	ctx->sq_entries = p->sq_entries;
	ctx->cq_entries = p->cq_entries;

	size = rings_size(p->sq_entries, p->cq_entries, &sq_array_offset);
	if (size == SIZE_MAX)
		return -EOVERFLOW;

	rings = io_mem_alloc(size);
	if (!rings)
		return -ENOMEM;

	ctx->rings = rings;
	ctx->sq_array = (u32 *)((char *)rings + sq_array_offset);
	rings->sq_ring_mask = p->sq_entries - 1;
	rings->cq_ring_mask = p->cq_entries - 1;
	rings->sq_ring_entries = p->sq_entries;
	rings->cq_ring_entries = p->cq_entries;
	ctx->sq_mask = rings->sq_ring_mask;
	ctx->cq_mask = rings->cq_ring_mask;

	size = array_size(sizeof(struct io_uring_sqe), p->sq_entries);
	if (size == SIZE_MAX) {
		io_mem_free(ctx->rings);
		ctx->rings = NULL;
		return -EOVERFLOW;
	}

	ctx->sq_sqes = io_mem_alloc(size);
	if (!ctx->sq_sqes) {
		io_mem_free(ctx->rings);
		ctx->rings = NULL;
		return -ENOMEM;
	}

	return 0;
}

/*
 * Allocate an anonymous fd, this is what constitutes the application
 * visible backing of an io_uring instance. The application mmaps this
 * fd to gain access to the SQ/CQ ring details. If UNIX sockets are enabled,
 * we have to tie this fd to a socket for file garbage collection purposes.
 */
static int io_uring_get_fd(struct io_ring_ctx *ctx)
{
	struct file *file;
	int ret;

#if defined(CONFIG_UNIX)
	ret = sock_create_kern(&init_net, PF_UNIX, SOCK_RAW, IPPROTO_IP,
				&ctx->ring_sock);
	if (ret)
		return ret;
#endif

	ret = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
	if (ret < 0)
		goto err;

	file = anon_inode_getfile("[io_uring]", &io_uring_fops, ctx,
					O_RDWR | O_CLOEXEC);
	if (IS_ERR(file)) {
		put_unused_fd(ret);
		ret = PTR_ERR(file);
		goto err;
	}

#if defined(CONFIG_UNIX)
	ctx->ring_sock->file = file;
	ctx->ring_sock->sk->sk_user_data = ctx;
#endif
	fd_install(ret, file);
	return ret;
err:
#if defined(CONFIG_UNIX)
	sock_release(ctx->ring_sock);
	ctx->ring_sock = NULL;
#endif
	return ret;
}

static int io_uring_create(unsigned entries, struct io_uring_params *p)
{
	struct user_struct *user = NULL;
	struct io_ring_ctx *ctx;
	bool account_mem;
	int ret;

	if (!entries || entries > IORING_MAX_ENTRIES)
		return -EINVAL;

	/*
	 * Use twice as many entries for the CQ ring. It's possible for the
	 * application to drive a higher depth than the size of the SQ ring,
	 * since the sqes are only used at submission time. This allows for
	 * some flexibility in overcommitting a bit.
	 */
	p->sq_entries = roundup_pow_of_two(entries);
	p->cq_entries = 2 * p->sq_entries;

	user = get_uid(current_user());
	account_mem = !capable(CAP_IPC_LOCK);

	if (account_mem) {
		ret = io_account_mem(user,
				ring_pages(p->sq_entries, p->cq_entries));
		if (ret) {
			free_uid(user);
			return ret;
		}
	}

	ctx = io_ring_ctx_alloc(p);
	if (!ctx) {
		if (account_mem)
			io_unaccount_mem(user, ring_pages(p->sq_entries,
								p->cq_entries));
		free_uid(user);
		return -ENOMEM;
	}
	ctx->compat = in_compat_syscall();
	ctx->account_mem = account_mem;
	ctx->user = user;

	ctx->creds = get_current_cred();
	if (!ctx->creds) {
		ret = -ENOMEM;
		goto err;
	}

	ret = io_allocate_scq_urings(ctx, p);
	if (ret)
		goto err;

	ret = io_sq_offload_start(ctx, p);
	if (ret)
		goto err;

	memset(&p->sq_off, 0, sizeof(p->sq_off));
	p->sq_off.head = offsetof(struct io_rings, sq.head);
	p->sq_off.tail = offsetof(struct io_rings, sq.tail);
	p->sq_off.ring_mask = offsetof(struct io_rings, sq_ring_mask);
	p->sq_off.ring_entries = offsetof(struct io_rings, sq_ring_entries);
	p->sq_off.flags = offsetof(struct io_rings, sq_flags);
	p->sq_off.dropped = offsetof(struct io_rings, sq_dropped);
	p->sq_off.array = (char *)ctx->sq_array - (char *)ctx->rings;

	memset(&p->cq_off, 0, sizeof(p->cq_off));
	p->cq_off.head = offsetof(struct io_rings, cq.head);
	p->cq_off.tail = offsetof(struct io_rings, cq.tail);
	p->cq_off.ring_mask = offsetof(struct io_rings, cq_ring_mask);
	p->cq_off.ring_entries = offsetof(struct io_rings, cq_ring_entries);
	p->cq_off.overflow = offsetof(struct io_rings, cq_overflow);
	p->cq_off.cqes = offsetof(struct io_rings, cqes);

	/*
	 * Install ring fd as the very last thing, so we don't risk someone
	 * having closed it before we finish setup
	 */
	ret = io_uring_get_fd(ctx);
	if (ret < 0)
		goto err;

	p->features = IORING_FEAT_SINGLE_MMAP;
	return ret;
err:
	io_ring_ctx_wait_and_kill(ctx);
	return ret;
}

/*
 * Sets up an aio uring context, and returns the fd. Applications asks for a
 * ring size, we return the actual sq/cq ring sizes (among other things) in the
 * params structure passed in.
 */
static long io_uring_setup(u32 entries, struct io_uring_params __user *params)
{
	struct io_uring_params p;
	long ret;
	int i;

	if (copy_from_user(&p, params, sizeof(p)))
		return -EFAULT;
	for (i = 0; i < ARRAY_SIZE(p.resv); i++) {
		if (p.resv[i])
			return -EINVAL;
	}

	if (p.flags & ~(IORING_SETUP_IOPOLL | IORING_SETUP_SQPOLL |
			IORING_SETUP_SQ_AFF))
		return -EINVAL;

	ret = io_uring_create(entries, &p);
	if (ret < 0)
		return ret;

	if (copy_to_user(params, &p, sizeof(p)))
		return -EFAULT;

	return ret;
}

SYSCALL_DEFINE2(io_uring_setup, u32, entries,
		struct io_uring_params __user *, params)
{
	return io_uring_setup(entries, params);
}

static int __io_uring_register(struct io_ring_ctx *ctx, unsigned opcode,
			       void __user *arg, unsigned nr_args)
	__releases(ctx->uring_lock)
	__acquires(ctx->uring_lock)
{
	int ret;

	/*
	 * We're inside the ring mutex, if the ref is already dying, then
	 * someone else killed the ctx or is already going through
	 * io_uring_register().
	 */
	if (percpu_ref_is_dying(&ctx->refs))
		return -ENXIO;

	percpu_ref_kill(&ctx->refs);

	/*
	 * Drop uring mutex before waiting for references to exit. If another
	 * thread is currently inside io_uring_enter() it might need to grab
	 * the uring_lock to make progress. If we hold it here across the drain
	 * wait, then we can deadlock. It's safe to drop the mutex here, since
	 * no new references will come in after we've killed the percpu ref.
	 */
	mutex_unlock(&ctx->uring_lock);
	wait_for_completion(&ctx->ctx_done);
	mutex_lock(&ctx->uring_lock);

	switch (opcode) {
	case IORING_REGISTER_BUFFERS:
		ret = io_sqe_buffer_register(ctx, arg, nr_args);
		break;
	case IORING_UNREGISTER_BUFFERS:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_sqe_buffer_unregister(ctx);
		break;
	case IORING_REGISTER_FILES:
		ret = io_sqe_files_register(ctx, arg, nr_args);
		break;
	case IORING_UNREGISTER_FILES:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_sqe_files_unregister(ctx);
		break;
	case IORING_REGISTER_EVENTFD:
		ret = -EINVAL;
		if (nr_args != 1)
			break;
		ret = io_eventfd_register(ctx, arg);
		break;
	case IORING_UNREGISTER_EVENTFD:
		ret = -EINVAL;
		if (arg || nr_args)
			break;
		ret = io_eventfd_unregister(ctx);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	/* bring the ctx back to life */
	reinit_completion(&ctx->ctx_done);
	percpu_ref_reinit(&ctx->refs);
	return ret;
}

SYSCALL_DEFINE4(io_uring_register, unsigned int, fd, unsigned int, opcode,
		void __user *, arg, unsigned int, nr_args)
{
	struct io_ring_ctx *ctx;
	long ret = -EBADF;
	struct fd f;

	f = fdget(fd);
	if (!f.file)
		return -EBADF;

	ret = -EOPNOTSUPP;
	if (f.file->f_op != &io_uring_fops)
		goto out_fput;

	ctx = f.file->private_data;

	mutex_lock(&ctx->uring_lock);
	ret = __io_uring_register(ctx, opcode, arg, nr_args);
	mutex_unlock(&ctx->uring_lock);
out_fput:
	fdput(f);
	return ret;
}

static int __init io_uring_init(void)
{
	req_cachep = KMEM_CACHE(io_kiocb, SLAB_HWCACHE_ALIGN | SLAB_PANIC);
	return 0;
};
__initcall(io_uring_init);
