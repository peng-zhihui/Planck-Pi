// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO: IOMMU DMA mapping support for Type1 IOMMU
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 *
 * We arbitrarily define a Type1 IOMMU as one matching the below code.
 * It could be called the x86 IOMMU as it's designed for AMD-Vi & Intel
 * VT-d, but that makes it harder to re-use as theoretically anyone
 * implementing a similar IOMMU could make use of this.  We expect the
 * IOMMU to support the IOMMU API and have few to no restrictions around
 * the IOVA range that can be mapped.  The Type1 IOMMU is currently
 * optimized for relatively static mappings of a userspace process with
 * userpsace pages pinned into memory.  We also assume devices and IOMMU
 * domains are PCI based as the IOMMU API is still centered around a
 * device/bus interface rather than a group interface.
 */

#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/rbtree.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/workqueue.h>
#include <linux/mdev.h>
#include <linux/notifier.h>
#include <linux/dma-iommu.h>
#include <linux/irqdomain.h>

#define DRIVER_VERSION  "0.2"
#define DRIVER_AUTHOR   "Alex Williamson <alex.williamson@redhat.com>"
#define DRIVER_DESC     "Type1 IOMMU driver for VFIO"

static bool allow_unsafe_interrupts;
module_param_named(allow_unsafe_interrupts,
		   allow_unsafe_interrupts, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(allow_unsafe_interrupts,
		 "Enable VFIO IOMMU support for on platforms without interrupt remapping support.");

static bool disable_hugepages;
module_param_named(disable_hugepages,
		   disable_hugepages, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(disable_hugepages,
		 "Disable VFIO IOMMU support for IOMMU hugepages.");

static unsigned int dma_entry_limit __read_mostly = U16_MAX;
module_param_named(dma_entry_limit, dma_entry_limit, uint, 0644);
MODULE_PARM_DESC(dma_entry_limit,
		 "Maximum number of user DMA mappings per container (65535).");

struct vfio_iommu {
	struct list_head	domain_list;
	struct list_head	iova_list;
	struct vfio_domain	*external_domain; /* domain for external user */
	struct mutex		lock;
	struct rb_root		dma_list;
	struct blocking_notifier_head notifier;
	unsigned int		dma_avail;
	bool			v2;
	bool			nesting;
};

struct vfio_domain {
	struct iommu_domain	*domain;
	struct list_head	next;
	struct list_head	group_list;
	int			prot;		/* IOMMU_CACHE */
	bool			fgsp;		/* Fine-grained super pages */
};

struct vfio_dma {
	struct rb_node		node;
	dma_addr_t		iova;		/* Device address */
	unsigned long		vaddr;		/* Process virtual addr */
	size_t			size;		/* Map size (bytes) */
	int			prot;		/* IOMMU_READ/WRITE */
	bool			iommu_mapped;
	bool			lock_cap;	/* capable(CAP_IPC_LOCK) */
	struct task_struct	*task;
	struct rb_root		pfn_list;	/* Ex-user pinned pfn list */
};

struct vfio_group {
	struct iommu_group	*iommu_group;
	struct list_head	next;
	bool			mdev_group;	/* An mdev group */
};

struct vfio_iova {
	struct list_head	list;
	dma_addr_t		start;
	dma_addr_t		end;
};

/*
 * Guest RAM pinning working set or DMA target
 */
struct vfio_pfn {
	struct rb_node		node;
	dma_addr_t		iova;		/* Device address */
	unsigned long		pfn;		/* Host pfn */
	atomic_t		ref_count;
};

struct vfio_regions {
	struct list_head list;
	dma_addr_t iova;
	phys_addr_t phys;
	size_t len;
};

#define IS_IOMMU_CAP_DOMAIN_IN_CONTAINER(iommu)	\
					(!list_empty(&iommu->domain_list))

static int put_pfn(unsigned long pfn, int prot);

/*
 * This code handles mapping and unmapping of user data buffers
 * into DMA'ble space using the IOMMU
 */

static struct vfio_dma *vfio_find_dma(struct vfio_iommu *iommu,
				      dma_addr_t start, size_t size)
{
	struct rb_node *node = iommu->dma_list.rb_node;

	while (node) {
		struct vfio_dma *dma = rb_entry(node, struct vfio_dma, node);

		if (start + size <= dma->iova)
			node = node->rb_left;
		else if (start >= dma->iova + dma->size)
			node = node->rb_right;
		else
			return dma;
	}

	return NULL;
}

static void vfio_link_dma(struct vfio_iommu *iommu, struct vfio_dma *new)
{
	struct rb_node **link = &iommu->dma_list.rb_node, *parent = NULL;
	struct vfio_dma *dma;

	while (*link) {
		parent = *link;
		dma = rb_entry(parent, struct vfio_dma, node);

		if (new->iova + new->size <= dma->iova)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, &iommu->dma_list);
}

static void vfio_unlink_dma(struct vfio_iommu *iommu, struct vfio_dma *old)
{
	rb_erase(&old->node, &iommu->dma_list);
}

/*
 * Helper Functions for host iova-pfn list
 */
static struct vfio_pfn *vfio_find_vpfn(struct vfio_dma *dma, dma_addr_t iova)
{
	struct vfio_pfn *vpfn;
	struct rb_node *node = dma->pfn_list.rb_node;

	while (node) {
		vpfn = rb_entry(node, struct vfio_pfn, node);

		if (iova < vpfn->iova)
			node = node->rb_left;
		else if (iova > vpfn->iova)
			node = node->rb_right;
		else
			return vpfn;
	}
	return NULL;
}

static void vfio_link_pfn(struct vfio_dma *dma,
			  struct vfio_pfn *new)
{
	struct rb_node **link, *parent = NULL;
	struct vfio_pfn *vpfn;

	link = &dma->pfn_list.rb_node;
	while (*link) {
		parent = *link;
		vpfn = rb_entry(parent, struct vfio_pfn, node);

		if (new->iova < vpfn->iova)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, &dma->pfn_list);
}

static void vfio_unlink_pfn(struct vfio_dma *dma, struct vfio_pfn *old)
{
	rb_erase(&old->node, &dma->pfn_list);
}

static int vfio_add_to_pfn_list(struct vfio_dma *dma, dma_addr_t iova,
				unsigned long pfn)
{
	struct vfio_pfn *vpfn;

	vpfn = kzalloc(sizeof(*vpfn), GFP_KERNEL);
	if (!vpfn)
		return -ENOMEM;

	vpfn->iova = iova;
	vpfn->pfn = pfn;
	atomic_set(&vpfn->ref_count, 1);
	vfio_link_pfn(dma, vpfn);
	return 0;
}

static void vfio_remove_from_pfn_list(struct vfio_dma *dma,
				      struct vfio_pfn *vpfn)
{
	vfio_unlink_pfn(dma, vpfn);
	kfree(vpfn);
}

static struct vfio_pfn *vfio_iova_get_vfio_pfn(struct vfio_dma *dma,
					       unsigned long iova)
{
	struct vfio_pfn *vpfn = vfio_find_vpfn(dma, iova);

	if (vpfn)
		atomic_inc(&vpfn->ref_count);
	return vpfn;
}

static int vfio_iova_put_vfio_pfn(struct vfio_dma *dma, struct vfio_pfn *vpfn)
{
	int ret = 0;

	if (atomic_dec_and_test(&vpfn->ref_count)) {
		ret = put_pfn(vpfn->pfn, dma->prot);
		vfio_remove_from_pfn_list(dma, vpfn);
	}
	return ret;
}

static int vfio_lock_acct(struct vfio_dma *dma, long npage, bool async)
{
	struct mm_struct *mm;
	int ret;

	if (!npage)
		return 0;

	mm = async ? get_task_mm(dma->task) : dma->task->mm;
	if (!mm)
		return -ESRCH; /* process exited */

	ret = down_write_killable(&mm->mmap_sem);
	if (!ret) {
		ret = __account_locked_vm(mm, abs(npage), npage > 0, dma->task,
					  dma->lock_cap);
		up_write(&mm->mmap_sem);
	}

	if (async)
		mmput(mm);

	return ret;
}

/*
 * Some mappings aren't backed by a struct page, for example an mmap'd
 * MMIO range for our own or another device.  These use a different
 * pfn conversion and shouldn't be tracked as locked pages.
 */
static bool is_invalid_reserved_pfn(unsigned long pfn)
{
	if (pfn_valid(pfn)) {
		bool reserved;
		struct page *tail = pfn_to_page(pfn);
		struct page *head = compound_head(tail);
		reserved = !!(PageReserved(head));
		if (head != tail) {
			/*
			 * "head" is not a dangling pointer
			 * (compound_head takes care of that)
			 * but the hugepage may have been split
			 * from under us (and we may not hold a
			 * reference count on the head page so it can
			 * be reused before we run PageReferenced), so
			 * we've to check PageTail before returning
			 * what we just read.
			 */
			smp_rmb();
			if (PageTail(tail))
				return reserved;
		}
		return PageReserved(tail);
	}

	return true;
}

static int put_pfn(unsigned long pfn, int prot)
{
	if (!is_invalid_reserved_pfn(pfn)) {
		struct page *page = pfn_to_page(pfn);
		if (prot & IOMMU_WRITE)
			SetPageDirty(page);
		put_page(page);
		return 1;
	}
	return 0;
}

static int follow_fault_pfn(struct vm_area_struct *vma, struct mm_struct *mm,
			    unsigned long vaddr, unsigned long *pfn,
			    bool write_fault)
{
	int ret;

	ret = follow_pfn(vma, vaddr, pfn);
	if (ret) {
		bool unlocked = false;

		ret = fixup_user_fault(NULL, mm, vaddr,
				       FAULT_FLAG_REMOTE |
				       (write_fault ?  FAULT_FLAG_WRITE : 0),
				       &unlocked);
		if (unlocked)
			return -EAGAIN;

		if (ret)
			return ret;

		ret = follow_pfn(vma, vaddr, pfn);
	}

	return ret;
}

static int vaddr_get_pfn(struct mm_struct *mm, unsigned long vaddr,
			 int prot, unsigned long *pfn)
{
	struct page *page[1];
	struct vm_area_struct *vma;
	struct vm_area_struct *vmas[1];
	unsigned int flags = 0;
	int ret;

	if (prot & IOMMU_WRITE)
		flags |= FOLL_WRITE;

	down_read(&mm->mmap_sem);
	if (mm == current->mm) {
		ret = get_user_pages(vaddr, 1, flags | FOLL_LONGTERM, page,
				     vmas);
	} else {
		ret = get_user_pages_remote(NULL, mm, vaddr, 1, flags, page,
					    vmas, NULL);
		/*
		 * The lifetime of a vaddr_get_pfn() page pin is
		 * userspace-controlled. In the fs-dax case this could
		 * lead to indefinite stalls in filesystem operations.
		 * Disallow attempts to pin fs-dax pages via this
		 * interface.
		 */
		if (ret > 0 && vma_is_fsdax(vmas[0])) {
			ret = -EOPNOTSUPP;
			put_page(page[0]);
		}
	}
	up_read(&mm->mmap_sem);

	if (ret == 1) {
		*pfn = page_to_pfn(page[0]);
		return 0;
	}

	down_read(&mm->mmap_sem);

	vaddr = untagged_addr(vaddr);

retry:
	vma = find_vma_intersection(mm, vaddr, vaddr + 1);

	if (vma && vma->vm_flags & VM_PFNMAP) {
		ret = follow_fault_pfn(vma, mm, vaddr, pfn, prot & IOMMU_WRITE);
		if (ret == -EAGAIN)
			goto retry;

		if (!ret && !is_invalid_reserved_pfn(*pfn))
			ret = -EFAULT;
	}

	up_read(&mm->mmap_sem);
	return ret;
}

/*
 * Attempt to pin pages.  We really don't want to track all the pfns and
 * the iommu can only map chunks of consecutive pfns anyway, so get the
 * first page and all consecutive pages with the same locking.
 */
static long vfio_pin_pages_remote(struct vfio_dma *dma, unsigned long vaddr,
				  long npage, unsigned long *pfn_base,
				  unsigned long limit)
{
	unsigned long pfn = 0;
	long ret, pinned = 0, lock_acct = 0;
	bool rsvd;
	dma_addr_t iova = vaddr - dma->vaddr + dma->iova;

	/* This code path is only user initiated */
	if (!current->mm)
		return -ENODEV;

	ret = vaddr_get_pfn(current->mm, vaddr, dma->prot, pfn_base);
	if (ret)
		return ret;

	pinned++;
	rsvd = is_invalid_reserved_pfn(*pfn_base);

	/*
	 * Reserved pages aren't counted against the user, externally pinned
	 * pages are already counted against the user.
	 */
	if (!rsvd && !vfio_find_vpfn(dma, iova)) {
		if (!dma->lock_cap && current->mm->locked_vm + 1 > limit) {
			put_pfn(*pfn_base, dma->prot);
			pr_warn("%s: RLIMIT_MEMLOCK (%ld) exceeded\n", __func__,
					limit << PAGE_SHIFT);
			return -ENOMEM;
		}
		lock_acct++;
	}

	if (unlikely(disable_hugepages))
		goto out;

	/* Lock all the consecutive pages from pfn_base */
	for (vaddr += PAGE_SIZE, iova += PAGE_SIZE; pinned < npage;
	     pinned++, vaddr += PAGE_SIZE, iova += PAGE_SIZE) {
		ret = vaddr_get_pfn(current->mm, vaddr, dma->prot, &pfn);
		if (ret)
			break;

		if (pfn != *pfn_base + pinned ||
		    rsvd != is_invalid_reserved_pfn(pfn)) {
			put_pfn(pfn, dma->prot);
			break;
		}

		if (!rsvd && !vfio_find_vpfn(dma, iova)) {
			if (!dma->lock_cap &&
			    current->mm->locked_vm + lock_acct + 1 > limit) {
				put_pfn(pfn, dma->prot);
				pr_warn("%s: RLIMIT_MEMLOCK (%ld) exceeded\n",
					__func__, limit << PAGE_SHIFT);
				ret = -ENOMEM;
				goto unpin_out;
			}
			lock_acct++;
		}
	}

out:
	ret = vfio_lock_acct(dma, lock_acct, false);

unpin_out:
	if (ret) {
		if (!rsvd) {
			for (pfn = *pfn_base ; pinned ; pfn++, pinned--)
				put_pfn(pfn, dma->prot);
		}

		return ret;
	}

	return pinned;
}

static long vfio_unpin_pages_remote(struct vfio_dma *dma, dma_addr_t iova,
				    unsigned long pfn, long npage,
				    bool do_accounting)
{
	long unlocked = 0, locked = 0;
	long i;

	for (i = 0; i < npage; i++, iova += PAGE_SIZE) {
		if (put_pfn(pfn++, dma->prot)) {
			unlocked++;
			if (vfio_find_vpfn(dma, iova))
				locked++;
		}
	}

	if (do_accounting)
		vfio_lock_acct(dma, locked - unlocked, true);

	return unlocked;
}

static int vfio_pin_page_external(struct vfio_dma *dma, unsigned long vaddr,
				  unsigned long *pfn_base, bool do_accounting)
{
	struct mm_struct *mm;
	int ret;

	mm = get_task_mm(dma->task);
	if (!mm)
		return -ENODEV;

	ret = vaddr_get_pfn(mm, vaddr, dma->prot, pfn_base);
	if (!ret && do_accounting && !is_invalid_reserved_pfn(*pfn_base)) {
		ret = vfio_lock_acct(dma, 1, true);
		if (ret) {
			put_pfn(*pfn_base, dma->prot);
			if (ret == -ENOMEM)
				pr_warn("%s: Task %s (%d) RLIMIT_MEMLOCK "
					"(%ld) exceeded\n", __func__,
					dma->task->comm, task_pid_nr(dma->task),
					task_rlimit(dma->task, RLIMIT_MEMLOCK));
		}
	}

	mmput(mm);
	return ret;
}

static int vfio_unpin_page_external(struct vfio_dma *dma, dma_addr_t iova,
				    bool do_accounting)
{
	int unlocked;
	struct vfio_pfn *vpfn = vfio_find_vpfn(dma, iova);

	if (!vpfn)
		return 0;

	unlocked = vfio_iova_put_vfio_pfn(dma, vpfn);

	if (do_accounting)
		vfio_lock_acct(dma, -unlocked, true);

	return unlocked;
}

static int vfio_iommu_type1_pin_pages(void *iommu_data,
				      unsigned long *user_pfn,
				      int npage, int prot,
				      unsigned long *phys_pfn)
{
	struct vfio_iommu *iommu = iommu_data;
	int i, j, ret;
	unsigned long remote_vaddr;
	struct vfio_dma *dma;
	bool do_accounting;

	if (!iommu || !user_pfn || !phys_pfn)
		return -EINVAL;

	/* Supported for v2 version only */
	if (!iommu->v2)
		return -EACCES;

	mutex_lock(&iommu->lock);

	/* Fail if notifier list is empty */
	if (!iommu->notifier.head) {
		ret = -EINVAL;
		goto pin_done;
	}

	/*
	 * If iommu capable domain exist in the container then all pages are
	 * already pinned and accounted. Accouting should be done if there is no
	 * iommu capable domain in the container.
	 */
	do_accounting = !IS_IOMMU_CAP_DOMAIN_IN_CONTAINER(iommu);

	for (i = 0; i < npage; i++) {
		dma_addr_t iova;
		struct vfio_pfn *vpfn;

		iova = user_pfn[i] << PAGE_SHIFT;
		dma = vfio_find_dma(iommu, iova, PAGE_SIZE);
		if (!dma) {
			ret = -EINVAL;
			goto pin_unwind;
		}

		if ((dma->prot & prot) != prot) {
			ret = -EPERM;
			goto pin_unwind;
		}

		vpfn = vfio_iova_get_vfio_pfn(dma, iova);
		if (vpfn) {
			phys_pfn[i] = vpfn->pfn;
			continue;
		}

		remote_vaddr = dma->vaddr + (iova - dma->iova);
		ret = vfio_pin_page_external(dma, remote_vaddr, &phys_pfn[i],
					     do_accounting);
		if (ret)
			goto pin_unwind;

		ret = vfio_add_to_pfn_list(dma, iova, phys_pfn[i]);
		if (ret) {
			if (put_pfn(phys_pfn[i], dma->prot) && do_accounting)
				vfio_lock_acct(dma, -1, true);
			goto pin_unwind;
		}
	}

	ret = i;
	goto pin_done;

pin_unwind:
	phys_pfn[i] = 0;
	for (j = 0; j < i; j++) {
		dma_addr_t iova;

		iova = user_pfn[j] << PAGE_SHIFT;
		dma = vfio_find_dma(iommu, iova, PAGE_SIZE);
		vfio_unpin_page_external(dma, iova, do_accounting);
		phys_pfn[j] = 0;
	}
pin_done:
	mutex_unlock(&iommu->lock);
	return ret;
}

static int vfio_iommu_type1_unpin_pages(void *iommu_data,
					unsigned long *user_pfn,
					int npage)
{
	struct vfio_iommu *iommu = iommu_data;
	bool do_accounting;
	int i;

	if (!iommu || !user_pfn)
		return -EINVAL;

	/* Supported for v2 version only */
	if (!iommu->v2)
		return -EACCES;

	mutex_lock(&iommu->lock);

	do_accounting = !IS_IOMMU_CAP_DOMAIN_IN_CONTAINER(iommu);
	for (i = 0; i < npage; i++) {
		struct vfio_dma *dma;
		dma_addr_t iova;

		iova = user_pfn[i] << PAGE_SHIFT;
		dma = vfio_find_dma(iommu, iova, PAGE_SIZE);
		if (!dma)
			goto unpin_exit;
		vfio_unpin_page_external(dma, iova, do_accounting);
	}

unpin_exit:
	mutex_unlock(&iommu->lock);
	return i > npage ? npage : (i > 0 ? i : -EINVAL);
}

static long vfio_sync_unpin(struct vfio_dma *dma, struct vfio_domain *domain,
			    struct list_head *regions,
			    struct iommu_iotlb_gather *iotlb_gather)
{
	long unlocked = 0;
	struct vfio_regions *entry, *next;

	iommu_tlb_sync(domain->domain, iotlb_gather);

	list_for_each_entry_safe(entry, next, regions, list) {
		unlocked += vfio_unpin_pages_remote(dma,
						    entry->iova,
						    entry->phys >> PAGE_SHIFT,
						    entry->len >> PAGE_SHIFT,
						    false);
		list_del(&entry->list);
		kfree(entry);
	}

	cond_resched();

	return unlocked;
}

/*
 * Generally, VFIO needs to unpin remote pages after each IOTLB flush.
 * Therefore, when using IOTLB flush sync interface, VFIO need to keep track
 * of these regions (currently using a list).
 *
 * This value specifies maximum number of regions for each IOTLB flush sync.
 */
#define VFIO_IOMMU_TLB_SYNC_MAX		512

static size_t unmap_unpin_fast(struct vfio_domain *domain,
			       struct vfio_dma *dma, dma_addr_t *iova,
			       size_t len, phys_addr_t phys, long *unlocked,
			       struct list_head *unmapped_list,
			       int *unmapped_cnt,
			       struct iommu_iotlb_gather *iotlb_gather)
{
	size_t unmapped = 0;
	struct vfio_regions *entry = kzalloc(sizeof(*entry), GFP_KERNEL);

	if (entry) {
		unmapped = iommu_unmap_fast(domain->domain, *iova, len,
					    iotlb_gather);

		if (!unmapped) {
			kfree(entry);
		} else {
			entry->iova = *iova;
			entry->phys = phys;
			entry->len  = unmapped;
			list_add_tail(&entry->list, unmapped_list);

			*iova += unmapped;
			(*unmapped_cnt)++;
		}
	}

	/*
	 * Sync if the number of fast-unmap regions hits the limit
	 * or in case of errors.
	 */
	if (*unmapped_cnt >= VFIO_IOMMU_TLB_SYNC_MAX || !unmapped) {
		*unlocked += vfio_sync_unpin(dma, domain, unmapped_list,
					     iotlb_gather);
		*unmapped_cnt = 0;
	}

	return unmapped;
}

static size_t unmap_unpin_slow(struct vfio_domain *domain,
			       struct vfio_dma *dma, dma_addr_t *iova,
			       size_t len, phys_addr_t phys,
			       long *unlocked)
{
	size_t unmapped = iommu_unmap(domain->domain, *iova, len);

	if (unmapped) {
		*unlocked += vfio_unpin_pages_remote(dma, *iova,
						     phys >> PAGE_SHIFT,
						     unmapped >> PAGE_SHIFT,
						     false);
		*iova += unmapped;
		cond_resched();
	}
	return unmapped;
}

static long vfio_unmap_unpin(struct vfio_iommu *iommu, struct vfio_dma *dma,
			     bool do_accounting)
{
	dma_addr_t iova = dma->iova, end = dma->iova + dma->size;
	struct vfio_domain *domain, *d;
	LIST_HEAD(unmapped_region_list);
	struct iommu_iotlb_gather iotlb_gather;
	int unmapped_region_cnt = 0;
	long unlocked = 0;

	if (!dma->size)
		return 0;

	if (!IS_IOMMU_CAP_DOMAIN_IN_CONTAINER(iommu))
		return 0;

	/*
	 * We use the IOMMU to track the physical addresses, otherwise we'd
	 * need a much more complicated tracking system.  Unfortunately that
	 * means we need to use one of the iommu domains to figure out the
	 * pfns to unpin.  The rest need to be unmapped in advance so we have
	 * no iommu translations remaining when the pages are unpinned.
	 */
	domain = d = list_first_entry(&iommu->domain_list,
				      struct vfio_domain, next);

	list_for_each_entry_continue(d, &iommu->domain_list, next) {
		iommu_unmap(d->domain, dma->iova, dma->size);
		cond_resched();
	}

	iommu_iotlb_gather_init(&iotlb_gather);
	while (iova < end) {
		size_t unmapped, len;
		phys_addr_t phys, next;

		phys = iommu_iova_to_phys(domain->domain, iova);
		if (WARN_ON(!phys)) {
			iova += PAGE_SIZE;
			continue;
		}

		/*
		 * To optimize for fewer iommu_unmap() calls, each of which
		 * may require hardware cache flushing, try to find the
		 * largest contiguous physical memory chunk to unmap.
		 */
		for (len = PAGE_SIZE;
		     !domain->fgsp && iova + len < end; len += PAGE_SIZE) {
			next = iommu_iova_to_phys(domain->domain, iova + len);
			if (next != phys + len)
				break;
		}

		/*
		 * First, try to use fast unmap/unpin. In case of failure,
		 * switch to slow unmap/unpin path.
		 */
		unmapped = unmap_unpin_fast(domain, dma, &iova, len, phys,
					    &unlocked, &unmapped_region_list,
					    &unmapped_region_cnt,
					    &iotlb_gather);
		if (!unmapped) {
			unmapped = unmap_unpin_slow(domain, dma, &iova, len,
						    phys, &unlocked);
			if (WARN_ON(!unmapped))
				break;
		}
	}

	dma->iommu_mapped = false;

	if (unmapped_region_cnt) {
		unlocked += vfio_sync_unpin(dma, domain, &unmapped_region_list,
					    &iotlb_gather);
	}

	if (do_accounting) {
		vfio_lock_acct(dma, -unlocked, true);
		return 0;
	}
	return unlocked;
}

static void vfio_remove_dma(struct vfio_iommu *iommu, struct vfio_dma *dma)
{
	vfio_unmap_unpin(iommu, dma, true);
	vfio_unlink_dma(iommu, dma);
	put_task_struct(dma->task);
	kfree(dma);
	iommu->dma_avail++;
}

static unsigned long vfio_pgsize_bitmap(struct vfio_iommu *iommu)
{
	struct vfio_domain *domain;
	unsigned long bitmap = ULONG_MAX;

	mutex_lock(&iommu->lock);
	list_for_each_entry(domain, &iommu->domain_list, next)
		bitmap &= domain->domain->pgsize_bitmap;
	mutex_unlock(&iommu->lock);

	/*
	 * In case the IOMMU supports page sizes smaller than PAGE_SIZE
	 * we pretend PAGE_SIZE is supported and hide sub-PAGE_SIZE sizes.
	 * That way the user will be able to map/unmap buffers whose size/
	 * start address is aligned with PAGE_SIZE. Pinning code uses that
	 * granularity while iommu driver can use the sub-PAGE_SIZE size
	 * to map the buffer.
	 */
	if (bitmap & ~PAGE_MASK) {
		bitmap &= PAGE_MASK;
		bitmap |= PAGE_SIZE;
	}

	return bitmap;
}

static int vfio_dma_do_unmap(struct vfio_iommu *iommu,
			     struct vfio_iommu_type1_dma_unmap *unmap)
{
	uint64_t mask;
	struct vfio_dma *dma, *dma_last = NULL;
	size_t unmapped = 0;
	int ret = 0, retries = 0;

	mask = ((uint64_t)1 << __ffs(vfio_pgsize_bitmap(iommu))) - 1;

	if (unmap->iova & mask)
		return -EINVAL;
	if (!unmap->size || unmap->size & mask)
		return -EINVAL;
	if (unmap->iova + unmap->size - 1 < unmap->iova ||
	    unmap->size > SIZE_MAX)
		return -EINVAL;

	WARN_ON(mask & PAGE_MASK);
again:
	mutex_lock(&iommu->lock);

	/*
	 * vfio-iommu-type1 (v1) - User mappings were coalesced together to
	 * avoid tracking individual mappings.  This means that the granularity
	 * of the original mapping was lost and the user was allowed to attempt
	 * to unmap any range.  Depending on the contiguousness of physical
	 * memory and page sizes supported by the IOMMU, arbitrary unmaps may
	 * or may not have worked.  We only guaranteed unmap granularity
	 * matching the original mapping; even though it was untracked here,
	 * the original mappings are reflected in IOMMU mappings.  This
	 * resulted in a couple unusual behaviors.  First, if a range is not
	 * able to be unmapped, ex. a set of 4k pages that was mapped as a
	 * 2M hugepage into the IOMMU, the unmap ioctl returns success but with
	 * a zero sized unmap.  Also, if an unmap request overlaps the first
	 * address of a hugepage, the IOMMU will unmap the entire hugepage.
	 * This also returns success and the returned unmap size reflects the
	 * actual size unmapped.
	 *
	 * We attempt to maintain compatibility with this "v1" interface, but
	 * we take control out of the hands of the IOMMU.  Therefore, an unmap
	 * request offset from the beginning of the original mapping will
	 * return success with zero sized unmap.  And an unmap request covering
	 * the first iova of mapping will unmap the entire range.
	 *
	 * The v2 version of this interface intends to be more deterministic.
	 * Unmap requests must fully cover previous mappings.  Multiple
	 * mappings may still be unmaped by specifying large ranges, but there
	 * must not be any previous mappings bisected by the range.  An error
	 * will be returned if these conditions are not met.  The v2 interface
	 * will only return success and a size of zero if there were no
	 * mappings within the range.
	 */
	if (iommu->v2) {
		dma = vfio_find_dma(iommu, unmap->iova, 1);
		if (dma && dma->iova != unmap->iova) {
			ret = -EINVAL;
			goto unlock;
		}
		dma = vfio_find_dma(iommu, unmap->iova + unmap->size - 1, 0);
		if (dma && dma->iova + dma->size != unmap->iova + unmap->size) {
			ret = -EINVAL;
			goto unlock;
		}
	}

	while ((dma = vfio_find_dma(iommu, unmap->iova, unmap->size))) {
		if (!iommu->v2 && unmap->iova > dma->iova)
			break;
		/*
		 * Task with same address space who mapped this iova range is
		 * allowed to unmap the iova range.
		 */
		if (dma->task->mm != current->mm)
			break;

		if (!RB_EMPTY_ROOT(&dma->pfn_list)) {
			struct vfio_iommu_type1_dma_unmap nb_unmap;

			if (dma_last == dma) {
				BUG_ON(++retries > 10);
			} else {
				dma_last = dma;
				retries = 0;
			}

			nb_unmap.iova = dma->iova;
			nb_unmap.size = dma->size;

			/*
			 * Notify anyone (mdev vendor drivers) to invalidate and
			 * unmap iovas within the range we're about to unmap.
			 * Vendor drivers MUST unpin pages in response to an
			 * invalidation.
			 */
			mutex_unlock(&iommu->lock);
			blocking_notifier_call_chain(&iommu->notifier,
						    VFIO_IOMMU_NOTIFY_DMA_UNMAP,
						    &nb_unmap);
			goto again;
		}
		unmapped += dma->size;
		vfio_remove_dma(iommu, dma);
	}

unlock:
	mutex_unlock(&iommu->lock);

	/* Report how much was unmapped */
	unmap->size = unmapped;

	return ret;
}

static int vfio_iommu_map(struct vfio_iommu *iommu, dma_addr_t iova,
			  unsigned long pfn, long npage, int prot)
{
	struct vfio_domain *d;
	int ret;

	list_for_each_entry(d, &iommu->domain_list, next) {
		ret = iommu_map(d->domain, iova, (phys_addr_t)pfn << PAGE_SHIFT,
				npage << PAGE_SHIFT, prot | d->prot);
		if (ret)
			goto unwind;

		cond_resched();
	}

	return 0;

unwind:
	list_for_each_entry_continue_reverse(d, &iommu->domain_list, next)
		iommu_unmap(d->domain, iova, npage << PAGE_SHIFT);

	return ret;
}

static int vfio_pin_map_dma(struct vfio_iommu *iommu, struct vfio_dma *dma,
			    size_t map_size)
{
	dma_addr_t iova = dma->iova;
	unsigned long vaddr = dma->vaddr;
	size_t size = map_size;
	long npage;
	unsigned long pfn, limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;
	int ret = 0;

	while (size) {
		/* Pin a contiguous chunk of memory */
		npage = vfio_pin_pages_remote(dma, vaddr + dma->size,
					      size >> PAGE_SHIFT, &pfn, limit);
		if (npage <= 0) {
			WARN_ON(!npage);
			ret = (int)npage;
			break;
		}

		/* Map it! */
		ret = vfio_iommu_map(iommu, iova + dma->size, pfn, npage,
				     dma->prot);
		if (ret) {
			vfio_unpin_pages_remote(dma, iova + dma->size, pfn,
						npage, true);
			break;
		}

		size -= npage << PAGE_SHIFT;
		dma->size += npage << PAGE_SHIFT;
	}

	dma->iommu_mapped = true;

	if (ret)
		vfio_remove_dma(iommu, dma);

	return ret;
}

/*
 * Check dma map request is within a valid iova range
 */
static bool vfio_iommu_iova_dma_valid(struct vfio_iommu *iommu,
				      dma_addr_t start, dma_addr_t end)
{
	struct list_head *iova = &iommu->iova_list;
	struct vfio_iova *node;

	list_for_each_entry(node, iova, list) {
		if (start >= node->start && end <= node->end)
			return true;
	}

	/*
	 * Check for list_empty() as well since a container with
	 * a single mdev device will have an empty list.
	 */
	return list_empty(iova);
}

static int vfio_dma_do_map(struct vfio_iommu *iommu,
			   struct vfio_iommu_type1_dma_map *map)
{
	dma_addr_t iova = map->iova;
	unsigned long vaddr = map->vaddr;
	size_t size = map->size;
	int ret = 0, prot = 0;
	uint64_t mask;
	struct vfio_dma *dma;

	/* Verify that none of our __u64 fields overflow */
	if (map->size != size || map->vaddr != vaddr || map->iova != iova)
		return -EINVAL;

	mask = ((uint64_t)1 << __ffs(vfio_pgsize_bitmap(iommu))) - 1;

	WARN_ON(mask & PAGE_MASK);

	/* READ/WRITE from device perspective */
	if (map->flags & VFIO_DMA_MAP_FLAG_WRITE)
		prot |= IOMMU_WRITE;
	if (map->flags & VFIO_DMA_MAP_FLAG_READ)
		prot |= IOMMU_READ;

	if (!prot || !size || (size | iova | vaddr) & mask)
		return -EINVAL;

	/* Don't allow IOVA or virtual address wrap */
	if (iova + size - 1 < iova || vaddr + size - 1 < vaddr)
		return -EINVAL;

	mutex_lock(&iommu->lock);

	if (vfio_find_dma(iommu, iova, size)) {
		ret = -EEXIST;
		goto out_unlock;
	}

	if (!iommu->dma_avail) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	if (!vfio_iommu_iova_dma_valid(iommu, iova, iova + size - 1)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (!dma) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	iommu->dma_avail--;
	dma->iova = iova;
	dma->vaddr = vaddr;
	dma->prot = prot;

	/*
	 * We need to be able to both add to a task's locked memory and test
	 * against the locked memory limit and we need to be able to do both
	 * outside of this call path as pinning can be asynchronous via the
	 * external interfaces for mdev devices.  RLIMIT_MEMLOCK requires a
	 * task_struct and VM locked pages requires an mm_struct, however
	 * holding an indefinite mm reference is not recommended, therefore we
	 * only hold a reference to a task.  We could hold a reference to
	 * current, however QEMU uses this call path through vCPU threads,
	 * which can be killed resulting in a NULL mm and failure in the unmap
	 * path when called via a different thread.  Avoid this problem by
	 * using the group_leader as threads within the same group require
	 * both CLONE_THREAD and CLONE_VM and will therefore use the same
	 * mm_struct.
	 *
	 * Previously we also used the task for testing CAP_IPC_LOCK at the
	 * time of pinning and accounting, however has_capability() makes use
	 * of real_cred, a copy-on-write field, so we can't guarantee that it
	 * matches group_leader, or in fact that it might not change by the
	 * time it's evaluated.  If a process were to call MAP_DMA with
	 * CAP_IPC_LOCK but later drop it, it doesn't make sense that they
	 * possibly see different results for an iommu_mapped vfio_dma vs
	 * externally mapped.  Therefore track CAP_IPC_LOCK in vfio_dma at the
	 * time of calling MAP_DMA.
	 */
	get_task_struct(current->group_leader);
	dma->task = current->group_leader;
	dma->lock_cap = capable(CAP_IPC_LOCK);

	dma->pfn_list = RB_ROOT;

	/* Insert zero-sized and grow as we map chunks of it */
	vfio_link_dma(iommu, dma);

	/* Don't pin and map if container doesn't contain IOMMU capable domain*/
	if (!IS_IOMMU_CAP_DOMAIN_IN_CONTAINER(iommu))
		dma->size = size;
	else
		ret = vfio_pin_map_dma(iommu, dma, size);

out_unlock:
	mutex_unlock(&iommu->lock);
	return ret;
}

static int vfio_bus_type(struct device *dev, void *data)
{
	struct bus_type **bus = data;

	if (*bus && *bus != dev->bus)
		return -EINVAL;

	*bus = dev->bus;

	return 0;
}

static int vfio_iommu_replay(struct vfio_iommu *iommu,
			     struct vfio_domain *domain)
{
	struct vfio_domain *d = NULL;
	struct rb_node *n;
	unsigned long limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;
	int ret;

	/* Arbitrarily pick the first domain in the list for lookups */
	if (!list_empty(&iommu->domain_list))
		d = list_first_entry(&iommu->domain_list,
				     struct vfio_domain, next);

	n = rb_first(&iommu->dma_list);

	for (; n; n = rb_next(n)) {
		struct vfio_dma *dma;
		dma_addr_t iova;

		dma = rb_entry(n, struct vfio_dma, node);
		iova = dma->iova;

		while (iova < dma->iova + dma->size) {
			phys_addr_t phys;
			size_t size;

			if (dma->iommu_mapped) {
				phys_addr_t p;
				dma_addr_t i;

				if (WARN_ON(!d)) { /* mapped w/o a domain?! */
					ret = -EINVAL;
					goto unwind;
				}

				phys = iommu_iova_to_phys(d->domain, iova);

				if (WARN_ON(!phys)) {
					iova += PAGE_SIZE;
					continue;
				}

				size = PAGE_SIZE;
				p = phys + size;
				i = iova + size;
				while (i < dma->iova + dma->size &&
				       p == iommu_iova_to_phys(d->domain, i)) {
					size += PAGE_SIZE;
					p += PAGE_SIZE;
					i += PAGE_SIZE;
				}
			} else {
				unsigned long pfn;
				unsigned long vaddr = dma->vaddr +
						     (iova - dma->iova);
				size_t n = dma->iova + dma->size - iova;
				long npage;

				npage = vfio_pin_pages_remote(dma, vaddr,
							      n >> PAGE_SHIFT,
							      &pfn, limit);
				if (npage <= 0) {
					WARN_ON(!npage);
					ret = (int)npage;
					goto unwind;
				}

				phys = pfn << PAGE_SHIFT;
				size = npage << PAGE_SHIFT;
			}

			ret = iommu_map(domain->domain, iova, phys,
					size, dma->prot | domain->prot);
			if (ret) {
				if (!dma->iommu_mapped)
					vfio_unpin_pages_remote(dma, iova,
							phys >> PAGE_SHIFT,
							size >> PAGE_SHIFT,
							true);
				goto unwind;
			}

			iova += size;
		}
	}

	/* All dmas are now mapped, defer to second tree walk for unwind */
	for (n = rb_first(&iommu->dma_list); n; n = rb_next(n)) {
		struct vfio_dma *dma = rb_entry(n, struct vfio_dma, node);

		dma->iommu_mapped = true;
	}

	return 0;

unwind:
	for (; n; n = rb_prev(n)) {
		struct vfio_dma *dma = rb_entry(n, struct vfio_dma, node);
		dma_addr_t iova;

		if (dma->iommu_mapped) {
			iommu_unmap(domain->domain, dma->iova, dma->size);
			continue;
		}

		iova = dma->iova;
		while (iova < dma->iova + dma->size) {
			phys_addr_t phys, p;
			size_t size;
			dma_addr_t i;

			phys = iommu_iova_to_phys(domain->domain, iova);
			if (!phys) {
				iova += PAGE_SIZE;
				continue;
			}

			size = PAGE_SIZE;
			p = phys + size;
			i = iova + size;
			while (i < dma->iova + dma->size &&
			       p == iommu_iova_to_phys(domain->domain, i)) {
				size += PAGE_SIZE;
				p += PAGE_SIZE;
				i += PAGE_SIZE;
			}

			iommu_unmap(domain->domain, iova, size);
			vfio_unpin_pages_remote(dma, iova, phys >> PAGE_SHIFT,
						size >> PAGE_SHIFT, true);
		}
	}

	return ret;
}

/*
 * We change our unmap behavior slightly depending on whether the IOMMU
 * supports fine-grained superpages.  IOMMUs like AMD-Vi will use a superpage
 * for practically any contiguous power-of-two mapping we give it.  This means
 * we don't need to look for contiguous chunks ourselves to make unmapping
 * more efficient.  On IOMMUs with coarse-grained super pages, like Intel VT-d
 * with discrete 2M/1G/512G/1T superpages, identifying contiguous chunks
 * significantly boosts non-hugetlbfs mappings and doesn't seem to hurt when
 * hugetlbfs is in use.
 */
static void vfio_test_domain_fgsp(struct vfio_domain *domain)
{
	struct page *pages;
	int ret, order = get_order(PAGE_SIZE * 2);

	pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!pages)
		return;

	ret = iommu_map(domain->domain, 0, page_to_phys(pages), PAGE_SIZE * 2,
			IOMMU_READ | IOMMU_WRITE | domain->prot);
	if (!ret) {
		size_t unmapped = iommu_unmap(domain->domain, 0, PAGE_SIZE);

		if (unmapped == PAGE_SIZE)
			iommu_unmap(domain->domain, PAGE_SIZE, PAGE_SIZE);
		else
			domain->fgsp = true;
	}

	__free_pages(pages, order);
}

static struct vfio_group *find_iommu_group(struct vfio_domain *domain,
					   struct iommu_group *iommu_group)
{
	struct vfio_group *g;

	list_for_each_entry(g, &domain->group_list, next) {
		if (g->iommu_group == iommu_group)
			return g;
	}

	return NULL;
}

static bool vfio_iommu_has_sw_msi(struct list_head *group_resv_regions,
				  phys_addr_t *base)
{
	struct iommu_resv_region *region;
	bool ret = false;

	list_for_each_entry(region, group_resv_regions, list) {
		/*
		 * The presence of any 'real' MSI regions should take
		 * precedence over the software-managed one if the
		 * IOMMU driver happens to advertise both types.
		 */
		if (region->type == IOMMU_RESV_MSI) {
			ret = false;
			break;
		}

		if (region->type == IOMMU_RESV_SW_MSI) {
			*base = region->start;
			ret = true;
		}
	}

	return ret;
}

static struct device *vfio_mdev_get_iommu_device(struct device *dev)
{
	struct device *(*fn)(struct device *dev);
	struct device *iommu_device;

	fn = symbol_get(mdev_get_iommu_device);
	if (fn) {
		iommu_device = fn(dev);
		symbol_put(mdev_get_iommu_device);

		return iommu_device;
	}

	return NULL;
}

static int vfio_mdev_attach_domain(struct device *dev, void *data)
{
	struct iommu_domain *domain = data;
	struct device *iommu_device;

	iommu_device = vfio_mdev_get_iommu_device(dev);
	if (iommu_device) {
		if (iommu_dev_feature_enabled(iommu_device, IOMMU_DEV_FEAT_AUX))
			return iommu_aux_attach_device(domain, iommu_device);
		else
			return iommu_attach_device(domain, iommu_device);
	}

	return -EINVAL;
}

static int vfio_mdev_detach_domain(struct device *dev, void *data)
{
	struct iommu_domain *domain = data;
	struct device *iommu_device;

	iommu_device = vfio_mdev_get_iommu_device(dev);
	if (iommu_device) {
		if (iommu_dev_feature_enabled(iommu_device, IOMMU_DEV_FEAT_AUX))
			iommu_aux_detach_device(domain, iommu_device);
		else
			iommu_detach_device(domain, iommu_device);
	}

	return 0;
}

static int vfio_iommu_attach_group(struct vfio_domain *domain,
				   struct vfio_group *group)
{
	if (group->mdev_group)
		return iommu_group_for_each_dev(group->iommu_group,
						domain->domain,
						vfio_mdev_attach_domain);
	else
		return iommu_attach_group(domain->domain, group->iommu_group);
}

static void vfio_iommu_detach_group(struct vfio_domain *domain,
				    struct vfio_group *group)
{
	if (group->mdev_group)
		iommu_group_for_each_dev(group->iommu_group, domain->domain,
					 vfio_mdev_detach_domain);
	else
		iommu_detach_group(domain->domain, group->iommu_group);
}

static bool vfio_bus_is_mdev(struct bus_type *bus)
{
	struct bus_type *mdev_bus;
	bool ret = false;

	mdev_bus = symbol_get(mdev_bus_type);
	if (mdev_bus) {
		ret = (bus == mdev_bus);
		symbol_put(mdev_bus_type);
	}

	return ret;
}

static int vfio_mdev_iommu_device(struct device *dev, void *data)
{
	struct device **old = data, *new;

	new = vfio_mdev_get_iommu_device(dev);
	if (!new || (*old && *old != new))
		return -EINVAL;

	*old = new;

	return 0;
}

/*
 * This is a helper function to insert an address range to iova list.
 * The list is initially created with a single entry corresponding to
 * the IOMMU domain geometry to which the device group is attached.
 * The list aperture gets modified when a new domain is added to the
 * container if the new aperture doesn't conflict with the current one
 * or with any existing dma mappings. The list is also modified to
 * exclude any reserved regions associated with the device group.
 */
static int vfio_iommu_iova_insert(struct list_head *head,
				  dma_addr_t start, dma_addr_t end)
{
	struct vfio_iova *region;

	region = kmalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	INIT_LIST_HEAD(&region->list);
	region->start = start;
	region->end = end;

	list_add_tail(&region->list, head);
	return 0;
}

/*
 * Check the new iommu aperture conflicts with existing aper or with any
 * existing dma mappings.
 */
static bool vfio_iommu_aper_conflict(struct vfio_iommu *iommu,
				     dma_addr_t start, dma_addr_t end)
{
	struct vfio_iova *first, *last;
	struct list_head *iova = &iommu->iova_list;

	if (list_empty(iova))
		return false;

	/* Disjoint sets, return conflict */
	first = list_first_entry(iova, struct vfio_iova, list);
	last = list_last_entry(iova, struct vfio_iova, list);
	if (start > last->end || end < first->start)
		return true;

	/* Check for any existing dma mappings below the new start */
	if (start > first->start) {
		if (vfio_find_dma(iommu, first->start, start - first->start))
			return true;
	}

	/* Check for any existing dma mappings beyond the new end */
	if (end < last->end) {
		if (vfio_find_dma(iommu, end + 1, last->end - end))
			return true;
	}

	return false;
}

/*
 * Resize iommu iova aperture window. This is called only if the new
 * aperture has no conflict with existing aperture and dma mappings.
 */
static int vfio_iommu_aper_resize(struct list_head *iova,
				  dma_addr_t start, dma_addr_t end)
{
	struct vfio_iova *node, *next;

	if (list_empty(iova))
		return vfio_iommu_iova_insert(iova, start, end);

	/* Adjust iova list start */
	list_for_each_entry_safe(node, next, iova, list) {
		if (start < node->start)
			break;
		if (start >= node->start && start < node->end) {
			node->start = start;
			break;
		}
		/* Delete nodes before new start */
		list_del(&node->list);
		kfree(node);
	}

	/* Adjust iova list end */
	list_for_each_entry_safe(node, next, iova, list) {
		if (end > node->end)
			continue;
		if (end > node->start && end <= node->end) {
			node->end = end;
			continue;
		}
		/* Delete nodes after new end */
		list_del(&node->list);
		kfree(node);
	}

	return 0;
}

/*
 * Check reserved region conflicts with existing dma mappings
 */
static bool vfio_iommu_resv_conflict(struct vfio_iommu *iommu,
				     struct list_head *resv_regions)
{
	struct iommu_resv_region *region;

	/* Check for conflict with existing dma mappings */
	list_for_each_entry(region, resv_regions, list) {
		if (region->type == IOMMU_RESV_DIRECT_RELAXABLE)
			continue;

		if (vfio_find_dma(iommu, region->start, region->length))
			return true;
	}

	return false;
}

/*
 * Check iova region overlap with  reserved regions and
 * exclude them from the iommu iova range
 */
static int vfio_iommu_resv_exclude(struct list_head *iova,
				   struct list_head *resv_regions)
{
	struct iommu_resv_region *resv;
	struct vfio_iova *n, *next;

	list_for_each_entry(resv, resv_regions, list) {
		phys_addr_t start, end;

		if (resv->type == IOMMU_RESV_DIRECT_RELAXABLE)
			continue;

		start = resv->start;
		end = resv->start + resv->length - 1;

		list_for_each_entry_safe(n, next, iova, list) {
			int ret = 0;

			/* No overlap */
			if (start > n->end || end < n->start)
				continue;
			/*
			 * Insert a new node if current node overlaps with the
			 * reserve region to exlude that from valid iova range.
			 * Note that, new node is inserted before the current
			 * node and finally the current node is deleted keeping
			 * the list updated and sorted.
			 */
			if (start > n->start)
				ret = vfio_iommu_iova_insert(&n->list, n->start,
							     start - 1);
			if (!ret && end < n->end)
				ret = vfio_iommu_iova_insert(&n->list, end + 1,
							     n->end);
			if (ret)
				return ret;

			list_del(&n->list);
			kfree(n);
		}
	}

	if (list_empty(iova))
		return -EINVAL;

	return 0;
}

static void vfio_iommu_resv_free(struct list_head *resv_regions)
{
	struct iommu_resv_region *n, *next;

	list_for_each_entry_safe(n, next, resv_regions, list) {
		list_del(&n->list);
		kfree(n);
	}
}

static void vfio_iommu_iova_free(struct list_head *iova)
{
	struct vfio_iova *n, *next;

	list_for_each_entry_safe(n, next, iova, list) {
		list_del(&n->list);
		kfree(n);
	}
}

static int vfio_iommu_iova_get_copy(struct vfio_iommu *iommu,
				    struct list_head *iova_copy)
{
	struct list_head *iova = &iommu->iova_list;
	struct vfio_iova *n;
	int ret;

	list_for_each_entry(n, iova, list) {
		ret = vfio_iommu_iova_insert(iova_copy, n->start, n->end);
		if (ret)
			goto out_free;
	}

	return 0;

out_free:
	vfio_iommu_iova_free(iova_copy);
	return ret;
}

static void vfio_iommu_iova_insert_copy(struct vfio_iommu *iommu,
					struct list_head *iova_copy)
{
	struct list_head *iova = &iommu->iova_list;

	vfio_iommu_iova_free(iova);

	list_splice_tail(iova_copy, iova);
}
static int vfio_iommu_type1_attach_group(void *iommu_data,
					 struct iommu_group *iommu_group)
{
	struct vfio_iommu *iommu = iommu_data;
	struct vfio_group *group;
	struct vfio_domain *domain, *d;
	struct bus_type *bus = NULL;
	int ret;
	bool resv_msi, msi_remap;
	phys_addr_t resv_msi_base = 0;
	struct iommu_domain_geometry geo;
	LIST_HEAD(iova_copy);
	LIST_HEAD(group_resv_regions);

	mutex_lock(&iommu->lock);

	list_for_each_entry(d, &iommu->domain_list, next) {
		if (find_iommu_group(d, iommu_group)) {
			mutex_unlock(&iommu->lock);
			return -EINVAL;
		}
	}

	if (iommu->external_domain) {
		if (find_iommu_group(iommu->external_domain, iommu_group)) {
			mutex_unlock(&iommu->lock);
			return -EINVAL;
		}
	}

	group = kzalloc(sizeof(*group), GFP_KERNEL);
	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!group || !domain) {
		ret = -ENOMEM;
		goto out_free;
	}

	group->iommu_group = iommu_group;

	/* Determine bus_type in order to allocate a domain */
	ret = iommu_group_for_each_dev(iommu_group, &bus, vfio_bus_type);
	if (ret)
		goto out_free;

	if (vfio_bus_is_mdev(bus)) {
		struct device *iommu_device = NULL;

		group->mdev_group = true;

		/* Determine the isolation type */
		ret = iommu_group_for_each_dev(iommu_group, &iommu_device,
					       vfio_mdev_iommu_device);
		if (ret || !iommu_device) {
			if (!iommu->external_domain) {
				INIT_LIST_HEAD(&domain->group_list);
				iommu->external_domain = domain;
			} else {
				kfree(domain);
			}

			list_add(&group->next,
				 &iommu->external_domain->group_list);
			mutex_unlock(&iommu->lock);

			return 0;
		}

		bus = iommu_device->bus;
	}

	domain->domain = iommu_domain_alloc(bus);
	if (!domain->domain) {
		ret = -EIO;
		goto out_free;
	}

	if (iommu->nesting) {
		int attr = 1;

		ret = iommu_domain_set_attr(domain->domain, DOMAIN_ATTR_NESTING,
					    &attr);
		if (ret)
			goto out_domain;
	}

	ret = vfio_iommu_attach_group(domain, group);
	if (ret)
		goto out_domain;

	/* Get aperture info */
	iommu_domain_get_attr(domain->domain, DOMAIN_ATTR_GEOMETRY, &geo);

	if (vfio_iommu_aper_conflict(iommu, geo.aperture_start,
				     geo.aperture_end)) {
		ret = -EINVAL;
		goto out_detach;
	}

	ret = iommu_get_group_resv_regions(iommu_group, &group_resv_regions);
	if (ret)
		goto out_detach;

	if (vfio_iommu_resv_conflict(iommu, &group_resv_regions)) {
		ret = -EINVAL;
		goto out_detach;
	}

	/*
	 * We don't want to work on the original iova list as the list
	 * gets modified and in case of failure we have to retain the
	 * original list. Get a copy here.
	 */
	ret = vfio_iommu_iova_get_copy(iommu, &iova_copy);
	if (ret)
		goto out_detach;

	ret = vfio_iommu_aper_resize(&iova_copy, geo.aperture_start,
				     geo.aperture_end);
	if (ret)
		goto out_detach;

	ret = vfio_iommu_resv_exclude(&iova_copy, &group_resv_regions);
	if (ret)
		goto out_detach;

	resv_msi = vfio_iommu_has_sw_msi(&group_resv_regions, &resv_msi_base);

	INIT_LIST_HEAD(&domain->group_list);
	list_add(&group->next, &domain->group_list);

	msi_remap = irq_domain_check_msi_remap() ||
		    iommu_capable(bus, IOMMU_CAP_INTR_REMAP);

	if (!allow_unsafe_interrupts && !msi_remap) {
		pr_warn("%s: No interrupt remapping support.  Use the module param \"allow_unsafe_interrupts\" to enable VFIO IOMMU support on this platform\n",
		       __func__);
		ret = -EPERM;
		goto out_detach;
	}

	if (iommu_capable(bus, IOMMU_CAP_CACHE_COHERENCY))
		domain->prot |= IOMMU_CACHE;

	/*
	 * Try to match an existing compatible domain.  We don't want to
	 * preclude an IOMMU driver supporting multiple bus_types and being
	 * able to include different bus_types in the same IOMMU domain, so
	 * we test whether the domains use the same iommu_ops rather than
	 * testing if they're on the same bus_type.
	 */
	list_for_each_entry(d, &iommu->domain_list, next) {
		if (d->domain->ops == domain->domain->ops &&
		    d->prot == domain->prot) {
			vfio_iommu_detach_group(domain, group);
			if (!vfio_iommu_attach_group(d, group)) {
				list_add(&group->next, &d->group_list);
				iommu_domain_free(domain->domain);
				kfree(domain);
				goto done;
			}

			ret = vfio_iommu_attach_group(domain, group);
			if (ret)
				goto out_domain;
		}
	}

	vfio_test_domain_fgsp(domain);

	/* replay mappings on new domains */
	ret = vfio_iommu_replay(iommu, domain);
	if (ret)
		goto out_detach;

	if (resv_msi) {
		ret = iommu_get_msi_cookie(domain->domain, resv_msi_base);
		if (ret)
			goto out_detach;
	}

	list_add(&domain->next, &iommu->domain_list);
done:
	/* Delete the old one and insert new iova list */
	vfio_iommu_iova_insert_copy(iommu, &iova_copy);
	mutex_unlock(&iommu->lock);
	vfio_iommu_resv_free(&group_resv_regions);

	return 0;

out_detach:
	vfio_iommu_detach_group(domain, group);
out_domain:
	iommu_domain_free(domain->domain);
	vfio_iommu_iova_free(&iova_copy);
	vfio_iommu_resv_free(&group_resv_regions);
out_free:
	kfree(domain);
	kfree(group);
	mutex_unlock(&iommu->lock);
	return ret;
}

static void vfio_iommu_unmap_unpin_all(struct vfio_iommu *iommu)
{
	struct rb_node *node;

	while ((node = rb_first(&iommu->dma_list)))
		vfio_remove_dma(iommu, rb_entry(node, struct vfio_dma, node));
}

static void vfio_iommu_unmap_unpin_reaccount(struct vfio_iommu *iommu)
{
	struct rb_node *n, *p;

	n = rb_first(&iommu->dma_list);
	for (; n; n = rb_next(n)) {
		struct vfio_dma *dma;
		long locked = 0, unlocked = 0;

		dma = rb_entry(n, struct vfio_dma, node);
		unlocked += vfio_unmap_unpin(iommu, dma, false);
		p = rb_first(&dma->pfn_list);
		for (; p; p = rb_next(p)) {
			struct vfio_pfn *vpfn = rb_entry(p, struct vfio_pfn,
							 node);

			if (!is_invalid_reserved_pfn(vpfn->pfn))
				locked++;
		}
		vfio_lock_acct(dma, locked - unlocked, true);
	}
}

static void vfio_sanity_check_pfn_list(struct vfio_iommu *iommu)
{
	struct rb_node *n;

	n = rb_first(&iommu->dma_list);
	for (; n; n = rb_next(n)) {
		struct vfio_dma *dma;

		dma = rb_entry(n, struct vfio_dma, node);

		if (WARN_ON(!RB_EMPTY_ROOT(&dma->pfn_list)))
			break;
	}
	/* mdev vendor driver must unregister notifier */
	WARN_ON(iommu->notifier.head);
}

/*
 * Called when a domain is removed in detach. It is possible that
 * the removed domain decided the iova aperture window. Modify the
 * iova aperture with the smallest window among existing domains.
 */
static void vfio_iommu_aper_expand(struct vfio_iommu *iommu,
				   struct list_head *iova_copy)
{
	struct vfio_domain *domain;
	struct iommu_domain_geometry geo;
	struct vfio_iova *node;
	dma_addr_t start = 0;
	dma_addr_t end = (dma_addr_t)~0;

	if (list_empty(iova_copy))
		return;

	list_for_each_entry(domain, &iommu->domain_list, next) {
		iommu_domain_get_attr(domain->domain, DOMAIN_ATTR_GEOMETRY,
				      &geo);
		if (geo.aperture_start > start)
			start = geo.aperture_start;
		if (geo.aperture_end < end)
			end = geo.aperture_end;
	}

	/* Modify aperture limits. The new aper is either same or bigger */
	node = list_first_entry(iova_copy, struct vfio_iova, list);
	node->start = start;
	node = list_last_entry(iova_copy, struct vfio_iova, list);
	node->end = end;
}

/*
 * Called when a group is detached. The reserved regions for that
 * group can be part of valid iova now. But since reserved regions
 * may be duplicated among groups, populate the iova valid regions
 * list again.
 */
static int vfio_iommu_resv_refresh(struct vfio_iommu *iommu,
				   struct list_head *iova_copy)
{
	struct vfio_domain *d;
	struct vfio_group *g;
	struct vfio_iova *node;
	dma_addr_t start, end;
	LIST_HEAD(resv_regions);
	int ret;

	if (list_empty(iova_copy))
		return -EINVAL;

	list_for_each_entry(d, &iommu->domain_list, next) {
		list_for_each_entry(g, &d->group_list, next) {
			ret = iommu_get_group_resv_regions(g->iommu_group,
							   &resv_regions);
			if (ret)
				goto done;
		}
	}

	node = list_first_entry(iova_copy, struct vfio_iova, list);
	start = node->start;
	node = list_last_entry(iova_copy, struct vfio_iova, list);
	end = node->end;

	/* purge the iova list and create new one */
	vfio_iommu_iova_free(iova_copy);

	ret = vfio_iommu_aper_resize(iova_copy, start, end);
	if (ret)
		goto done;

	/* Exclude current reserved regions from iova ranges */
	ret = vfio_iommu_resv_exclude(iova_copy, &resv_regions);
done:
	vfio_iommu_resv_free(&resv_regions);
	return ret;
}

static void vfio_iommu_type1_detach_group(void *iommu_data,
					  struct iommu_group *iommu_group)
{
	struct vfio_iommu *iommu = iommu_data;
	struct vfio_domain *domain;
	struct vfio_group *group;
	LIST_HEAD(iova_copy);

	mutex_lock(&iommu->lock);

	if (iommu->external_domain) {
		group = find_iommu_group(iommu->external_domain, iommu_group);
		if (group) {
			list_del(&group->next);
			kfree(group);

			if (list_empty(&iommu->external_domain->group_list)) {
				vfio_sanity_check_pfn_list(iommu);

				if (!IS_IOMMU_CAP_DOMAIN_IN_CONTAINER(iommu))
					vfio_iommu_unmap_unpin_all(iommu);

				kfree(iommu->external_domain);
				iommu->external_domain = NULL;
			}
			goto detach_group_done;
		}
	}

	/*
	 * Get a copy of iova list. This will be used to update
	 * and to replace the current one later. Please note that
	 * we will leave the original list as it is if update fails.
	 */
	vfio_iommu_iova_get_copy(iommu, &iova_copy);

	list_for_each_entry(domain, &iommu->domain_list, next) {
		group = find_iommu_group(domain, iommu_group);
		if (!group)
			continue;

		vfio_iommu_detach_group(domain, group);
		list_del(&group->next);
		kfree(group);
		/*
		 * Group ownership provides privilege, if the group list is
		 * empty, the domain goes away. If it's the last domain with
		 * iommu and external domain doesn't exist, then all the
		 * mappings go away too. If it's the last domain with iommu and
		 * external domain exist, update accounting
		 */
		if (list_empty(&domain->group_list)) {
			if (list_is_singular(&iommu->domain_list)) {
				if (!iommu->external_domain)
					vfio_iommu_unmap_unpin_all(iommu);
				else
					vfio_iommu_unmap_unpin_reaccount(iommu);
			}
			iommu_domain_free(domain->domain);
			list_del(&domain->next);
			kfree(domain);
			vfio_iommu_aper_expand(iommu, &iova_copy);
		}
		break;
	}

	if (!vfio_iommu_resv_refresh(iommu, &iova_copy))
		vfio_iommu_iova_insert_copy(iommu, &iova_copy);
	else
		vfio_iommu_iova_free(&iova_copy);

detach_group_done:
	mutex_unlock(&iommu->lock);
}

static void *vfio_iommu_type1_open(unsigned long arg)
{
	struct vfio_iommu *iommu;

	iommu = kzalloc(sizeof(*iommu), GFP_KERNEL);
	if (!iommu)
		return ERR_PTR(-ENOMEM);

	switch (arg) {
	case VFIO_TYPE1_IOMMU:
		break;
	case VFIO_TYPE1_NESTING_IOMMU:
		iommu->nesting = true;
		/* fall through */
	case VFIO_TYPE1v2_IOMMU:
		iommu->v2 = true;
		break;
	default:
		kfree(iommu);
		return ERR_PTR(-EINVAL);
	}

	INIT_LIST_HEAD(&iommu->domain_list);
	INIT_LIST_HEAD(&iommu->iova_list);
	iommu->dma_list = RB_ROOT;
	iommu->dma_avail = dma_entry_limit;
	mutex_init(&iommu->lock);
	BLOCKING_INIT_NOTIFIER_HEAD(&iommu->notifier);

	return iommu;
}

static void vfio_release_domain(struct vfio_domain *domain, bool external)
{
	struct vfio_group *group, *group_tmp;

	list_for_each_entry_safe(group, group_tmp,
				 &domain->group_list, next) {
		if (!external)
			vfio_iommu_detach_group(domain, group);
		list_del(&group->next);
		kfree(group);
	}

	if (!external)
		iommu_domain_free(domain->domain);
}

static void vfio_iommu_type1_release(void *iommu_data)
{
	struct vfio_iommu *iommu = iommu_data;
	struct vfio_domain *domain, *domain_tmp;

	if (iommu->external_domain) {
		vfio_release_domain(iommu->external_domain, true);
		vfio_sanity_check_pfn_list(iommu);
		kfree(iommu->external_domain);
	}

	vfio_iommu_unmap_unpin_all(iommu);

	list_for_each_entry_safe(domain, domain_tmp,
				 &iommu->domain_list, next) {
		vfio_release_domain(domain, false);
		list_del(&domain->next);
		kfree(domain);
	}

	vfio_iommu_iova_free(&iommu->iova_list);

	kfree(iommu);
}

static int vfio_domains_have_iommu_cache(struct vfio_iommu *iommu)
{
	struct vfio_domain *domain;
	int ret = 1;

	mutex_lock(&iommu->lock);
	list_for_each_entry(domain, &iommu->domain_list, next) {
		if (!(domain->prot & IOMMU_CACHE)) {
			ret = 0;
			break;
		}
	}
	mutex_unlock(&iommu->lock);

	return ret;
}

static int vfio_iommu_iova_add_cap(struct vfio_info_cap *caps,
		 struct vfio_iommu_type1_info_cap_iova_range *cap_iovas,
		 size_t size)
{
	struct vfio_info_cap_header *header;
	struct vfio_iommu_type1_info_cap_iova_range *iova_cap;

	header = vfio_info_cap_add(caps, size,
				   VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE, 1);
	if (IS_ERR(header))
		return PTR_ERR(header);

	iova_cap = container_of(header,
				struct vfio_iommu_type1_info_cap_iova_range,
				header);
	iova_cap->nr_iovas = cap_iovas->nr_iovas;
	memcpy(iova_cap->iova_ranges, cap_iovas->iova_ranges,
	       cap_iovas->nr_iovas * sizeof(*cap_iovas->iova_ranges));
	return 0;
}

static int vfio_iommu_iova_build_caps(struct vfio_iommu *iommu,
				      struct vfio_info_cap *caps)
{
	struct vfio_iommu_type1_info_cap_iova_range *cap_iovas;
	struct vfio_iova *iova;
	size_t size;
	int iovas = 0, i = 0, ret;

	mutex_lock(&iommu->lock);

	list_for_each_entry(iova, &iommu->iova_list, list)
		iovas++;

	if (!iovas) {
		/*
		 * Return 0 as a container with a single mdev device
		 * will have an empty list
		 */
		ret = 0;
		goto out_unlock;
	}

	size = sizeof(*cap_iovas) + (iovas * sizeof(*cap_iovas->iova_ranges));

	cap_iovas = kzalloc(size, GFP_KERNEL);
	if (!cap_iovas) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	cap_iovas->nr_iovas = iovas;

	list_for_each_entry(iova, &iommu->iova_list, list) {
		cap_iovas->iova_ranges[i].start = iova->start;
		cap_iovas->iova_ranges[i].end = iova->end;
		i++;
	}

	ret = vfio_iommu_iova_add_cap(caps, cap_iovas, size);

	kfree(cap_iovas);
out_unlock:
	mutex_unlock(&iommu->lock);
	return ret;
}

static long vfio_iommu_type1_ioctl(void *iommu_data,
				   unsigned int cmd, unsigned long arg)
{
	struct vfio_iommu *iommu = iommu_data;
	unsigned long minsz;

	if (cmd == VFIO_CHECK_EXTENSION) {
		switch (arg) {
		case VFIO_TYPE1_IOMMU:
		case VFIO_TYPE1v2_IOMMU:
		case VFIO_TYPE1_NESTING_IOMMU:
			return 1;
		case VFIO_DMA_CC_IOMMU:
			if (!iommu)
				return 0;
			return vfio_domains_have_iommu_cache(iommu);
		default:
			return 0;
		}
	} else if (cmd == VFIO_IOMMU_GET_INFO) {
		struct vfio_iommu_type1_info info;
		struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
		unsigned long capsz;
		int ret;

		minsz = offsetofend(struct vfio_iommu_type1_info, iova_pgsizes);

		/* For backward compatibility, cannot require this */
		capsz = offsetofend(struct vfio_iommu_type1_info, cap_offset);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		if (info.argsz >= capsz) {
			minsz = capsz;
			info.cap_offset = 0; /* output, no-recopy necessary */
		}

		info.flags = VFIO_IOMMU_INFO_PGSIZES;

		info.iova_pgsizes = vfio_pgsize_bitmap(iommu);

		ret = vfio_iommu_iova_build_caps(iommu, &caps);
		if (ret)
			return ret;

		if (caps.size) {
			info.flags |= VFIO_IOMMU_INFO_CAPS;

			if (info.argsz < sizeof(info) + caps.size) {
				info.argsz = sizeof(info) + caps.size;
			} else {
				vfio_info_cap_shift(&caps, sizeof(info));
				if (copy_to_user((void __user *)arg +
						sizeof(info), caps.buf,
						caps.size)) {
					kfree(caps.buf);
					return -EFAULT;
				}
				info.cap_offset = sizeof(info);
			}

			kfree(caps.buf);
		}

		return copy_to_user((void __user *)arg, &info, minsz) ?
			-EFAULT : 0;

	} else if (cmd == VFIO_IOMMU_MAP_DMA) {
		struct vfio_iommu_type1_dma_map map;
		uint32_t mask = VFIO_DMA_MAP_FLAG_READ |
				VFIO_DMA_MAP_FLAG_WRITE;

		minsz = offsetofend(struct vfio_iommu_type1_dma_map, size);

		if (copy_from_user(&map, (void __user *)arg, minsz))
			return -EFAULT;

		if (map.argsz < minsz || map.flags & ~mask)
			return -EINVAL;

		return vfio_dma_do_map(iommu, &map);

	} else if (cmd == VFIO_IOMMU_UNMAP_DMA) {
		struct vfio_iommu_type1_dma_unmap unmap;
		long ret;

		minsz = offsetofend(struct vfio_iommu_type1_dma_unmap, size);

		if (copy_from_user(&unmap, (void __user *)arg, minsz))
			return -EFAULT;

		if (unmap.argsz < minsz || unmap.flags)
			return -EINVAL;

		ret = vfio_dma_do_unmap(iommu, &unmap);
		if (ret)
			return ret;

		return copy_to_user((void __user *)arg, &unmap, minsz) ?
			-EFAULT : 0;
	}

	return -ENOTTY;
}

static int vfio_iommu_type1_register_notifier(void *iommu_data,
					      unsigned long *events,
					      struct notifier_block *nb)
{
	struct vfio_iommu *iommu = iommu_data;

	/* clear known events */
	*events &= ~VFIO_IOMMU_NOTIFY_DMA_UNMAP;

	/* refuse to register if still events remaining */
	if (*events)
		return -EINVAL;

	return blocking_notifier_chain_register(&iommu->notifier, nb);
}

static int vfio_iommu_type1_unregister_notifier(void *iommu_data,
						struct notifier_block *nb)
{
	struct vfio_iommu *iommu = iommu_data;

	return blocking_notifier_chain_unregister(&iommu->notifier, nb);
}

static const struct vfio_iommu_driver_ops vfio_iommu_driver_ops_type1 = {
	.name			= "vfio-iommu-type1",
	.owner			= THIS_MODULE,
	.open			= vfio_iommu_type1_open,
	.release		= vfio_iommu_type1_release,
	.ioctl			= vfio_iommu_type1_ioctl,
	.attach_group		= vfio_iommu_type1_attach_group,
	.detach_group		= vfio_iommu_type1_detach_group,
	.pin_pages		= vfio_iommu_type1_pin_pages,
	.unpin_pages		= vfio_iommu_type1_unpin_pages,
	.register_notifier	= vfio_iommu_type1_register_notifier,
	.unregister_notifier	= vfio_iommu_type1_unregister_notifier,
};

static int __init vfio_iommu_type1_init(void)
{
	return vfio_register_iommu_driver(&vfio_iommu_driver_ops_type1);
}

static void __exit vfio_iommu_type1_cleanup(void)
{
	vfio_unregister_iommu_driver(&vfio_iommu_driver_ops_type1);
}

module_init(vfio_iommu_type1_init);
module_exit(vfio_iommu_type1_cleanup);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
