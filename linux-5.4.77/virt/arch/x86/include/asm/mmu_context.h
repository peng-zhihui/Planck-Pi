/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MMU_CONTEXT_H
#define _ASM_X86_MMU_CONTEXT_H

#include <asm/desc.h>
#include <linux/atomic.h>
#include <linux/mm_types.h>
#include <linux/pkeys.h>

#include <trace/events/tlb.h>

#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/paravirt.h>
#include <asm/mpx.h>
#include <asm/debugreg.h>

extern atomic64_t last_mm_ctx_id;

#ifndef CONFIG_PARAVIRT_XXL
static inline void paravirt_activate_mm(struct mm_struct *prev,
					struct mm_struct *next)
{
}
#endif	/* !CONFIG_PARAVIRT_XXL */

#ifdef CONFIG_PERF_EVENTS

DECLARE_STATIC_KEY_FALSE(rdpmc_always_available_key);

static inline void load_mm_cr4_irqsoff(struct mm_struct *mm)
{
	if (static_branch_unlikely(&rdpmc_always_available_key) ||
	    atomic_read(&mm->context.perf_rdpmc_allowed))
		cr4_set_bits_irqsoff(X86_CR4_PCE);
	else
		cr4_clear_bits_irqsoff(X86_CR4_PCE);
}
#else
static inline void load_mm_cr4_irqsoff(struct mm_struct *mm) {}
#endif

#ifdef CONFIG_MODIFY_LDT_SYSCALL
/*
 * ldt_structs can be allocated, used, and freed, but they are never
 * modified while live.
 */
struct ldt_struct {
	/*
	 * Xen requires page-aligned LDTs with special permissions.  This is
	 * needed to prevent us from installing evil descriptors such as
	 * call gates.  On native, we could merge the ldt_struct and LDT
	 * allocations, but it's not worth trying to optimize.
	 */
	struct desc_struct	*entries;
	unsigned int		nr_entries;

	/*
	 * If PTI is in use, then the entries array is not mapped while we're
	 * in user mode.  The whole array will be aliased at the addressed
	 * given by ldt_slot_va(slot).  We use two slots so that we can allocate
	 * and map, and enable a new LDT without invalidating the mapping
	 * of an older, still-in-use LDT.
	 *
	 * slot will be -1 if this LDT doesn't have an alias mapping.
	 */
	int			slot;
};

/* This is a multiple of PAGE_SIZE. */
#define LDT_SLOT_STRIDE (LDT_ENTRIES * LDT_ENTRY_SIZE)

static inline void *ldt_slot_va(int slot)
{
	return (void *)(LDT_BASE_ADDR + LDT_SLOT_STRIDE * slot);
}

/*
 * Used for LDT copy/destruction.
 */
static inline void init_new_context_ldt(struct mm_struct *mm)
{
	mm->context.ldt = NULL;
	init_rwsem(&mm->context.ldt_usr_sem);
}
int ldt_dup_context(struct mm_struct *oldmm, struct mm_struct *mm);
void destroy_context_ldt(struct mm_struct *mm);
void ldt_arch_exit_mmap(struct mm_struct *mm);
#else	/* CONFIG_MODIFY_LDT_SYSCALL */
static inline void init_new_context_ldt(struct mm_struct *mm) { }
static inline int ldt_dup_context(struct mm_struct *oldmm,
				  struct mm_struct *mm)
{
	return 0;
}
static inline void destroy_context_ldt(struct mm_struct *mm) { }
static inline void ldt_arch_exit_mmap(struct mm_struct *mm) { }
#endif

static inline void load_mm_ldt(struct mm_struct *mm)
{
#ifdef CONFIG_MODIFY_LDT_SYSCALL
	struct ldt_struct *ldt;

	/* READ_ONCE synchronizes with smp_store_release */
	ldt = READ_ONCE(mm->context.ldt);

	/*
	 * Any change to mm->context.ldt is followed by an IPI to all
	 * CPUs with the mm active.  The LDT will not be freed until
	 * after the IPI is handled by all such CPUs.  This means that,
	 * if the ldt_struct changes before we return, the values we see
	 * will be safe, and the new values will be loaded before we run
	 * any user code.
	 *
	 * NB: don't try to convert this to use RCU without extreme care.
	 * We would still need IRQs off, because we don't want to change
	 * the local LDT after an IPI loaded a newer value than the one
	 * that we can see.
	 */

	if (unlikely(ldt)) {
		if (static_cpu_has(X86_FEATURE_PTI)) {
			if (WARN_ON_ONCE((unsigned long)ldt->slot > 1)) {
				/*
				 * Whoops -- either the new LDT isn't mapped
				 * (if slot == -1) or is mapped into a bogus
				 * slot (if slot > 1).
				 */
				clear_LDT();
				return;
			}

			/*
			 * If page table isolation is enabled, ldt->entries
			 * will not be mapped in the userspace pagetables.
			 * Tell the CPU to access the LDT through the alias
			 * at ldt_slot_va(ldt->slot).
			 */
			set_ldt(ldt_slot_va(ldt->slot), ldt->nr_entries);
		} else {
			set_ldt(ldt->entries, ldt->nr_entries);
		}
	} else {
		clear_LDT();
	}
#else
	clear_LDT();
#endif
}

static inline void switch_ldt(struct mm_struct *prev, struct mm_struct *next)
{
#ifdef CONFIG_MODIFY_LDT_SYSCALL
	/*
	 * Load the LDT if either the old or new mm had an LDT.
	 *
	 * An mm will never go from having an LDT to not having an LDT.  Two
	 * mms never share an LDT, so we don't gain anything by checking to
	 * see whether the LDT changed.  There's also no guarantee that
	 * prev->context.ldt actually matches LDTR, but, if LDTR is non-NULL,
	 * then prev->context.ldt will also be non-NULL.
	 *
	 * If we really cared, we could optimize the case where prev == next
	 * and we're exiting lazy mode.  Most of the time, if this happens,
	 * we don't actually need to reload LDTR, but modify_ldt() is mostly
	 * used by legacy code and emulators where we don't need this level of
	 * performance.
	 *
	 * This uses | instead of || because it generates better code.
	 */
	if (unlikely((unsigned long)prev->context.ldt |
		     (unsigned long)next->context.ldt))
		load_mm_ldt(next);
#endif

	DEBUG_LOCKS_WARN_ON(preemptible());
}

void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk);

/*
 * Init a new mm.  Used on mm copies, like at fork()
 * and on mm's that are brand-new, like at execve().
 */
static inline int init_new_context(struct task_struct *tsk,
				   struct mm_struct *mm)
{
	mutex_init(&mm->context.lock);

	mm->context.ctx_id = atomic64_inc_return(&last_mm_ctx_id);
	atomic64_set(&mm->context.tlb_gen, 0);

#ifdef CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS
	if (cpu_feature_enabled(X86_FEATURE_OSPKE)) {
		/* pkey 0 is the default and allocated implicitly */
		mm->context.pkey_allocation_map = 0x1;
		/* -1 means unallocated or invalid */
		mm->context.execute_only_pkey = -1;
	}
#endif
	init_new_context_ldt(mm);
	return 0;
}
static inline void destroy_context(struct mm_struct *mm)
{
	destroy_context_ldt(mm);
}

extern void switch_mm(struct mm_struct *prev, struct mm_struct *next,
		      struct task_struct *tsk);

extern void switch_mm_irqs_off(struct mm_struct *prev, struct mm_struct *next,
			       struct task_struct *tsk);
#define switch_mm_irqs_off switch_mm_irqs_off

#define activate_mm(prev, next)			\
do {						\
	paravirt_activate_mm((prev), (next));	\
	switch_mm((prev), (next), NULL);	\
} while (0);

#ifdef CONFIG_X86_32
#define deactivate_mm(tsk, mm)			\
do {						\
	lazy_load_gs(0);			\
} while (0)
#else
#define deactivate_mm(tsk, mm)			\
do {						\
	load_gs_index(0);			\
	loadsegment(fs, 0);			\
} while (0)
#endif

static inline void arch_dup_pkeys(struct mm_struct *oldmm,
				  struct mm_struct *mm)
{
#ifdef CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS
	if (!cpu_feature_enabled(X86_FEATURE_OSPKE))
		return;

	/* Duplicate the oldmm pkey state in mm: */
	mm->context.pkey_allocation_map = oldmm->context.pkey_allocation_map;
	mm->context.execute_only_pkey   = oldmm->context.execute_only_pkey;
#endif
}

static inline int arch_dup_mmap(struct mm_struct *oldmm, struct mm_struct *mm)
{
	arch_dup_pkeys(oldmm, mm);
	paravirt_arch_dup_mmap(oldmm, mm);
	return ldt_dup_context(oldmm, mm);
}

static inline void arch_exit_mmap(struct mm_struct *mm)
{
	paravirt_arch_exit_mmap(mm);
	ldt_arch_exit_mmap(mm);
}

#ifdef CONFIG_X86_64
static inline bool is_64bit_mm(struct mm_struct *mm)
{
	return	!IS_ENABLED(CONFIG_IA32_EMULATION) ||
		!(mm->context.ia32_compat == TIF_IA32);
}
#else
static inline bool is_64bit_mm(struct mm_struct *mm)
{
	return false;
}
#endif

static inline void arch_bprm_mm_init(struct mm_struct *mm,
		struct vm_area_struct *vma)
{
	mpx_mm_init(mm);
}

static inline void arch_unmap(struct mm_struct *mm, unsigned long start,
			      unsigned long end)
{
	/*
	 * mpx_notify_unmap() goes and reads a rarely-hot
	 * cacheline in the mm_struct.  That can be expensive
	 * enough to be seen in profiles.
	 *
	 * The mpx_notify_unmap() call and its contents have been
	 * observed to affect munmap() performance on hardware
	 * where MPX is not present.
	 *
	 * The unlikely() optimizes for the fast case: no MPX
	 * in the CPU, or no MPX use in the process.  Even if
	 * we get this wrong (in the unlikely event that MPX
	 * is widely enabled on some system) the overhead of
	 * MPX itself (reading bounds tables) is expected to
	 * overwhelm the overhead of getting this unlikely()
	 * consistently wrong.
	 */
	if (unlikely(cpu_feature_enabled(X86_FEATURE_MPX)))
		mpx_notify_unmap(mm, start, end);
}

/*
 * We only want to enforce protection keys on the current process
 * because we effectively have no access to PKRU for other
 * processes or any way to tell *which * PKRU in a threaded
 * process we could use.
 *
 * So do not enforce things if the VMA is not from the current
 * mm, or if we are in a kernel thread.
 */
static inline bool vma_is_foreign(struct vm_area_struct *vma)
{
	if (!current->mm)
		return true;
	/*
	 * Should PKRU be enforced on the access to this VMA?  If
	 * the VMA is from another process, then PKRU has no
	 * relevance and should not be enforced.
	 */
	if (current->mm != vma->vm_mm)
		return true;

	return false;
}

static inline bool arch_vma_access_permitted(struct vm_area_struct *vma,
		bool write, bool execute, bool foreign)
{
	/* pkeys never affect instruction fetches */
	if (execute)
		return true;
	/* allow access if the VMA is not one from this process */
	if (foreign || vma_is_foreign(vma))
		return true;
	return __pkru_allows_pkey(vma_pkey(vma), write);
}

/*
 * This can be used from process context to figure out what the value of
 * CR3 is without needing to do a (slow) __read_cr3().
 *
 * It's intended to be used for code like KVM that sneakily changes CR3
 * and needs to restore it.  It needs to be used very carefully.
 */
static inline unsigned long __get_current_cr3_fast(void)
{
	unsigned long cr3 = build_cr3(this_cpu_read(cpu_tlbstate.loaded_mm)->pgd,
		this_cpu_read(cpu_tlbstate.loaded_mm_asid));

	/* For now, be very restrictive about when this can be called. */
	VM_WARN_ON(in_nmi() || preemptible());

	VM_BUG_ON(cr3 != __read_cr3());
	return cr3;
}

typedef struct {
	struct mm_struct *mm;
} temp_mm_state_t;

/*
 * Using a temporary mm allows to set temporary mappings that are not accessible
 * by other CPUs. Such mappings are needed to perform sensitive memory writes
 * that override the kernel memory protections (e.g., W^X), without exposing the
 * temporary page-table mappings that are required for these write operations to
 * other CPUs. Using a temporary mm also allows to avoid TLB shootdowns when the
 * mapping is torn down.
 *
 * Context: The temporary mm needs to be used exclusively by a single core. To
 *          harden security IRQs must be disabled while the temporary mm is
 *          loaded, thereby preventing interrupt handler bugs from overriding
 *          the kernel memory protection.
 */
static inline temp_mm_state_t use_temporary_mm(struct mm_struct *mm)
{
	temp_mm_state_t temp_state;

	lockdep_assert_irqs_disabled();
	temp_state.mm = this_cpu_read(cpu_tlbstate.loaded_mm);
	switch_mm_irqs_off(NULL, mm, current);

	/*
	 * If breakpoints are enabled, disable them while the temporary mm is
	 * used. Userspace might set up watchpoints on addresses that are used
	 * in the temporary mm, which would lead to wrong signals being sent or
	 * crashes.
	 *
	 * Note that breakpoints are not disabled selectively, which also causes
	 * kernel breakpoints (e.g., perf's) to be disabled. This might be
	 * undesirable, but still seems reasonable as the code that runs in the
	 * temporary mm should be short.
	 */
	if (hw_breakpoint_active())
		hw_breakpoint_disable();

	return temp_state;
}

static inline void unuse_temporary_mm(temp_mm_state_t prev_state)
{
	lockdep_assert_irqs_disabled();
	switch_mm_irqs_off(NULL, prev_state.mm, current);

	/*
	 * Restore the breakpoints if they were disabled before the temporary mm
	 * was loaded.
	 */
	if (hw_breakpoint_active())
		hw_breakpoint_restore();
}

#endif /* _ASM_X86_MMU_CONTEXT_H */
