// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Author: Kumar Gala <galak@kernel.crashing.org>
 *
 * Copyright 2009 Freescale Semiconductor Inc.
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/threads.h>
#include <linux/hardirq.h>

#include <asm/dbell.h>
#include <asm/irq_regs.h>
#include <asm/kvm_ppc.h>
#include <asm/trace.h>

#ifdef CONFIG_SMP

/*
 * Doorbells must only be used if CPU_FTR_DBELL is available.
 * msgsnd is used in HV, and msgsndp is used in !HV.
 *
 * These should be used by platform code that is aware of restrictions.
 * Other arch code should use ->cause_ipi.
 *
 * doorbell_global_ipi() sends a dbell to any target CPU.
 * Must be used only by architectures that address msgsnd target
 * by PIR/get_hard_smp_processor_id.
 */
void doorbell_global_ipi(int cpu)
{
	u32 tag = get_hard_smp_processor_id(cpu);

	kvmppc_set_host_ipi(cpu);
	/* Order previous accesses vs. msgsnd, which is treated as a store */
	ppc_msgsnd_sync();
	ppc_msgsnd(PPC_DBELL_MSGTYPE, 0, tag);
}

/*
 * doorbell_core_ipi() sends a dbell to a target CPU in the same core.
 * Must be used only by architectures that address msgsnd target
 * by TIR/cpu_thread_in_core.
 */
void doorbell_core_ipi(int cpu)
{
	u32 tag = cpu_thread_in_core(cpu);

	kvmppc_set_host_ipi(cpu);
	/* Order previous accesses vs. msgsnd, which is treated as a store */
	ppc_msgsnd_sync();
	ppc_msgsnd(PPC_DBELL_MSGTYPE, 0, tag);
}

/*
 * Attempt to cause a core doorbell if destination is on the same core.
 * Returns 1 on success, 0 on failure.
 */
int doorbell_try_core_ipi(int cpu)
{
	int this_cpu = get_cpu();
	int ret = 0;

	if (cpumask_test_cpu(cpu, cpu_sibling_mask(this_cpu))) {
		doorbell_core_ipi(cpu);
		ret = 1;
	}

	put_cpu();

	return ret;
}

void doorbell_exception(struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);

	irq_enter();
	trace_doorbell_entry(regs);

	ppc_msgsync();

	may_hard_irq_enable();

	kvmppc_clear_host_ipi(smp_processor_id());
	__this_cpu_inc(irq_stat.doorbell_irqs);

	smp_ipi_demux_relaxed(); /* already performed the barrier */

	trace_doorbell_exit(regs);
	irq_exit();
	set_irq_regs(old_regs);
}
#else /* CONFIG_SMP */
void doorbell_exception(struct pt_regs *regs)
{
	printk(KERN_WARNING "Received doorbell on non-smp system\n");
}
#endif /* CONFIG_SMP */

