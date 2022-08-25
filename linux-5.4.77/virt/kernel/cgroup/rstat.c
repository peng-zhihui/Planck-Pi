// SPDX-License-Identifier: GPL-2.0-only
#include "cgroup-internal.h"

#include <linux/sched/cputime.h>

static DEFINE_SPINLOCK(cgroup_rstat_lock);
static DEFINE_PER_CPU(raw_spinlock_t, cgroup_rstat_cpu_lock);

static void cgroup_base_stat_flush(struct cgroup *cgrp, int cpu);

static struct cgroup_rstat_cpu *cgroup_rstat_cpu(struct cgroup *cgrp, int cpu)
{
	return per_cpu_ptr(cgrp->rstat_cpu, cpu);
}

/**
 * cgroup_rstat_updated - keep track of updated rstat_cpu
 * @cgrp: target cgroup
 * @cpu: cpu on which rstat_cpu was updated
 *
 * @cgrp's rstat_cpu on @cpu was updated.  Put it on the parent's matching
 * rstat_cpu->updated_children list.  See the comment on top of
 * cgroup_rstat_cpu definition for details.
 */
void cgroup_rstat_updated(struct cgroup *cgrp, int cpu)
{
	raw_spinlock_t *cpu_lock = per_cpu_ptr(&cgroup_rstat_cpu_lock, cpu);
	struct cgroup *parent;
	unsigned long flags;

	/* nothing to do for root */
	if (!cgroup_parent(cgrp))
		return;

	/*
	 * Speculative already-on-list test. This may race leading to
	 * temporary inaccuracies, which is fine.
	 *
	 * Because @parent's updated_children is terminated with @parent
	 * instead of NULL, we can tell whether @cgrp is on the list by
	 * testing the next pointer for NULL.
	 */
	if (cgroup_rstat_cpu(cgrp, cpu)->updated_next)
		return;

	raw_spin_lock_irqsave(cpu_lock, flags);

	/* put @cgrp and all ancestors on the corresponding updated lists */
	for (parent = cgroup_parent(cgrp); parent;
	     cgrp = parent, parent = cgroup_parent(cgrp)) {
		struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(cgrp, cpu);
		struct cgroup_rstat_cpu *prstatc = cgroup_rstat_cpu(parent, cpu);

		/*
		 * Both additions and removals are bottom-up.  If a cgroup
		 * is already in the tree, all ancestors are.
		 */
		if (rstatc->updated_next)
			break;

		rstatc->updated_next = prstatc->updated_children;
		prstatc->updated_children = cgrp;
	}

	raw_spin_unlock_irqrestore(cpu_lock, flags);
}
EXPORT_SYMBOL_GPL(cgroup_rstat_updated);

/**
 * cgroup_rstat_cpu_pop_updated - iterate and dismantle rstat_cpu updated tree
 * @pos: current position
 * @root: root of the tree to traversal
 * @cpu: target cpu
 *
 * Walks the udpated rstat_cpu tree on @cpu from @root.  %NULL @pos starts
 * the traversal and %NULL return indicates the end.  During traversal,
 * each returned cgroup is unlinked from the tree.  Must be called with the
 * matching cgroup_rstat_cpu_lock held.
 *
 * The only ordering guarantee is that, for a parent and a child pair
 * covered by a given traversal, if a child is visited, its parent is
 * guaranteed to be visited afterwards.
 */
static struct cgroup *cgroup_rstat_cpu_pop_updated(struct cgroup *pos,
						   struct cgroup *root, int cpu)
{
	struct cgroup_rstat_cpu *rstatc;

	if (pos == root)
		return NULL;

	/*
	 * We're gonna walk down to the first leaf and visit/remove it.  We
	 * can pick whatever unvisited node as the starting point.
	 */
	if (!pos)
		pos = root;
	else
		pos = cgroup_parent(pos);

	/* walk down to the first leaf */
	while (true) {
		rstatc = cgroup_rstat_cpu(pos, cpu);
		if (rstatc->updated_children == pos)
			break;
		pos = rstatc->updated_children;
	}

	/*
	 * Unlink @pos from the tree.  As the updated_children list is
	 * singly linked, we have to walk it to find the removal point.
	 * However, due to the way we traverse, @pos will be the first
	 * child in most cases. The only exception is @root.
	 */
	if (rstatc->updated_next) {
		struct cgroup *parent = cgroup_parent(pos);
		struct cgroup_rstat_cpu *prstatc = cgroup_rstat_cpu(parent, cpu);
		struct cgroup_rstat_cpu *nrstatc;
		struct cgroup **nextp;

		nextp = &prstatc->updated_children;
		while (true) {
			nrstatc = cgroup_rstat_cpu(*nextp, cpu);
			if (*nextp == pos)
				break;

			WARN_ON_ONCE(*nextp == parent);
			nextp = &nrstatc->updated_next;
		}

		*nextp = rstatc->updated_next;
		rstatc->updated_next = NULL;

		return pos;
	}

	/* only happens for @root */
	return NULL;
}

/* see cgroup_rstat_flush() */
static void cgroup_rstat_flush_locked(struct cgroup *cgrp, bool may_sleep)
	__releases(&cgroup_rstat_lock) __acquires(&cgroup_rstat_lock)
{
	int cpu;

	lockdep_assert_held(&cgroup_rstat_lock);

	for_each_possible_cpu(cpu) {
		raw_spinlock_t *cpu_lock = per_cpu_ptr(&cgroup_rstat_cpu_lock,
						       cpu);
		struct cgroup *pos = NULL;

		raw_spin_lock(cpu_lock);
		while ((pos = cgroup_rstat_cpu_pop_updated(pos, cgrp, cpu))) {
			struct cgroup_subsys_state *css;

			cgroup_base_stat_flush(pos, cpu);

			rcu_read_lock();
			list_for_each_entry_rcu(css, &pos->rstat_css_list,
						rstat_css_node)
				css->ss->css_rstat_flush(css, cpu);
			rcu_read_unlock();
		}
		raw_spin_unlock(cpu_lock);

		/* if @may_sleep, play nice and yield if necessary */
		if (may_sleep && (need_resched() ||
				  spin_needbreak(&cgroup_rstat_lock))) {
			spin_unlock_irq(&cgroup_rstat_lock);
			if (!cond_resched())
				cpu_relax();
			spin_lock_irq(&cgroup_rstat_lock);
		}
	}
}

/**
 * cgroup_rstat_flush - flush stats in @cgrp's subtree
 * @cgrp: target cgroup
 *
 * Collect all per-cpu stats in @cgrp's subtree into the global counters
 * and propagate them upwards.  After this function returns, all cgroups in
 * the subtree have up-to-date ->stat.
 *
 * This also gets all cgroups in the subtree including @cgrp off the
 * ->updated_children lists.
 *
 * This function may block.
 */
void cgroup_rstat_flush(struct cgroup *cgrp)
{
	might_sleep();

	spin_lock_irq(&cgroup_rstat_lock);
	cgroup_rstat_flush_locked(cgrp, true);
	spin_unlock_irq(&cgroup_rstat_lock);
}

/**
 * cgroup_rstat_flush_irqsafe - irqsafe version of cgroup_rstat_flush()
 * @cgrp: target cgroup
 *
 * This function can be called from any context.
 */
void cgroup_rstat_flush_irqsafe(struct cgroup *cgrp)
{
	unsigned long flags;

	spin_lock_irqsave(&cgroup_rstat_lock, flags);
	cgroup_rstat_flush_locked(cgrp, false);
	spin_unlock_irqrestore(&cgroup_rstat_lock, flags);
}

/**
 * cgroup_rstat_flush_begin - flush stats in @cgrp's subtree and hold
 * @cgrp: target cgroup
 *
 * Flush stats in @cgrp's subtree and prevent further flushes.  Must be
 * paired with cgroup_rstat_flush_release().
 *
 * This function may block.
 */
void cgroup_rstat_flush_hold(struct cgroup *cgrp)
	__acquires(&cgroup_rstat_lock)
{
	might_sleep();
	spin_lock_irq(&cgroup_rstat_lock);
	cgroup_rstat_flush_locked(cgrp, true);
}

/**
 * cgroup_rstat_flush_release - release cgroup_rstat_flush_hold()
 */
void cgroup_rstat_flush_release(void)
	__releases(&cgroup_rstat_lock)
{
	spin_unlock_irq(&cgroup_rstat_lock);
}

int cgroup_rstat_init(struct cgroup *cgrp)
{
	int cpu;

	/* the root cgrp has rstat_cpu preallocated */
	if (!cgrp->rstat_cpu) {
		cgrp->rstat_cpu = alloc_percpu(struct cgroup_rstat_cpu);
		if (!cgrp->rstat_cpu)
			return -ENOMEM;
	}

	/* ->updated_children list is self terminated */
	for_each_possible_cpu(cpu) {
		struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(cgrp, cpu);

		rstatc->updated_children = cgrp;
		u64_stats_init(&rstatc->bsync);
	}

	return 0;
}

void cgroup_rstat_exit(struct cgroup *cgrp)
{
	int cpu;

	cgroup_rstat_flush(cgrp);

	/* sanity check */
	for_each_possible_cpu(cpu) {
		struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(cgrp, cpu);

		if (WARN_ON_ONCE(rstatc->updated_children != cgrp) ||
		    WARN_ON_ONCE(rstatc->updated_next))
			return;
	}

	free_percpu(cgrp->rstat_cpu);
	cgrp->rstat_cpu = NULL;
}

void __init cgroup_rstat_boot(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		raw_spin_lock_init(per_cpu_ptr(&cgroup_rstat_cpu_lock, cpu));

	BUG_ON(cgroup_rstat_init(&cgrp_dfl_root.cgrp));
}

/*
 * Functions for cgroup basic resource statistics implemented on top of
 * rstat.
 */
static void cgroup_base_stat_accumulate(struct cgroup_base_stat *dst_bstat,
					struct cgroup_base_stat *src_bstat)
{
	dst_bstat->cputime.utime += src_bstat->cputime.utime;
	dst_bstat->cputime.stime += src_bstat->cputime.stime;
	dst_bstat->cputime.sum_exec_runtime += src_bstat->cputime.sum_exec_runtime;
}

static void cgroup_base_stat_flush(struct cgroup *cgrp, int cpu)
{
	struct cgroup *parent = cgroup_parent(cgrp);
	struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(cgrp, cpu);
	struct task_cputime *last_cputime = &rstatc->last_bstat.cputime;
	struct task_cputime cputime;
	struct cgroup_base_stat delta;
	unsigned seq;

	/* fetch the current per-cpu values */
	do {
		seq = __u64_stats_fetch_begin(&rstatc->bsync);
		cputime = rstatc->bstat.cputime;
	} while (__u64_stats_fetch_retry(&rstatc->bsync, seq));

	/* calculate the delta to propgate */
	delta.cputime.utime = cputime.utime - last_cputime->utime;
	delta.cputime.stime = cputime.stime - last_cputime->stime;
	delta.cputime.sum_exec_runtime = cputime.sum_exec_runtime -
					 last_cputime->sum_exec_runtime;
	*last_cputime = cputime;

	/* transfer the pending stat into delta */
	cgroup_base_stat_accumulate(&delta, &cgrp->pending_bstat);
	memset(&cgrp->pending_bstat, 0, sizeof(cgrp->pending_bstat));

	/* propagate delta into the global stat and the parent's pending */
	cgroup_base_stat_accumulate(&cgrp->bstat, &delta);
	if (parent)
		cgroup_base_stat_accumulate(&parent->pending_bstat, &delta);
}

static struct cgroup_rstat_cpu *
cgroup_base_stat_cputime_account_begin(struct cgroup *cgrp)
{
	struct cgroup_rstat_cpu *rstatc;

	rstatc = get_cpu_ptr(cgrp->rstat_cpu);
	u64_stats_update_begin(&rstatc->bsync);
	return rstatc;
}

static void cgroup_base_stat_cputime_account_end(struct cgroup *cgrp,
						 struct cgroup_rstat_cpu *rstatc)
{
	u64_stats_update_end(&rstatc->bsync);
	cgroup_rstat_updated(cgrp, smp_processor_id());
	put_cpu_ptr(rstatc);
}

void __cgroup_account_cputime(struct cgroup *cgrp, u64 delta_exec)
{
	struct cgroup_rstat_cpu *rstatc;

	rstatc = cgroup_base_stat_cputime_account_begin(cgrp);
	rstatc->bstat.cputime.sum_exec_runtime += delta_exec;
	cgroup_base_stat_cputime_account_end(cgrp, rstatc);
}

void __cgroup_account_cputime_field(struct cgroup *cgrp,
				    enum cpu_usage_stat index, u64 delta_exec)
{
	struct cgroup_rstat_cpu *rstatc;

	rstatc = cgroup_base_stat_cputime_account_begin(cgrp);

	switch (index) {
	case CPUTIME_USER:
	case CPUTIME_NICE:
		rstatc->bstat.cputime.utime += delta_exec;
		break;
	case CPUTIME_SYSTEM:
	case CPUTIME_IRQ:
	case CPUTIME_SOFTIRQ:
		rstatc->bstat.cputime.stime += delta_exec;
		break;
	default:
		break;
	}

	cgroup_base_stat_cputime_account_end(cgrp, rstatc);
}

void cgroup_base_stat_cputime_show(struct seq_file *seq)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	u64 usage, utime, stime;

	if (!cgroup_parent(cgrp))
		return;

	cgroup_rstat_flush_hold(cgrp);
	usage = cgrp->bstat.cputime.sum_exec_runtime;
	cputime_adjust(&cgrp->bstat.cputime, &cgrp->prev_cputime, &utime, &stime);
	cgroup_rstat_flush_release();

	do_div(usage, NSEC_PER_USEC);
	do_div(utime, NSEC_PER_USEC);
	do_div(stime, NSEC_PER_USEC);

	seq_printf(seq, "usage_usec %llu\n"
		   "user_usec %llu\n"
		   "system_usec %llu\n",
		   usage, utime, stime);
}
