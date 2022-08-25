// SPDX-License-Identifier: GPL-2.0-only
#include <linux/extable.h>
#include <linux/uaccess.h>
#include <linux/sched/debug.h>
#include <xen/xen.h>

#include <asm/fpu/internal.h>
#include <asm/traps.h>
#include <asm/kdebug.h>

typedef bool (*ex_handler_t)(const struct exception_table_entry *,
			    struct pt_regs *, int, unsigned long,
			    unsigned long);

static inline unsigned long
ex_fixup_addr(const struct exception_table_entry *x)
{
	return (unsigned long)&x->fixup + x->fixup;
}
static inline ex_handler_t
ex_fixup_handler(const struct exception_table_entry *x)
{
	return (ex_handler_t)((unsigned long)&x->handler + x->handler);
}

__visible bool ex_handler_default(const struct exception_table_entry *fixup,
				  struct pt_regs *regs, int trapnr,
				  unsigned long error_code,
				  unsigned long fault_addr)
{
	regs->ip = ex_fixup_addr(fixup);
	return true;
}
EXPORT_SYMBOL(ex_handler_default);

__visible bool ex_handler_fault(const struct exception_table_entry *fixup,
				struct pt_regs *regs, int trapnr,
				unsigned long error_code,
				unsigned long fault_addr)
{
	regs->ip = ex_fixup_addr(fixup);
	regs->ax = trapnr;
	return true;
}
EXPORT_SYMBOL_GPL(ex_handler_fault);

/*
 * Handler for UD0 exception following a failed test against the
 * result of a refcount inc/dec/add/sub.
 */
__visible bool ex_handler_refcount(const struct exception_table_entry *fixup,
				   struct pt_regs *regs, int trapnr,
				   unsigned long error_code,
				   unsigned long fault_addr)
{
	/* First unconditionally saturate the refcount. */
	*(int *)regs->cx = INT_MIN / 2;

	/*
	 * Strictly speaking, this reports the fixup destination, not
	 * the fault location, and not the actually overflowing
	 * instruction, which is the instruction before the "js", but
	 * since that instruction could be a variety of lengths, just
	 * report the location after the overflow, which should be close
	 * enough for finding the overflow, as it's at least back in
	 * the function, having returned from .text.unlikely.
	 */
	regs->ip = ex_fixup_addr(fixup);

	/*
	 * This function has been called because either a negative refcount
	 * value was seen by any of the refcount functions, or a zero
	 * refcount value was seen by refcount_dec().
	 *
	 * If we crossed from INT_MAX to INT_MIN, OF (Overflow Flag: result
	 * wrapped around) will be set. Additionally, seeing the refcount
	 * reach 0 will set ZF (Zero Flag: result was zero). In each of
	 * these cases we want a report, since it's a boundary condition.
	 * The SF case is not reported since it indicates post-boundary
	 * manipulations below zero or above INT_MAX. And if none of the
	 * flags are set, something has gone very wrong, so report it.
	 */
	if (regs->flags & (X86_EFLAGS_OF | X86_EFLAGS_ZF)) {
		bool zero = regs->flags & X86_EFLAGS_ZF;

		refcount_error_report(regs, zero ? "hit zero" : "overflow");
	} else if ((regs->flags & X86_EFLAGS_SF) == 0) {
		/* Report if none of OF, ZF, nor SF are set. */
		refcount_error_report(regs, "unexpected saturation");
	}

	return true;
}
EXPORT_SYMBOL(ex_handler_refcount);

/*
 * Handler for when we fail to restore a task's FPU state.  We should never get
 * here because the FPU state of a task using the FPU (task->thread.fpu.state)
 * should always be valid.  However, past bugs have allowed userspace to set
 * reserved bits in the XSAVE area using PTRACE_SETREGSET or sys_rt_sigreturn().
 * These caused XRSTOR to fail when switching to the task, leaking the FPU
 * registers of the task previously executing on the CPU.  Mitigate this class
 * of vulnerability by restoring from the initial state (essentially, zeroing
 * out all the FPU registers) if we can't restore from the task's FPU state.
 */
__visible bool ex_handler_fprestore(const struct exception_table_entry *fixup,
				    struct pt_regs *regs, int trapnr,
				    unsigned long error_code,
				    unsigned long fault_addr)
{
	regs->ip = ex_fixup_addr(fixup);

	WARN_ONCE(1, "Bad FPU state detected at %pB, reinitializing FPU registers.",
		  (void *)instruction_pointer(regs));

	__copy_kernel_to_fpregs(&init_fpstate, -1);
	return true;
}
EXPORT_SYMBOL_GPL(ex_handler_fprestore);

__visible bool ex_handler_uaccess(const struct exception_table_entry *fixup,
				  struct pt_regs *regs, int trapnr,
				  unsigned long error_code,
				  unsigned long fault_addr)
{
	WARN_ONCE(trapnr == X86_TRAP_GP, "General protection fault in user access. Non-canonical address?");
	regs->ip = ex_fixup_addr(fixup);
	return true;
}
EXPORT_SYMBOL(ex_handler_uaccess);

__visible bool ex_handler_ext(const struct exception_table_entry *fixup,
			      struct pt_regs *regs, int trapnr,
			      unsigned long error_code,
			      unsigned long fault_addr)
{
	/* Special hack for uaccess_err */
	current->thread.uaccess_err = 1;
	regs->ip = ex_fixup_addr(fixup);
	return true;
}
EXPORT_SYMBOL(ex_handler_ext);

__visible bool ex_handler_rdmsr_unsafe(const struct exception_table_entry *fixup,
				       struct pt_regs *regs, int trapnr,
				       unsigned long error_code,
				       unsigned long fault_addr)
{
	if (pr_warn_once("unchecked MSR access error: RDMSR from 0x%x at rIP: 0x%lx (%pS)\n",
			 (unsigned int)regs->cx, regs->ip, (void *)regs->ip))
		show_stack_regs(regs);

	/* Pretend that the read succeeded and returned 0. */
	regs->ip = ex_fixup_addr(fixup);
	regs->ax = 0;
	regs->dx = 0;
	return true;
}
EXPORT_SYMBOL(ex_handler_rdmsr_unsafe);

__visible bool ex_handler_wrmsr_unsafe(const struct exception_table_entry *fixup,
				       struct pt_regs *regs, int trapnr,
				       unsigned long error_code,
				       unsigned long fault_addr)
{
	if (pr_warn_once("unchecked MSR access error: WRMSR to 0x%x (tried to write 0x%08x%08x) at rIP: 0x%lx (%pS)\n",
			 (unsigned int)regs->cx, (unsigned int)regs->dx,
			 (unsigned int)regs->ax,  regs->ip, (void *)regs->ip))
		show_stack_regs(regs);

	/* Pretend that the write succeeded. */
	regs->ip = ex_fixup_addr(fixup);
	return true;
}
EXPORT_SYMBOL(ex_handler_wrmsr_unsafe);

__visible bool ex_handler_clear_fs(const struct exception_table_entry *fixup,
				   struct pt_regs *regs, int trapnr,
				   unsigned long error_code,
				   unsigned long fault_addr)
{
	if (static_cpu_has(X86_BUG_NULL_SEG))
		asm volatile ("mov %0, %%fs" : : "rm" (__USER_DS));
	asm volatile ("mov %0, %%fs" : : "rm" (0));
	return ex_handler_default(fixup, regs, trapnr, error_code, fault_addr);
}
EXPORT_SYMBOL(ex_handler_clear_fs);

__visible bool ex_has_fault_handler(unsigned long ip)
{
	const struct exception_table_entry *e;
	ex_handler_t handler;

	e = search_exception_tables(ip);
	if (!e)
		return false;
	handler = ex_fixup_handler(e);

	return handler == ex_handler_fault;
}

int fixup_exception(struct pt_regs *regs, int trapnr, unsigned long error_code,
		    unsigned long fault_addr)
{
	const struct exception_table_entry *e;
	ex_handler_t handler;

#ifdef CONFIG_PNPBIOS
	if (unlikely(SEGMENT_IS_PNP_CODE(regs->cs))) {
		extern u32 pnp_bios_fault_eip, pnp_bios_fault_esp;
		extern u32 pnp_bios_is_utter_crap;
		pnp_bios_is_utter_crap = 1;
		printk(KERN_CRIT "PNPBIOS fault.. attempting recovery.\n");
		__asm__ volatile(
			"movl %0, %%esp\n\t"
			"jmp *%1\n\t"
			: : "g" (pnp_bios_fault_esp), "g" (pnp_bios_fault_eip));
		panic("do_trap: can't hit this");
	}
#endif

	e = search_exception_tables(regs->ip);
	if (!e)
		return 0;

	handler = ex_fixup_handler(e);
	return handler(e, regs, trapnr, error_code, fault_addr);
}

extern unsigned int early_recursion_flag;

/* Restricted version used during very early boot */
void __init early_fixup_exception(struct pt_regs *regs, int trapnr)
{
	/* Ignore early NMIs. */
	if (trapnr == X86_TRAP_NMI)
		return;

	if (early_recursion_flag > 2)
		goto halt_loop;

	/*
	 * Old CPUs leave the high bits of CS on the stack
	 * undefined.  I'm not sure which CPUs do this, but at least
	 * the 486 DX works this way.
	 * Xen pv domains are not using the default __KERNEL_CS.
	 */
	if (!xen_pv_domain() && regs->cs != __KERNEL_CS)
		goto fail;

	/*
	 * The full exception fixup machinery is available as soon as
	 * the early IDT is loaded.  This means that it is the
	 * responsibility of extable users to either function correctly
	 * when handlers are invoked early or to simply avoid causing
	 * exceptions before they're ready to handle them.
	 *
	 * This is better than filtering which handlers can be used,
	 * because refusing to call a handler here is guaranteed to
	 * result in a hard-to-debug panic.
	 *
	 * Keep in mind that not all vectors actually get here.  Early
	 * page faults, for example, are special.
	 */
	if (fixup_exception(regs, trapnr, regs->orig_ax, 0))
		return;

	if (fixup_bug(regs, trapnr))
		return;

fail:
	early_printk("PANIC: early exception 0x%02x IP %lx:%lx error %lx cr2 0x%lx\n",
		     (unsigned)trapnr, (unsigned long)regs->cs, regs->ip,
		     regs->orig_ax, read_cr2());

	show_regs(regs);

halt_loop:
	while (true)
		halt();
}
