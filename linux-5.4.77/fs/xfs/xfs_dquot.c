// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2003 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_inode.h"
#include "xfs_bmap.h"
#include "xfs_quota.h"
#include "xfs_trans.h"
#include "xfs_buf_item.h"
#include "xfs_trans_space.h"
#include "xfs_trans_priv.h"
#include "xfs_qm.h"
#include "xfs_trace.h"
#include "xfs_log.h"
#include "xfs_bmap_btree.h"

/*
 * Lock order:
 *
 * ip->i_lock
 *   qi->qi_tree_lock
 *     dquot->q_qlock (xfs_dqlock() and friends)
 *       dquot->q_flush (xfs_dqflock() and friends)
 *       qi->qi_lru_lock
 *
 * If two dquots need to be locked the order is user before group/project,
 * otherwise by the lowest id first, see xfs_dqlock2.
 */

struct kmem_zone		*xfs_qm_dqtrxzone;
static struct kmem_zone		*xfs_qm_dqzone;

static struct lock_class_key xfs_dquot_group_class;
static struct lock_class_key xfs_dquot_project_class;

/*
 * This is called to free all the memory associated with a dquot
 */
void
xfs_qm_dqdestroy(
	xfs_dquot_t	*dqp)
{
	ASSERT(list_empty(&dqp->q_lru));

	kmem_free(dqp->q_logitem.qli_item.li_lv_shadow);
	mutex_destroy(&dqp->q_qlock);

	XFS_STATS_DEC(dqp->q_mount, xs_qm_dquot);
	kmem_zone_free(xfs_qm_dqzone, dqp);
}

/*
 * If default limits are in force, push them into the dquot now.
 * We overwrite the dquot limits only if they are zero and this
 * is not the root dquot.
 */
void
xfs_qm_adjust_dqlimits(
	struct xfs_mount	*mp,
	struct xfs_dquot	*dq)
{
	struct xfs_quotainfo	*q = mp->m_quotainfo;
	struct xfs_disk_dquot	*d = &dq->q_core;
	struct xfs_def_quota	*defq;
	int			prealloc = 0;

	ASSERT(d->d_id);
	defq = xfs_get_defquota(dq, q);

	if (defq->bsoftlimit && !d->d_blk_softlimit) {
		d->d_blk_softlimit = cpu_to_be64(defq->bsoftlimit);
		prealloc = 1;
	}
	if (defq->bhardlimit && !d->d_blk_hardlimit) {
		d->d_blk_hardlimit = cpu_to_be64(defq->bhardlimit);
		prealloc = 1;
	}
	if (defq->isoftlimit && !d->d_ino_softlimit)
		d->d_ino_softlimit = cpu_to_be64(defq->isoftlimit);
	if (defq->ihardlimit && !d->d_ino_hardlimit)
		d->d_ino_hardlimit = cpu_to_be64(defq->ihardlimit);
	if (defq->rtbsoftlimit && !d->d_rtb_softlimit)
		d->d_rtb_softlimit = cpu_to_be64(defq->rtbsoftlimit);
	if (defq->rtbhardlimit && !d->d_rtb_hardlimit)
		d->d_rtb_hardlimit = cpu_to_be64(defq->rtbhardlimit);

	if (prealloc)
		xfs_dquot_set_prealloc_limits(dq);
}

/*
 * Check the limits and timers of a dquot and start or reset timers
 * if necessary.
 * This gets called even when quota enforcement is OFF, which makes our
 * life a little less complicated. (We just don't reject any quota
 * reservations in that case, when enforcement is off).
 * We also return 0 as the values of the timers in Q_GETQUOTA calls, when
 * enforcement's off.
 * In contrast, warnings are a little different in that they don't
 * 'automatically' get started when limits get exceeded.  They do
 * get reset to zero, however, when we find the count to be under
 * the soft limit (they are only ever set non-zero via userspace).
 */
void
xfs_qm_adjust_dqtimers(
	xfs_mount_t		*mp,
	xfs_disk_dquot_t	*d)
{
	ASSERT(d->d_id);

#ifdef DEBUG
	if (d->d_blk_hardlimit)
		ASSERT(be64_to_cpu(d->d_blk_softlimit) <=
		       be64_to_cpu(d->d_blk_hardlimit));
	if (d->d_ino_hardlimit)
		ASSERT(be64_to_cpu(d->d_ino_softlimit) <=
		       be64_to_cpu(d->d_ino_hardlimit));
	if (d->d_rtb_hardlimit)
		ASSERT(be64_to_cpu(d->d_rtb_softlimit) <=
		       be64_to_cpu(d->d_rtb_hardlimit));
#endif

	if (!d->d_btimer) {
		if ((d->d_blk_softlimit &&
		     (be64_to_cpu(d->d_bcount) >
		      be64_to_cpu(d->d_blk_softlimit))) ||
		    (d->d_blk_hardlimit &&
		     (be64_to_cpu(d->d_bcount) >
		      be64_to_cpu(d->d_blk_hardlimit)))) {
			d->d_btimer = cpu_to_be32(get_seconds() +
					mp->m_quotainfo->qi_btimelimit);
		} else {
			d->d_bwarns = 0;
		}
	} else {
		if ((!d->d_blk_softlimit ||
		     (be64_to_cpu(d->d_bcount) <=
		      be64_to_cpu(d->d_blk_softlimit))) &&
		    (!d->d_blk_hardlimit ||
		    (be64_to_cpu(d->d_bcount) <=
		     be64_to_cpu(d->d_blk_hardlimit)))) {
			d->d_btimer = 0;
		}
	}

	if (!d->d_itimer) {
		if ((d->d_ino_softlimit &&
		     (be64_to_cpu(d->d_icount) >
		      be64_to_cpu(d->d_ino_softlimit))) ||
		    (d->d_ino_hardlimit &&
		     (be64_to_cpu(d->d_icount) >
		      be64_to_cpu(d->d_ino_hardlimit)))) {
			d->d_itimer = cpu_to_be32(get_seconds() +
					mp->m_quotainfo->qi_itimelimit);
		} else {
			d->d_iwarns = 0;
		}
	} else {
		if ((!d->d_ino_softlimit ||
		     (be64_to_cpu(d->d_icount) <=
		      be64_to_cpu(d->d_ino_softlimit)))  &&
		    (!d->d_ino_hardlimit ||
		     (be64_to_cpu(d->d_icount) <=
		      be64_to_cpu(d->d_ino_hardlimit)))) {
			d->d_itimer = 0;
		}
	}

	if (!d->d_rtbtimer) {
		if ((d->d_rtb_softlimit &&
		     (be64_to_cpu(d->d_rtbcount) >
		      be64_to_cpu(d->d_rtb_softlimit))) ||
		    (d->d_rtb_hardlimit &&
		     (be64_to_cpu(d->d_rtbcount) >
		      be64_to_cpu(d->d_rtb_hardlimit)))) {
			d->d_rtbtimer = cpu_to_be32(get_seconds() +
					mp->m_quotainfo->qi_rtbtimelimit);
		} else {
			d->d_rtbwarns = 0;
		}
	} else {
		if ((!d->d_rtb_softlimit ||
		     (be64_to_cpu(d->d_rtbcount) <=
		      be64_to_cpu(d->d_rtb_softlimit))) &&
		    (!d->d_rtb_hardlimit ||
		     (be64_to_cpu(d->d_rtbcount) <=
		      be64_to_cpu(d->d_rtb_hardlimit)))) {
			d->d_rtbtimer = 0;
		}
	}
}

/*
 * initialize a buffer full of dquots and log the whole thing
 */
STATIC void
xfs_qm_init_dquot_blk(
	xfs_trans_t	*tp,
	xfs_mount_t	*mp,
	xfs_dqid_t	id,
	uint		type,
	xfs_buf_t	*bp)
{
	struct xfs_quotainfo	*q = mp->m_quotainfo;
	xfs_dqblk_t	*d;
	xfs_dqid_t	curid;
	int		i;

	ASSERT(tp);
	ASSERT(xfs_buf_islocked(bp));

	d = bp->b_addr;

	/*
	 * ID of the first dquot in the block - id's are zero based.
	 */
	curid = id - (id % q->qi_dqperchunk);
	memset(d, 0, BBTOB(q->qi_dqchunklen));
	for (i = 0; i < q->qi_dqperchunk; i++, d++, curid++) {
		d->dd_diskdq.d_magic = cpu_to_be16(XFS_DQUOT_MAGIC);
		d->dd_diskdq.d_version = XFS_DQUOT_VERSION;
		d->dd_diskdq.d_id = cpu_to_be32(curid);
		d->dd_diskdq.d_flags = type;
		if (xfs_sb_version_hascrc(&mp->m_sb)) {
			uuid_copy(&d->dd_uuid, &mp->m_sb.sb_meta_uuid);
			xfs_update_cksum((char *)d, sizeof(struct xfs_dqblk),
					 XFS_DQUOT_CRC_OFF);
		}
	}

	xfs_trans_dquot_buf(tp, bp,
			    (type & XFS_DQ_USER ? XFS_BLF_UDQUOT_BUF :
			    ((type & XFS_DQ_PROJ) ? XFS_BLF_PDQUOT_BUF :
			     XFS_BLF_GDQUOT_BUF)));
	xfs_trans_log_buf(tp, bp, 0, BBTOB(q->qi_dqchunklen) - 1);
}

/*
 * Initialize the dynamic speculative preallocation thresholds. The lo/hi
 * watermarks correspond to the soft and hard limits by default. If a soft limit
 * is not specified, we use 95% of the hard limit.
 */
void
xfs_dquot_set_prealloc_limits(struct xfs_dquot *dqp)
{
	uint64_t space;

	dqp->q_prealloc_hi_wmark = be64_to_cpu(dqp->q_core.d_blk_hardlimit);
	dqp->q_prealloc_lo_wmark = be64_to_cpu(dqp->q_core.d_blk_softlimit);
	if (!dqp->q_prealloc_lo_wmark) {
		dqp->q_prealloc_lo_wmark = dqp->q_prealloc_hi_wmark;
		do_div(dqp->q_prealloc_lo_wmark, 100);
		dqp->q_prealloc_lo_wmark *= 95;
	}

	space = dqp->q_prealloc_hi_wmark;

	do_div(space, 100);
	dqp->q_low_space[XFS_QLOWSP_1_PCNT] = space;
	dqp->q_low_space[XFS_QLOWSP_3_PCNT] = space * 3;
	dqp->q_low_space[XFS_QLOWSP_5_PCNT] = space * 5;
}

/*
 * Ensure that the given in-core dquot has a buffer on disk backing it, and
 * return the buffer locked and held. This is called when the bmapi finds a
 * hole.
 */
STATIC int
xfs_dquot_disk_alloc(
	struct xfs_trans	**tpp,
	struct xfs_dquot	*dqp,
	struct xfs_buf		**bpp)
{
	struct xfs_bmbt_irec	map;
	struct xfs_trans	*tp = *tpp;
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_buf		*bp;
	struct xfs_inode	*quotip = xfs_quota_inode(mp, dqp->dq_flags);
	int			nmaps = 1;
	int			error;

	trace_xfs_dqalloc(dqp);

	xfs_ilock(quotip, XFS_ILOCK_EXCL);
	if (!xfs_this_quota_on(dqp->q_mount, dqp->dq_flags)) {
		/*
		 * Return if this type of quotas is turned off while we didn't
		 * have an inode lock
		 */
		xfs_iunlock(quotip, XFS_ILOCK_EXCL);
		return -ESRCH;
	}

	/* Create the block mapping. */
	xfs_trans_ijoin(tp, quotip, XFS_ILOCK_EXCL);
	error = xfs_bmapi_write(tp, quotip, dqp->q_fileoffset,
			XFS_DQUOT_CLUSTER_SIZE_FSB, XFS_BMAPI_METADATA,
			XFS_QM_DQALLOC_SPACE_RES(mp), &map, &nmaps);
	if (error)
		return error;
	ASSERT(map.br_blockcount == XFS_DQUOT_CLUSTER_SIZE_FSB);
	ASSERT(nmaps == 1);
	ASSERT((map.br_startblock != DELAYSTARTBLOCK) &&
	       (map.br_startblock != HOLESTARTBLOCK));

	/*
	 * Keep track of the blkno to save a lookup later
	 */
	dqp->q_blkno = XFS_FSB_TO_DADDR(mp, map.br_startblock);

	/* now we can just get the buffer (there's nothing to read yet) */
	bp = xfs_trans_get_buf(tp, mp->m_ddev_targp, dqp->q_blkno,
			mp->m_quotainfo->qi_dqchunklen, 0);
	if (!bp)
		return -ENOMEM;
	bp->b_ops = &xfs_dquot_buf_ops;

	/*
	 * Make a chunk of dquots out of this buffer and log
	 * the entire thing.
	 */
	xfs_qm_init_dquot_blk(tp, mp, be32_to_cpu(dqp->q_core.d_id),
			      dqp->dq_flags & XFS_DQ_ALLTYPES, bp);
	xfs_buf_set_ref(bp, XFS_DQUOT_REF);

	/*
	 * Hold the buffer and join it to the dfops so that we'll still own
	 * the buffer when we return to the caller.  The buffer disposal on
	 * error must be paid attention to very carefully, as it has been
	 * broken since commit efa092f3d4c6 "[XFS] Fixes a bug in the quota
	 * code when allocating a new dquot record" in 2005, and the later
	 * conversion to xfs_defer_ops in commit 310a75a3c6c747 failed to keep
	 * the buffer locked across the _defer_finish call.  We can now do
	 * this correctly with xfs_defer_bjoin.
	 *
	 * Above, we allocated a disk block for the dquot information and used
	 * get_buf to initialize the dquot. If the _defer_finish fails, the old
	 * transaction is gone but the new buffer is not joined or held to any
	 * transaction, so we must _buf_relse it.
	 *
	 * If everything succeeds, the caller of this function is returned a
	 * buffer that is locked and held to the transaction.  The caller
	 * is responsible for unlocking any buffer passed back, either
	 * manually or by committing the transaction.  On error, the buffer is
	 * released and not passed back.
	 */
	xfs_trans_bhold(tp, bp);
	error = xfs_defer_finish(tpp);
	if (error) {
		xfs_trans_bhold_release(*tpp, bp);
		xfs_trans_brelse(*tpp, bp);
		return error;
	}
	*bpp = bp;
	return 0;
}

/*
 * Read in the in-core dquot's on-disk metadata and return the buffer.
 * Returns ENOENT to signal a hole.
 */
STATIC int
xfs_dquot_disk_read(
	struct xfs_mount	*mp,
	struct xfs_dquot	*dqp,
	struct xfs_buf		**bpp)
{
	struct xfs_bmbt_irec	map;
	struct xfs_buf		*bp;
	struct xfs_inode	*quotip = xfs_quota_inode(mp, dqp->dq_flags);
	uint			lock_mode;
	int			nmaps = 1;
	int			error;

	lock_mode = xfs_ilock_data_map_shared(quotip);
	if (!xfs_this_quota_on(mp, dqp->dq_flags)) {
		/*
		 * Return if this type of quotas is turned off while we
		 * didn't have the quota inode lock.
		 */
		xfs_iunlock(quotip, lock_mode);
		return -ESRCH;
	}

	/*
	 * Find the block map; no allocations yet
	 */
	error = xfs_bmapi_read(quotip, dqp->q_fileoffset,
			XFS_DQUOT_CLUSTER_SIZE_FSB, &map, &nmaps, 0);
	xfs_iunlock(quotip, lock_mode);
	if (error)
		return error;

	ASSERT(nmaps == 1);
	ASSERT(map.br_blockcount >= 1);
	ASSERT(map.br_startblock != DELAYSTARTBLOCK);
	if (map.br_startblock == HOLESTARTBLOCK)
		return -ENOENT;

	trace_xfs_dqtobp_read(dqp);

	/*
	 * store the blkno etc so that we don't have to do the
	 * mapping all the time
	 */
	dqp->q_blkno = XFS_FSB_TO_DADDR(mp, map.br_startblock);

	error = xfs_trans_read_buf(mp, NULL, mp->m_ddev_targp, dqp->q_blkno,
			mp->m_quotainfo->qi_dqchunklen, 0, &bp,
			&xfs_dquot_buf_ops);
	if (error) {
		ASSERT(bp == NULL);
		return error;
	}

	ASSERT(xfs_buf_islocked(bp));
	xfs_buf_set_ref(bp, XFS_DQUOT_REF);
	*bpp = bp;

	return 0;
}

/* Allocate and initialize everything we need for an incore dquot. */
STATIC struct xfs_dquot *
xfs_dquot_alloc(
	struct xfs_mount	*mp,
	xfs_dqid_t		id,
	uint			type)
{
	struct xfs_dquot	*dqp;

	dqp = kmem_zone_zalloc(xfs_qm_dqzone, 0);

	dqp->dq_flags = type;
	dqp->q_core.d_id = cpu_to_be32(id);
	dqp->q_mount = mp;
	INIT_LIST_HEAD(&dqp->q_lru);
	mutex_init(&dqp->q_qlock);
	init_waitqueue_head(&dqp->q_pinwait);
	dqp->q_fileoffset = (xfs_fileoff_t)id / mp->m_quotainfo->qi_dqperchunk;
	/*
	 * Offset of dquot in the (fixed sized) dquot chunk.
	 */
	dqp->q_bufoffset = (id % mp->m_quotainfo->qi_dqperchunk) *
			sizeof(xfs_dqblk_t);

	/*
	 * Because we want to use a counting completion, complete
	 * the flush completion once to allow a single access to
	 * the flush completion without blocking.
	 */
	init_completion(&dqp->q_flush);
	complete(&dqp->q_flush);

	/*
	 * Make sure group quotas have a different lock class than user
	 * quotas.
	 */
	switch (type) {
	case XFS_DQ_USER:
		/* uses the default lock class */
		break;
	case XFS_DQ_GROUP:
		lockdep_set_class(&dqp->q_qlock, &xfs_dquot_group_class);
		break;
	case XFS_DQ_PROJ:
		lockdep_set_class(&dqp->q_qlock, &xfs_dquot_project_class);
		break;
	default:
		ASSERT(0);
		break;
	}

	xfs_qm_dquot_logitem_init(dqp);

	XFS_STATS_INC(mp, xs_qm_dquot);
	return dqp;
}

/* Copy the in-core quota fields in from the on-disk buffer. */
STATIC void
xfs_dquot_from_disk(
	struct xfs_dquot	*dqp,
	struct xfs_buf		*bp)
{
	struct xfs_disk_dquot	*ddqp = bp->b_addr + dqp->q_bufoffset;

	/* copy everything from disk dquot to the incore dquot */
	memcpy(&dqp->q_core, ddqp, sizeof(xfs_disk_dquot_t));

	/*
	 * Reservation counters are defined as reservation plus current usage
	 * to avoid having to add every time.
	 */
	dqp->q_res_bcount = be64_to_cpu(ddqp->d_bcount);
	dqp->q_res_icount = be64_to_cpu(ddqp->d_icount);
	dqp->q_res_rtbcount = be64_to_cpu(ddqp->d_rtbcount);

	/* initialize the dquot speculative prealloc thresholds */
	xfs_dquot_set_prealloc_limits(dqp);
}

/* Allocate and initialize the dquot buffer for this in-core dquot. */
static int
xfs_qm_dqread_alloc(
	struct xfs_mount	*mp,
	struct xfs_dquot	*dqp,
	struct xfs_buf		**bpp)
{
	struct xfs_trans	*tp;
	int			error;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_qm_dqalloc,
			XFS_QM_DQALLOC_SPACE_RES(mp), 0, 0, &tp);
	if (error)
		goto err;

	error = xfs_dquot_disk_alloc(&tp, dqp, bpp);
	if (error)
		goto err_cancel;

	error = xfs_trans_commit(tp);
	if (error) {
		/*
		 * Buffer was held to the transaction, so we have to unlock it
		 * manually here because we're not passing it back.
		 */
		xfs_buf_relse(*bpp);
		*bpp = NULL;
		goto err;
	}
	return 0;

err_cancel:
	xfs_trans_cancel(tp);
err:
	return error;
}

/*
 * Read in the ondisk dquot using dqtobp() then copy it to an incore version,
 * and release the buffer immediately.  If @can_alloc is true, fill any
 * holes in the on-disk metadata.
 */
static int
xfs_qm_dqread(
	struct xfs_mount	*mp,
	xfs_dqid_t		id,
	uint			type,
	bool			can_alloc,
	struct xfs_dquot	**dqpp)
{
	struct xfs_dquot	*dqp;
	struct xfs_buf		*bp;
	int			error;

	dqp = xfs_dquot_alloc(mp, id, type);
	trace_xfs_dqread(dqp);

	/* Try to read the buffer, allocating if necessary. */
	error = xfs_dquot_disk_read(mp, dqp, &bp);
	if (error == -ENOENT && can_alloc)
		error = xfs_qm_dqread_alloc(mp, dqp, &bp);
	if (error)
		goto err;

	/*
	 * At this point we should have a clean locked buffer.  Copy the data
	 * to the incore dquot and release the buffer since the incore dquot
	 * has its own locking protocol so we needn't tie up the buffer any
	 * further.
	 */
	ASSERT(xfs_buf_islocked(bp));
	xfs_dquot_from_disk(dqp, bp);

	xfs_buf_relse(bp);
	*dqpp = dqp;
	return error;

err:
	trace_xfs_dqread_fail(dqp);
	xfs_qm_dqdestroy(dqp);
	*dqpp = NULL;
	return error;
}

/*
 * Advance to the next id in the current chunk, or if at the
 * end of the chunk, skip ahead to first id in next allocated chunk
 * using the SEEK_DATA interface.
 */
static int
xfs_dq_get_next_id(
	struct xfs_mount	*mp,
	uint			type,
	xfs_dqid_t		*id)
{
	struct xfs_inode	*quotip = xfs_quota_inode(mp, type);
	xfs_dqid_t		next_id = *id + 1; /* simple advance */
	uint			lock_flags;
	struct xfs_bmbt_irec	got;
	struct xfs_iext_cursor	cur;
	xfs_fsblock_t		start;
	int			error = 0;

	/* If we'd wrap past the max ID, stop */
	if (next_id < *id)
		return -ENOENT;

	/* If new ID is within the current chunk, advancing it sufficed */
	if (next_id % mp->m_quotainfo->qi_dqperchunk) {
		*id = next_id;
		return 0;
	}

	/* Nope, next_id is now past the current chunk, so find the next one */
	start = (xfs_fsblock_t)next_id / mp->m_quotainfo->qi_dqperchunk;

	lock_flags = xfs_ilock_data_map_shared(quotip);
	if (!(quotip->i_df.if_flags & XFS_IFEXTENTS)) {
		error = xfs_iread_extents(NULL, quotip, XFS_DATA_FORK);
		if (error)
			return error;
	}

	if (xfs_iext_lookup_extent(quotip, &quotip->i_df, start, &cur, &got)) {
		/* contiguous chunk, bump startoff for the id calculation */
		if (got.br_startoff < start)
			got.br_startoff = start;
		*id = got.br_startoff * mp->m_quotainfo->qi_dqperchunk;
	} else {
		error = -ENOENT;
	}

	xfs_iunlock(quotip, lock_flags);

	return error;
}

/*
 * Look up the dquot in the in-core cache.  If found, the dquot is returned
 * locked and ready to go.
 */
static struct xfs_dquot *
xfs_qm_dqget_cache_lookup(
	struct xfs_mount	*mp,
	struct xfs_quotainfo	*qi,
	struct radix_tree_root	*tree,
	xfs_dqid_t		id)
{
	struct xfs_dquot	*dqp;

restart:
	mutex_lock(&qi->qi_tree_lock);
	dqp = radix_tree_lookup(tree, id);
	if (!dqp) {
		mutex_unlock(&qi->qi_tree_lock);
		XFS_STATS_INC(mp, xs_qm_dqcachemisses);
		return NULL;
	}

	xfs_dqlock(dqp);
	if (dqp->dq_flags & XFS_DQ_FREEING) {
		xfs_dqunlock(dqp);
		mutex_unlock(&qi->qi_tree_lock);
		trace_xfs_dqget_freeing(dqp);
		delay(1);
		goto restart;
	}

	dqp->q_nrefs++;
	mutex_unlock(&qi->qi_tree_lock);

	trace_xfs_dqget_hit(dqp);
	XFS_STATS_INC(mp, xs_qm_dqcachehits);
	return dqp;
}

/*
 * Try to insert a new dquot into the in-core cache.  If an error occurs the
 * caller should throw away the dquot and start over.  Otherwise, the dquot
 * is returned locked (and held by the cache) as if there had been a cache
 * hit.
 */
static int
xfs_qm_dqget_cache_insert(
	struct xfs_mount	*mp,
	struct xfs_quotainfo	*qi,
	struct radix_tree_root	*tree,
	xfs_dqid_t		id,
	struct xfs_dquot	*dqp)
{
	int			error;

	mutex_lock(&qi->qi_tree_lock);
	error = radix_tree_insert(tree, id, dqp);
	if (unlikely(error)) {
		/* Duplicate found!  Caller must try again. */
		WARN_ON(error != -EEXIST);
		mutex_unlock(&qi->qi_tree_lock);
		trace_xfs_dqget_dup(dqp);
		return error;
	}

	/* Return a locked dquot to the caller, with a reference taken. */
	xfs_dqlock(dqp);
	dqp->q_nrefs = 1;

	qi->qi_dquots++;
	mutex_unlock(&qi->qi_tree_lock);

	return 0;
}

/* Check our input parameters. */
static int
xfs_qm_dqget_checks(
	struct xfs_mount	*mp,
	uint			type)
{
	if (WARN_ON_ONCE(!XFS_IS_QUOTA_RUNNING(mp)))
		return -ESRCH;

	switch (type) {
	case XFS_DQ_USER:
		if (!XFS_IS_UQUOTA_ON(mp))
			return -ESRCH;
		return 0;
	case XFS_DQ_GROUP:
		if (!XFS_IS_GQUOTA_ON(mp))
			return -ESRCH;
		return 0;
	case XFS_DQ_PROJ:
		if (!XFS_IS_PQUOTA_ON(mp))
			return -ESRCH;
		return 0;
	default:
		WARN_ON_ONCE(0);
		return -EINVAL;
	}
}

/*
 * Given the file system, id, and type (UDQUOT/GDQUOT), return a a locked
 * dquot, doing an allocation (if requested) as needed.
 */
int
xfs_qm_dqget(
	struct xfs_mount	*mp,
	xfs_dqid_t		id,
	uint			type,
	bool			can_alloc,
	struct xfs_dquot	**O_dqpp)
{
	struct xfs_quotainfo	*qi = mp->m_quotainfo;
	struct radix_tree_root	*tree = xfs_dquot_tree(qi, type);
	struct xfs_dquot	*dqp;
	int			error;

	error = xfs_qm_dqget_checks(mp, type);
	if (error)
		return error;

restart:
	dqp = xfs_qm_dqget_cache_lookup(mp, qi, tree, id);
	if (dqp) {
		*O_dqpp = dqp;
		return 0;
	}

	error = xfs_qm_dqread(mp, id, type, can_alloc, &dqp);
	if (error)
		return error;

	error = xfs_qm_dqget_cache_insert(mp, qi, tree, id, dqp);
	if (error) {
		/*
		 * Duplicate found. Just throw away the new dquot and start
		 * over.
		 */
		xfs_qm_dqdestroy(dqp);
		XFS_STATS_INC(mp, xs_qm_dquot_dups);
		goto restart;
	}

	trace_xfs_dqget_miss(dqp);
	*O_dqpp = dqp;
	return 0;
}

/*
 * Given a dquot id and type, read and initialize a dquot from the on-disk
 * metadata.  This function is only for use during quota initialization so
 * it ignores the dquot cache assuming that the dquot shrinker isn't set up.
 * The caller is responsible for _qm_dqdestroy'ing the returned dquot.
 */
int
xfs_qm_dqget_uncached(
	struct xfs_mount	*mp,
	xfs_dqid_t		id,
	uint			type,
	struct xfs_dquot	**dqpp)
{
	int			error;

	error = xfs_qm_dqget_checks(mp, type);
	if (error)
		return error;

	return xfs_qm_dqread(mp, id, type, 0, dqpp);
}

/* Return the quota id for a given inode and type. */
xfs_dqid_t
xfs_qm_id_for_quotatype(
	struct xfs_inode	*ip,
	uint			type)
{
	switch (type) {
	case XFS_DQ_USER:
		return ip->i_d.di_uid;
	case XFS_DQ_GROUP:
		return ip->i_d.di_gid;
	case XFS_DQ_PROJ:
		return xfs_get_projid(ip);
	}
	ASSERT(0);
	return 0;
}

/*
 * Return the dquot for a given inode and type.  If @can_alloc is true, then
 * allocate blocks if needed.  The inode's ILOCK must be held and it must not
 * have already had an inode attached.
 */
int
xfs_qm_dqget_inode(
	struct xfs_inode	*ip,
	uint			type,
	bool			can_alloc,
	struct xfs_dquot	**O_dqpp)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_quotainfo	*qi = mp->m_quotainfo;
	struct radix_tree_root	*tree = xfs_dquot_tree(qi, type);
	struct xfs_dquot	*dqp;
	xfs_dqid_t		id;
	int			error;

	error = xfs_qm_dqget_checks(mp, type);
	if (error)
		return error;

	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	ASSERT(xfs_inode_dquot(ip, type) == NULL);

	id = xfs_qm_id_for_quotatype(ip, type);

restart:
	dqp = xfs_qm_dqget_cache_lookup(mp, qi, tree, id);
	if (dqp) {
		*O_dqpp = dqp;
		return 0;
	}

	/*
	 * Dquot cache miss. We don't want to keep the inode lock across
	 * a (potential) disk read. Also we don't want to deal with the lock
	 * ordering between quotainode and this inode. OTOH, dropping the inode
	 * lock here means dealing with a chown that can happen before
	 * we re-acquire the lock.
	 */
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	error = xfs_qm_dqread(mp, id, type, can_alloc, &dqp);
	xfs_ilock(ip, XFS_ILOCK_EXCL);
	if (error)
		return error;

	/*
	 * A dquot could be attached to this inode by now, since we had
	 * dropped the ilock.
	 */
	if (xfs_this_quota_on(mp, type)) {
		struct xfs_dquot	*dqp1;

		dqp1 = xfs_inode_dquot(ip, type);
		if (dqp1) {
			xfs_qm_dqdestroy(dqp);
			dqp = dqp1;
			xfs_dqlock(dqp);
			goto dqret;
		}
	} else {
		/* inode stays locked on return */
		xfs_qm_dqdestroy(dqp);
		return -ESRCH;
	}

	error = xfs_qm_dqget_cache_insert(mp, qi, tree, id, dqp);
	if (error) {
		/*
		 * Duplicate found. Just throw away the new dquot and start
		 * over.
		 */
		xfs_qm_dqdestroy(dqp);
		XFS_STATS_INC(mp, xs_qm_dquot_dups);
		goto restart;
	}

dqret:
	ASSERT(xfs_isilocked(ip, XFS_ILOCK_EXCL));
	trace_xfs_dqget_miss(dqp);
	*O_dqpp = dqp;
	return 0;
}

/*
 * Starting at @id and progressing upwards, look for an initialized incore
 * dquot, lock it, and return it.
 */
int
xfs_qm_dqget_next(
	struct xfs_mount	*mp,
	xfs_dqid_t		id,
	uint			type,
	struct xfs_dquot	**dqpp)
{
	struct xfs_dquot	*dqp;
	int			error = 0;

	*dqpp = NULL;
	for (; !error; error = xfs_dq_get_next_id(mp, type, &id)) {
		error = xfs_qm_dqget(mp, id, type, false, &dqp);
		if (error == -ENOENT)
			continue;
		else if (error != 0)
			break;

		if (!XFS_IS_DQUOT_UNINITIALIZED(dqp)) {
			*dqpp = dqp;
			return 0;
		}

		xfs_qm_dqput(dqp);
	}

	return error;
}

/*
 * Release a reference to the dquot (decrement ref-count) and unlock it.
 *
 * If there is a group quota attached to this dquot, carefully release that
 * too without tripping over deadlocks'n'stuff.
 */
void
xfs_qm_dqput(
	struct xfs_dquot	*dqp)
{
	ASSERT(dqp->q_nrefs > 0);
	ASSERT(XFS_DQ_IS_LOCKED(dqp));

	trace_xfs_dqput(dqp);

	if (--dqp->q_nrefs == 0) {
		struct xfs_quotainfo	*qi = dqp->q_mount->m_quotainfo;
		trace_xfs_dqput_free(dqp);

		if (list_lru_add(&qi->qi_lru, &dqp->q_lru))
			XFS_STATS_INC(dqp->q_mount, xs_qm_dquot_unused);
	}
	xfs_dqunlock(dqp);
}

/*
 * Release a dquot. Flush it if dirty, then dqput() it.
 * dquot must not be locked.
 */
void
xfs_qm_dqrele(
	xfs_dquot_t	*dqp)
{
	if (!dqp)
		return;

	trace_xfs_dqrele(dqp);

	xfs_dqlock(dqp);
	/*
	 * We don't care to flush it if the dquot is dirty here.
	 * That will create stutters that we want to avoid.
	 * Instead we do a delayed write when we try to reclaim
	 * a dirty dquot. Also xfs_sync will take part of the burden...
	 */
	xfs_qm_dqput(dqp);
}

/*
 * This is the dquot flushing I/O completion routine.  It is called
 * from interrupt level when the buffer containing the dquot is
 * flushed to disk.  It is responsible for removing the dquot logitem
 * from the AIL if it has not been re-logged, and unlocking the dquot's
 * flush lock. This behavior is very similar to that of inodes..
 */
STATIC void
xfs_qm_dqflush_done(
	struct xfs_buf		*bp,
	struct xfs_log_item	*lip)
{
	xfs_dq_logitem_t	*qip = (struct xfs_dq_logitem *)lip;
	xfs_dquot_t		*dqp = qip->qli_dquot;
	struct xfs_ail		*ailp = lip->li_ailp;

	/*
	 * We only want to pull the item from the AIL if its
	 * location in the log has not changed since we started the flush.
	 * Thus, we only bother if the dquot's lsn has
	 * not changed. First we check the lsn outside the lock
	 * since it's cheaper, and then we recheck while
	 * holding the lock before removing the dquot from the AIL.
	 */
	if (test_bit(XFS_LI_IN_AIL, &lip->li_flags) &&
	    ((lip->li_lsn == qip->qli_flush_lsn) ||
	     test_bit(XFS_LI_FAILED, &lip->li_flags))) {

		/* xfs_trans_ail_delete() drops the AIL lock. */
		spin_lock(&ailp->ail_lock);
		if (lip->li_lsn == qip->qli_flush_lsn) {
			xfs_trans_ail_delete(ailp, lip, SHUTDOWN_CORRUPT_INCORE);
		} else {
			/*
			 * Clear the failed state since we are about to drop the
			 * flush lock
			 */
			xfs_clear_li_failed(lip);
			spin_unlock(&ailp->ail_lock);
		}
	}

	/*
	 * Release the dq's flush lock since we're done with it.
	 */
	xfs_dqfunlock(dqp);
}

/*
 * Write a modified dquot to disk.
 * The dquot must be locked and the flush lock too taken by caller.
 * The flush lock will not be unlocked until the dquot reaches the disk,
 * but the dquot is free to be unlocked and modified by the caller
 * in the interim. Dquot is still locked on return. This behavior is
 * identical to that of inodes.
 */
int
xfs_qm_dqflush(
	struct xfs_dquot	*dqp,
	struct xfs_buf		**bpp)
{
	struct xfs_mount	*mp = dqp->q_mount;
	struct xfs_buf		*bp;
	struct xfs_dqblk	*dqb;
	struct xfs_disk_dquot	*ddqp;
	xfs_failaddr_t		fa;
	int			error;

	ASSERT(XFS_DQ_IS_LOCKED(dqp));
	ASSERT(!completion_done(&dqp->q_flush));

	trace_xfs_dqflush(dqp);

	*bpp = NULL;

	xfs_qm_dqunpin_wait(dqp);

	/*
	 * This may have been unpinned because the filesystem is shutting
	 * down forcibly. If that's the case we must not write this dquot
	 * to disk, because the log record didn't make it to disk.
	 *
	 * We also have to remove the log item from the AIL in this case,
	 * as we wait for an emptry AIL as part of the unmount process.
	 */
	if (XFS_FORCED_SHUTDOWN(mp)) {
		struct xfs_log_item	*lip = &dqp->q_logitem.qli_item;
		dqp->dq_flags &= ~XFS_DQ_DIRTY;

		xfs_trans_ail_remove(lip, SHUTDOWN_CORRUPT_INCORE);

		error = -EIO;
		goto out_unlock;
	}

	/*
	 * Get the buffer containing the on-disk dquot
	 */
	error = xfs_trans_read_buf(mp, NULL, mp->m_ddev_targp, dqp->q_blkno,
				   mp->m_quotainfo->qi_dqchunklen, 0, &bp,
				   &xfs_dquot_buf_ops);
	if (error)
		goto out_unlock;

	/*
	 * Calculate the location of the dquot inside the buffer.
	 */
	dqb = bp->b_addr + dqp->q_bufoffset;
	ddqp = &dqb->dd_diskdq;

	/* sanity check the in-core structure before we flush */
	fa = xfs_dquot_verify(mp, &dqp->q_core, be32_to_cpu(dqp->q_core.d_id),
			      0);
	if (fa) {
		xfs_alert(mp, "corrupt dquot ID 0x%x in memory at %pS",
				be32_to_cpu(dqp->q_core.d_id), fa);
		xfs_buf_relse(bp);
		xfs_dqfunlock(dqp);
		xfs_force_shutdown(mp, SHUTDOWN_CORRUPT_INCORE);
		return -EIO;
	}

	/* This is the only portion of data that needs to persist */
	memcpy(ddqp, &dqp->q_core, sizeof(xfs_disk_dquot_t));

	/*
	 * Clear the dirty field and remember the flush lsn for later use.
	 */
	dqp->dq_flags &= ~XFS_DQ_DIRTY;

	xfs_trans_ail_copy_lsn(mp->m_ail, &dqp->q_logitem.qli_flush_lsn,
					&dqp->q_logitem.qli_item.li_lsn);

	/*
	 * copy the lsn into the on-disk dquot now while we have the in memory
	 * dquot here. This can't be done later in the write verifier as we
	 * can't get access to the log item at that point in time.
	 *
	 * We also calculate the CRC here so that the on-disk dquot in the
	 * buffer always has a valid CRC. This ensures there is no possibility
	 * of a dquot without an up-to-date CRC getting to disk.
	 */
	if (xfs_sb_version_hascrc(&mp->m_sb)) {
		dqb->dd_lsn = cpu_to_be64(dqp->q_logitem.qli_item.li_lsn);
		xfs_update_cksum((char *)dqb, sizeof(struct xfs_dqblk),
				 XFS_DQUOT_CRC_OFF);
	}

	/*
	 * Attach an iodone routine so that we can remove this dquot from the
	 * AIL and release the flush lock once the dquot is synced to disk.
	 */
	xfs_buf_attach_iodone(bp, xfs_qm_dqflush_done,
				  &dqp->q_logitem.qli_item);

	/*
	 * If the buffer is pinned then push on the log so we won't
	 * get stuck waiting in the write for too long.
	 */
	if (xfs_buf_ispinned(bp)) {
		trace_xfs_dqflush_force(dqp);
		xfs_log_force(mp, 0);
	}

	trace_xfs_dqflush_done(dqp);
	*bpp = bp;
	return 0;

out_unlock:
	xfs_dqfunlock(dqp);
	return -EIO;
}

/*
 * Lock two xfs_dquot structures.
 *
 * To avoid deadlocks we always lock the quota structure with
 * the lowerd id first.
 */
void
xfs_dqlock2(
	xfs_dquot_t	*d1,
	xfs_dquot_t	*d2)
{
	if (d1 && d2) {
		ASSERT(d1 != d2);
		if (be32_to_cpu(d1->q_core.d_id) >
		    be32_to_cpu(d2->q_core.d_id)) {
			mutex_lock(&d2->q_qlock);
			mutex_lock_nested(&d1->q_qlock, XFS_QLOCK_NESTED);
		} else {
			mutex_lock(&d1->q_qlock);
			mutex_lock_nested(&d2->q_qlock, XFS_QLOCK_NESTED);
		}
	} else if (d1) {
		mutex_lock(&d1->q_qlock);
	} else if (d2) {
		mutex_lock(&d2->q_qlock);
	}
}

int __init
xfs_qm_init(void)
{
	xfs_qm_dqzone =
		kmem_zone_init(sizeof(struct xfs_dquot), "xfs_dquot");
	if (!xfs_qm_dqzone)
		goto out;

	xfs_qm_dqtrxzone =
		kmem_zone_init(sizeof(struct xfs_dquot_acct), "xfs_dqtrx");
	if (!xfs_qm_dqtrxzone)
		goto out_free_dqzone;

	return 0;

out_free_dqzone:
	kmem_zone_destroy(xfs_qm_dqzone);
out:
	return -ENOMEM;
}

void
xfs_qm_exit(void)
{
	kmem_zone_destroy(xfs_qm_dqtrxzone);
	kmem_zone_destroy(xfs_qm_dqzone);
}

/*
 * Iterate every dquot of a particular type.  The caller must ensure that the
 * particular quota type is active.  iter_fn can return negative error codes,
 * or -ECANCELED to indicate that it wants to stop iterating.
 */
int
xfs_qm_dqiterate(
	struct xfs_mount	*mp,
	uint			dqtype,
	xfs_qm_dqiterate_fn	iter_fn,
	void			*priv)
{
	struct xfs_dquot	*dq;
	xfs_dqid_t		id = 0;
	int			error;

	do {
		error = xfs_qm_dqget_next(mp, id, dqtype, &dq);
		if (error == -ENOENT)
			return 0;
		if (error)
			return error;

		error = iter_fn(dq, dqtype, priv);
		id = be32_to_cpu(dq->q_core.d_id);
		xfs_qm_dqput(dq);
		id++;
	} while (error == 0 && id != 0);

	return error;
}
