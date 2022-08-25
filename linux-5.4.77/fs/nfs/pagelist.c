// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/fs/nfs/pagelist.c
 *
 * A set of helper functions for managing NFS read and write requests.
 * The main purpose of these routines is to provide support for the
 * coalescing of several requests into a single RPC call.
 *
 * Copyright 2000, 2001 (c) Trond Myklebust <trond.myklebust@fys.uio.no>
 *
 */

#include <linux/slab.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs.h>
#include <linux/nfs3.h>
#include <linux/nfs4.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_page.h>
#include <linux/nfs_mount.h>
#include <linux/export.h>

#include "internal.h"
#include "pnfs.h"

#define NFSDBG_FACILITY		NFSDBG_PAGECACHE

static struct kmem_cache *nfs_page_cachep;
static const struct rpc_call_ops nfs_pgio_common_ops;

struct nfs_pgio_mirror *
nfs_pgio_current_mirror(struct nfs_pageio_descriptor *desc)
{
	return nfs_pgio_has_mirroring(desc) ?
		&desc->pg_mirrors[desc->pg_mirror_idx] :
		&desc->pg_mirrors[0];
}
EXPORT_SYMBOL_GPL(nfs_pgio_current_mirror);

void nfs_pgheader_init(struct nfs_pageio_descriptor *desc,
		       struct nfs_pgio_header *hdr,
		       void (*release)(struct nfs_pgio_header *hdr))
{
	struct nfs_pgio_mirror *mirror = nfs_pgio_current_mirror(desc);


	hdr->req = nfs_list_entry(mirror->pg_list.next);
	hdr->inode = desc->pg_inode;
	hdr->cred = nfs_req_openctx(hdr->req)->cred;
	hdr->io_start = req_offset(hdr->req);
	hdr->good_bytes = mirror->pg_count;
	hdr->io_completion = desc->pg_io_completion;
	hdr->dreq = desc->pg_dreq;
	hdr->release = release;
	hdr->completion_ops = desc->pg_completion_ops;
	if (hdr->completion_ops->init_hdr)
		hdr->completion_ops->init_hdr(hdr);

	hdr->pgio_mirror_idx = desc->pg_mirror_idx;
}
EXPORT_SYMBOL_GPL(nfs_pgheader_init);

void nfs_set_pgio_error(struct nfs_pgio_header *hdr, int error, loff_t pos)
{
	unsigned int new = pos - hdr->io_start;

	if (hdr->good_bytes > new) {
		hdr->good_bytes = new;
		clear_bit(NFS_IOHDR_EOF, &hdr->flags);
		if (!test_and_set_bit(NFS_IOHDR_ERROR, &hdr->flags))
			hdr->error = error;
	}
}

static inline struct nfs_page *
nfs_page_alloc(void)
{
	struct nfs_page	*p = kmem_cache_zalloc(nfs_page_cachep, GFP_KERNEL);
	if (p)
		INIT_LIST_HEAD(&p->wb_list);
	return p;
}

static inline void
nfs_page_free(struct nfs_page *p)
{
	kmem_cache_free(nfs_page_cachep, p);
}

/**
 * nfs_iocounter_wait - wait for i/o to complete
 * @l_ctx: nfs_lock_context with io_counter to use
 *
 * returns -ERESTARTSYS if interrupted by a fatal signal.
 * Otherwise returns 0 once the io_count hits 0.
 */
int
nfs_iocounter_wait(struct nfs_lock_context *l_ctx)
{
	return wait_var_event_killable(&l_ctx->io_count,
				       !atomic_read(&l_ctx->io_count));
}

/**
 * nfs_async_iocounter_wait - wait on a rpc_waitqueue for I/O
 * to complete
 * @task: the rpc_task that should wait
 * @l_ctx: nfs_lock_context with io_counter to check
 *
 * Returns true if there is outstanding I/O to wait on and the
 * task has been put to sleep.
 */
bool
nfs_async_iocounter_wait(struct rpc_task *task, struct nfs_lock_context *l_ctx)
{
	struct inode *inode = d_inode(l_ctx->open_context->dentry);
	bool ret = false;

	if (atomic_read(&l_ctx->io_count) > 0) {
		rpc_sleep_on(&NFS_SERVER(inode)->uoc_rpcwaitq, task, NULL);
		ret = true;
	}

	if (atomic_read(&l_ctx->io_count) == 0) {
		rpc_wake_up_queued_task(&NFS_SERVER(inode)->uoc_rpcwaitq, task);
		ret = false;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(nfs_async_iocounter_wait);

/*
 * nfs_page_set_headlock - set the request PG_HEADLOCK
 * @req: request that is to be locked
 *
 * this lock must be held when modifying req->wb_head
 *
 * return 0 on success, < 0 on error
 */
int
nfs_page_set_headlock(struct nfs_page *req)
{
	if (!test_and_set_bit(PG_HEADLOCK, &req->wb_flags))
		return 0;

	set_bit(PG_CONTENDED1, &req->wb_flags);
	smp_mb__after_atomic();
	return wait_on_bit_lock(&req->wb_flags, PG_HEADLOCK,
				TASK_UNINTERRUPTIBLE);
}

/*
 * nfs_page_clear_headlock - clear the request PG_HEADLOCK
 * @req: request that is to be locked
 */
void
nfs_page_clear_headlock(struct nfs_page *req)
{
	smp_mb__before_atomic();
	clear_bit(PG_HEADLOCK, &req->wb_flags);
	smp_mb__after_atomic();
	if (!test_bit(PG_CONTENDED1, &req->wb_flags))
		return;
	wake_up_bit(&req->wb_flags, PG_HEADLOCK);
}

/*
 * nfs_page_group_lock - lock the head of the page group
 * @req: request in group that is to be locked
 *
 * this lock must be held when traversing or modifying the page
 * group list
 *
 * return 0 on success, < 0 on error
 */
int
nfs_page_group_lock(struct nfs_page *req)
{
	int ret;

	ret = nfs_page_set_headlock(req);
	if (ret || req->wb_head == req)
		return ret;
	return nfs_page_set_headlock(req->wb_head);
}

/*
 * nfs_page_group_unlock - unlock the head of the page group
 * @req: request in group that is to be unlocked
 */
void
nfs_page_group_unlock(struct nfs_page *req)
{
	if (req != req->wb_head)
		nfs_page_clear_headlock(req->wb_head);
	nfs_page_clear_headlock(req);
}

/*
 * nfs_page_group_sync_on_bit_locked
 *
 * must be called with page group lock held
 */
static bool
nfs_page_group_sync_on_bit_locked(struct nfs_page *req, unsigned int bit)
{
	struct nfs_page *head = req->wb_head;
	struct nfs_page *tmp;

	WARN_ON_ONCE(!test_bit(PG_HEADLOCK, &head->wb_flags));
	WARN_ON_ONCE(test_and_set_bit(bit, &req->wb_flags));

	tmp = req->wb_this_page;
	while (tmp != req) {
		if (!test_bit(bit, &tmp->wb_flags))
			return false;
		tmp = tmp->wb_this_page;
	}

	/* true! reset all bits */
	tmp = req;
	do {
		clear_bit(bit, &tmp->wb_flags);
		tmp = tmp->wb_this_page;
	} while (tmp != req);

	return true;
}

/*
 * nfs_page_group_sync_on_bit - set bit on current request, but only
 *   return true if the bit is set for all requests in page group
 * @req - request in page group
 * @bit - PG_* bit that is used to sync page group
 */
bool nfs_page_group_sync_on_bit(struct nfs_page *req, unsigned int bit)
{
	bool ret;

	nfs_page_group_lock(req);
	ret = nfs_page_group_sync_on_bit_locked(req, bit);
	nfs_page_group_unlock(req);

	return ret;
}

/*
 * nfs_page_group_init - Initialize the page group linkage for @req
 * @req - a new nfs request
 * @prev - the previous request in page group, or NULL if @req is the first
 *         or only request in the group (the head).
 */
static inline void
nfs_page_group_init(struct nfs_page *req, struct nfs_page *prev)
{
	struct inode *inode;
	WARN_ON_ONCE(prev == req);

	if (!prev) {
		/* a head request */
		req->wb_head = req;
		req->wb_this_page = req;
	} else {
		/* a subrequest */
		WARN_ON_ONCE(prev->wb_this_page != prev->wb_head);
		WARN_ON_ONCE(!test_bit(PG_HEADLOCK, &prev->wb_head->wb_flags));
		req->wb_head = prev->wb_head;
		req->wb_this_page = prev->wb_this_page;
		prev->wb_this_page = req;

		/* All subrequests take a ref on the head request until
		 * nfs_page_group_destroy is called */
		kref_get(&req->wb_head->wb_kref);

		/* grab extra ref and bump the request count if head request
		 * has extra ref from the write/commit path to handle handoff
		 * between write and commit lists. */
		if (test_bit(PG_INODE_REF, &prev->wb_head->wb_flags)) {
			inode = page_file_mapping(req->wb_page)->host;
			set_bit(PG_INODE_REF, &req->wb_flags);
			kref_get(&req->wb_kref);
			atomic_long_inc(&NFS_I(inode)->nrequests);
		}
	}
}

/*
 * nfs_page_group_destroy - sync the destruction of page groups
 * @req - request that no longer needs the page group
 *
 * releases the page group reference from each member once all
 * members have called this function.
 */
static void
nfs_page_group_destroy(struct kref *kref)
{
	struct nfs_page *req = container_of(kref, struct nfs_page, wb_kref);
	struct nfs_page *head = req->wb_head;
	struct nfs_page *tmp, *next;

	if (!nfs_page_group_sync_on_bit(req, PG_TEARDOWN))
		goto out;

	tmp = req;
	do {
		next = tmp->wb_this_page;
		/* unlink and free */
		tmp->wb_this_page = tmp;
		tmp->wb_head = tmp;
		nfs_free_request(tmp);
		tmp = next;
	} while (tmp != req);
out:
	/* subrequests must release the ref on the head request */
	if (head != req)
		nfs_release_request(head);
}

static struct nfs_page *
__nfs_create_request(struct nfs_lock_context *l_ctx, struct page *page,
		   unsigned int pgbase, unsigned int offset,
		   unsigned int count)
{
	struct nfs_page		*req;
	struct nfs_open_context *ctx = l_ctx->open_context;

	if (test_bit(NFS_CONTEXT_BAD, &ctx->flags))
		return ERR_PTR(-EBADF);
	/* try to allocate the request struct */
	req = nfs_page_alloc();
	if (req == NULL)
		return ERR_PTR(-ENOMEM);

	req->wb_lock_context = l_ctx;
	refcount_inc(&l_ctx->count);
	atomic_inc(&l_ctx->io_count);

	/* Initialize the request struct. Initially, we assume a
	 * long write-back delay. This will be adjusted in
	 * update_nfs_request below if the region is not locked. */
	req->wb_page    = page;
	if (page) {
		req->wb_index = page_index(page);
		get_page(page);
	}
	req->wb_offset  = offset;
	req->wb_pgbase	= pgbase;
	req->wb_bytes   = count;
	kref_init(&req->wb_kref);
	req->wb_nio = 0;
	return req;
}

/**
 * nfs_create_request - Create an NFS read/write request.
 * @ctx: open context to use
 * @page: page to write
 * @offset: starting offset within the page for the write
 * @count: number of bytes to read/write
 *
 * The page must be locked by the caller. This makes sure we never
 * create two different requests for the same page.
 * User should ensure it is safe to sleep in this function.
 */
struct nfs_page *
nfs_create_request(struct nfs_open_context *ctx, struct page *page,
		   unsigned int offset, unsigned int count)
{
	struct nfs_lock_context *l_ctx = nfs_get_lock_context(ctx);
	struct nfs_page *ret;

	if (IS_ERR(l_ctx))
		return ERR_CAST(l_ctx);
	ret = __nfs_create_request(l_ctx, page, offset, offset, count);
	if (!IS_ERR(ret))
		nfs_page_group_init(ret, NULL);
	nfs_put_lock_context(l_ctx);
	return ret;
}

static struct nfs_page *
nfs_create_subreq(struct nfs_page *req, struct nfs_page *last,
		  unsigned int pgbase, unsigned int offset,
		  unsigned int count)
{
	struct nfs_page *ret;

	ret = __nfs_create_request(req->wb_lock_context, req->wb_page,
			pgbase, offset, count);
	if (!IS_ERR(ret)) {
		nfs_lock_request(ret);
		ret->wb_index = req->wb_index;
		nfs_page_group_init(ret, last);
		ret->wb_nio = req->wb_nio;
	}
	return ret;
}

/**
 * nfs_unlock_request - Unlock request and wake up sleepers.
 * @req: pointer to request
 */
void nfs_unlock_request(struct nfs_page *req)
{
	if (!NFS_WBACK_BUSY(req)) {
		printk(KERN_ERR "NFS: Invalid unlock attempted\n");
		BUG();
	}
	smp_mb__before_atomic();
	clear_bit(PG_BUSY, &req->wb_flags);
	smp_mb__after_atomic();
	if (!test_bit(PG_CONTENDED2, &req->wb_flags))
		return;
	wake_up_bit(&req->wb_flags, PG_BUSY);
}

/**
 * nfs_unlock_and_release_request - Unlock request and release the nfs_page
 * @req: pointer to request
 */
void nfs_unlock_and_release_request(struct nfs_page *req)
{
	nfs_unlock_request(req);
	nfs_release_request(req);
}

/*
 * nfs_clear_request - Free up all resources allocated to the request
 * @req:
 *
 * Release page and open context resources associated with a read/write
 * request after it has completed.
 */
static void nfs_clear_request(struct nfs_page *req)
{
	struct page *page = req->wb_page;
	struct nfs_lock_context *l_ctx = req->wb_lock_context;
	struct nfs_open_context *ctx;

	if (page != NULL) {
		put_page(page);
		req->wb_page = NULL;
	}
	if (l_ctx != NULL) {
		if (atomic_dec_and_test(&l_ctx->io_count)) {
			wake_up_var(&l_ctx->io_count);
			ctx = l_ctx->open_context;
			if (test_bit(NFS_CONTEXT_UNLOCK, &ctx->flags))
				rpc_wake_up(&NFS_SERVER(d_inode(ctx->dentry))->uoc_rpcwaitq);
		}
		nfs_put_lock_context(l_ctx);
		req->wb_lock_context = NULL;
	}
}

/**
 * nfs_release_request - Release the count on an NFS read/write request
 * @req: request to release
 *
 * Note: Should never be called with the spinlock held!
 */
void nfs_free_request(struct nfs_page *req)
{
	WARN_ON_ONCE(req->wb_this_page != req);

	/* extra debug: make sure no sync bits are still set */
	WARN_ON_ONCE(test_bit(PG_TEARDOWN, &req->wb_flags));
	WARN_ON_ONCE(test_bit(PG_UNLOCKPAGE, &req->wb_flags));
	WARN_ON_ONCE(test_bit(PG_UPTODATE, &req->wb_flags));
	WARN_ON_ONCE(test_bit(PG_WB_END, &req->wb_flags));
	WARN_ON_ONCE(test_bit(PG_REMOVE, &req->wb_flags));

	/* Release struct file and open context */
	nfs_clear_request(req);
	nfs_page_free(req);
}

void nfs_release_request(struct nfs_page *req)
{
	kref_put(&req->wb_kref, nfs_page_group_destroy);
}
EXPORT_SYMBOL_GPL(nfs_release_request);

/**
 * nfs_wait_on_request - Wait for a request to complete.
 * @req: request to wait upon.
 *
 * Interruptible by fatal signals only.
 * The user is responsible for holding a count on the request.
 */
int
nfs_wait_on_request(struct nfs_page *req)
{
	if (!test_bit(PG_BUSY, &req->wb_flags))
		return 0;
	set_bit(PG_CONTENDED2, &req->wb_flags);
	smp_mb__after_atomic();
	return wait_on_bit_io(&req->wb_flags, PG_BUSY,
			      TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL_GPL(nfs_wait_on_request);

/*
 * nfs_generic_pg_test - determine if requests can be coalesced
 * @desc: pointer to descriptor
 * @prev: previous request in desc, or NULL
 * @req: this request
 *
 * Returns zero if @req cannot be coalesced into @desc, otherwise it returns
 * the size of the request.
 */
size_t nfs_generic_pg_test(struct nfs_pageio_descriptor *desc,
			   struct nfs_page *prev, struct nfs_page *req)
{
	struct nfs_pgio_mirror *mirror = nfs_pgio_current_mirror(desc);


	if (mirror->pg_count > mirror->pg_bsize) {
		/* should never happen */
		WARN_ON_ONCE(1);
		return 0;
	}

	/*
	 * Limit the request size so that we can still allocate a page array
	 * for it without upsetting the slab allocator.
	 */
	if (((mirror->pg_count + req->wb_bytes) >> PAGE_SHIFT) *
			sizeof(struct page *) > PAGE_SIZE)
		return 0;

	return min(mirror->pg_bsize - mirror->pg_count, (size_t)req->wb_bytes);
}
EXPORT_SYMBOL_GPL(nfs_generic_pg_test);

struct nfs_pgio_header *nfs_pgio_header_alloc(const struct nfs_rw_ops *ops)
{
	struct nfs_pgio_header *hdr = ops->rw_alloc_header();

	if (hdr) {
		INIT_LIST_HEAD(&hdr->pages);
		hdr->rw_ops = ops;
	}
	return hdr;
}
EXPORT_SYMBOL_GPL(nfs_pgio_header_alloc);

/**
 * nfs_pgio_data_destroy - make @hdr suitable for reuse
 *
 * Frees memory and releases refs from nfs_generic_pgio, so that it may
 * be called again.
 *
 * @hdr: A header that has had nfs_generic_pgio called
 */
static void nfs_pgio_data_destroy(struct nfs_pgio_header *hdr)
{
	if (hdr->args.context)
		put_nfs_open_context(hdr->args.context);
	if (hdr->page_array.pagevec != hdr->page_array.page_array)
		kfree(hdr->page_array.pagevec);
}

/*
 * nfs_pgio_header_free - Free a read or write header
 * @hdr: The header to free
 */
void nfs_pgio_header_free(struct nfs_pgio_header *hdr)
{
	nfs_pgio_data_destroy(hdr);
	hdr->rw_ops->rw_free_header(hdr);
}
EXPORT_SYMBOL_GPL(nfs_pgio_header_free);

/**
 * nfs_pgio_rpcsetup - Set up arguments for a pageio call
 * @hdr: The pageio hdr
 * @count: Number of bytes to read
 * @how: How to commit data (writes only)
 * @cinfo: Commit information for the call (writes only)
 */
static void nfs_pgio_rpcsetup(struct nfs_pgio_header *hdr,
			      unsigned int count,
			      int how, struct nfs_commit_info *cinfo)
{
	struct nfs_page *req = hdr->req;

	/* Set up the RPC argument and reply structs
	 * NB: take care not to mess about with hdr->commit et al. */

	hdr->args.fh     = NFS_FH(hdr->inode);
	hdr->args.offset = req_offset(req);
	/* pnfs_set_layoutcommit needs this */
	hdr->mds_offset = hdr->args.offset;
	hdr->args.pgbase = req->wb_pgbase;
	hdr->args.pages  = hdr->page_array.pagevec;
	hdr->args.count  = count;
	hdr->args.context = get_nfs_open_context(nfs_req_openctx(req));
	hdr->args.lock_context = req->wb_lock_context;
	hdr->args.stable  = NFS_UNSTABLE;
	switch (how & (FLUSH_STABLE | FLUSH_COND_STABLE)) {
	case 0:
		break;
	case FLUSH_COND_STABLE:
		if (nfs_reqs_to_commit(cinfo))
			break;
		/* fall through */
	default:
		hdr->args.stable = NFS_FILE_SYNC;
	}

	hdr->res.fattr   = &hdr->fattr;
	hdr->res.count   = 0;
	hdr->res.eof     = 0;
	hdr->res.verf    = &hdr->verf;
	nfs_fattr_init(&hdr->fattr);
}

/**
 * nfs_pgio_prepare - Prepare pageio hdr to go over the wire
 * @task: The current task
 * @calldata: pageio header to prepare
 */
static void nfs_pgio_prepare(struct rpc_task *task, void *calldata)
{
	struct nfs_pgio_header *hdr = calldata;
	int err;
	err = NFS_PROTO(hdr->inode)->pgio_rpc_prepare(task, hdr);
	if (err)
		rpc_exit(task, err);
}

int nfs_initiate_pgio(struct rpc_clnt *clnt, struct nfs_pgio_header *hdr,
		      const struct cred *cred, const struct nfs_rpc_ops *rpc_ops,
		      const struct rpc_call_ops *call_ops, int how, int flags)
{
	struct rpc_task *task;
	struct rpc_message msg = {
		.rpc_argp = &hdr->args,
		.rpc_resp = &hdr->res,
		.rpc_cred = cred,
	};
	struct rpc_task_setup task_setup_data = {
		.rpc_client = clnt,
		.task = &hdr->task,
		.rpc_message = &msg,
		.callback_ops = call_ops,
		.callback_data = hdr,
		.workqueue = nfsiod_workqueue,
		.flags = RPC_TASK_ASYNC | flags,
	};
	int ret = 0;

	hdr->rw_ops->rw_initiate(hdr, &msg, rpc_ops, &task_setup_data, how);

	dprintk("NFS: initiated pgio call "
		"(req %s/%llu, %u bytes @ offset %llu)\n",
		hdr->inode->i_sb->s_id,
		(unsigned long long)NFS_FILEID(hdr->inode),
		hdr->args.count,
		(unsigned long long)hdr->args.offset);

	task = rpc_run_task(&task_setup_data);
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto out;
	}
	if (how & FLUSH_SYNC) {
		ret = rpc_wait_for_completion_task(task);
		if (ret == 0)
			ret = task->tk_status;
	}
	rpc_put_task(task);
out:
	return ret;
}
EXPORT_SYMBOL_GPL(nfs_initiate_pgio);

/**
 * nfs_pgio_error - Clean up from a pageio error
 * @hdr: pageio header
 */
static void nfs_pgio_error(struct nfs_pgio_header *hdr)
{
	set_bit(NFS_IOHDR_REDO, &hdr->flags);
	hdr->completion_ops->completion(hdr);
}

/**
 * nfs_pgio_release - Release pageio data
 * @calldata: The pageio header to release
 */
static void nfs_pgio_release(void *calldata)
{
	struct nfs_pgio_header *hdr = calldata;
	hdr->completion_ops->completion(hdr);
}

static void nfs_pageio_mirror_init(struct nfs_pgio_mirror *mirror,
				   unsigned int bsize)
{
	INIT_LIST_HEAD(&mirror->pg_list);
	mirror->pg_bytes_written = 0;
	mirror->pg_count = 0;
	mirror->pg_bsize = bsize;
	mirror->pg_base = 0;
	mirror->pg_recoalesce = 0;
}

/**
 * nfs_pageio_init - initialise a page io descriptor
 * @desc: pointer to descriptor
 * @inode: pointer to inode
 * @pg_ops: pointer to pageio operations
 * @compl_ops: pointer to pageio completion operations
 * @rw_ops: pointer to nfs read/write operations
 * @bsize: io block size
 * @io_flags: extra parameters for the io function
 */
void nfs_pageio_init(struct nfs_pageio_descriptor *desc,
		     struct inode *inode,
		     const struct nfs_pageio_ops *pg_ops,
		     const struct nfs_pgio_completion_ops *compl_ops,
		     const struct nfs_rw_ops *rw_ops,
		     size_t bsize,
		     int io_flags)
{
	desc->pg_moreio = 0;
	desc->pg_inode = inode;
	desc->pg_ops = pg_ops;
	desc->pg_completion_ops = compl_ops;
	desc->pg_rw_ops = rw_ops;
	desc->pg_ioflags = io_flags;
	desc->pg_error = 0;
	desc->pg_lseg = NULL;
	desc->pg_io_completion = NULL;
	desc->pg_dreq = NULL;
	desc->pg_bsize = bsize;

	desc->pg_mirror_count = 1;
	desc->pg_mirror_idx = 0;

	desc->pg_mirrors_dynamic = NULL;
	desc->pg_mirrors = desc->pg_mirrors_static;
	nfs_pageio_mirror_init(&desc->pg_mirrors[0], bsize);
	desc->pg_maxretrans = 0;
}

/**
 * nfs_pgio_result - Basic pageio error handling
 * @task: The task that ran
 * @calldata: Pageio header to check
 */
static void nfs_pgio_result(struct rpc_task *task, void *calldata)
{
	struct nfs_pgio_header *hdr = calldata;
	struct inode *inode = hdr->inode;

	dprintk("NFS: %s: %5u, (status %d)\n", __func__,
		task->tk_pid, task->tk_status);

	if (hdr->rw_ops->rw_done(task, hdr, inode) != 0)
		return;
	if (task->tk_status < 0)
		nfs_set_pgio_error(hdr, task->tk_status, hdr->args.offset);
	else
		hdr->rw_ops->rw_result(task, hdr);
}

/*
 * Create an RPC task for the given read or write request and kick it.
 * The page must have been locked by the caller.
 *
 * It may happen that the page we're passed is not marked dirty.
 * This is the case if nfs_updatepage detects a conflicting request
 * that has been written but not committed.
 */
int nfs_generic_pgio(struct nfs_pageio_descriptor *desc,
		     struct nfs_pgio_header *hdr)
{
	struct nfs_pgio_mirror *mirror = nfs_pgio_current_mirror(desc);

	struct nfs_page		*req;
	struct page		**pages,
				*last_page;
	struct list_head *head = &mirror->pg_list;
	struct nfs_commit_info cinfo;
	struct nfs_page_array *pg_array = &hdr->page_array;
	unsigned int pagecount, pageused;
	gfp_t gfp_flags = GFP_KERNEL;

	pagecount = nfs_page_array_len(mirror->pg_base, mirror->pg_count);
	pg_array->npages = pagecount;

	if (pagecount <= ARRAY_SIZE(pg_array->page_array))
		pg_array->pagevec = pg_array->page_array;
	else {
		pg_array->pagevec = kcalloc(pagecount, sizeof(struct page *), gfp_flags);
		if (!pg_array->pagevec) {
			pg_array->npages = 0;
			nfs_pgio_error(hdr);
			desc->pg_error = -ENOMEM;
			return desc->pg_error;
		}
	}

	nfs_init_cinfo(&cinfo, desc->pg_inode, desc->pg_dreq);
	pages = hdr->page_array.pagevec;
	last_page = NULL;
	pageused = 0;
	while (!list_empty(head)) {
		req = nfs_list_entry(head->next);
		nfs_list_move_request(req, &hdr->pages);

		if (!last_page || last_page != req->wb_page) {
			pageused++;
			if (pageused > pagecount)
				break;
			*pages++ = last_page = req->wb_page;
		}
	}
	if (WARN_ON_ONCE(pageused != pagecount)) {
		nfs_pgio_error(hdr);
		desc->pg_error = -EINVAL;
		return desc->pg_error;
	}

	if ((desc->pg_ioflags & FLUSH_COND_STABLE) &&
	    (desc->pg_moreio || nfs_reqs_to_commit(&cinfo)))
		desc->pg_ioflags &= ~FLUSH_COND_STABLE;

	/* Set up the argument struct */
	nfs_pgio_rpcsetup(hdr, mirror->pg_count, desc->pg_ioflags, &cinfo);
	desc->pg_rpc_callops = &nfs_pgio_common_ops;
	return 0;
}
EXPORT_SYMBOL_GPL(nfs_generic_pgio);

static int nfs_generic_pg_pgios(struct nfs_pageio_descriptor *desc)
{
	struct nfs_pgio_header *hdr;
	int ret;

	hdr = nfs_pgio_header_alloc(desc->pg_rw_ops);
	if (!hdr) {
		desc->pg_error = -ENOMEM;
		return desc->pg_error;
	}
	nfs_pgheader_init(desc, hdr, nfs_pgio_header_free);
	ret = nfs_generic_pgio(desc, hdr);
	if (ret == 0)
		ret = nfs_initiate_pgio(NFS_CLIENT(hdr->inode),
					hdr,
					hdr->cred,
					NFS_PROTO(hdr->inode),
					desc->pg_rpc_callops,
					desc->pg_ioflags, 0);
	return ret;
}

static struct nfs_pgio_mirror *
nfs_pageio_alloc_mirrors(struct nfs_pageio_descriptor *desc,
		unsigned int mirror_count)
{
	struct nfs_pgio_mirror *ret;
	unsigned int i;

	kfree(desc->pg_mirrors_dynamic);
	desc->pg_mirrors_dynamic = NULL;
	if (mirror_count == 1)
		return desc->pg_mirrors_static;
	ret = kmalloc_array(mirror_count, sizeof(*ret), GFP_KERNEL);
	if (ret != NULL) {
		for (i = 0; i < mirror_count; i++)
			nfs_pageio_mirror_init(&ret[i], desc->pg_bsize);
		desc->pg_mirrors_dynamic = ret;
	}
	return ret;
}

/*
 * nfs_pageio_setup_mirroring - determine if mirroring is to be used
 *				by calling the pg_get_mirror_count op
 */
static void nfs_pageio_setup_mirroring(struct nfs_pageio_descriptor *pgio,
				       struct nfs_page *req)
{
	unsigned int mirror_count = 1;

	if (pgio->pg_ops->pg_get_mirror_count)
		mirror_count = pgio->pg_ops->pg_get_mirror_count(pgio, req);
	if (mirror_count == pgio->pg_mirror_count || pgio->pg_error < 0)
		return;

	if (!mirror_count || mirror_count > NFS_PAGEIO_DESCRIPTOR_MIRROR_MAX) {
		pgio->pg_error = -EINVAL;
		return;
	}

	pgio->pg_mirrors = nfs_pageio_alloc_mirrors(pgio, mirror_count);
	if (pgio->pg_mirrors == NULL) {
		pgio->pg_error = -ENOMEM;
		pgio->pg_mirrors = pgio->pg_mirrors_static;
		mirror_count = 1;
	}
	pgio->pg_mirror_count = mirror_count;
}

static void nfs_pageio_cleanup_mirroring(struct nfs_pageio_descriptor *pgio)
{
	pgio->pg_mirror_count = 1;
	pgio->pg_mirror_idx = 0;
	pgio->pg_mirrors = pgio->pg_mirrors_static;
	kfree(pgio->pg_mirrors_dynamic);
	pgio->pg_mirrors_dynamic = NULL;
}

static bool nfs_match_lock_context(const struct nfs_lock_context *l1,
		const struct nfs_lock_context *l2)
{
	return l1->lockowner == l2->lockowner;
}

/**
 * nfs_can_coalesce_requests - test two requests for compatibility
 * @prev: pointer to nfs_page
 * @req: pointer to nfs_page
 * @pgio: pointer to nfs_pagio_descriptor
 *
 * The nfs_page structures 'prev' and 'req' are compared to ensure that the
 * page data area they describe is contiguous, and that their RPC
 * credentials, NFSv4 open state, and lockowners are the same.
 *
 * Return 'true' if this is the case, else return 'false'.
 */
static bool nfs_can_coalesce_requests(struct nfs_page *prev,
				      struct nfs_page *req,
				      struct nfs_pageio_descriptor *pgio)
{
	size_t size;
	struct file_lock_context *flctx;

	if (prev) {
		if (!nfs_match_open_context(nfs_req_openctx(req), nfs_req_openctx(prev)))
			return false;
		flctx = d_inode(nfs_req_openctx(req)->dentry)->i_flctx;
		if (flctx != NULL &&
		    !(list_empty_careful(&flctx->flc_posix) &&
		      list_empty_careful(&flctx->flc_flock)) &&
		    !nfs_match_lock_context(req->wb_lock_context,
					    prev->wb_lock_context))
			return false;
		if (req_offset(req) != req_offset(prev) + prev->wb_bytes)
			return false;
		if (req->wb_page == prev->wb_page) {
			if (req->wb_pgbase != prev->wb_pgbase + prev->wb_bytes)
				return false;
		} else {
			if (req->wb_pgbase != 0 ||
			    prev->wb_pgbase + prev->wb_bytes != PAGE_SIZE)
				return false;
		}
	}
	size = pgio->pg_ops->pg_test(pgio, prev, req);
	WARN_ON_ONCE(size > req->wb_bytes);
	if (size && size < req->wb_bytes)
		req->wb_bytes = size;
	return size > 0;
}

/**
 * nfs_pageio_do_add_request - Attempt to coalesce a request into a page list.
 * @desc: destination io descriptor
 * @req: request
 *
 * Returns true if the request 'req' was successfully coalesced into the
 * existing list of pages 'desc'.
 */
static int nfs_pageio_do_add_request(struct nfs_pageio_descriptor *desc,
				     struct nfs_page *req)
{
	struct nfs_pgio_mirror *mirror = nfs_pgio_current_mirror(desc);

	struct nfs_page *prev = NULL;

	if (mirror->pg_count != 0) {
		prev = nfs_list_entry(mirror->pg_list.prev);
	} else {
		if (desc->pg_ops->pg_init)
			desc->pg_ops->pg_init(desc, req);
		if (desc->pg_error < 0)
			return 0;
		mirror->pg_base = req->wb_pgbase;
	}

	if (desc->pg_maxretrans && req->wb_nio > desc->pg_maxretrans) {
		if (NFS_SERVER(desc->pg_inode)->flags & NFS_MOUNT_SOFTERR)
			desc->pg_error = -ETIMEDOUT;
		else
			desc->pg_error = -EIO;
		return 0;
	}

	if (!nfs_can_coalesce_requests(prev, req, desc))
		return 0;
	nfs_list_move_request(req, &mirror->pg_list);
	mirror->pg_count += req->wb_bytes;
	return 1;
}

/*
 * Helper for nfs_pageio_add_request and nfs_pageio_complete
 */
static void nfs_pageio_doio(struct nfs_pageio_descriptor *desc)
{
	struct nfs_pgio_mirror *mirror = nfs_pgio_current_mirror(desc);


	if (!list_empty(&mirror->pg_list)) {
		int error = desc->pg_ops->pg_doio(desc);
		if (error < 0)
			desc->pg_error = error;
		else
			mirror->pg_bytes_written += mirror->pg_count;
	}
	if (list_empty(&mirror->pg_list)) {
		mirror->pg_count = 0;
		mirror->pg_base = 0;
	}
}

static void
nfs_pageio_cleanup_request(struct nfs_pageio_descriptor *desc,
		struct nfs_page *req)
{
	LIST_HEAD(head);

	nfs_list_move_request(req, &head);
	desc->pg_completion_ops->error_cleanup(&head, desc->pg_error);
}

/**
 * nfs_pageio_add_request - Attempt to coalesce a request into a page list.
 * @desc: destination io descriptor
 * @req: request
 *
 * This may split a request into subrequests which are all part of the
 * same page group.
 *
 * Returns true if the request 'req' was successfully coalesced into the
 * existing list of pages 'desc'.
 */
static int __nfs_pageio_add_request(struct nfs_pageio_descriptor *desc,
			   struct nfs_page *req)
{
	struct nfs_pgio_mirror *mirror = nfs_pgio_current_mirror(desc);

	struct nfs_page *subreq;
	unsigned int bytes_left = 0;
	unsigned int offset, pgbase;

	nfs_page_group_lock(req);

	subreq = req;
	bytes_left = subreq->wb_bytes;
	offset = subreq->wb_offset;
	pgbase = subreq->wb_pgbase;

	do {
		if (!nfs_pageio_do_add_request(desc, subreq)) {
			/* make sure pg_test call(s) did nothing */
			WARN_ON_ONCE(subreq->wb_bytes != bytes_left);
			WARN_ON_ONCE(subreq->wb_offset != offset);
			WARN_ON_ONCE(subreq->wb_pgbase != pgbase);

			nfs_page_group_unlock(req);
			desc->pg_moreio = 1;
			nfs_pageio_doio(desc);
			if (desc->pg_error < 0 || mirror->pg_recoalesce)
				goto out_cleanup_subreq;
			/* retry add_request for this subreq */
			nfs_page_group_lock(req);
			continue;
		}

		/* check for buggy pg_test call(s) */
		WARN_ON_ONCE(subreq->wb_bytes + subreq->wb_pgbase > PAGE_SIZE);
		WARN_ON_ONCE(subreq->wb_bytes > bytes_left);
		WARN_ON_ONCE(subreq->wb_bytes == 0);

		bytes_left -= subreq->wb_bytes;
		offset += subreq->wb_bytes;
		pgbase += subreq->wb_bytes;

		if (bytes_left) {
			subreq = nfs_create_subreq(req, subreq, pgbase,
					offset, bytes_left);
			if (IS_ERR(subreq))
				goto err_ptr;
		}
	} while (bytes_left > 0);

	nfs_page_group_unlock(req);
	return 1;
err_ptr:
	desc->pg_error = PTR_ERR(subreq);
	nfs_page_group_unlock(req);
	return 0;
out_cleanup_subreq:
	if (req != subreq)
		nfs_pageio_cleanup_request(desc, subreq);
	return 0;
}

static int nfs_do_recoalesce(struct nfs_pageio_descriptor *desc)
{
	struct nfs_pgio_mirror *mirror = nfs_pgio_current_mirror(desc);
	LIST_HEAD(head);

	do {
		list_splice_init(&mirror->pg_list, &head);
		mirror->pg_bytes_written -= mirror->pg_count;
		mirror->pg_count = 0;
		mirror->pg_base = 0;
		mirror->pg_recoalesce = 0;

		while (!list_empty(&head)) {
			struct nfs_page *req;

			req = list_first_entry(&head, struct nfs_page, wb_list);
			if (__nfs_pageio_add_request(desc, req))
				continue;
			if (desc->pg_error < 0) {
				list_splice_tail(&head, &mirror->pg_list);
				mirror->pg_recoalesce = 1;
				return 0;
			}
			break;
		}
	} while (mirror->pg_recoalesce);
	return 1;
}

static int nfs_pageio_add_request_mirror(struct nfs_pageio_descriptor *desc,
		struct nfs_page *req)
{
	int ret;

	do {
		ret = __nfs_pageio_add_request(desc, req);
		if (ret)
			break;
		if (desc->pg_error < 0)
			break;
		ret = nfs_do_recoalesce(desc);
	} while (ret);

	return ret;
}

static void nfs_pageio_error_cleanup(struct nfs_pageio_descriptor *desc)
{
	u32 midx;
	struct nfs_pgio_mirror *mirror;

	if (!desc->pg_error)
		return;

	for (midx = 0; midx < desc->pg_mirror_count; midx++) {
		mirror = &desc->pg_mirrors[midx];
		desc->pg_completion_ops->error_cleanup(&mirror->pg_list,
				desc->pg_error);
	}
}

int nfs_pageio_add_request(struct nfs_pageio_descriptor *desc,
			   struct nfs_page *req)
{
	u32 midx;
	unsigned int pgbase, offset, bytes;
	struct nfs_page *dupreq, *lastreq;

	pgbase = req->wb_pgbase;
	offset = req->wb_offset;
	bytes = req->wb_bytes;

	nfs_pageio_setup_mirroring(desc, req);
	if (desc->pg_error < 0)
		goto out_failed;

	/* Create the mirror instances first, and fire them off */
	for (midx = 1; midx < desc->pg_mirror_count; midx++) {
		nfs_page_group_lock(req);

		/* find the last request */
		for (lastreq = req->wb_head;
		     lastreq->wb_this_page != req->wb_head;
		     lastreq = lastreq->wb_this_page)
			;

		dupreq = nfs_create_subreq(req, lastreq,
				pgbase, offset, bytes);

		nfs_page_group_unlock(req);
		if (IS_ERR(dupreq)) {
			desc->pg_error = PTR_ERR(dupreq);
			goto out_failed;
		}

		desc->pg_mirror_idx = midx;
		if (!nfs_pageio_add_request_mirror(desc, dupreq))
			goto out_cleanup_subreq;
	}

	desc->pg_mirror_idx = 0;
	if (!nfs_pageio_add_request_mirror(desc, req))
		goto out_failed;

	return 1;

out_cleanup_subreq:
	nfs_pageio_cleanup_request(desc, dupreq);
out_failed:
	nfs_pageio_error_cleanup(desc);
	return 0;
}

/*
 * nfs_pageio_complete_mirror - Complete I/O on the current mirror of an
 *				nfs_pageio_descriptor
 * @desc: pointer to io descriptor
 * @mirror_idx: pointer to mirror index
 */
static void nfs_pageio_complete_mirror(struct nfs_pageio_descriptor *desc,
				       u32 mirror_idx)
{
	struct nfs_pgio_mirror *mirror = &desc->pg_mirrors[mirror_idx];
	u32 restore_idx = desc->pg_mirror_idx;

	if (nfs_pgio_has_mirroring(desc))
		desc->pg_mirror_idx = mirror_idx;
	for (;;) {
		nfs_pageio_doio(desc);
		if (desc->pg_error < 0 || !mirror->pg_recoalesce)
			break;
		if (!nfs_do_recoalesce(desc))
			break;
	}
	desc->pg_mirror_idx = restore_idx;
}

/*
 * nfs_pageio_resend - Transfer requests to new descriptor and resend
 * @hdr - the pgio header to move request from
 * @desc - the pageio descriptor to add requests to
 *
 * Try to move each request (nfs_page) from @hdr to @desc then attempt
 * to send them.
 *
 * Returns 0 on success and < 0 on error.
 */
int nfs_pageio_resend(struct nfs_pageio_descriptor *desc,
		      struct nfs_pgio_header *hdr)
{
	LIST_HEAD(pages);

	desc->pg_io_completion = hdr->io_completion;
	desc->pg_dreq = hdr->dreq;
	list_splice_init(&hdr->pages, &pages);
	while (!list_empty(&pages)) {
		struct nfs_page *req = nfs_list_entry(pages.next);

		if (!nfs_pageio_add_request(desc, req))
			break;
	}
	nfs_pageio_complete(desc);
	if (!list_empty(&pages)) {
		int err = desc->pg_error < 0 ? desc->pg_error : -EIO;
		hdr->completion_ops->error_cleanup(&pages, err);
		nfs_set_pgio_error(hdr, err, hdr->io_start);
		return err;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(nfs_pageio_resend);

/**
 * nfs_pageio_complete - Complete I/O then cleanup an nfs_pageio_descriptor
 * @desc: pointer to io descriptor
 */
void nfs_pageio_complete(struct nfs_pageio_descriptor *desc)
{
	u32 midx;

	for (midx = 0; midx < desc->pg_mirror_count; midx++)
		nfs_pageio_complete_mirror(desc, midx);

	if (desc->pg_error < 0)
		nfs_pageio_error_cleanup(desc);
	if (desc->pg_ops->pg_cleanup)
		desc->pg_ops->pg_cleanup(desc);
	nfs_pageio_cleanup_mirroring(desc);
}

/**
 * nfs_pageio_cond_complete - Conditional I/O completion
 * @desc: pointer to io descriptor
 * @index: page index
 *
 * It is important to ensure that processes don't try to take locks
 * on non-contiguous ranges of pages as that might deadlock. This
 * function should be called before attempting to wait on a locked
 * nfs_page. It will complete the I/O if the page index 'index'
 * is not contiguous with the existing list of pages in 'desc'.
 */
void nfs_pageio_cond_complete(struct nfs_pageio_descriptor *desc, pgoff_t index)
{
	struct nfs_pgio_mirror *mirror;
	struct nfs_page *prev;
	u32 midx;

	for (midx = 0; midx < desc->pg_mirror_count; midx++) {
		mirror = &desc->pg_mirrors[midx];
		if (!list_empty(&mirror->pg_list)) {
			prev = nfs_list_entry(mirror->pg_list.prev);
			if (index != prev->wb_index + 1) {
				nfs_pageio_complete(desc);
				break;
			}
		}
	}
}

/*
 * nfs_pageio_stop_mirroring - stop using mirroring (set mirror count to 1)
 */
void nfs_pageio_stop_mirroring(struct nfs_pageio_descriptor *pgio)
{
	nfs_pageio_complete(pgio);
}

int __init nfs_init_nfspagecache(void)
{
	nfs_page_cachep = kmem_cache_create("nfs_page",
					    sizeof(struct nfs_page),
					    0, SLAB_HWCACHE_ALIGN,
					    NULL);
	if (nfs_page_cachep == NULL)
		return -ENOMEM;

	return 0;
}

void nfs_destroy_nfspagecache(void)
{
	kmem_cache_destroy(nfs_page_cachep);
}

static const struct rpc_call_ops nfs_pgio_common_ops = {
	.rpc_call_prepare = nfs_pgio_prepare,
	.rpc_call_done = nfs_pgio_result,
	.rpc_release = nfs_pgio_release,
};

const struct nfs_pageio_ops nfs_pgio_rw_ops = {
	.pg_test = nfs_generic_pg_test,
	.pg_doio = nfs_generic_pg_pgios,
};
