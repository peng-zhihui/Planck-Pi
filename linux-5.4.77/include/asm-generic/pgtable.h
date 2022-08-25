/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_PGTABLE_H
#define _ASM_GENERIC_PGTABLE_H

#include <linux/pfn.h>

#ifndef __ASSEMBLY__
#ifdef CONFIG_MMU

#include <linux/mm_types.h>
#include <linux/bug.h>
#include <linux/errno.h>

#if 5 - defined(__PAGETABLE_P4D_FOLDED) - defined(__PAGETABLE_PUD_FOLDED) - \
	defined(__PAGETABLE_PMD_FOLDED) != CONFIG_PGTABLE_LEVELS
#error CONFIG_PGTABLE_LEVELS is not consistent with __PAGETABLE_{P4D,PUD,PMD}_FOLDED
#endif

/*
 * On almost all architectures and configurations, 0 can be used as the
 * upper ceiling to free_pgtables(): on many architectures it has the same
 * effect as using TASK_SIZE.  However, there is one configuration which
 * must impose a more careful limit, to avoid freeing kernel pgtables.
 */
#ifndef USER_PGTABLES_CEILING
#define USER_PGTABLES_CEILING	0UL
#endif

#ifndef __HAVE_ARCH_PTEP_SET_ACCESS_FLAGS
extern int ptep_set_access_flags(struct vm_area_struct *vma,
				 unsigned long address, pte_t *ptep,
				 pte_t entry, int dirty);
#endif

#ifndef __HAVE_ARCH_PMDP_SET_ACCESS_FLAGS
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
extern int pmdp_set_access_flags(struct vm_area_struct *vma,
				 unsigned long address, pmd_t *pmdp,
				 pmd_t entry, int dirty);
extern int pudp_set_access_flags(struct vm_area_struct *vma,
				 unsigned long address, pud_t *pudp,
				 pud_t entry, int dirty);
#else
static inline int pmdp_set_access_flags(struct vm_area_struct *vma,
					unsigned long address, pmd_t *pmdp,
					pmd_t entry, int dirty)
{
	BUILD_BUG();
	return 0;
}
static inline int pudp_set_access_flags(struct vm_area_struct *vma,
					unsigned long address, pud_t *pudp,
					pud_t entry, int dirty)
{
	BUILD_BUG();
	return 0;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif

#ifndef __HAVE_ARCH_PTEP_TEST_AND_CLEAR_YOUNG
static inline int ptep_test_and_clear_young(struct vm_area_struct *vma,
					    unsigned long address,
					    pte_t *ptep)
{
	pte_t pte = *ptep;
	int r = 1;
	if (!pte_young(pte))
		r = 0;
	else
		set_pte_at(vma->vm_mm, address, ptep, pte_mkold(pte));
	return r;
}
#endif

#ifndef __HAVE_ARCH_PMDP_TEST_AND_CLEAR_YOUNG
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline int pmdp_test_and_clear_young(struct vm_area_struct *vma,
					    unsigned long address,
					    pmd_t *pmdp)
{
	pmd_t pmd = *pmdp;
	int r = 1;
	if (!pmd_young(pmd))
		r = 0;
	else
		set_pmd_at(vma->vm_mm, address, pmdp, pmd_mkold(pmd));
	return r;
}
#else
static inline int pmdp_test_and_clear_young(struct vm_area_struct *vma,
					    unsigned long address,
					    pmd_t *pmdp)
{
	BUILD_BUG();
	return 0;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif

#ifndef __HAVE_ARCH_PTEP_CLEAR_YOUNG_FLUSH
int ptep_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pte_t *ptep);
#endif

#ifndef __HAVE_ARCH_PMDP_CLEAR_YOUNG_FLUSH
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
extern int pmdp_clear_flush_young(struct vm_area_struct *vma,
				  unsigned long address, pmd_t *pmdp);
#else
/*
 * Despite relevant to THP only, this API is called from generic rmap code
 * under PageTransHuge(), hence needs a dummy implementation for !THP
 */
static inline int pmdp_clear_flush_young(struct vm_area_struct *vma,
					 unsigned long address, pmd_t *pmdp)
{
	BUILD_BUG();
	return 0;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif

#ifndef __HAVE_ARCH_PTEP_GET_AND_CLEAR
static inline pte_t ptep_get_and_clear(struct mm_struct *mm,
				       unsigned long address,
				       pte_t *ptep)
{
	pte_t pte = *ptep;
	pte_clear(mm, address, ptep);
	return pte;
}
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#ifndef __HAVE_ARCH_PMDP_HUGE_GET_AND_CLEAR
static inline pmd_t pmdp_huge_get_and_clear(struct mm_struct *mm,
					    unsigned long address,
					    pmd_t *pmdp)
{
	pmd_t pmd = *pmdp;
	pmd_clear(pmdp);
	return pmd;
}
#endif /* __HAVE_ARCH_PMDP_HUGE_GET_AND_CLEAR */
#ifndef __HAVE_ARCH_PUDP_HUGE_GET_AND_CLEAR
static inline pud_t pudp_huge_get_and_clear(struct mm_struct *mm,
					    unsigned long address,
					    pud_t *pudp)
{
	pud_t pud = *pudp;

	pud_clear(pudp);
	return pud;
}
#endif /* __HAVE_ARCH_PUDP_HUGE_GET_AND_CLEAR */
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#ifndef __HAVE_ARCH_PMDP_HUGE_GET_AND_CLEAR_FULL
static inline pmd_t pmdp_huge_get_and_clear_full(struct mm_struct *mm,
					    unsigned long address, pmd_t *pmdp,
					    int full)
{
	return pmdp_huge_get_and_clear(mm, address, pmdp);
}
#endif

#ifndef __HAVE_ARCH_PUDP_HUGE_GET_AND_CLEAR_FULL
static inline pud_t pudp_huge_get_and_clear_full(struct mm_struct *mm,
					    unsigned long address, pud_t *pudp,
					    int full)
{
	return pudp_huge_get_and_clear(mm, address, pudp);
}
#endif
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifndef __HAVE_ARCH_PTEP_GET_AND_CLEAR_FULL
static inline pte_t ptep_get_and_clear_full(struct mm_struct *mm,
					    unsigned long address, pte_t *ptep,
					    int full)
{
	pte_t pte;
	pte = ptep_get_and_clear(mm, address, ptep);
	return pte;
}
#endif

/*
 * Some architectures may be able to avoid expensive synchronization
 * primitives when modifications are made to PTE's which are already
 * not present, or in the process of an address space destruction.
 */
#ifndef __HAVE_ARCH_PTE_CLEAR_NOT_PRESENT_FULL
static inline void pte_clear_not_present_full(struct mm_struct *mm,
					      unsigned long address,
					      pte_t *ptep,
					      int full)
{
	pte_clear(mm, address, ptep);
}
#endif

#ifndef __HAVE_ARCH_PTEP_CLEAR_FLUSH
extern pte_t ptep_clear_flush(struct vm_area_struct *vma,
			      unsigned long address,
			      pte_t *ptep);
#endif

#ifndef __HAVE_ARCH_PMDP_HUGE_CLEAR_FLUSH
extern pmd_t pmdp_huge_clear_flush(struct vm_area_struct *vma,
			      unsigned long address,
			      pmd_t *pmdp);
extern pud_t pudp_huge_clear_flush(struct vm_area_struct *vma,
			      unsigned long address,
			      pud_t *pudp);
#endif

#ifndef __HAVE_ARCH_PTEP_SET_WRPROTECT
struct mm_struct;
static inline void ptep_set_wrprotect(struct mm_struct *mm, unsigned long address, pte_t *ptep)
{
	pte_t old_pte = *ptep;
	set_pte_at(mm, address, ptep, pte_wrprotect(old_pte));
}
#endif

#ifndef pte_savedwrite
#define pte_savedwrite pte_write
#endif

#ifndef pte_mk_savedwrite
#define pte_mk_savedwrite pte_mkwrite
#endif

#ifndef pte_clear_savedwrite
#define pte_clear_savedwrite pte_wrprotect
#endif

#ifndef pmd_savedwrite
#define pmd_savedwrite pmd_write
#endif

#ifndef pmd_mk_savedwrite
#define pmd_mk_savedwrite pmd_mkwrite
#endif

#ifndef pmd_clear_savedwrite
#define pmd_clear_savedwrite pmd_wrprotect
#endif

#ifndef __HAVE_ARCH_PMDP_SET_WRPROTECT
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline void pmdp_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pmd_t *pmdp)
{
	pmd_t old_pmd = *pmdp;
	set_pmd_at(mm, address, pmdp, pmd_wrprotect(old_pmd));
}
#else
static inline void pmdp_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pmd_t *pmdp)
{
	BUILD_BUG();
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif
#ifndef __HAVE_ARCH_PUDP_SET_WRPROTECT
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
static inline void pudp_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pud_t *pudp)
{
	pud_t old_pud = *pudp;

	set_pud_at(mm, address, pudp, pud_wrprotect(old_pud));
}
#else
static inline void pudp_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pud_t *pudp)
{
	BUILD_BUG();
}
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#endif

#ifndef pmdp_collapse_flush
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
extern pmd_t pmdp_collapse_flush(struct vm_area_struct *vma,
				 unsigned long address, pmd_t *pmdp);
#else
static inline pmd_t pmdp_collapse_flush(struct vm_area_struct *vma,
					unsigned long address,
					pmd_t *pmdp)
{
	BUILD_BUG();
	return *pmdp;
}
#define pmdp_collapse_flush pmdp_collapse_flush
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif

#ifndef __HAVE_ARCH_PGTABLE_DEPOSIT
extern void pgtable_trans_huge_deposit(struct mm_struct *mm, pmd_t *pmdp,
				       pgtable_t pgtable);
#endif

#ifndef __HAVE_ARCH_PGTABLE_WITHDRAW
extern pgtable_t pgtable_trans_huge_withdraw(struct mm_struct *mm, pmd_t *pmdp);
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * This is an implementation of pmdp_establish() that is only suitable for an
 * architecture that doesn't have hardware dirty/accessed bits. In this case we
 * can't race with CPU which sets these bits and non-atomic aproach is fine.
 */
static inline pmd_t generic_pmdp_establish(struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmdp, pmd_t pmd)
{
	pmd_t old_pmd = *pmdp;
	set_pmd_at(vma->vm_mm, address, pmdp, pmd);
	return old_pmd;
}
#endif

#ifndef __HAVE_ARCH_PMDP_INVALIDATE
extern pmd_t pmdp_invalidate(struct vm_area_struct *vma, unsigned long address,
			    pmd_t *pmdp);
#endif

#ifndef __HAVE_ARCH_PTE_SAME
static inline int pte_same(pte_t pte_a, pte_t pte_b)
{
	return pte_val(pte_a) == pte_val(pte_b);
}
#endif

#ifndef __HAVE_ARCH_PTE_UNUSED
/*
 * Some architectures provide facilities to virtualization guests
 * so that they can flag allocated pages as unused. This allows the
 * host to transparently reclaim unused pages. This function returns
 * whether the pte's page is unused.
 */
static inline int pte_unused(pte_t pte)
{
	return 0;
}
#endif

#ifndef pte_access_permitted
#define pte_access_permitted(pte, write) \
	(pte_present(pte) && (!(write) || pte_write(pte)))
#endif

#ifndef pmd_access_permitted
#define pmd_access_permitted(pmd, write) \
	(pmd_present(pmd) && (!(write) || pmd_write(pmd)))
#endif

#ifndef pud_access_permitted
#define pud_access_permitted(pud, write) \
	(pud_present(pud) && (!(write) || pud_write(pud)))
#endif

#ifndef p4d_access_permitted
#define p4d_access_permitted(p4d, write) \
	(p4d_present(p4d) && (!(write) || p4d_write(p4d)))
#endif

#ifndef pgd_access_permitted
#define pgd_access_permitted(pgd, write) \
	(pgd_present(pgd) && (!(write) || pgd_write(pgd)))
#endif

#ifndef __HAVE_ARCH_PMD_SAME
static inline int pmd_same(pmd_t pmd_a, pmd_t pmd_b)
{
	return pmd_val(pmd_a) == pmd_val(pmd_b);
}

static inline int pud_same(pud_t pud_a, pud_t pud_b)
{
	return pud_val(pud_a) == pud_val(pud_b);
}
#endif

#ifndef __HAVE_ARCH_P4D_SAME
static inline int p4d_same(p4d_t p4d_a, p4d_t p4d_b)
{
	return p4d_val(p4d_a) == p4d_val(p4d_b);
}
#endif

#ifndef __HAVE_ARCH_PGD_SAME
static inline int pgd_same(pgd_t pgd_a, pgd_t pgd_b)
{
	return pgd_val(pgd_a) == pgd_val(pgd_b);
}
#endif

/*
 * Use set_p*_safe(), and elide TLB flushing, when confident that *no*
 * TLB flush will be required as a result of the "set". For example, use
 * in scenarios where it is known ahead of time that the routine is
 * setting non-present entries, or re-setting an existing entry to the
 * same value. Otherwise, use the typical "set" helpers and flush the
 * TLB.
 */
#define set_pte_safe(ptep, pte) \
({ \
	WARN_ON_ONCE(pte_present(*ptep) && !pte_same(*ptep, pte)); \
	set_pte(ptep, pte); \
})

#define set_pmd_safe(pmdp, pmd) \
({ \
	WARN_ON_ONCE(pmd_present(*pmdp) && !pmd_same(*pmdp, pmd)); \
	set_pmd(pmdp, pmd); \
})

#define set_pud_safe(pudp, pud) \
({ \
	WARN_ON_ONCE(pud_present(*pudp) && !pud_same(*pudp, pud)); \
	set_pud(pudp, pud); \
})

#define set_p4d_safe(p4dp, p4d) \
({ \
	WARN_ON_ONCE(p4d_present(*p4dp) && !p4d_same(*p4dp, p4d)); \
	set_p4d(p4dp, p4d); \
})

#define set_pgd_safe(pgdp, pgd) \
({ \
	WARN_ON_ONCE(pgd_present(*pgdp) && !pgd_same(*pgdp, pgd)); \
	set_pgd(pgdp, pgd); \
})

#ifndef __HAVE_ARCH_DO_SWAP_PAGE
/*
 * Some architectures support metadata associated with a page. When a
 * page is being swapped out, this metadata must be saved so it can be
 * restored when the page is swapped back in. SPARC M7 and newer
 * processors support an ADI (Application Data Integrity) tag for the
 * page as metadata for the page. arch_do_swap_page() can restore this
 * metadata when a page is swapped back in.
 */
static inline void arch_do_swap_page(struct mm_struct *mm,
				     struct vm_area_struct *vma,
				     unsigned long addr,
				     pte_t pte, pte_t oldpte)
{

}
#endif

#ifndef __HAVE_ARCH_UNMAP_ONE
/*
 * Some architectures support metadata associated with a page. When a
 * page is being swapped out, this metadata must be saved so it can be
 * restored when the page is swapped back in. SPARC M7 and newer
 * processors support an ADI (Application Data Integrity) tag for the
 * page as metadata for the page. arch_unmap_one() can save this
 * metadata on a swap-out of a page.
 */
static inline int arch_unmap_one(struct mm_struct *mm,
				  struct vm_area_struct *vma,
				  unsigned long addr,
				  pte_t orig_pte)
{
	return 0;
}
#endif

#ifndef __HAVE_ARCH_PGD_OFFSET_GATE
#define pgd_offset_gate(mm, addr)	pgd_offset(mm, addr)
#endif

#ifndef __HAVE_ARCH_MOVE_PTE
#define move_pte(pte, prot, old_addr, new_addr)	(pte)
#endif

#ifndef pte_accessible
# define pte_accessible(mm, pte)	((void)(pte), 1)
#endif

#ifndef flush_tlb_fix_spurious_fault
#define flush_tlb_fix_spurious_fault(vma, address) flush_tlb_page(vma, address)
#endif

#ifndef pgprot_noncached
#define pgprot_noncached(prot)	(prot)
#endif

#ifndef pgprot_writecombine
#define pgprot_writecombine pgprot_noncached
#endif

#ifndef pgprot_writethrough
#define pgprot_writethrough pgprot_noncached
#endif

#ifndef pgprot_device
#define pgprot_device pgprot_noncached
#endif

#ifndef pgprot_modify
#define pgprot_modify pgprot_modify
static inline pgprot_t pgprot_modify(pgprot_t oldprot, pgprot_t newprot)
{
	if (pgprot_val(oldprot) == pgprot_val(pgprot_noncached(oldprot)))
		newprot = pgprot_noncached(newprot);
	if (pgprot_val(oldprot) == pgprot_val(pgprot_writecombine(oldprot)))
		newprot = pgprot_writecombine(newprot);
	if (pgprot_val(oldprot) == pgprot_val(pgprot_device(oldprot)))
		newprot = pgprot_device(newprot);
	return newprot;
}
#endif

/*
 * When walking page tables, get the address of the next boundary,
 * or the end address of the range if that comes earlier.  Although no
 * vma end wraps to 0, rounded up __boundary may wrap to 0 throughout.
 */

#define pgd_addr_end(addr, end)						\
({	unsigned long __boundary = ((addr) + PGDIR_SIZE) & PGDIR_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})

#ifndef p4d_addr_end
#define p4d_addr_end(addr, end)						\
({	unsigned long __boundary = ((addr) + P4D_SIZE) & P4D_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})
#endif

#ifndef pud_addr_end
#define pud_addr_end(addr, end)						\
({	unsigned long __boundary = ((addr) + PUD_SIZE) & PUD_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})
#endif

#ifndef pmd_addr_end
#define pmd_addr_end(addr, end)						\
({	unsigned long __boundary = ((addr) + PMD_SIZE) & PMD_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})
#endif

/*
 * When walking page tables, we usually want to skip any p?d_none entries;
 * and any p?d_bad entries - reporting the error before resetting to none.
 * Do the tests inline, but report and clear the bad entry in mm/memory.c.
 */
void pgd_clear_bad(pgd_t *);
void p4d_clear_bad(p4d_t *);
void pud_clear_bad(pud_t *);
void pmd_clear_bad(pmd_t *);

static inline int pgd_none_or_clear_bad(pgd_t *pgd)
{
	if (pgd_none(*pgd))
		return 1;
	if (unlikely(pgd_bad(*pgd))) {
		pgd_clear_bad(pgd);
		return 1;
	}
	return 0;
}

static inline int p4d_none_or_clear_bad(p4d_t *p4d)
{
	if (p4d_none(*p4d))
		return 1;
	if (unlikely(p4d_bad(*p4d))) {
		p4d_clear_bad(p4d);
		return 1;
	}
	return 0;
}

static inline int pud_none_or_clear_bad(pud_t *pud)
{
	if (pud_none(*pud))
		return 1;
	if (unlikely(pud_bad(*pud))) {
		pud_clear_bad(pud);
		return 1;
	}
	return 0;
}

static inline int pmd_none_or_clear_bad(pmd_t *pmd)
{
	if (pmd_none(*pmd))
		return 1;
	if (unlikely(pmd_bad(*pmd))) {
		pmd_clear_bad(pmd);
		return 1;
	}
	return 0;
}

static inline pte_t __ptep_modify_prot_start(struct vm_area_struct *vma,
					     unsigned long addr,
					     pte_t *ptep)
{
	/*
	 * Get the current pte state, but zero it out to make it
	 * non-present, preventing the hardware from asynchronously
	 * updating it.
	 */
	return ptep_get_and_clear(vma->vm_mm, addr, ptep);
}

static inline void __ptep_modify_prot_commit(struct vm_area_struct *vma,
					     unsigned long addr,
					     pte_t *ptep, pte_t pte)
{
	/*
	 * The pte is non-present, so there's no hardware state to
	 * preserve.
	 */
	set_pte_at(vma->vm_mm, addr, ptep, pte);
}

#ifndef __HAVE_ARCH_PTEP_MODIFY_PROT_TRANSACTION
/*
 * Start a pte protection read-modify-write transaction, which
 * protects against asynchronous hardware modifications to the pte.
 * The intention is not to prevent the hardware from making pte
 * updates, but to prevent any updates it may make from being lost.
 *
 * This does not protect against other software modifications of the
 * pte; the appropriate pte lock must be held over the transation.
 *
 * Note that this interface is intended to be batchable, meaning that
 * ptep_modify_prot_commit may not actually update the pte, but merely
 * queue the update to be done at some later time.  The update must be
 * actually committed before the pte lock is released, however.
 */
static inline pte_t ptep_modify_prot_start(struct vm_area_struct *vma,
					   unsigned long addr,
					   pte_t *ptep)
{
	return __ptep_modify_prot_start(vma, addr, ptep);
}

/*
 * Commit an update to a pte, leaving any hardware-controlled bits in
 * the PTE unmodified.
 */
static inline void ptep_modify_prot_commit(struct vm_area_struct *vma,
					   unsigned long addr,
					   pte_t *ptep, pte_t old_pte, pte_t pte)
{
	__ptep_modify_prot_commit(vma, addr, ptep, pte);
}
#endif /* __HAVE_ARCH_PTEP_MODIFY_PROT_TRANSACTION */
#endif /* CONFIG_MMU */

/*
 * No-op macros that just return the current protection value. Defined here
 * because these macros can be used used even if CONFIG_MMU is not defined.
 */
#ifndef pgprot_encrypted
#define pgprot_encrypted(prot)	(prot)
#endif

#ifndef pgprot_decrypted
#define pgprot_decrypted(prot)	(prot)
#endif

/*
 * A facility to provide lazy MMU batching.  This allows PTE updates and
 * page invalidations to be delayed until a call to leave lazy MMU mode
 * is issued.  Some architectures may benefit from doing this, and it is
 * beneficial for both shadow and direct mode hypervisors, which may batch
 * the PTE updates which happen during this window.  Note that using this
 * interface requires that read hazards be removed from the code.  A read
 * hazard could result in the direct mode hypervisor case, since the actual
 * write to the page tables may not yet have taken place, so reads though
 * a raw PTE pointer after it has been modified are not guaranteed to be
 * up to date.  This mode can only be entered and left under the protection of
 * the page table locks for all page tables which may be modified.  In the UP
 * case, this is required so that preemption is disabled, and in the SMP case,
 * it must synchronize the delayed page table writes properly on other CPUs.
 */
#ifndef __HAVE_ARCH_ENTER_LAZY_MMU_MODE
#define arch_enter_lazy_mmu_mode()	do {} while (0)
#define arch_leave_lazy_mmu_mode()	do {} while (0)
#define arch_flush_lazy_mmu_mode()	do {} while (0)
#endif

/*
 * A facility to provide batching of the reload of page tables and
 * other process state with the actual context switch code for
 * paravirtualized guests.  By convention, only one of the batched
 * update (lazy) modes (CPU, MMU) should be active at any given time,
 * entry should never be nested, and entry and exits should always be
 * paired.  This is for sanity of maintaining and reasoning about the
 * kernel code.  In this case, the exit (end of the context switch) is
 * in architecture-specific code, and so doesn't need a generic
 * definition.
 */
#ifndef __HAVE_ARCH_START_CONTEXT_SWITCH
#define arch_start_context_switch(prev)	do {} while (0)
#endif

#ifdef CONFIG_HAVE_ARCH_SOFT_DIRTY
#ifndef CONFIG_ARCH_ENABLE_THP_MIGRATION
static inline pmd_t pmd_swp_mksoft_dirty(pmd_t pmd)
{
	return pmd;
}

static inline int pmd_swp_soft_dirty(pmd_t pmd)
{
	return 0;
}

static inline pmd_t pmd_swp_clear_soft_dirty(pmd_t pmd)
{
	return pmd;
}
#endif
#else /* !CONFIG_HAVE_ARCH_SOFT_DIRTY */
static inline int pte_soft_dirty(pte_t pte)
{
	return 0;
}

static inline int pmd_soft_dirty(pmd_t pmd)
{
	return 0;
}

static inline pte_t pte_mksoft_dirty(pte_t pte)
{
	return pte;
}

static inline pmd_t pmd_mksoft_dirty(pmd_t pmd)
{
	return pmd;
}

static inline pte_t pte_clear_soft_dirty(pte_t pte)
{
	return pte;
}

static inline pmd_t pmd_clear_soft_dirty(pmd_t pmd)
{
	return pmd;
}

static inline pte_t pte_swp_mksoft_dirty(pte_t pte)
{
	return pte;
}

static inline int pte_swp_soft_dirty(pte_t pte)
{
	return 0;
}

static inline pte_t pte_swp_clear_soft_dirty(pte_t pte)
{
	return pte;
}

static inline pmd_t pmd_swp_mksoft_dirty(pmd_t pmd)
{
	return pmd;
}

static inline int pmd_swp_soft_dirty(pmd_t pmd)
{
	return 0;
}

static inline pmd_t pmd_swp_clear_soft_dirty(pmd_t pmd)
{
	return pmd;
}
#endif

#ifndef __HAVE_PFNMAP_TRACKING
/*
 * Interfaces that can be used by architecture code to keep track of
 * memory type of pfn mappings specified by the remap_pfn_range,
 * vmf_insert_pfn.
 */

/*
 * track_pfn_remap is called when a _new_ pfn mapping is being established
 * by remap_pfn_range() for physical range indicated by pfn and size.
 */
static inline int track_pfn_remap(struct vm_area_struct *vma, pgprot_t *prot,
				  unsigned long pfn, unsigned long addr,
				  unsigned long size)
{
	return 0;
}

/*
 * track_pfn_insert is called when a _new_ single pfn is established
 * by vmf_insert_pfn().
 */
static inline void track_pfn_insert(struct vm_area_struct *vma, pgprot_t *prot,
				    pfn_t pfn)
{
}

/*
 * track_pfn_copy is called when vma that is covering the pfnmap gets
 * copied through copy_page_range().
 */
static inline int track_pfn_copy(struct vm_area_struct *vma)
{
	return 0;
}

/*
 * untrack_pfn is called while unmapping a pfnmap for a region.
 * untrack can be called for a specific region indicated by pfn and size or
 * can be for the entire vma (in which case pfn, size are zero).
 */
static inline void untrack_pfn(struct vm_area_struct *vma,
			       unsigned long pfn, unsigned long size)
{
}

/*
 * untrack_pfn_moved is called while mremapping a pfnmap for a new region.
 */
static inline void untrack_pfn_moved(struct vm_area_struct *vma)
{
}
#else
extern int track_pfn_remap(struct vm_area_struct *vma, pgprot_t *prot,
			   unsigned long pfn, unsigned long addr,
			   unsigned long size);
extern void track_pfn_insert(struct vm_area_struct *vma, pgprot_t *prot,
			     pfn_t pfn);
extern int track_pfn_copy(struct vm_area_struct *vma);
extern void untrack_pfn(struct vm_area_struct *vma, unsigned long pfn,
			unsigned long size);
extern void untrack_pfn_moved(struct vm_area_struct *vma);
#endif

#ifdef __HAVE_COLOR_ZERO_PAGE
static inline int is_zero_pfn(unsigned long pfn)
{
	extern unsigned long zero_pfn;
	unsigned long offset_from_zero_pfn = pfn - zero_pfn;
	return offset_from_zero_pfn <= (zero_page_mask >> PAGE_SHIFT);
}

#define my_zero_pfn(addr)	page_to_pfn(ZERO_PAGE(addr))

#else
static inline int is_zero_pfn(unsigned long pfn)
{
	extern unsigned long zero_pfn;
	return pfn == zero_pfn;
}

static inline unsigned long my_zero_pfn(unsigned long addr)
{
	extern unsigned long zero_pfn;
	return zero_pfn;
}
#endif

#ifdef CONFIG_MMU

#ifndef CONFIG_TRANSPARENT_HUGEPAGE
static inline int pmd_trans_huge(pmd_t pmd)
{
	return 0;
}
#ifndef pmd_write
static inline int pmd_write(pmd_t pmd)
{
	BUG();
	return 0;
}
#endif /* pmd_write */
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifndef pud_write
static inline int pud_write(pud_t pud)
{
	BUG();
	return 0;
}
#endif /* pud_write */

#if !defined(CONFIG_TRANSPARENT_HUGEPAGE) || \
	(defined(CONFIG_TRANSPARENT_HUGEPAGE) && \
	 !defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD))
static inline int pud_trans_huge(pud_t pud)
{
	return 0;
}
#endif

#ifndef pmd_read_atomic
static inline pmd_t pmd_read_atomic(pmd_t *pmdp)
{
	/*
	 * Depend on compiler for an atomic pmd read. NOTE: this is
	 * only going to work, if the pmdval_t isn't larger than
	 * an unsigned long.
	 */
	return *pmdp;
}
#endif

#ifndef arch_needs_pgtable_deposit
#define arch_needs_pgtable_deposit() (false)
#endif
/*
 * This function is meant to be used by sites walking pagetables with
 * the mmap_sem hold in read mode to protect against MADV_DONTNEED and
 * transhuge page faults. MADV_DONTNEED can convert a transhuge pmd
 * into a null pmd and the transhuge page fault can convert a null pmd
 * into an hugepmd or into a regular pmd (if the hugepage allocation
 * fails). While holding the mmap_sem in read mode the pmd becomes
 * stable and stops changing under us only if it's not null and not a
 * transhuge pmd. When those races occurs and this function makes a
 * difference vs the standard pmd_none_or_clear_bad, the result is
 * undefined so behaving like if the pmd was none is safe (because it
 * can return none anyway). The compiler level barrier() is critically
 * important to compute the two checks atomically on the same pmdval.
 *
 * For 32bit kernels with a 64bit large pmd_t this automatically takes
 * care of reading the pmd atomically to avoid SMP race conditions
 * against pmd_populate() when the mmap_sem is hold for reading by the
 * caller (a special atomic read not done by "gcc" as in the generic
 * version above, is also needed when THP is disabled because the page
 * fault can populate the pmd from under us).
 */
static inline int pmd_none_or_trans_huge_or_clear_bad(pmd_t *pmd)
{
	pmd_t pmdval = pmd_read_atomic(pmd);
	/*
	 * The barrier will stabilize the pmdval in a register or on
	 * the stack so that it will stop changing under the code.
	 *
	 * When CONFIG_TRANSPARENT_HUGEPAGE=y on x86 32bit PAE,
	 * pmd_read_atomic is allowed to return a not atomic pmdval
	 * (for example pointing to an hugepage that has never been
	 * mapped in the pmd). The below checks will only care about
	 * the low part of the pmd with 32bit PAE x86 anyway, with the
	 * exception of pmd_none(). So the important thing is that if
	 * the low part of the pmd is found null, the high part will
	 * be also null or the pmd_none() check below would be
	 * confused.
	 */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	barrier();
#endif
	/*
	 * !pmd_present() checks for pmd migration entries
	 *
	 * The complete check uses is_pmd_migration_entry() in linux/swapops.h
	 * But using that requires moving current function and pmd_trans_unstable()
	 * to linux/swapops.h to resovle dependency, which is too much code move.
	 *
	 * !pmd_present() is equivalent to is_pmd_migration_entry() currently,
	 * because !pmd_present() pages can only be under migration not swapped
	 * out.
	 *
	 * pmd_none() is preseved for future condition checks on pmd migration
	 * entries and not confusing with this function name, although it is
	 * redundant with !pmd_present().
	 */
	if (pmd_none(pmdval) || pmd_trans_huge(pmdval) ||
		(IS_ENABLED(CONFIG_ARCH_ENABLE_THP_MIGRATION) && !pmd_present(pmdval)))
		return 1;
	if (unlikely(pmd_bad(pmdval))) {
		pmd_clear_bad(pmd);
		return 1;
	}
	return 0;
}

/*
 * This is a noop if Transparent Hugepage Support is not built into
 * the kernel. Otherwise it is equivalent to
 * pmd_none_or_trans_huge_or_clear_bad(), and shall only be called in
 * places that already verified the pmd is not none and they want to
 * walk ptes while holding the mmap sem in read mode (write mode don't
 * need this). If THP is not enabled, the pmd can't go away under the
 * code even if MADV_DONTNEED runs, but if THP is enabled we need to
 * run a pmd_trans_unstable before walking the ptes after
 * split_huge_pmd returns (because it may have run when the pmd become
 * null, but then a page fault can map in a THP and not a regular page).
 */
static inline int pmd_trans_unstable(pmd_t *pmd)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	return pmd_none_or_trans_huge_or_clear_bad(pmd);
#else
	return 0;
#endif
}

#ifndef CONFIG_NUMA_BALANCING
/*
 * Technically a PTE can be PROTNONE even when not doing NUMA balancing but
 * the only case the kernel cares is for NUMA balancing and is only ever set
 * when the VMA is accessible. For PROT_NONE VMAs, the PTEs are not marked
 * _PAGE_PROTNONE so by by default, implement the helper as "always no". It
 * is the responsibility of the caller to distinguish between PROT_NONE
 * protections and NUMA hinting fault protections.
 */
static inline int pte_protnone(pte_t pte)
{
	return 0;
}

static inline int pmd_protnone(pmd_t pmd)
{
	return 0;
}
#endif /* CONFIG_NUMA_BALANCING */

#endif /* CONFIG_MMU */

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP

#ifndef __PAGETABLE_P4D_FOLDED
int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot);
int p4d_clear_huge(p4d_t *p4d);
#else
static inline int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}
static inline int p4d_clear_huge(p4d_t *p4d)
{
	return 0;
}
#endif /* !__PAGETABLE_P4D_FOLDED */

int pud_set_huge(pud_t *pud, phys_addr_t addr, pgprot_t prot);
int pmd_set_huge(pmd_t *pmd, phys_addr_t addr, pgprot_t prot);
int pud_clear_huge(pud_t *pud);
int pmd_clear_huge(pmd_t *pmd);
int p4d_free_pud_page(p4d_t *p4d, unsigned long addr);
int pud_free_pmd_page(pud_t *pud, unsigned long addr);
int pmd_free_pte_page(pmd_t *pmd, unsigned long addr);
#else	/* !CONFIG_HAVE_ARCH_HUGE_VMAP */
static inline int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}
static inline int pud_set_huge(pud_t *pud, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}
static inline int pmd_set_huge(pmd_t *pmd, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}
static inline int p4d_clear_huge(p4d_t *p4d)
{
	return 0;
}
static inline int pud_clear_huge(pud_t *pud)
{
	return 0;
}
static inline int pmd_clear_huge(pmd_t *pmd)
{
	return 0;
}
static inline int p4d_free_pud_page(p4d_t *p4d, unsigned long addr)
{
	return 0;
}
static inline int pud_free_pmd_page(pud_t *pud, unsigned long addr)
{
	return 0;
}
static inline int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)
{
	return 0;
}
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMAP */

#ifndef __HAVE_ARCH_FLUSH_PMD_TLB_RANGE
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * ARCHes with special requirements for evicting THP backing TLB entries can
 * implement this. Otherwise also, it can help optimize normal TLB flush in
 * THP regime. stock flush_tlb_range() typically has optimization to nuke the
 * entire TLB TLB if flush span is greater than a threshold, which will
 * likely be true for a single huge page. Thus a single thp flush will
 * invalidate the entire TLB which is not desitable.
 * e.g. see arch/arc: flush_pmd_tlb_range
 */
#define flush_pmd_tlb_range(vma, addr, end)	flush_tlb_range(vma, addr, end)
#define flush_pud_tlb_range(vma, addr, end)	flush_tlb_range(vma, addr, end)
#else
#define flush_pmd_tlb_range(vma, addr, end)	BUILD_BUG()
#define flush_pud_tlb_range(vma, addr, end)	BUILD_BUG()
#endif
#endif

struct file;
int phys_mem_access_prot_allowed(struct file *file, unsigned long pfn,
			unsigned long size, pgprot_t *vma_prot);

#ifndef CONFIG_X86_ESPFIX64
static inline void init_espfix_bsp(void) { }
#endif

extern void __init pgtable_cache_init(void);

#ifndef __HAVE_ARCH_PFN_MODIFY_ALLOWED
static inline bool pfn_modify_allowed(unsigned long pfn, pgprot_t prot)
{
	return true;
}

static inline bool arch_has_pfn_modify_check(void)
{
	return false;
}
#endif /* !_HAVE_ARCH_PFN_MODIFY_ALLOWED */

/*
 * Architecture PAGE_KERNEL_* fallbacks
 *
 * Some architectures don't define certain PAGE_KERNEL_* flags. This is either
 * because they really don't support them, or the port needs to be updated to
 * reflect the required functionality. Below are a set of relatively safe
 * fallbacks, as best effort, which we can count on in lieu of the architectures
 * not defining them on their own yet.
 */

#ifndef PAGE_KERNEL_RO
# define PAGE_KERNEL_RO PAGE_KERNEL
#endif

#ifndef PAGE_KERNEL_EXEC
# define PAGE_KERNEL_EXEC PAGE_KERNEL
#endif

#endif /* !__ASSEMBLY__ */

#ifndef has_transparent_hugepage
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define has_transparent_hugepage() 1
#else
#define has_transparent_hugepage() 0
#endif
#endif

#ifndef p4d_offset_lockless
#define p4d_offset_lockless(pgdp, pgd, address) p4d_offset(&(pgd), address)
#endif
#ifndef pud_offset_lockless
#define pud_offset_lockless(p4dp, p4d, address) pud_offset(&(p4d), address)
#endif
#ifndef pmd_offset_lockless
#define pmd_offset_lockless(pudp, pud, address) pmd_offset(&(pud), address)
#endif

/*
 * On some architectures it depends on the mm if the p4d/pud or pmd
 * layer of the page table hierarchy is folded or not.
 */
#ifndef mm_p4d_folded
#define mm_p4d_folded(mm)	__is_defined(__PAGETABLE_P4D_FOLDED)
#endif

#ifndef mm_pud_folded
#define mm_pud_folded(mm)	__is_defined(__PAGETABLE_PUD_FOLDED)
#endif

#ifndef mm_pmd_folded
#define mm_pmd_folded(mm)	__is_defined(__PAGETABLE_PMD_FOLDED)
#endif

#endif /* _ASM_GENERIC_PGTABLE_H */
