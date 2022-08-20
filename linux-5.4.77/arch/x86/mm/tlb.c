// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>

#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/export.h>
#include <linux/cpu.h>
#include <linux/debugfs.h>

#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include <asm/nospec-branch.h>
#include <asm/cache.h>
#include <asm/apic.h>
#include <asm/uv/uv.h>

#include "mm_internal.h"

/*
 *	TLB flushing, formerly SMP-only
 *		c/o Linus Torvalds.
 *
 *	These mean you can really definitely utterly forget about
 *	writing to user space from interrupts. (Its not allowed anyway).
 *
 *	Optimizations Manfred Spraul <manfred@colorfullife.com>
 *
 *	More scalable flush, from Andi Kleen
 *
 *	Implement flush IPI by CALL_FUNCTION_VECTOR, Alex Shi
 */

/*
 * Use bit 0 to mangle the TIF_SPEC_IB state into the mm pointer which is
 * stored in cpu_tlb_state.last_user_mm_ibpb.
 */
#define LAST_USER_MM_IBPB	0x1UL

/*
 * We get here when we do something requiring a TLB invalidation
 * but could not go invalidate all of the contexts.  We do the
 * necessary invalidation by clearing out the 'ctx_id' which
 * forces a TLB flush when the context is loaded.
 */
static void clear_asid_other(void)
{
	u16 asid;

	/*
	 * This is only expected to be set if we have disabled
	 * kernel _PAGE_GLOBAL pages.
	 */
	if (!static_cpu_has(X86_FEATURE_PTI)) {
		WARN_ON_ONCE(1);
		return;
	}

	for (asid = 0; asid < TLB_NR_DYN_ASIDS; asid++) {
		/* Do not need to flush the current asid */
		if (asid == this_cpu_read(cpu_tlbstate.loaded_mm_asid))
			continue;
		/*
		 * Make sure the next time we go to switch to
		 * this asid, we do a flush:
		 */
		this_cpu_write(cpu_tlbstate.ctxs[asid].ctx_id, 0);
	}
	this_cpu_write(cpu_tlbstate.invalidate_other, false);
}

atomic64_t last_mm_ctx_id = ATOMIC64_INIT(1);


static void choose_new_asid(struct mm_struct *next, u64 next_tlb_gen,
			    u16 *new_asid, bool *need_flush)
{
	u16 asid;

	if (!static_cpu_has(X86_FEATURE_PCID)) {
		*new_asid = 0;
		*need_flush = true;
		return;
	}

	if (this_cpu_read(cpu_tlbstate.invalidate_other))
		clear_asid_other();

	for (asid = 0; asid < TLB_NR_DYN_ASIDS; asid++) {
		if (this_cpu_read(cpu_tlbstate.ctxs[asid].ctx_id) !=
		    next->context.ctx_id)
			continue;

		*new_asid = asid;
		*need_flush = (this_cpu_read(cpu_tlbstate.ctxs[asid].tlb_gen) <
			       next_tlb_gen);
		return;
	}

	/*
	 * We don't currently own an ASID slot on this CPU.
	 * Allocate a slot.
	 */
	*new_asid = this_cpu_add_return(cpu_tlbstate.next_asid, 1) - 1;
	if (*new_asid >= TLB_NR_DYN_ASIDS) {
		*new_asid = 0;
		this_cpu_write(cpu_tlbstate.next_asid, 1);
	}
	*need_flush = true;
}

static void load_new_mm_cr3(pgd_t *pgdir, u16 new_asid, bool need_flush)
{
	unsigned long new_mm_cr3;

	if (need_flush) {
		invalidate_user_asid(new_asid);
		new_mm_cr3 = build_cr3(pgdir, new_asid);
	} else {
		new_mm_cr3 = build_cr3_noflush(pgdir, new_asid);
	}

	/*
	 * Caution: many callers of this function expect
	 * that load_cr3() is serializing and orders TLB
	 * fills with respect to the mm_cpumask writes.
	 */
	write_cr3(new_mm_cr3);
}

void leave_mm(int cpu)
{
	struct mm_struct *loaded_mm = this_cpu_read(cpu_tlbstate.loaded_mm);

	/*
	 * It's plausible that we're in lazy TLB mode while our mm is init_mm.
	 * If so, our callers still expect us to flush the TLB, but there
	 * aren't any user TLB entries in init_mm to worry about.
	 *
	 * This needs to happen before any other sanity checks due to
	 * intel_idle's shenanigans.
	 */
	if (loaded_mm == &init_mm)
		return;

	/* Warn if we're not lazy. */
	WARN_ON(!this_cpu_read(cpu_tlbstate.is_lazy));

	switch_mm(NULL, &init_mm, NULL);
}
EXPORT_SYMBOL_GPL(leave_mm);

void switch_mm(struct mm_struct *prev, struct mm_struct *next,
	       struct task_struct *tsk)
{
	unsigned long flags;

	local_irq_save(flags);
	switch_mm_irqs_off(prev, next, tsk);
	local_irq_restore(flags);
}

static void sync_current_stack_to_mm(struct mm_struct *mm)
{
	unsigned long sp = current_stack_pointer;
	pgd_t *pgd = pgd_offset(mm, sp);

	if (pgtable_l5_enabled()) {
		if (unlikely(pgd_none(*pgd))) {
			pgd_t *pgd_ref = pgd_offset_k(sp);

			set_pgd(pgd, *pgd_ref);
		}
	} else {
		/*
		 * "pgd" is faked.  The top level entries are "p4d"s, so sync
		 * the p4d.  This compiles to approximately the same code as
		 * the 5-level case.
		 */
		p4d_t *p4d = p4d_offset(pgd, sp);

		if (unlikely(p4d_none(*p4d))) {
			pgd_t *pgd_ref = pgd_offset_k(sp);
			p4d_t *p4d_ref = p4d_offset(pgd_ref, sp);

			set_p4d(p4d, *p4d_ref);
		}
	}
}

static inline unsigned long mm_mangle_tif_spec_ib(struct task_struct *next)
{
	unsigned long next_tif = task_thread_info(next)->flags;
	unsigned long ibpb = (next_tif >> TIF_SPEC_IB) & LAST_USER_MM_IBPB;

	return (unsigned long)next->mm | ibpb;
}

static void cond_ibpb(struct task_struct *next)
{
	if (!next || !next->mm)
		return;

	/*
	 * Both, the conditional and the always IBPB mode use the mm
	 * pointer to avoid the IBPB when switching between tasks of the
	 * same process. Using the mm pointer instead of mm->context.ctx_id
	 * opens a hypothetical hole vs. mm_struct reuse, which is more or
	 * less impossible to control by an attacker. Aside of that it
	 * would only affect the first schedule so the theoretically
	 * exposed data is not really interesting.
	 */
	if (static_branch_likely(&switch_mm_cond_ibpb)) {
		unsigned long prev_mm, next_mm;

		/*
		 * This is a bit more complex than the always mode because
		 * it has to handle two cases:
		 *
		 * 1) Switch from a user space task (potential attacker)
		 *    which has TIF_SPEC_IB set to a user space task
		 *    (potential victim) which has TIF_SPEC_IB not set.
		 *
		 * 2) Switch from a user space task (potential attacker)
		 *    which has TIF_SPEC_IB not set to a user space task
		 *    (potential victim) which has TIF_SPEC_IB set.
		 *
		 * This could be done by unconditionally issuing IBPB when
		 * a task which has TIF_SPEC_IB set is either scheduled in
		 * or out. Though that results in two flushes when:
		 *
		 * - the same user space task is scheduled out and later
		 *   scheduled in again and only a kernel thread ran in
		 *   between.
		 *
		 * - a user space task belonging to the same process is
		 *   scheduled in after a kernel thread ran in between
		 *
		 * - a user space task belonging to the same process is
		 *   scheduled in immediately.
		 *
		 * Optimize this with reasonably small overhead for the
		 * above cases. Mangle the TIF_SPEC_IB bit into the mm
		 * pointer of the incoming task which is stored in
		 * cpu_tlbstate.last_user_mm_ibpb for comparison.
		 */
		next_mm = mm_mangle_tif_spec_ib(next);
		prev_mm = this_cpu_read(cpu_tlbstate.last_user_mm_ibpb);

		/*
		 * Issue IBPB only if the mm's are different and one or
		 * both have the IBPB bit set.
		 */
		if (next_mm != prev_mm &&
		    (next_mm | prev_mm) & LAST_USER_MM_IBPB)
			indirect_branch_prediction_barrier();

		this_cpu_write(cpu_tlbstate.last_user_mm_ibpb, next_mm);
	}

	if (static_branch_unlikely(&switch_mm_always_ibpb)) {
		/*
		 * Only flush when switching to a user space task with a
		 * different context than the user space task which ran
		 * last on this CPU.
		 */
		if (this_cpu_read(cpu_tlbstate.last_user_mm) != next->mm) {
			indirect_branch_prediction_barrier();
			this_cpu_write(cpu_tlbstate.last_user_mm, next->mm);
		}
	}
}

void switch_mm_irqs_off(struct mm_struct *prev, struct mm_struct *next,
			struct task_struct *tsk)
{
	struct mm_struct *real_prev = this_cpu_read(cpu_tlbstate.loaded_mm);
	u16 prev_asid = this_cpu_read(cpu_tlbstate.loaded_mm_asid);
	bool was_lazy = this_cpu_read(cpu_tlbstate.is_lazy);
	unsigned cpu = smp_processor_id();
	u64 next_tlb_gen;
	bool need_flush;
	u16 new_asid;

	/*
	 * NB: The scheduler will call us with prev == next when switching
	 * from lazy TLB mode to normal mode if active_mm isn't changing.
	 * When this happens, we don't assume that CR3 (and hence
	 * cpu_tlbstate.loaded_mm) matches next.
	 *
	 * NB: leave_mm() calls us with prev == NULL and tsk == NULL.
	 */

	/* We don't want flush_tlb_func_* to run concurrently with us. */
	if (IS_ENABLED(CONFIG_PROVE_LOCKING))
		WARN_ON_ONCE(!irqs_disabled());

	/*
	 * Verify that CR3 is what we think it is.  This will catch
	 * hypothetical buggy code that directly switches to swapper_pg_dir
	 * without going through leave_mm() / switch_mm_irqs_off() or that
	 * does something like write_cr3(read_cr3_pa()).
	 *
	 * Only do this check if CONFIG_DEBUG_VM=y because __read_cr3()
	 * isn't free.
	 */
#ifdef CONFIG_DEBUG_VM
	if (WARN_ON_ONCE(__read_cr3() != build_cr3(real_prev->pgd, prev_asid))) {
		/*
		 * If we were to BUG here, we'd be very likely to kill
		 * the system so hard that we don't see the call trace.
		 * Try to recover instead by ignoring the error and doing
		 * a global flush to minimize the chance of corruption.
		 *
		 * (This is far from being a fully correct recovery.
		 *  Architecturally, the CPU could prefetch something
		 *  back into an incorrect ASID slot and leave it there
		 *  to cause trouble down the road.  It's better than
		 *  nothing, though.)
		 */
		__flush_tlb_all();
	}
#endif
	this_cpu_write(cpu_tlbstate.is_lazy, false);

	/*
	 * The membarrier system call requires a full memory barrier and
	 * core serialization before returning to user-space, after
	 * storing to rq->curr. Writing to CR3 provides that full
	 * memory barrier and core serializing instruction.
	 */
	if (real_prev == next) {
		VM_WARN_ON(this_cpu_read(cpu_tlbstate.ctxs[prev_asid].ctx_id) !=
			   next->context.ctx_id);

		/*
		 * Even in lazy TLB mode, the CPU should stay set in the
		 * mm_cpumask. The TLB shootdown code can figure out from
		 * from cpu_tlbstate.is_lazy whether or not to send an IPI.
		 */
		if (WARN_ON_ONCE(real_prev != &init_mm &&
				 !cpumask_test_cpu(cpu, mm_cpumask(next))))
			cpumask_set_cpu(cpu, mm_cpumask(next));

		/*
		 * If the CPU is not in lazy TLB mode, we are just switching
		 * from one thread in a process to another thread in the same
		 * process. No TLB flush required.
		 */
		if (!was_lazy)
			return;

		/*
		 * Read the tlb_gen to check whether a flush is needed.
		 * If the TLB is up to date, just use it.
		 * The barrier synchronizes with the tlb_gen increment in
		 * the TLB shootdown code.
		 */
		smp_mb();
		next_tlb_gen = atomic64_read(&next->context.tlb_gen);
		if (this_cpu_read(cpu_tlbstate.ctxs[prev_asid].tlb_gen) ==
				next_tlb_gen)
			return;

		/*
		 * TLB contents went out of date while we were in lazy
		 * mode. Fall through to the TLB switching code below.
		 */
		new_asid = prev_asid;
		need_flush = true;
	} else {
		/*
		 * Avoid user/user BTB poisoning by flushing the branch
		 * predictor when switching between processes. This stops
		 * one process from doing Spectre-v2 attacks on another.
		 */
		cond_ibpb(tsk);

		if (IS_ENABLED(CONFIG_VMAP_STACK)) {
			/*
			 * If our current stack is in vmalloc space and isn't
			 * mapped in the new pgd, we'll double-fault.  Forcibly
			 * map it.
			 */
			sync_current_stack_to_mm(next);
		}

		/*
		 * Stop remote flushes for the previous mm.
		 * Skip kernel threads; we never send init_mm TLB flushing IPIs,
		 * but the bitmap manipulation can cause cache line contention.
		 */
		if (real_prev != &init_mm) {
			VM_WARN_ON_ONCE(!cpumask_test_cpu(cpu,
						mm_cpumask(real_prev)));
			cpumask_clear_cpu(cpu, mm_cpumask(real_prev));
		}

		/*
		 * Start remote flushes and then read tlb_gen.
		 */
		if (next != &init_mm)
			cpumask_set_cpu(cpu, mm_cpumask(next));
		next_tlb_gen = atomic64_read(&next->context.tlb_gen);

		choose_new_asid(next, next_tlb_gen, &new_asid, &need_flush);

		/* Let nmi_uaccess_okay() know that we're changing CR3. */
		this_cpu_write(cpu_tlbstate.loaded_mm, LOADED_MM_SWITCHING);
		barrier();
	}

	if (need_flush) {
		this_cpu_write(cpu_tlbstate.ctxs[new_asid].ctx_id, next->context.ctx_id);
		this_cpu_write(cpu_tlbstate.ctxs[new_asid].tlb_gen, next_tlb_gen);
		load_new_mm_cr3(next->pgd, new_asid, true);

		/*
		 * NB: This gets called via leave_mm() in the idle path
		 * where RCU functions differently.  Tracing normally
		 * uses RCU, so we need to use the _rcuidle variant.
		 *
		 * (There is no good reason for this.  The idle code should
		 *  be rearranged to call this before rcu_idle_enter().)
		 */
		trace_tlb_flush_rcuidle(TLB_FLUSH_ON_TASK_SWITCH, TLB_FLUSH_ALL);
	} else {
		/* The new ASID is already up to date. */
		load_new_mm_cr3(next->pgd, new_asid, false);

		/* See above wrt _rcuidle. */
		trace_tlb_flush_rcuidle(TLB_FLUSH_ON_TASK_SWITCH, 0);
	}

	/* Make sure we write CR3 before loaded_mm. */
	barrier();

	this_cpu_write(cpu_tlbstate.loaded_mm, next);
	this_cpu_write(cpu_tlbstate.loaded_mm_asid, new_asid);

	if (next != real_prev) {
		load_mm_cr4_irqsoff(next);
		switch_ldt(real_prev, next);
	}
}

/*
 * Please ignore the name of this function.  It should be called
 * switch_to_kernel_thread().
 *
 * enter_lazy_tlb() is a hint from the scheduler that we are entering a
 * kernel thread or other context without an mm.  Acceptable implementations
 * include doing nothing whatsoever, switching to init_mm, or various clever
 * lazy tricks to try to minimize TLB flushes.
 *
 * The scheduler reserves the right to call enter_lazy_tlb() several times
 * in a row.  It will notify us that we're going back to a real mm by
 * calling switch_mm_irqs_off().
 */
void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
	if (this_cpu_read(cpu_tlbstate.loaded_mm) == &init_mm)
		return;

	this_cpu_write(cpu_tlbstate.is_lazy, true);
}

/*
 * Call this when reinitializing a CPU.  It fixes the following potential
 * problems:
 *
 * - The ASID changed from what cpu_tlbstate thinks it is (most likely
 *   because the CPU was taken down and came back up with CR3's PCID
 *   bits clear.  CPU hotplug can do this.
 *
 * - The TLB contains junk in slots corresponding to inactive ASIDs.
 *
 * - The CPU went so far out to lunch that it may have missed a TLB
 *   flush.
 */
void initialize_tlbstate_and_flush(void)
{
	int i;
	struct mm_struct *mm = this_cpu_read(cpu_tlbstate.loaded_mm);
	u64 tlb_gen = atomic64_read(&init_mm.context.tlb_gen);
	unsigned long cr3 = __read_cr3();

	/* Assert that CR3 already references the right mm. */
	WARN_ON((cr3 & CR3_ADDR_MASK) != __pa(mm->pgd));

	/*
	 * Assert that CR4.PCIDE is set if needed.  (CR4.PCIDE initialization
	 * doesn't work like other CR4 bits because it can only be set from
	 * long mode.)
	 */
	WARN_ON(boot_cpu_has(X86_FEATURE_PCID) &&
		!(cr4_read_shadow() & X86_CR4_PCIDE));

	/* Force ASID 0 and force a TLB flush. */
	write_cr3(build_cr3(mm->pgd, 0));

	/* Reinitialize tlbstate. */
	this_cpu_write(cpu_tlbstate.last_user_mm_ibpb, LAST_USER_MM_IBPB);
	this_cpu_write(cpu_tlbstate.loaded_mm_asid, 0);
	this_cpu_write(cpu_tlbstate.next_asid, 1);
	this_cpu_write(cpu_tlbstate.ctxs[0].ctx_id, mm->context.ctx_id);
	this_cpu_write(cpu_tlbstate.ctxs[0].tlb_gen, tlb_gen);

	for (i = 1; i < TLB_NR_DYN_ASIDS; i++)
		this_cpu_write(cpu_tlbstate.ctxs[i].ctx_id, 0);
}

/*
 * flush_tlb_func_common()'s memory ordering requirement is that any
 * TLB fills that happen after we flush the TLB are ordered after we
 * read active_mm's tlb_gen.  We don't need any explicit barriers
 * because all x86 flush operations are serializing and the
 * atomic64_read operation won't be reordered by the compiler.
 */
static void flush_tlb_func_common(const struct flush_tlb_info *f,
				  bool local, enum tlb_flush_reason reason)
{
	/*
	 * We have three different tlb_gen values in here.  They are:
	 *
	 * - mm_tlb_gen:     the latest generation.
	 * - local_tlb_gen:  the generation that this CPU has already caught
	 *                   up to.
	 * - f->new_tlb_gen: the generation that the requester of the flush
	 *                   wants us to catch up to.
	 */
	struct mm_struct *loaded_mm = this_cpu_read(cpu_tlbstate.loaded_mm);
	u32 loaded_mm_asid = this_cpu_read(cpu_tlbstate.loaded_mm_asid);
	u64 mm_tlb_gen = atomic64_read(&loaded_mm->context.tlb_gen);
	u64 local_tlb_gen = this_cpu_read(cpu_tlbstate.ctxs[loaded_mm_asid].tlb_gen);

	/* This code cannot presently handle being reentered. */
	VM_WARN_ON(!irqs_disabled());

	if (unlikely(loaded_mm == &init_mm))
		return;

	VM_WARN_ON(this_cpu_read(cpu_tlbstate.ctxs[loaded_mm_asid].ctx_id) !=
		   loaded_mm->context.ctx_id);

	if (this_cpu_read(cpu_tlbstate.is_lazy)) {
		/*
		 * We're in lazy mode.  We need to at least flush our
		 * paging-structure cache to avoid speculatively reading
		 * garbage into our TLB.  Since switching to init_mm is barely
		 * slower than a minimal flush, just switch to init_mm.
		 *
		 * This should be rare, with native_flush_tlb_others skipping
		 * IPIs to lazy TLB mode CPUs.
		 */
		switch_mm_irqs_off(NULL, &init_mm, NULL);
		return;
	}

	if (unlikely(local_tlb_gen == mm_tlb_gen)) {
		/*
		 * There's nothing to do: we're already up to date.  This can
		 * happen if two concurrent flushes happen -- the first flush to
		 * be handled can catch us all the way up, leaving no work for
		 * the second flush.
		 */
		trace_tlb_flush(reason, 0);
		return;
	}

	WARN_ON_ONCE(local_tlb_gen > mm_tlb_gen);
	WARN_ON_ONCE(f->new_tlb_gen > mm_tlb_gen);

	/*
	 * If we get to this point, we know that our TLB is out of date.
	 * This does not strictly imply that we need to flush (it's
	 * possible that f->new_tlb_gen <= local_tlb_gen), but we're
	 * going to need to flush in the very near future, so we might
	 * as well get it over with.
	 *
	 * The only question is whether to do a full or partial flush.
	 *
	 * We do a partial flush if requested and two extra conditions
	 * are met:
	 *
	 * 1. f->new_tlb_gen == local_tlb_gen + 1.  We have an invariant that
	 *    we've always done all needed flushes to catch up to
	 *    local_tlb_gen.  If, for example, local_tlb_gen == 2 and
	 *    f->new_tlb_gen == 3, then we know that the flush needed to bring
	 *    us up to date for tlb_gen 3 is the partial flush we're
	 *    processing.
	 *
	 *    As an example of why this check is needed, suppose that there
	 *    are two concurrent flushes.  The first is a full flush that
	 *    changes context.tlb_gen from 1 to 2.  The second is a partial
	 *    flush that changes context.tlb_gen from 2 to 3.  If they get
	 *    processed on this CPU in reverse order, we'll see
	 *     local_tlb_gen == 1, mm_tlb_gen == 3, and end != TLB_FLUSH_ALL.
	 *    If we were to use __flush_tlb_one_user() and set local_tlb_gen to
	 *    3, we'd be break the invariant: we'd update local_tlb_gen above
	 *    1 without the full flush that's needed for tlb_gen 2.
	 *
	 * 2. f->new_tlb_gen == mm_tlb_gen.  This is purely an optimiation.
	 *    Partial TLB flushes are not all that much cheaper than full TLB
	 *    flushes, so it seems unlikely that it would be a performance win
	 *    to do a partial flush if that won't bring our TLB fully up to
	 *    date.  By doing a full flush instead, we can increase
	 *    local_tlb_gen all the way to mm_tlb_gen and we can probably
	 *    avoid another flush in the very near future.
	 */
	if (f->end != TLB_FLUSH_ALL &&
	    f->new_tlb_gen == local_tlb_gen + 1 &&
	    f->new_tlb_gen == mm_tlb_gen) {
		/* Partial flush */
		unsigned long nr_invalidate = (f->end - f->start) >> f->stride_shift;
		unsigned long addr = f->start;

		while (addr < f->end) {
			__flush_tlb_one_user(addr);
			addr += 1UL << f->stride_shift;
		}
		if (local)
			count_vm_tlb_events(NR_TLB_LOCAL_FLUSH_ONE, nr_invalidate);
		trace_tlb_flush(reason, nr_invalidate);
	} else {
		/* Full flush. */
		local_flush_tlb();
		if (local)
			count_vm_tlb_event(NR_TLB_LOCAL_FLUSH_ALL);
		trace_tlb_flush(reason, TLB_FLUSH_ALL);
	}

	/* Both paths above update our state to mm_tlb_gen. */
	this_cpu_write(cpu_tlbstate.ctxs[loaded_mm_asid].tlb_gen, mm_tlb_gen);
}

static void flush_tlb_func_local(const void *info, enum tlb_flush_reason reason)
{
	const struct flush_tlb_info *f = info;

	flush_tlb_func_common(f, true, reason);
}

static void flush_tlb_func_remote(void *info)
{
	const struct flush_tlb_info *f = info;

	inc_irq_stat(irq_tlb_count);

	if (f->mm && f->mm != this_cpu_read(cpu_tlbstate.loaded_mm))
		return;

	count_vm_tlb_event(NR_TLB_REMOTE_FLUSH_RECEIVED);
	flush_tlb_func_common(f, false, TLB_REMOTE_SHOOTDOWN);
}

static bool tlb_is_not_lazy(int cpu, void *data)
{
	return !per_cpu(cpu_tlbstate.is_lazy, cpu);
}

void native_flush_tlb_others(const struct cpumask *cpumask,
			     const struct flush_tlb_info *info)
{
	count_vm_tlb_event(NR_TLB_REMOTE_FLUSH);
	if (info->end == TLB_FLUSH_ALL)
		trace_tlb_flush(TLB_REMOTE_SEND_IPI, TLB_FLUSH_ALL);
	else
		trace_tlb_flush(TLB_REMOTE_SEND_IPI,
				(info->end - info->start) >> PAGE_SHIFT);

	if (is_uv_system()) {
		/*
		 * This whole special case is confused.  UV has a "Broadcast
		 * Assist Unit", which seems to be a fancy way to send IPIs.
		 * Back when x86 used an explicit TLB flush IPI, UV was
		 * optimized to use its own mechanism.  These days, x86 uses
		 * smp_call_function_many(), but UV still uses a manual IPI,
		 * and that IPI's action is out of date -- it does a manual
		 * flush instead of calling flush_tlb_func_remote().  This
		 * means that the percpu tlb_gen variables won't be updated
		 * and we'll do pointless flushes on future context switches.
		 *
		 * Rather than hooking native_flush_tlb_others() here, I think
		 * that UV should be updated so that smp_call_function_many(),
		 * etc, are optimal on UV.
		 */
		cpumask = uv_flush_tlb_others(cpumask, info);
		if (cpumask)
			smp_call_function_many(cpumask, flush_tlb_func_remote,
					       (void *)info, 1);
		return;
	}

	/*
	 * If no page tables were freed, we can skip sending IPIs to
	 * CPUs in lazy TLB mode. They will flush the CPU themselves
	 * at the next context switch.
	 *
	 * However, if page tables are getting freed, we need to send the
	 * IPI everywhere, to prevent CPUs in lazy TLB mode from tripping
	 * up on the new contents of what used to be page tables, while
	 * doing a speculative memory access.
	 */
	if (info->freed_tables)
		smp_call_function_many(cpumask, flush_tlb_func_remote,
			       (void *)info, 1);
	else
		on_each_cpu_cond_mask(tlb_is_not_lazy, flush_tlb_func_remote,
				(void *)info, 1, GFP_ATOMIC, cpumask);
}

/*
 * See Documentation/x86/tlb.rst for details.  We choose 33
 * because it is large enough to cover the vast majority (at
 * least 95%) of allocations, and is small enough that we are
 * confident it will not cause too much overhead.  Each single
 * flush is about 100 ns, so this caps the maximum overhead at
 * _about_ 3,000 ns.
 *
 * This is in units of pages.
 */
unsigned long tlb_single_page_flush_ceiling __read_mostly = 33;

static DEFINE_PER_CPU_SHARED_ALIGNED(struct flush_tlb_info, flush_tlb_info);

#ifdef CONFIG_DEBUG_VM
static DEFINE_PER_CPU(unsigned int, flush_tlb_info_idx);
#endif

static inline struct flush_tlb_info *get_flush_tlb_info(struct mm_struct *mm,
			unsigned long start, unsigned long end,
			unsigned int stride_shift, bool freed_tables,
			u64 new_tlb_gen)
{
	struct flush_tlb_info *info = this_cpu_ptr(&flush_tlb_info);

#ifdef CONFIG_DEBUG_VM
	/*
	 * Ensure that the following code is non-reentrant and flush_tlb_info
	 * is not overwritten. This means no TLB flushing is initiated by
	 * interrupt handlers and machine-check exception handlers.
	 */
	BUG_ON(this_cpu_inc_return(flush_tlb_info_idx) != 1);
#endif

	info->start		= start;
	info->end		= end;
	info->mm		= mm;
	info->stride_shift	= stride_shift;
	info->freed_tables	= freed_tables;
	info->new_tlb_gen	= new_tlb_gen;

	return info;
}

static inline void put_flush_tlb_info(void)
{
#ifdef CONFIG_DEBUG_VM
	/* Complete reentrency prevention checks */
	barrier();
	this_cpu_dec(flush_tlb_info_idx);
#endif
}

void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
				unsigned long end, unsigned int stride_shift,
				bool freed_tables)
{
	struct flush_tlb_info *info;
	u64 new_tlb_gen;
	int cpu;

	cpu = get_cpu();

	/* Should we flush just the requested range? */
	if ((end == TLB_FLUSH_ALL) ||
	    ((end - start) >> stride_shift) > tlb_single_page_flush_ceiling) {
		start = 0;
		end = TLB_FLUSH_ALL;
	}

	/* This is also a barrier that synchronizes with switch_mm(). */
	new_tlb_gen = inc_mm_tlb_gen(mm);

	info = get_flush_tlb_info(mm, start, end, stride_shift, freed_tables,
				  new_tlb_gen);

	if (mm == this_cpu_read(cpu_tlbstate.loaded_mm)) {
		lockdep_assert_irqs_enabled();
		local_irq_disable();
		flush_tlb_func_local(info, TLB_LOCAL_MM_SHOOTDOWN);
		local_irq_enable();
	}

	if (cpumask_any_but(mm_cpumask(mm), cpu) < nr_cpu_ids)
		flush_tlb_others(mm_cpumask(mm), info);

	put_flush_tlb_info();
	put_cpu();
}


static void do_flush_tlb_all(void *info)
{
	count_vm_tlb_event(NR_TLB_REMOTE_FLUSH_RECEIVED);
	__flush_tlb_all();
}

void flush_tlb_all(void)
{
	count_vm_tlb_event(NR_TLB_REMOTE_FLUSH);
	on_each_cpu(do_flush_tlb_all, NULL, 1);
}

static void do_kernel_range_flush(void *info)
{
	struct flush_tlb_info *f = info;
	unsigned long addr;

	/* flush range by one by one 'invlpg' */
	for (addr = f->start; addr < f->end; addr += PAGE_SIZE)
		__flush_tlb_one_kernel(addr);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	/* Balance as user space task's flush, a bit conservative */
	if (end == TLB_FLUSH_ALL ||
	    (end - start) > tlb_single_page_flush_ceiling << PAGE_SHIFT) {
		on_each_cpu(do_flush_tlb_all, NULL, 1);
	} else {
		struct flush_tlb_info *info;

		preempt_disable();
		info = get_flush_tlb_info(NULL, start, end, 0, false, 0);

		on_each_cpu(do_kernel_range_flush, info, 1);

		put_flush_tlb_info();
		preempt_enable();
	}
}

/*
 * arch_tlbbatch_flush() performs a full TLB flush regardless of the active mm.
 * This means that the 'struct flush_tlb_info' that describes which mappings to
 * flush is actually fixed. We therefore set a single fixed struct and use it in
 * arch_tlbbatch_flush().
 */
static const struct flush_tlb_info full_flush_tlb_info = {
	.mm = NULL,
	.start = 0,
	.end = TLB_FLUSH_ALL,
};

void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	int cpu = get_cpu();

	if (cpumask_test_cpu(cpu, &batch->cpumask)) {
		lockdep_assert_irqs_enabled();
		local_irq_disable();
		flush_tlb_func_local(&full_flush_tlb_info, TLB_LOCAL_SHOOTDOWN);
		local_irq_enable();
	}

	if (cpumask_any_but(&batch->cpumask, cpu) < nr_cpu_ids)
		flush_tlb_others(&batch->cpumask, &full_flush_tlb_info);

	cpumask_clear(&batch->cpumask);

	put_cpu();
}

static ssize_t tlbflush_read_file(struct file *file, char __user *user_buf,
			     size_t count, loff_t *ppos)
{
	char buf[32];
	unsigned int len;

	len = sprintf(buf, "%ld\n", tlb_single_page_flush_ceiling);
	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static ssize_t tlbflush_write_file(struct file *file,
		 const char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[32];
	ssize_t len;
	int ceiling;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, user_buf, len))
		return -EFAULT;

	buf[len] = '\0';
	if (kstrtoint(buf, 0, &ceiling))
		return -EINVAL;

	if (ceiling < 0)
		return -EINVAL;

	tlb_single_page_flush_ceiling = ceiling;
	return count;
}

static const struct file_operations fops_tlbflush = {
	.read = tlbflush_read_file,
	.write = tlbflush_write_file,
	.llseek = default_llseek,
};

static int __init create_tlb_single_page_flush_ceiling(void)
{
	debugfs_create_file("tlb_single_page_flush_ceiling", S_IRUSR | S_IWUSR,
			    arch_debugfs_dir, NULL, &fops_tlbflush);
	return 0;
}
late_initcall(create_tlb_single_page_flush_ceiling);
