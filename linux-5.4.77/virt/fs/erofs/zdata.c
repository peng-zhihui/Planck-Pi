// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Gao Xiang <gaoxiang25@huawei.com>
 */
#include "zdata.h"
#include "compress.h"
#include <linux/prefetch.h>

#include <trace/events/erofs.h>

/*
 * a compressed_pages[] placeholder in order to avoid
 * being filled with file pages for in-place decompression.
 */
#define PAGE_UNALLOCATED     ((void *)0x5F0E4B1D)

/* how to allocate cached pages for a pcluster */
enum z_erofs_cache_alloctype {
	DONTALLOC,	/* don't allocate any cached pages */
	DELAYEDALLOC,	/* delayed allocation (at the time of submitting io) */
};

/*
 * tagged pointer with 1-bit tag for all compressed pages
 * tag 0 - the page is just found with an extra page reference
 */
typedef tagptr1_t compressed_page_t;

#define tag_compressed_page_justfound(page) \
	tagptr_fold(compressed_page_t, page, 1)

static struct workqueue_struct *z_erofs_workqueue __read_mostly;
static struct kmem_cache *pcluster_cachep __read_mostly;

void z_erofs_exit_zip_subsystem(void)
{
	destroy_workqueue(z_erofs_workqueue);
	kmem_cache_destroy(pcluster_cachep);
}

static inline int z_erofs_init_workqueue(void)
{
	const unsigned int onlinecpus = num_possible_cpus();
	const unsigned int flags = WQ_UNBOUND | WQ_HIGHPRI | WQ_CPU_INTENSIVE;

	/*
	 * no need to spawn too many threads, limiting threads could minimum
	 * scheduling overhead, perhaps per-CPU threads should be better?
	 */
	z_erofs_workqueue = alloc_workqueue("erofs_unzipd", flags,
					    onlinecpus + onlinecpus / 4);
	return z_erofs_workqueue ? 0 : -ENOMEM;
}

static void z_erofs_pcluster_init_once(void *ptr)
{
	struct z_erofs_pcluster *pcl = ptr;
	struct z_erofs_collection *cl = z_erofs_primarycollection(pcl);
	unsigned int i;

	mutex_init(&cl->lock);
	cl->nr_pages = 0;
	cl->vcnt = 0;
	for (i = 0; i < Z_EROFS_CLUSTER_MAX_PAGES; ++i)
		pcl->compressed_pages[i] = NULL;
}

static void z_erofs_pcluster_init_always(struct z_erofs_pcluster *pcl)
{
	struct z_erofs_collection *cl = z_erofs_primarycollection(pcl);

	atomic_set(&pcl->obj.refcount, 1);

	DBG_BUGON(cl->nr_pages);
	DBG_BUGON(cl->vcnt);
}

int __init z_erofs_init_zip_subsystem(void)
{
	pcluster_cachep = kmem_cache_create("erofs_compress",
					    Z_EROFS_WORKGROUP_SIZE, 0,
					    SLAB_RECLAIM_ACCOUNT,
					    z_erofs_pcluster_init_once);
	if (pcluster_cachep) {
		if (!z_erofs_init_workqueue())
			return 0;

		kmem_cache_destroy(pcluster_cachep);
	}
	return -ENOMEM;
}

enum z_erofs_collectmode {
	COLLECT_SECONDARY,
	COLLECT_PRIMARY,
	/*
	 * The current collection was the tail of an exist chain, in addition
	 * that the previous processed chained collections are all decided to
	 * be hooked up to it.
	 * A new chain will be created for the remaining collections which are
	 * not processed yet, therefore different from COLLECT_PRIMARY_FOLLOWED,
	 * the next collection cannot reuse the whole page safely in
	 * the following scenario:
	 *  ________________________________________________________________
	 * |      tail (partial) page     |       head (partial) page       |
	 * |   (belongs to the next cl)   |   (belongs to the current cl)   |
	 * |_______PRIMARY_FOLLOWED_______|________PRIMARY_HOOKED___________|
	 */
	COLLECT_PRIMARY_HOOKED,
	COLLECT_PRIMARY_FOLLOWED_NOINPLACE,
	/*
	 * The current collection has been linked with the owned chain, and
	 * could also be linked with the remaining collections, which means
	 * if the processing page is the tail page of the collection, thus
	 * the current collection can safely use the whole page (since
	 * the previous collection is under control) for in-place I/O, as
	 * illustrated below:
	 *  ________________________________________________________________
	 * |  tail (partial) page |          head (partial) page           |
	 * |  (of the current cl) |      (of the previous collection)      |
	 * |  PRIMARY_FOLLOWED or |                                        |
	 * |_____PRIMARY_HOOKED___|____________PRIMARY_FOLLOWED____________|
	 *
	 * [  (*) the above page can be used as inplace I/O.               ]
	 */
	COLLECT_PRIMARY_FOLLOWED,
};

struct z_erofs_collector {
	struct z_erofs_pagevec_ctor vector;

	struct z_erofs_pcluster *pcl, *tailpcl;
	struct z_erofs_collection *cl;
	struct page **compressedpages;
	z_erofs_next_pcluster_t owned_head;

	enum z_erofs_collectmode mode;
};

struct z_erofs_decompress_frontend {
	struct inode *const inode;

	struct z_erofs_collector clt;
	struct erofs_map_blocks map;

	/* used for applying cache strategy on the fly */
	bool backmost;
	erofs_off_t headoffset;
};

#define COLLECTOR_INIT() { \
	.owned_head = Z_EROFS_PCLUSTER_TAIL, \
	.mode = COLLECT_PRIMARY_FOLLOWED }

#define DECOMPRESS_FRONTEND_INIT(__i) { \
	.inode = __i, .clt = COLLECTOR_INIT(), \
	.backmost = true, }

static struct page *z_pagemap_global[Z_EROFS_VMAP_GLOBAL_PAGES];
static DEFINE_MUTEX(z_pagemap_global_lock);

static void preload_compressed_pages(struct z_erofs_collector *clt,
				     struct address_space *mc,
				     enum z_erofs_cache_alloctype type,
				     struct list_head *pagepool)
{
	const struct z_erofs_pcluster *pcl = clt->pcl;
	const unsigned int clusterpages = BIT(pcl->clusterbits);
	struct page **pages = clt->compressedpages;
	pgoff_t index = pcl->obj.index + (pages - pcl->compressed_pages);
	bool standalone = true;

	if (clt->mode < COLLECT_PRIMARY_FOLLOWED)
		return;

	for (; pages < pcl->compressed_pages + clusterpages; ++pages) {
		struct page *page;
		compressed_page_t t;

		/* the compressed page was loaded before */
		if (READ_ONCE(*pages))
			continue;

		page = find_get_page(mc, index);

		if (page) {
			t = tag_compressed_page_justfound(page);
		} else if (type == DELAYEDALLOC) {
			t = tagptr_init(compressed_page_t, PAGE_UNALLOCATED);
		} else {	/* DONTALLOC */
			if (standalone)
				clt->compressedpages = pages;
			standalone = false;
			continue;
		}

		if (!cmpxchg_relaxed(pages, NULL, tagptr_cast_ptr(t)))
			continue;

		if (page)
			put_page(page);
	}

	if (standalone)		/* downgrade to PRIMARY_FOLLOWED_NOINPLACE */
		clt->mode = COLLECT_PRIMARY_FOLLOWED_NOINPLACE;
}

/* called by erofs_shrinker to get rid of all compressed_pages */
int erofs_try_to_free_all_cached_pages(struct erofs_sb_info *sbi,
				       struct erofs_workgroup *grp)
{
	struct z_erofs_pcluster *const pcl =
		container_of(grp, struct z_erofs_pcluster, obj);
	struct address_space *const mapping = MNGD_MAPPING(sbi);
	const unsigned int clusterpages = BIT(pcl->clusterbits);
	int i;

	/*
	 * refcount of workgroup is now freezed as 1,
	 * therefore no need to worry about available decompression users.
	 */
	for (i = 0; i < clusterpages; ++i) {
		struct page *page = pcl->compressed_pages[i];

		if (!page)
			continue;

		/* block other users from reclaiming or migrating the page */
		if (!trylock_page(page))
			return -EBUSY;

		if (page->mapping != mapping)
			continue;

		/* barrier is implied in the following 'unlock_page' */
		WRITE_ONCE(pcl->compressed_pages[i], NULL);
		set_page_private(page, 0);
		ClearPagePrivate(page);

		unlock_page(page);
		put_page(page);
	}
	return 0;
}

int erofs_try_to_free_cached_page(struct address_space *mapping,
				  struct page *page)
{
	struct z_erofs_pcluster *const pcl = (void *)page_private(page);
	const unsigned int clusterpages = BIT(pcl->clusterbits);
	int ret = 0;	/* 0 - busy */

	if (erofs_workgroup_try_to_freeze(&pcl->obj, 1)) {
		unsigned int i;

		for (i = 0; i < clusterpages; ++i) {
			if (pcl->compressed_pages[i] == page) {
				WRITE_ONCE(pcl->compressed_pages[i], NULL);
				ret = 1;
				break;
			}
		}
		erofs_workgroup_unfreeze(&pcl->obj, 1);

		if (ret) {
			ClearPagePrivate(page);
			put_page(page);
		}
	}
	return ret;
}

/* page_type must be Z_EROFS_PAGE_TYPE_EXCLUSIVE */
static inline bool z_erofs_try_inplace_io(struct z_erofs_collector *clt,
					  struct page *page)
{
	struct z_erofs_pcluster *const pcl = clt->pcl;
	const unsigned int clusterpages = BIT(pcl->clusterbits);

	while (clt->compressedpages < pcl->compressed_pages + clusterpages) {
		if (!cmpxchg(clt->compressedpages++, NULL, page))
			return true;
	}
	return false;
}

/* callers must be with collection lock held */
static int z_erofs_attach_page(struct z_erofs_collector *clt,
			       struct page *page,
			       enum z_erofs_page_type type)
{
	int ret;
	bool occupied;

	/* give priority for inplaceio */
	if (clt->mode >= COLLECT_PRIMARY &&
	    type == Z_EROFS_PAGE_TYPE_EXCLUSIVE &&
	    z_erofs_try_inplace_io(clt, page))
		return 0;

	ret = z_erofs_pagevec_enqueue(&clt->vector,
				      page, type, &occupied);
	clt->cl->vcnt += (unsigned int)ret;

	return ret ? 0 : -EAGAIN;
}

static enum z_erofs_collectmode
try_to_claim_pcluster(struct z_erofs_pcluster *pcl,
		      z_erofs_next_pcluster_t *owned_head)
{
	/* let's claim these following types of pclusters */
retry:
	if (pcl->next == Z_EROFS_PCLUSTER_NIL) {
		/* type 1, nil pcluster */
		if (cmpxchg(&pcl->next, Z_EROFS_PCLUSTER_NIL,
			    *owned_head) != Z_EROFS_PCLUSTER_NIL)
			goto retry;

		*owned_head = &pcl->next;
		/* lucky, I am the followee :) */
		return COLLECT_PRIMARY_FOLLOWED;
	} else if (pcl->next == Z_EROFS_PCLUSTER_TAIL) {
		/*
		 * type 2, link to the end of a existing open chain,
		 * be careful that its submission itself is governed
		 * by the original owned chain.
		 */
		if (cmpxchg(&pcl->next, Z_EROFS_PCLUSTER_TAIL,
			    *owned_head) != Z_EROFS_PCLUSTER_TAIL)
			goto retry;
		*owned_head = Z_EROFS_PCLUSTER_TAIL;
		return COLLECT_PRIMARY_HOOKED;
	}
	return COLLECT_PRIMARY;	/* :( better luck next time */
}

static struct z_erofs_collection *cllookup(struct z_erofs_collector *clt,
					   struct inode *inode,
					   struct erofs_map_blocks *map)
{
	struct erofs_workgroup *grp;
	struct z_erofs_pcluster *pcl;
	struct z_erofs_collection *cl;
	unsigned int length;
	bool tag;

	grp = erofs_find_workgroup(inode->i_sb, map->m_pa >> PAGE_SHIFT, &tag);
	if (!grp)
		return NULL;

	pcl = container_of(grp, struct z_erofs_pcluster, obj);
	if (clt->owned_head == &pcl->next || pcl == clt->tailpcl) {
		DBG_BUGON(1);
		erofs_workgroup_put(grp);
		return ERR_PTR(-EFSCORRUPTED);
	}

	cl = z_erofs_primarycollection(pcl);
	if (cl->pageofs != (map->m_la & ~PAGE_MASK)) {
		DBG_BUGON(1);
		erofs_workgroup_put(grp);
		return ERR_PTR(-EFSCORRUPTED);
	}

	length = READ_ONCE(pcl->length);
	if (length & Z_EROFS_PCLUSTER_FULL_LENGTH) {
		if ((map->m_llen << Z_EROFS_PCLUSTER_LENGTH_BIT) > length) {
			DBG_BUGON(1);
			erofs_workgroup_put(grp);
			return ERR_PTR(-EFSCORRUPTED);
		}
	} else {
		unsigned int llen = map->m_llen << Z_EROFS_PCLUSTER_LENGTH_BIT;

		if (map->m_flags & EROFS_MAP_FULL_MAPPED)
			llen |= Z_EROFS_PCLUSTER_FULL_LENGTH;

		while (llen > length &&
		       length != cmpxchg_relaxed(&pcl->length, length, llen)) {
			cpu_relax();
			length = READ_ONCE(pcl->length);
		}
	}
	mutex_lock(&cl->lock);
	/* used to check tail merging loop due to corrupted images */
	if (clt->owned_head == Z_EROFS_PCLUSTER_TAIL)
		clt->tailpcl = pcl;
	clt->mode = try_to_claim_pcluster(pcl, &clt->owned_head);
	/* clean tailpcl if the current owned_head is Z_EROFS_PCLUSTER_TAIL */
	if (clt->owned_head == Z_EROFS_PCLUSTER_TAIL)
		clt->tailpcl = NULL;
	clt->pcl = pcl;
	clt->cl = cl;
	return cl;
}

static struct z_erofs_collection *clregister(struct z_erofs_collector *clt,
					     struct inode *inode,
					     struct erofs_map_blocks *map)
{
	struct z_erofs_pcluster *pcl;
	struct z_erofs_collection *cl;
	int err;

	/* no available workgroup, let's allocate one */
	pcl = kmem_cache_alloc(pcluster_cachep, GFP_NOFS);
	if (!pcl)
		return ERR_PTR(-ENOMEM);

	z_erofs_pcluster_init_always(pcl);
	pcl->obj.index = map->m_pa >> PAGE_SHIFT;

	pcl->length = (map->m_llen << Z_EROFS_PCLUSTER_LENGTH_BIT) |
		(map->m_flags & EROFS_MAP_FULL_MAPPED ?
			Z_EROFS_PCLUSTER_FULL_LENGTH : 0);

	if (map->m_flags & EROFS_MAP_ZIPPED)
		pcl->algorithmformat = Z_EROFS_COMPRESSION_LZ4;
	else
		pcl->algorithmformat = Z_EROFS_COMPRESSION_SHIFTED;

	pcl->clusterbits = EROFS_I(inode)->z_physical_clusterbits[0];
	pcl->clusterbits -= PAGE_SHIFT;

	/* new pclusters should be claimed as type 1, primary and followed */
	pcl->next = clt->owned_head;
	clt->mode = COLLECT_PRIMARY_FOLLOWED;

	cl = z_erofs_primarycollection(pcl);
	cl->pageofs = map->m_la & ~PAGE_MASK;

	/*
	 * lock all primary followed works before visible to others
	 * and mutex_trylock *never* fails for a new pcluster.
	 */
	mutex_trylock(&cl->lock);

	err = erofs_register_workgroup(inode->i_sb, &pcl->obj, 0);
	if (err) {
		mutex_unlock(&cl->lock);
		kmem_cache_free(pcluster_cachep, pcl);
		return ERR_PTR(-EAGAIN);
	}
	/* used to check tail merging loop due to corrupted images */
	if (clt->owned_head == Z_EROFS_PCLUSTER_TAIL)
		clt->tailpcl = pcl;
	clt->owned_head = &pcl->next;
	clt->pcl = pcl;
	clt->cl = cl;
	return cl;
}

static int z_erofs_collector_begin(struct z_erofs_collector *clt,
				   struct inode *inode,
				   struct erofs_map_blocks *map)
{
	struct z_erofs_collection *cl;

	DBG_BUGON(clt->cl);

	/* must be Z_EROFS_PCLUSTER_TAIL or pointed to previous collection */
	DBG_BUGON(clt->owned_head == Z_EROFS_PCLUSTER_NIL);
	DBG_BUGON(clt->owned_head == Z_EROFS_PCLUSTER_TAIL_CLOSED);

	if (!PAGE_ALIGNED(map->m_pa)) {
		DBG_BUGON(1);
		return -EINVAL;
	}

repeat:
	cl = cllookup(clt, inode, map);
	if (!cl) {
		cl = clregister(clt, inode, map);

		if (cl == ERR_PTR(-EAGAIN))
			goto repeat;
	}

	if (IS_ERR(cl))
		return PTR_ERR(cl);

	z_erofs_pagevec_ctor_init(&clt->vector, Z_EROFS_NR_INLINE_PAGEVECS,
				  cl->pagevec, cl->vcnt);

	clt->compressedpages = clt->pcl->compressed_pages;
	if (clt->mode <= COLLECT_PRIMARY) /* cannot do in-place I/O */
		clt->compressedpages += Z_EROFS_CLUSTER_MAX_PAGES;
	return 0;
}

/*
 * keep in mind that no referenced pclusters will be freed
 * only after a RCU grace period.
 */
static void z_erofs_rcu_callback(struct rcu_head *head)
{
	struct z_erofs_collection *const cl =
		container_of(head, struct z_erofs_collection, rcu);

	kmem_cache_free(pcluster_cachep,
			container_of(cl, struct z_erofs_pcluster,
				     primary_collection));
}

void erofs_workgroup_free_rcu(struct erofs_workgroup *grp)
{
	struct z_erofs_pcluster *const pcl =
		container_of(grp, struct z_erofs_pcluster, obj);
	struct z_erofs_collection *const cl = z_erofs_primarycollection(pcl);

	call_rcu(&cl->rcu, z_erofs_rcu_callback);
}

static void z_erofs_collection_put(struct z_erofs_collection *cl)
{
	struct z_erofs_pcluster *const pcl =
		container_of(cl, struct z_erofs_pcluster, primary_collection);

	erofs_workgroup_put(&pcl->obj);
}

static bool z_erofs_collector_end(struct z_erofs_collector *clt)
{
	struct z_erofs_collection *cl = clt->cl;

	if (!cl)
		return false;

	z_erofs_pagevec_ctor_exit(&clt->vector, false);
	mutex_unlock(&cl->lock);

	/*
	 * if all pending pages are added, don't hold its reference
	 * any longer if the pcluster isn't hosted by ourselves.
	 */
	if (clt->mode < COLLECT_PRIMARY_FOLLOWED_NOINPLACE)
		z_erofs_collection_put(cl);

	clt->cl = NULL;
	return true;
}

static inline struct page *__stagingpage_alloc(struct list_head *pagepool,
					       gfp_t gfp)
{
	struct page *page = erofs_allocpage(pagepool, gfp, true);

	page->mapping = Z_EROFS_MAPPING_STAGING;
	return page;
}

static bool should_alloc_managed_pages(struct z_erofs_decompress_frontend *fe,
				       unsigned int cachestrategy,
				       erofs_off_t la)
{
	if (cachestrategy <= EROFS_ZIP_CACHE_DISABLED)
		return false;

	if (fe->backmost)
		return true;

	return cachestrategy >= EROFS_ZIP_CACHE_READAROUND &&
		la < fe->headoffset;
}

static int z_erofs_do_read_page(struct z_erofs_decompress_frontend *fe,
				struct page *page,
				struct list_head *pagepool)
{
	struct inode *const inode = fe->inode;
	struct erofs_sb_info *const sbi __maybe_unused = EROFS_I_SB(inode);
	struct erofs_map_blocks *const map = &fe->map;
	struct z_erofs_collector *const clt = &fe->clt;
	const loff_t offset = page_offset(page);
	bool tight = true;

	enum z_erofs_cache_alloctype cache_strategy;
	enum z_erofs_page_type page_type;
	unsigned int cur, end, spiltted, index;
	int err = 0;

	/* register locked file pages as online pages in pack */
	z_erofs_onlinepage_init(page);

	spiltted = 0;
	end = PAGE_SIZE;
repeat:
	cur = end - 1;

	/* lucky, within the range of the current map_blocks */
	if (offset + cur >= map->m_la &&
	    offset + cur < map->m_la + map->m_llen) {
		/* didn't get a valid collection previously (very rare) */
		if (!clt->cl)
			goto restart_now;
		goto hitted;
	}

	/* go ahead the next map_blocks */
	erofs_dbg("%s: [out-of-range] pos %llu", __func__, offset + cur);

	if (z_erofs_collector_end(clt))
		fe->backmost = false;

	map->m_la = offset + cur;
	map->m_llen = 0;
	err = z_erofs_map_blocks_iter(inode, map, 0);
	if (err)
		goto err_out;

restart_now:
	if (!(map->m_flags & EROFS_MAP_MAPPED))
		goto hitted;

	err = z_erofs_collector_begin(clt, inode, map);
	if (err)
		goto err_out;

	/* preload all compressed pages (maybe downgrade role if necessary) */
	if (should_alloc_managed_pages(fe, sbi->cache_strategy, map->m_la))
		cache_strategy = DELAYEDALLOC;
	else
		cache_strategy = DONTALLOC;

	preload_compressed_pages(clt, MNGD_MAPPING(sbi),
				 cache_strategy, pagepool);

hitted:
	/*
	 * Ensure the current partial page belongs to this submit chain rather
	 * than other concurrent submit chains or the noio(bypass) chain since
	 * those chains are handled asynchronously thus the page cannot be used
	 * for inplace I/O or pagevec (should be processed in strict order.)
	 */
	tight &= (clt->mode >= COLLECT_PRIMARY_HOOKED &&
		  clt->mode != COLLECT_PRIMARY_FOLLOWED_NOINPLACE);

	cur = end - min_t(unsigned int, offset + end - map->m_la, end);
	if (!(map->m_flags & EROFS_MAP_MAPPED)) {
		zero_user_segment(page, cur, end);
		goto next_part;
	}

	/* let's derive page type */
	page_type = cur ? Z_EROFS_VLE_PAGE_TYPE_HEAD :
		(!spiltted ? Z_EROFS_PAGE_TYPE_EXCLUSIVE :
			(tight ? Z_EROFS_PAGE_TYPE_EXCLUSIVE :
				Z_EROFS_VLE_PAGE_TYPE_TAIL_SHARED));

	if (cur)
		tight &= (clt->mode >= COLLECT_PRIMARY_FOLLOWED);

retry:
	err = z_erofs_attach_page(clt, page, page_type);
	/* should allocate an additional staging page for pagevec */
	if (err == -EAGAIN) {
		struct page *const newpage =
			__stagingpage_alloc(pagepool, GFP_NOFS);

		err = z_erofs_attach_page(clt, newpage,
					  Z_EROFS_PAGE_TYPE_EXCLUSIVE);
		if (!err)
			goto retry;
	}

	if (err)
		goto err_out;

	index = page->index - (map->m_la >> PAGE_SHIFT);

	z_erofs_onlinepage_fixup(page, index, true);

	/* bump up the number of spiltted parts of a page */
	++spiltted;
	/* also update nr_pages */
	clt->cl->nr_pages = max_t(pgoff_t, clt->cl->nr_pages, index + 1);
next_part:
	/* can be used for verification */
	map->m_llen = offset + cur - map->m_la;

	end = cur;
	if (end > 0)
		goto repeat;

out:
	z_erofs_onlinepage_endio(page);

	erofs_dbg("%s, finish page: %pK spiltted: %u map->m_llen %llu",
		  __func__, page, spiltted, map->m_llen);
	return err;

	/* if some error occurred while processing this page */
err_out:
	SetPageError(page);
	goto out;
}

static void z_erofs_vle_unzip_kickoff(void *ptr, int bios)
{
	tagptr1_t t = tagptr_init(tagptr1_t, ptr);
	struct z_erofs_unzip_io *io = tagptr_unfold_ptr(t);
	bool background = tagptr_unfold_tags(t);

	if (!background) {
		unsigned long flags;

		spin_lock_irqsave(&io->u.wait.lock, flags);
		if (!atomic_add_return(bios, &io->pending_bios))
			wake_up_locked(&io->u.wait);
		spin_unlock_irqrestore(&io->u.wait.lock, flags);
		return;
	}

	if (!atomic_add_return(bios, &io->pending_bios))
		queue_work(z_erofs_workqueue, &io->u.work);
}

static inline void z_erofs_vle_read_endio(struct bio *bio)
{
	struct erofs_sb_info *sbi = NULL;
	blk_status_t err = bio->bi_status;
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;
		bool cachemngd = false;

		DBG_BUGON(PageUptodate(page));
		DBG_BUGON(!page->mapping);

		if (!sbi && !z_erofs_page_is_staging(page))
			sbi = EROFS_SB(page->mapping->host->i_sb);

		/* sbi should already be gotten if the page is managed */
		if (sbi)
			cachemngd = erofs_page_is_managed(sbi, page);

		if (err)
			SetPageError(page);
		else if (cachemngd)
			SetPageUptodate(page);

		if (cachemngd)
			unlock_page(page);
	}

	z_erofs_vle_unzip_kickoff(bio->bi_private, -1);
	bio_put(bio);
}

static int z_erofs_decompress_pcluster(struct super_block *sb,
				       struct z_erofs_pcluster *pcl,
				       struct list_head *pagepool)
{
	struct erofs_sb_info *const sbi = EROFS_SB(sb);
	const unsigned int clusterpages = BIT(pcl->clusterbits);
	struct z_erofs_pagevec_ctor ctor;
	unsigned int i, outputsize, llen, nr_pages;
	struct page *pages_onstack[Z_EROFS_VMAP_ONSTACK_PAGES];
	struct page **pages, **compressed_pages, *page;

	enum z_erofs_page_type page_type;
	bool overlapped, partial;
	struct z_erofs_collection *cl;
	int err;

	might_sleep();
	cl = z_erofs_primarycollection(pcl);
	DBG_BUGON(!READ_ONCE(cl->nr_pages));

	mutex_lock(&cl->lock);
	nr_pages = cl->nr_pages;

	if (nr_pages <= Z_EROFS_VMAP_ONSTACK_PAGES) {
		pages = pages_onstack;
	} else if (nr_pages <= Z_EROFS_VMAP_GLOBAL_PAGES &&
		   mutex_trylock(&z_pagemap_global_lock)) {
		pages = z_pagemap_global;
	} else {
		gfp_t gfp_flags = GFP_KERNEL;

		if (nr_pages > Z_EROFS_VMAP_GLOBAL_PAGES)
			gfp_flags |= __GFP_NOFAIL;

		pages = kvmalloc_array(nr_pages, sizeof(struct page *),
				       gfp_flags);

		/* fallback to global pagemap for the lowmem scenario */
		if (!pages) {
			mutex_lock(&z_pagemap_global_lock);
			pages = z_pagemap_global;
		}
	}

	for (i = 0; i < nr_pages; ++i)
		pages[i] = NULL;

	err = 0;
	z_erofs_pagevec_ctor_init(&ctor, Z_EROFS_NR_INLINE_PAGEVECS,
				  cl->pagevec, 0);

	for (i = 0; i < cl->vcnt; ++i) {
		unsigned int pagenr;

		page = z_erofs_pagevec_dequeue(&ctor, &page_type);

		/* all pages in pagevec ought to be valid */
		DBG_BUGON(!page);
		DBG_BUGON(!page->mapping);

		if (z_erofs_put_stagingpage(pagepool, page))
			continue;

		if (page_type == Z_EROFS_VLE_PAGE_TYPE_HEAD)
			pagenr = 0;
		else
			pagenr = z_erofs_onlinepage_index(page);

		DBG_BUGON(pagenr >= nr_pages);

		/*
		 * currently EROFS doesn't support multiref(dedup),
		 * so here erroring out one multiref page.
		 */
		if (pages[pagenr]) {
			DBG_BUGON(1);
			SetPageError(pages[pagenr]);
			z_erofs_onlinepage_endio(pages[pagenr]);
			err = -EFSCORRUPTED;
		}
		pages[pagenr] = page;
	}
	z_erofs_pagevec_ctor_exit(&ctor, true);

	overlapped = false;
	compressed_pages = pcl->compressed_pages;

	for (i = 0; i < clusterpages; ++i) {
		unsigned int pagenr;

		page = compressed_pages[i];

		/* all compressed pages ought to be valid */
		DBG_BUGON(!page);
		DBG_BUGON(!page->mapping);

		if (!z_erofs_page_is_staging(page)) {
			if (erofs_page_is_managed(sbi, page)) {
				if (!PageUptodate(page))
					err = -EIO;
				continue;
			}

			/*
			 * only if non-head page can be selected
			 * for inplace decompression
			 */
			pagenr = z_erofs_onlinepage_index(page);

			DBG_BUGON(pagenr >= nr_pages);
			if (pages[pagenr]) {
				DBG_BUGON(1);
				SetPageError(pages[pagenr]);
				z_erofs_onlinepage_endio(pages[pagenr]);
				err = -EFSCORRUPTED;
			}
			pages[pagenr] = page;

			overlapped = true;
		}

		/* PG_error needs checking for inplaced and staging pages */
		if (PageError(page)) {
			DBG_BUGON(PageUptodate(page));
			err = -EIO;
		}
	}

	if (err)
		goto out;

	llen = pcl->length >> Z_EROFS_PCLUSTER_LENGTH_BIT;
	if (nr_pages << PAGE_SHIFT >= cl->pageofs + llen) {
		outputsize = llen;
		partial = !(pcl->length & Z_EROFS_PCLUSTER_FULL_LENGTH);
	} else {
		outputsize = (nr_pages << PAGE_SHIFT) - cl->pageofs;
		partial = true;
	}

	err = z_erofs_decompress(&(struct z_erofs_decompress_req) {
					.sb = sb,
					.in = compressed_pages,
					.out = pages,
					.pageofs_out = cl->pageofs,
					.inputsize = PAGE_SIZE,
					.outputsize = outputsize,
					.alg = pcl->algorithmformat,
					.inplace_io = overlapped,
					.partial_decoding = partial
				 }, pagepool);

out:
	/* must handle all compressed pages before endding pages */
	for (i = 0; i < clusterpages; ++i) {
		page = compressed_pages[i];

		if (erofs_page_is_managed(sbi, page))
			continue;

		/* recycle all individual staging pages */
		(void)z_erofs_put_stagingpage(pagepool, page);

		WRITE_ONCE(compressed_pages[i], NULL);
	}

	for (i = 0; i < nr_pages; ++i) {
		page = pages[i];
		if (!page)
			continue;

		DBG_BUGON(!page->mapping);

		/* recycle all individual staging pages */
		if (z_erofs_put_stagingpage(pagepool, page))
			continue;

		if (err < 0)
			SetPageError(page);

		z_erofs_onlinepage_endio(page);
	}

	if (pages == z_pagemap_global)
		mutex_unlock(&z_pagemap_global_lock);
	else if (pages != pages_onstack)
		kvfree(pages);

	cl->nr_pages = 0;
	cl->vcnt = 0;

	/* all cl locks MUST be taken before the following line */
	WRITE_ONCE(pcl->next, Z_EROFS_PCLUSTER_NIL);

	/* all cl locks SHOULD be released right now */
	mutex_unlock(&cl->lock);

	z_erofs_collection_put(cl);
	return err;
}

static void z_erofs_vle_unzip_all(struct super_block *sb,
				  struct z_erofs_unzip_io *io,
				  struct list_head *pagepool)
{
	z_erofs_next_pcluster_t owned = io->head;

	while (owned != Z_EROFS_PCLUSTER_TAIL_CLOSED) {
		struct z_erofs_pcluster *pcl;

		/* no possible that 'owned' equals Z_EROFS_WORK_TPTR_TAIL */
		DBG_BUGON(owned == Z_EROFS_PCLUSTER_TAIL);

		/* no possible that 'owned' equals NULL */
		DBG_BUGON(owned == Z_EROFS_PCLUSTER_NIL);

		pcl = container_of(owned, struct z_erofs_pcluster, next);
		owned = READ_ONCE(pcl->next);

		z_erofs_decompress_pcluster(sb, pcl, pagepool);
	}
}

static void z_erofs_vle_unzip_wq(struct work_struct *work)
{
	struct z_erofs_unzip_io_sb *iosb =
		container_of(work, struct z_erofs_unzip_io_sb, io.u.work);
	LIST_HEAD(pagepool);

	DBG_BUGON(iosb->io.head == Z_EROFS_PCLUSTER_TAIL_CLOSED);
	z_erofs_vle_unzip_all(iosb->sb, &iosb->io, &pagepool);

	put_pages_list(&pagepool);
	kvfree(iosb);
}

static struct page *pickup_page_for_submission(struct z_erofs_pcluster *pcl,
					       unsigned int nr,
					       struct list_head *pagepool,
					       struct address_space *mc,
					       gfp_t gfp)
{
	/* determined at compile time to avoid too many #ifdefs */
	const bool nocache = __builtin_constant_p(mc) ? !mc : false;
	const pgoff_t index = pcl->obj.index;
	bool tocache = false;

	struct address_space *mapping;
	struct page *oldpage, *page;

	compressed_page_t t;
	int justfound;

repeat:
	page = READ_ONCE(pcl->compressed_pages[nr]);
	oldpage = page;

	if (!page)
		goto out_allocpage;

	/*
	 * the cached page has not been allocated and
	 * an placeholder is out there, prepare it now.
	 */
	if (!nocache && page == PAGE_UNALLOCATED) {
		tocache = true;
		goto out_allocpage;
	}

	/* process the target tagged pointer */
	t = tagptr_init(compressed_page_t, page);
	justfound = tagptr_unfold_tags(t);
	page = tagptr_unfold_ptr(t);

	mapping = READ_ONCE(page->mapping);

	/*
	 * if managed cache is disabled, it's no way to
	 * get such a cached-like page.
	 */
	if (nocache) {
		/* if managed cache is disabled, it is impossible `justfound' */
		DBG_BUGON(justfound);

		/* and it should be locked, not uptodate, and not truncated */
		DBG_BUGON(!PageLocked(page));
		DBG_BUGON(PageUptodate(page));
		DBG_BUGON(!mapping);
		goto out;
	}

	/*
	 * unmanaged (file) pages are all locked solidly,
	 * therefore it is impossible for `mapping' to be NULL.
	 */
	if (mapping && mapping != mc)
		/* ought to be unmanaged pages */
		goto out;

	lock_page(page);

	/* only true if page reclaim goes wrong, should never happen */
	DBG_BUGON(justfound && PagePrivate(page));

	/* the page is still in manage cache */
	if (page->mapping == mc) {
		WRITE_ONCE(pcl->compressed_pages[nr], page);

		ClearPageError(page);
		if (!PagePrivate(page)) {
			/*
			 * impossible to be !PagePrivate(page) for
			 * the current restriction as well if
			 * the page is already in compressed_pages[].
			 */
			DBG_BUGON(!justfound);

			justfound = 0;
			set_page_private(page, (unsigned long)pcl);
			SetPagePrivate(page);
		}

		/* no need to submit io if it is already up-to-date */
		if (PageUptodate(page)) {
			unlock_page(page);
			page = NULL;
		}
		goto out;
	}

	/*
	 * the managed page has been truncated, it's unsafe to
	 * reuse this one, let's allocate a new cache-managed page.
	 */
	DBG_BUGON(page->mapping);
	DBG_BUGON(!justfound);

	tocache = true;
	unlock_page(page);
	put_page(page);
out_allocpage:
	page = __stagingpage_alloc(pagepool, gfp);
	if (oldpage != cmpxchg(&pcl->compressed_pages[nr], oldpage, page)) {
		list_add(&page->lru, pagepool);
		cpu_relax();
		goto repeat;
	}
	if (nocache || !tocache)
		goto out;
	if (add_to_page_cache_lru(page, mc, index + nr, gfp)) {
		page->mapping = Z_EROFS_MAPPING_STAGING;
		goto out;
	}

	set_page_private(page, (unsigned long)pcl);
	SetPagePrivate(page);
out:	/* the only exit (for tracing and debugging) */
	return page;
}

static struct z_erofs_unzip_io *jobqueue_init(struct super_block *sb,
					      struct z_erofs_unzip_io *io,
					      bool foreground)
{
	struct z_erofs_unzip_io_sb *iosb;

	if (foreground) {
		/* waitqueue available for foreground io */
		DBG_BUGON(!io);

		init_waitqueue_head(&io->u.wait);
		atomic_set(&io->pending_bios, 0);
		goto out;
	}

	iosb = kvzalloc(sizeof(*iosb), GFP_KERNEL | __GFP_NOFAIL);
	DBG_BUGON(!iosb);

	/* initialize fields in the allocated descriptor */
	io = &iosb->io;
	iosb->sb = sb;
	INIT_WORK(&io->u.work, z_erofs_vle_unzip_wq);
out:
	io->head = Z_EROFS_PCLUSTER_TAIL_CLOSED;
	return io;
}

/* define decompression jobqueue types */
enum {
	JQ_BYPASS,
	JQ_SUBMIT,
	NR_JOBQUEUES,
};

static void *jobqueueset_init(struct super_block *sb,
			      z_erofs_next_pcluster_t qtail[],
			      struct z_erofs_unzip_io *q[],
			      struct z_erofs_unzip_io *fgq,
			      bool forcefg)
{
	/*
	 * if managed cache is enabled, bypass jobqueue is needed,
	 * no need to read from device for all pclusters in this queue.
	 */
	q[JQ_BYPASS] = jobqueue_init(sb, fgq + JQ_BYPASS, true);
	qtail[JQ_BYPASS] = &q[JQ_BYPASS]->head;

	q[JQ_SUBMIT] = jobqueue_init(sb, fgq + JQ_SUBMIT, forcefg);
	qtail[JQ_SUBMIT] = &q[JQ_SUBMIT]->head;

	return tagptr_cast_ptr(tagptr_fold(tagptr1_t, q[JQ_SUBMIT], !forcefg));
}

static void move_to_bypass_jobqueue(struct z_erofs_pcluster *pcl,
				    z_erofs_next_pcluster_t qtail[],
				    z_erofs_next_pcluster_t owned_head)
{
	z_erofs_next_pcluster_t *const submit_qtail = qtail[JQ_SUBMIT];
	z_erofs_next_pcluster_t *const bypass_qtail = qtail[JQ_BYPASS];

	DBG_BUGON(owned_head == Z_EROFS_PCLUSTER_TAIL_CLOSED);
	if (owned_head == Z_EROFS_PCLUSTER_TAIL)
		owned_head = Z_EROFS_PCLUSTER_TAIL_CLOSED;

	WRITE_ONCE(pcl->next, Z_EROFS_PCLUSTER_TAIL_CLOSED);

	WRITE_ONCE(*submit_qtail, owned_head);
	WRITE_ONCE(*bypass_qtail, &pcl->next);

	qtail[JQ_BYPASS] = &pcl->next;
}

static bool postsubmit_is_all_bypassed(struct z_erofs_unzip_io *q[],
				       unsigned int nr_bios,
				       bool force_fg)
{
	/*
	 * although background is preferred, no one is pending for submission.
	 * don't issue workqueue for decompression but drop it directly instead.
	 */
	if (force_fg || nr_bios)
		return false;

	kvfree(container_of(q[JQ_SUBMIT], struct z_erofs_unzip_io_sb, io));
	return true;
}

static bool z_erofs_vle_submit_all(struct super_block *sb,
				   z_erofs_next_pcluster_t owned_head,
				   struct list_head *pagepool,
				   struct z_erofs_unzip_io *fgq,
				   bool force_fg)
{
	struct erofs_sb_info *const sbi __maybe_unused = EROFS_SB(sb);
	z_erofs_next_pcluster_t qtail[NR_JOBQUEUES];
	struct z_erofs_unzip_io *q[NR_JOBQUEUES];
	struct bio *bio;
	void *bi_private;
	/* since bio will be NULL, no need to initialize last_index */
	pgoff_t uninitialized_var(last_index);
	bool force_submit = false;
	unsigned int nr_bios;

	if (owned_head == Z_EROFS_PCLUSTER_TAIL)
		return false;

	force_submit = false;
	bio = NULL;
	nr_bios = 0;
	bi_private = jobqueueset_init(sb, qtail, q, fgq, force_fg);

	/* by default, all need io submission */
	q[JQ_SUBMIT]->head = owned_head;

	do {
		struct z_erofs_pcluster *pcl;
		unsigned int clusterpages;
		pgoff_t first_index;
		struct page *page;
		unsigned int i = 0, bypass = 0;
		int err;

		/* no possible 'owned_head' equals the following */
		DBG_BUGON(owned_head == Z_EROFS_PCLUSTER_TAIL_CLOSED);
		DBG_BUGON(owned_head == Z_EROFS_PCLUSTER_NIL);

		pcl = container_of(owned_head, struct z_erofs_pcluster, next);

		clusterpages = BIT(pcl->clusterbits);

		/* close the main owned chain at first */
		owned_head = cmpxchg(&pcl->next, Z_EROFS_PCLUSTER_TAIL,
				     Z_EROFS_PCLUSTER_TAIL_CLOSED);

		first_index = pcl->obj.index;
		force_submit |= (first_index != last_index + 1);

repeat:
		page = pickup_page_for_submission(pcl, i, pagepool,
						  MNGD_MAPPING(sbi),
						  GFP_NOFS);
		if (!page) {
			force_submit = true;
			++bypass;
			goto skippage;
		}

		if (bio && force_submit) {
submit_bio_retry:
			submit_bio(bio);
			bio = NULL;
		}

		if (!bio) {
			bio = bio_alloc(GFP_NOIO, BIO_MAX_PAGES);

			bio->bi_end_io = z_erofs_vle_read_endio;
			bio_set_dev(bio, sb->s_bdev);
			bio->bi_iter.bi_sector = (sector_t)(first_index + i) <<
				LOG_SECTORS_PER_BLOCK;
			bio->bi_private = bi_private;
			bio->bi_opf = REQ_OP_READ;

			++nr_bios;
		}

		err = bio_add_page(bio, page, PAGE_SIZE, 0);
		if (err < PAGE_SIZE)
			goto submit_bio_retry;

		force_submit = false;
		last_index = first_index + i;
skippage:
		if (++i < clusterpages)
			goto repeat;

		if (bypass < clusterpages)
			qtail[JQ_SUBMIT] = &pcl->next;
		else
			move_to_bypass_jobqueue(pcl, qtail, owned_head);
	} while (owned_head != Z_EROFS_PCLUSTER_TAIL);

	if (bio)
		submit_bio(bio);

	if (postsubmit_is_all_bypassed(q, nr_bios, force_fg))
		return true;

	z_erofs_vle_unzip_kickoff(bi_private, nr_bios);
	return true;
}

static void z_erofs_submit_and_unzip(struct super_block *sb,
				     struct z_erofs_collector *clt,
				     struct list_head *pagepool,
				     bool force_fg)
{
	struct z_erofs_unzip_io io[NR_JOBQUEUES];

	if (!z_erofs_vle_submit_all(sb, clt->owned_head,
				    pagepool, io, force_fg))
		return;

	/* decompress no I/O pclusters immediately */
	z_erofs_vle_unzip_all(sb, &io[JQ_BYPASS], pagepool);

	if (!force_fg)
		return;

	/* wait until all bios are completed */
	wait_event(io[JQ_SUBMIT].u.wait,
		   !atomic_read(&io[JQ_SUBMIT].pending_bios));

	/* let's synchronous decompression */
	z_erofs_vle_unzip_all(sb, &io[JQ_SUBMIT], pagepool);
}

static int z_erofs_vle_normalaccess_readpage(struct file *file,
					     struct page *page)
{
	struct inode *const inode = page->mapping->host;
	struct z_erofs_decompress_frontend f = DECOMPRESS_FRONTEND_INIT(inode);
	int err;
	LIST_HEAD(pagepool);

	trace_erofs_readpage(page, false);

	f.headoffset = (erofs_off_t)page->index << PAGE_SHIFT;

	err = z_erofs_do_read_page(&f, page, &pagepool);
	(void)z_erofs_collector_end(&f.clt);

	/* if some compressed cluster ready, need submit them anyway */
	z_erofs_submit_and_unzip(inode->i_sb, &f.clt, &pagepool, true);

	if (err)
		erofs_err(inode->i_sb, "failed to read, err [%d]", err);

	if (f.map.mpage)
		put_page(f.map.mpage);

	/* clean up the remaining free pages */
	put_pages_list(&pagepool);
	return err;
}

static bool should_decompress_synchronously(struct erofs_sb_info *sbi,
					    unsigned int nr)
{
	return nr <= sbi->max_sync_decompress_pages;
}

static int z_erofs_vle_normalaccess_readpages(struct file *filp,
					      struct address_space *mapping,
					      struct list_head *pages,
					      unsigned int nr_pages)
{
	struct inode *const inode = mapping->host;
	struct erofs_sb_info *const sbi = EROFS_I_SB(inode);

	bool sync = should_decompress_synchronously(sbi, nr_pages);
	struct z_erofs_decompress_frontend f = DECOMPRESS_FRONTEND_INIT(inode);
	gfp_t gfp = mapping_gfp_constraint(mapping, GFP_KERNEL);
	struct page *head = NULL;
	LIST_HEAD(pagepool);

	trace_erofs_readpages(mapping->host, lru_to_page(pages),
			      nr_pages, false);

	f.headoffset = (erofs_off_t)lru_to_page(pages)->index << PAGE_SHIFT;

	for (; nr_pages; --nr_pages) {
		struct page *page = lru_to_page(pages);

		prefetchw(&page->flags);
		list_del(&page->lru);

		/*
		 * A pure asynchronous readahead is indicated if
		 * a PG_readahead marked page is hitted at first.
		 * Let's also do asynchronous decompression for this case.
		 */
		sync &= !(PageReadahead(page) && !head);

		if (add_to_page_cache_lru(page, mapping, page->index, gfp)) {
			list_add(&page->lru, &pagepool);
			continue;
		}

		set_page_private(page, (unsigned long)head);
		head = page;
	}

	while (head) {
		struct page *page = head;
		int err;

		/* traversal in reverse order */
		head = (void *)page_private(page);

		err = z_erofs_do_read_page(&f, page, &pagepool);
		if (err)
			erofs_err(inode->i_sb,
				  "readahead error at page %lu @ nid %llu",
				  page->index, EROFS_I(inode)->nid);
		put_page(page);
	}

	(void)z_erofs_collector_end(&f.clt);

	z_erofs_submit_and_unzip(inode->i_sb, &f.clt, &pagepool, sync);

	if (f.map.mpage)
		put_page(f.map.mpage);

	/* clean up the remaining free pages */
	put_pages_list(&pagepool);
	return 0;
}

const struct address_space_operations z_erofs_vle_normalaccess_aops = {
	.readpage = z_erofs_vle_normalaccess_readpage,
	.readpages = z_erofs_vle_normalaccess_readpages,
};

