/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HUGE_MM_H
#define _LINUX_HUGE_MM_H

#include <linux/sched/coredump.h>
#include <linux/mm_types.h>

#include <linux/fs.h> /* only for vma_is_dax() */

extern vm_fault_t do_huge_pmd_anonymous_page(struct vm_fault *vmf);
extern int copy_huge_pmd(struct mm_struct *dst_mm, struct mm_struct *src_mm,
			 pmd_t *dst_pmd, pmd_t *src_pmd, unsigned long addr,
			 struct vm_area_struct *vma);
extern void huge_pmd_set_accessed(struct vm_fault *vmf, pmd_t orig_pmd);
extern int copy_huge_pud(struct mm_struct *dst_mm, struct mm_struct *src_mm,
			 pud_t *dst_pud, pud_t *src_pud, unsigned long addr,
			 struct vm_area_struct *vma);

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
extern void huge_pud_set_accessed(struct vm_fault *vmf, pud_t orig_pud);
#else
static inline void huge_pud_set_accessed(struct vm_fault *vmf, pud_t orig_pud)
{
}
#endif

extern vm_fault_t do_huge_pmd_wp_page(struct vm_fault *vmf, pmd_t orig_pmd);
extern struct page *follow_trans_huge_pmd(struct vm_area_struct *vma,
					  unsigned long addr,
					  pmd_t *pmd,
					  unsigned int flags);
extern bool madvise_free_huge_pmd(struct mmu_gather *tlb,
			struct vm_area_struct *vma,
			pmd_t *pmd, unsigned long addr, unsigned long next);
extern int zap_huge_pmd(struct mmu_gather *tlb,
			struct vm_area_struct *vma,
			pmd_t *pmd, unsigned long addr);
extern int zap_huge_pud(struct mmu_gather *tlb,
			struct vm_area_struct *vma,
			pud_t *pud, unsigned long addr);
extern int mincore_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
			unsigned long addr, unsigned long end,
			unsigned char *vec);
extern bool move_huge_pmd(struct vm_area_struct *vma, unsigned long old_addr,
			 unsigned long new_addr, unsigned long old_end,
			 pmd_t *old_pmd, pmd_t *new_pmd);
extern int change_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
			unsigned long addr, pgprot_t newprot,
			int prot_numa);
vm_fault_t vmf_insert_pfn_pmd(struct vm_fault *vmf, pfn_t pfn, bool write);
vm_fault_t vmf_insert_pfn_pud(struct vm_fault *vmf, pfn_t pfn, bool write);
enum transparent_hugepage_flag {
	TRANSPARENT_HUGEPAGE_FLAG,
	TRANSPARENT_HUGEPAGE_REQ_MADV_FLAG,
	TRANSPARENT_HUGEPAGE_DEFRAG_DIRECT_FLAG,
	TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_FLAG,
	TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_OR_MADV_FLAG,
	TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG,
	TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG,
	TRANSPARENT_HUGEPAGE_USE_ZERO_PAGE_FLAG,
#ifdef CONFIG_DEBUG_VM
	TRANSPARENT_HUGEPAGE_DEBUG_COW_FLAG,
#endif
};

struct kobject;
struct kobj_attribute;

extern ssize_t single_hugepage_flag_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count,
				 enum transparent_hugepage_flag flag);
extern ssize_t single_hugepage_flag_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf,
				enum transparent_hugepage_flag flag);
extern struct kobj_attribute shmem_enabled_attr;

#define HPAGE_PMD_ORDER (HPAGE_PMD_SHIFT-PAGE_SHIFT)
#define HPAGE_PMD_NR (1<<HPAGE_PMD_ORDER)

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define HPAGE_PMD_SHIFT PMD_SHIFT
#define HPAGE_PMD_SIZE	((1UL) << HPAGE_PMD_SHIFT)
#define HPAGE_PMD_MASK	(~(HPAGE_PMD_SIZE - 1))

#define HPAGE_PUD_SHIFT PUD_SHIFT
#define HPAGE_PUD_SIZE	((1UL) << HPAGE_PUD_SHIFT)
#define HPAGE_PUD_MASK	(~(HPAGE_PUD_SIZE - 1))

extern bool is_vma_temporary_stack(struct vm_area_struct *vma);

extern unsigned long transparent_hugepage_flags;

/*
 * to be used on vmas which are known to support THP.
 * Use transparent_hugepage_enabled otherwise
 */
static inline bool __transparent_hugepage_enabled(struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_NOHUGEPAGE)
		return false;

	if (is_vma_temporary_stack(vma))
		return false;

	if (test_bit(MMF_DISABLE_THP, &vma->vm_mm->flags))
		return false;

	if (transparent_hugepage_flags & (1 << TRANSPARENT_HUGEPAGE_FLAG))
		return true;
	/*
	 * For dax vmas, try to always use hugepage mappings. If the kernel does
	 * not support hugepages, fsdax mappings will fallback to PAGE_SIZE
	 * mappings, and device-dax namespaces, that try to guarantee a given
	 * mapping size, will fail to enable
	 */
	if (vma_is_dax(vma))
		return true;

	if (transparent_hugepage_flags &
				(1 << TRANSPARENT_HUGEPAGE_REQ_MADV_FLAG))
		return !!(vma->vm_flags & VM_HUGEPAGE);

	return false;
}

bool transparent_hugepage_enabled(struct vm_area_struct *vma);

#define HPAGE_CACHE_INDEX_MASK (HPAGE_PMD_NR - 1)

static inline bool transhuge_vma_suitable(struct vm_area_struct *vma,
		unsigned long haddr)
{
	/* Don't have to check pgoff for anonymous vma */
	if (!vma_is_anonymous(vma)) {
		if (((vma->vm_start >> PAGE_SHIFT) & HPAGE_CACHE_INDEX_MASK) !=
			(vma->vm_pgoff & HPAGE_CACHE_INDEX_MASK))
			return false;
	}

	if (haddr < vma->vm_start || haddr + HPAGE_PMD_SIZE > vma->vm_end)
		return false;
	return true;
}

#define transparent_hugepage_use_zero_page()				\
	(transparent_hugepage_flags &					\
	 (1<<TRANSPARENT_HUGEPAGE_USE_ZERO_PAGE_FLAG))
#ifdef CONFIG_DEBUG_VM
#define transparent_hugepage_debug_cow()				\
	(transparent_hugepage_flags &					\
	 (1<<TRANSPARENT_HUGEPAGE_DEBUG_COW_FLAG))
#else /* CONFIG_DEBUG_VM */
#define transparent_hugepage_debug_cow() 0
#endif /* CONFIG_DEBUG_VM */

extern unsigned long thp_get_unmapped_area(struct file *filp,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long flags);

extern void prep_transhuge_page(struct page *page);
extern void free_transhuge_page(struct page *page);

bool can_split_huge_page(struct page *page, int *pextra_pins);
int split_huge_page_to_list(struct page *page, struct list_head *list);
static inline int split_huge_page(struct page *page)
{
	return split_huge_page_to_list(page, NULL);
}
void deferred_split_huge_page(struct page *page);

void __split_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long address, bool freeze, struct page *page);

#define split_huge_pmd(__vma, __pmd, __address)				\
	do {								\
		pmd_t *____pmd = (__pmd);				\
		if (is_swap_pmd(*____pmd) || pmd_trans_huge(*____pmd)	\
					|| pmd_devmap(*____pmd))	\
			__split_huge_pmd(__vma, __pmd, __address,	\
						false, NULL);		\
	}  while (0)


void split_huge_pmd_address(struct vm_area_struct *vma, unsigned long address,
		bool freeze, struct page *page);

void __split_huge_pud(struct vm_area_struct *vma, pud_t *pud,
		unsigned long address);

#define split_huge_pud(__vma, __pud, __address)				\
	do {								\
		pud_t *____pud = (__pud);				\
		if (pud_trans_huge(*____pud)				\
					|| pud_devmap(*____pud))	\
			__split_huge_pud(__vma, __pud, __address);	\
	}  while (0)

extern int hugepage_madvise(struct vm_area_struct *vma,
			    unsigned long *vm_flags, int advice);
extern void vma_adjust_trans_huge(struct vm_area_struct *vma,
				    unsigned long start,
				    unsigned long end,
				    long adjust_next);
extern spinlock_t *__pmd_trans_huge_lock(pmd_t *pmd,
		struct vm_area_struct *vma);
extern spinlock_t *__pud_trans_huge_lock(pud_t *pud,
		struct vm_area_struct *vma);

static inline int is_swap_pmd(pmd_t pmd)
{
	return !pmd_none(pmd) && !pmd_present(pmd);
}

/* mmap_sem must be held on entry */
static inline spinlock_t *pmd_trans_huge_lock(pmd_t *pmd,
		struct vm_area_struct *vma)
{
	VM_BUG_ON_VMA(!rwsem_is_locked(&vma->vm_mm->mmap_sem), vma);
	if (is_swap_pmd(*pmd) || pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
		return __pmd_trans_huge_lock(pmd, vma);
	else
		return NULL;
}
static inline spinlock_t *pud_trans_huge_lock(pud_t *pud,
		struct vm_area_struct *vma)
{
	VM_BUG_ON_VMA(!rwsem_is_locked(&vma->vm_mm->mmap_sem), vma);
	if (pud_trans_huge(*pud) || pud_devmap(*pud))
		return __pud_trans_huge_lock(pud, vma);
	else
		return NULL;
}
static inline int hpage_nr_pages(struct page *page)
{
	if (unlikely(PageTransHuge(page)))
		return HPAGE_PMD_NR;
	return 1;
}

struct page *follow_devmap_pmd(struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, int flags, struct dev_pagemap **pgmap);
struct page *follow_devmap_pud(struct vm_area_struct *vma, unsigned long addr,
		pud_t *pud, int flags, struct dev_pagemap **pgmap);

extern vm_fault_t do_huge_pmd_numa_page(struct vm_fault *vmf, pmd_t orig_pmd);

extern struct page *huge_zero_page;

static inline bool is_huge_zero_page(struct page *page)
{
	return READ_ONCE(huge_zero_page) == page;
}

static inline bool is_huge_zero_pmd(pmd_t pmd)
{
	return is_huge_zero_page(pmd_page(pmd));
}

static inline bool is_huge_zero_pud(pud_t pud)
{
	return false;
}

struct page *mm_get_huge_zero_page(struct mm_struct *mm);
void mm_put_huge_zero_page(struct mm_struct *mm);

#define mk_huge_pmd(page, prot) pmd_mkhuge(mk_pmd(page, prot))

static inline bool thp_migration_supported(void)
{
	return IS_ENABLED(CONFIG_ARCH_ENABLE_THP_MIGRATION);
}

static inline struct list_head *page_deferred_list(struct page *page)
{
	/*
	 * Global or memcg deferred list in the second tail pages is
	 * occupied by compound_head.
	 */
	return &page[2].deferred_list;
}

#else /* CONFIG_TRANSPARENT_HUGEPAGE */
#define HPAGE_PMD_SHIFT ({ BUILD_BUG(); 0; })
#define HPAGE_PMD_MASK ({ BUILD_BUG(); 0; })
#define HPAGE_PMD_SIZE ({ BUILD_BUG(); 0; })

#define HPAGE_PUD_SHIFT ({ BUILD_BUG(); 0; })
#define HPAGE_PUD_MASK ({ BUILD_BUG(); 0; })
#define HPAGE_PUD_SIZE ({ BUILD_BUG(); 0; })

#define hpage_nr_pages(x) 1

static inline bool __transparent_hugepage_enabled(struct vm_area_struct *vma)
{
	return false;
}

static inline bool transparent_hugepage_enabled(struct vm_area_struct *vma)
{
	return false;
}

static inline bool transhuge_vma_suitable(struct vm_area_struct *vma,
		unsigned long haddr)
{
	return false;
}

static inline void prep_transhuge_page(struct page *page) {}

#define transparent_hugepage_flags 0UL

#define thp_get_unmapped_area	NULL

static inline bool
can_split_huge_page(struct page *page, int *pextra_pins)
{
	BUILD_BUG();
	return false;
}
static inline int
split_huge_page_to_list(struct page *page, struct list_head *list)
{
	return 0;
}
static inline int split_huge_page(struct page *page)
{
	return 0;
}
static inline void deferred_split_huge_page(struct page *page) {}
#define split_huge_pmd(__vma, __pmd, __address)	\
	do { } while (0)

static inline void __split_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long address, bool freeze, struct page *page) {}
static inline void split_huge_pmd_address(struct vm_area_struct *vma,
		unsigned long address, bool freeze, struct page *page) {}

#define split_huge_pud(__vma, __pmd, __address)	\
	do { } while (0)

static inline int hugepage_madvise(struct vm_area_struct *vma,
				   unsigned long *vm_flags, int advice)
{
	BUG();
	return 0;
}
static inline void vma_adjust_trans_huge(struct vm_area_struct *vma,
					 unsigned long start,
					 unsigned long end,
					 long adjust_next)
{
}
static inline int is_swap_pmd(pmd_t pmd)
{
	return 0;
}
static inline spinlock_t *pmd_trans_huge_lock(pmd_t *pmd,
		struct vm_area_struct *vma)
{
	return NULL;
}
static inline spinlock_t *pud_trans_huge_lock(pud_t *pud,
		struct vm_area_struct *vma)
{
	return NULL;
}

static inline vm_fault_t do_huge_pmd_numa_page(struct vm_fault *vmf,
		pmd_t orig_pmd)
{
	return 0;
}

static inline bool is_huge_zero_page(struct page *page)
{
	return false;
}

static inline bool is_huge_zero_pud(pud_t pud)
{
	return false;
}

static inline void mm_put_huge_zero_page(struct mm_struct *mm)
{
	return;
}

static inline struct page *follow_devmap_pmd(struct vm_area_struct *vma,
	unsigned long addr, pmd_t *pmd, int flags, struct dev_pagemap **pgmap)
{
	return NULL;
}

static inline struct page *follow_devmap_pud(struct vm_area_struct *vma,
	unsigned long addr, pud_t *pud, int flags, struct dev_pagemap **pgmap)
{
	return NULL;
}

static inline bool thp_migration_supported(void)
{
	return false;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#endif /* _LINUX_HUGE_MM_H */
