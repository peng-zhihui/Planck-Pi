// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Kernel Probes (KProbes)
 *
 * Copyright (C) IBM Corporation, 2002, 2004
 *
 * 2002-Oct	Created by Vamsi Krishna S <vamsi_krishna@in.ibm.com> Kernel
 *		Probes initial implementation ( includes contributions from
 *		Rusty Russell).
 * 2004-July	Suparna Bhattacharya <suparna@in.ibm.com> added jumper probes
 *		interface to access function arguments.
 * 2004-Oct	Jim Keniston <jkenisto@us.ibm.com> and Prasanna S Panchamukhi
 *		<prasanna@in.ibm.com> adapted for x86_64 from i386.
 * 2005-Mar	Roland McGrath <roland@redhat.com>
 *		Fixed to handle %rip-relative addressing mode correctly.
 * 2005-May	Hien Nguyen <hien@us.ibm.com>, Jim Keniston
 *		<jkenisto@us.ibm.com> and Prasanna S Panchamukhi
 *		<prasanna@in.ibm.com> added function-return probes.
 * 2005-May	Rusty Lynch <rusty.lynch@intel.com>
 *		Added function return probes functionality
 * 2006-Feb	Masami Hiramatsu <hiramatu@sdl.hitachi.co.jp> added
 *		kprobe-booster and kretprobe-booster for i386.
 * 2007-Dec	Masami Hiramatsu <mhiramat@redhat.com> added kprobe-booster
 *		and kretprobe-booster for x86-64
 * 2007-Dec	Masami Hiramatsu <mhiramat@redhat.com>, Arjan van de Ven
 *		<arjan@infradead.org> and Jim Keniston <jkenisto@us.ibm.com>
 *		unified x86 kprobes code.
 */
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/hardirq.h>
#include <linux/preempt.h>
#include <linux/sched/debug.h>
#include <linux/extable.h>
#include <linux/kdebug.h>
#include <linux/kallsyms.h>
#include <linux/ftrace.h>
#include <linux/frame.h>
#include <linux/kasan.h>
#include <linux/moduleloader.h>

#include <asm/text-patching.h>
#include <asm/cacheflush.h>
#include <asm/desc.h>
#include <asm/pgtable.h>
#include <linux/uaccess.h>
#include <asm/alternative.h>
#include <asm/insn.h>
#include <asm/debugreg.h>
#include <asm/set_memory.h>

#include "common.h"

DEFINE_PER_CPU(struct kprobe *, current_kprobe) = NULL;
DEFINE_PER_CPU(struct kprobe_ctlblk, kprobe_ctlblk);

#define stack_addr(regs) ((unsigned long *)regs->sp)

#define W(row, b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, ba, bb, bc, bd, be, bf)\
	(((b0##UL << 0x0)|(b1##UL << 0x1)|(b2##UL << 0x2)|(b3##UL << 0x3) |   \
	  (b4##UL << 0x4)|(b5##UL << 0x5)|(b6##UL << 0x6)|(b7##UL << 0x7) |   \
	  (b8##UL << 0x8)|(b9##UL << 0x9)|(ba##UL << 0xa)|(bb##UL << 0xb) |   \
	  (bc##UL << 0xc)|(bd##UL << 0xd)|(be##UL << 0xe)|(bf##UL << 0xf))    \
	 << (row % 32))
	/*
	 * Undefined/reserved opcodes, conditional jump, Opcode Extension
	 * Groups, and some special opcodes can not boost.
	 * This is non-const and volatile to keep gcc from statically
	 * optimizing it out, as variable_test_bit makes gcc think only
	 * *(unsigned long*) is used.
	 */
static volatile u32 twobyte_is_boostable[256 / 32] = {
	/*      0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f          */
	/*      ----------------------------------------------          */
	W(0x00, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0) | /* 00 */
	W(0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1) , /* 10 */
	W(0x20, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) | /* 20 */
	W(0x30, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) , /* 30 */
	W(0x40, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1) | /* 40 */
	W(0x50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) , /* 50 */
	W(0x60, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1) | /* 60 */
	W(0x70, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1) , /* 70 */
	W(0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) | /* 80 */
	W(0x90, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1) , /* 90 */
	W(0xa0, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1) | /* a0 */
	W(0xb0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1) , /* b0 */
	W(0xc0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1) | /* c0 */
	W(0xd0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1) , /* d0 */
	W(0xe0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1) | /* e0 */
	W(0xf0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0)   /* f0 */
	/*      -----------------------------------------------         */
	/*      0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f          */
};
#undef W

struct kretprobe_blackpoint kretprobe_blacklist[] = {
	{"__switch_to", }, /* This function switches only current task, but
			      doesn't switch kernel stack.*/
	{NULL, NULL}	/* Terminator */
};

const int kretprobe_blacklist_size = ARRAY_SIZE(kretprobe_blacklist);

static nokprobe_inline void
__synthesize_relative_insn(void *dest, void *from, void *to, u8 op)
{
	struct __arch_relative_insn {
		u8 op;
		s32 raddr;
	} __packed *insn;

	insn = (struct __arch_relative_insn *)dest;
	insn->raddr = (s32)((long)(to) - ((long)(from) + 5));
	insn->op = op;
}

/* Insert a jump instruction at address 'from', which jumps to address 'to'.*/
void synthesize_reljump(void *dest, void *from, void *to)
{
	__synthesize_relative_insn(dest, from, to, RELATIVEJUMP_OPCODE);
}
NOKPROBE_SYMBOL(synthesize_reljump);

/* Insert a call instruction at address 'from', which calls address 'to'.*/
void synthesize_relcall(void *dest, void *from, void *to)
{
	__synthesize_relative_insn(dest, from, to, RELATIVECALL_OPCODE);
}
NOKPROBE_SYMBOL(synthesize_relcall);

/*
 * Skip the prefixes of the instruction.
 */
static kprobe_opcode_t *skip_prefixes(kprobe_opcode_t *insn)
{
	insn_attr_t attr;

	attr = inat_get_opcode_attribute((insn_byte_t)*insn);
	while (inat_is_legacy_prefix(attr)) {
		insn++;
		attr = inat_get_opcode_attribute((insn_byte_t)*insn);
	}
#ifdef CONFIG_X86_64
	if (inat_is_rex_prefix(attr))
		insn++;
#endif
	return insn;
}
NOKPROBE_SYMBOL(skip_prefixes);

/*
 * Returns non-zero if INSN is boostable.
 * RIP relative instructions are adjusted at copying time in 64 bits mode
 */
int can_boost(struct insn *insn, void *addr)
{
	kprobe_opcode_t opcode;

	if (search_exception_tables((unsigned long)addr))
		return 0;	/* Page fault may occur on this address. */

	/* 2nd-byte opcode */
	if (insn->opcode.nbytes == 2)
		return test_bit(insn->opcode.bytes[1],
				(unsigned long *)twobyte_is_boostable);

	if (insn->opcode.nbytes != 1)
		return 0;

	/* Can't boost Address-size override prefix */
	if (unlikely(inat_is_address_size_prefix(insn->attr)))
		return 0;

	opcode = insn->opcode.bytes[0];

	switch (opcode & 0xf0) {
	case 0x60:
		/* can't boost "bound" */
		return (opcode != 0x62);
	case 0x70:
		return 0; /* can't boost conditional jump */
	case 0x90:
		return opcode != 0x9a;	/* can't boost call far */
	case 0xc0:
		/* can't boost software-interruptions */
		return (0xc1 < opcode && opcode < 0xcc) || opcode == 0xcf;
	case 0xd0:
		/* can boost AA* and XLAT */
		return (opcode == 0xd4 || opcode == 0xd5 || opcode == 0xd7);
	case 0xe0:
		/* can boost in/out and absolute jmps */
		return ((opcode & 0x04) || opcode == 0xea);
	case 0xf0:
		/* clear and set flags are boostable */
		return (opcode == 0xf5 || (0xf7 < opcode && opcode < 0xfe));
	default:
		/* CS override prefix and call are not boostable */
		return (opcode != 0x2e && opcode != 0x9a);
	}
}

static unsigned long
__recover_probed_insn(kprobe_opcode_t *buf, unsigned long addr)
{
	struct kprobe *kp;
	unsigned long faddr;

	kp = get_kprobe((void *)addr);
	faddr = ftrace_location(addr);
	/*
	 * Addresses inside the ftrace location are refused by
	 * arch_check_ftrace_location(). Something went terribly wrong
	 * if such an address is checked here.
	 */
	if (WARN_ON(faddr && faddr != addr))
		return 0UL;
	/*
	 * Use the current code if it is not modified by Kprobe
	 * and it cannot be modified by ftrace.
	 */
	if (!kp && !faddr)
		return addr;

	/*
	 * Basically, kp->ainsn.insn has an original instruction.
	 * However, RIP-relative instruction can not do single-stepping
	 * at different place, __copy_instruction() tweaks the displacement of
	 * that instruction. In that case, we can't recover the instruction
	 * from the kp->ainsn.insn.
	 *
	 * On the other hand, in case on normal Kprobe, kp->opcode has a copy
	 * of the first byte of the probed instruction, which is overwritten
	 * by int3. And the instruction at kp->addr is not modified by kprobes
	 * except for the first byte, we can recover the original instruction
	 * from it and kp->opcode.
	 *
	 * In case of Kprobes using ftrace, we do not have a copy of
	 * the original instruction. In fact, the ftrace location might
	 * be modified at anytime and even could be in an inconsistent state.
	 * Fortunately, we know that the original code is the ideal 5-byte
	 * long NOP.
	 */
	if (probe_kernel_read(buf, (void *)addr,
		MAX_INSN_SIZE * sizeof(kprobe_opcode_t)))
		return 0UL;

	if (faddr)
		memcpy(buf, ideal_nops[NOP_ATOMIC5], 5);
	else
		buf[0] = kp->opcode;
	return (unsigned long)buf;
}

/*
 * Recover the probed instruction at addr for further analysis.
 * Caller must lock kprobes by kprobe_mutex, or disable preemption
 * for preventing to release referencing kprobes.
 * Returns zero if the instruction can not get recovered (or access failed).
 */
unsigned long recover_probed_instruction(kprobe_opcode_t *buf, unsigned long addr)
{
	unsigned long __addr;

	__addr = __recover_optprobed_insn(buf, addr);
	if (__addr != addr)
		return __addr;

	return __recover_probed_insn(buf, addr);
}

/* Check if paddr is at an instruction boundary */
static int can_probe(unsigned long paddr)
{
	unsigned long addr, __addr, offset = 0;
	struct insn insn;
	kprobe_opcode_t buf[MAX_INSN_SIZE];

	if (!kallsyms_lookup_size_offset(paddr, NULL, &offset))
		return 0;

	/* Decode instructions */
	addr = paddr - offset;
	while (addr < paddr) {
		/*
		 * Check if the instruction has been modified by another
		 * kprobe, in which case we replace the breakpoint by the
		 * original instruction in our buffer.
		 * Also, jump optimization will change the breakpoint to
		 * relative-jump. Since the relative-jump itself is
		 * normally used, we just go through if there is no kprobe.
		 */
		__addr = recover_probed_instruction(buf, addr);
		if (!__addr)
			return 0;
		kernel_insn_init(&insn, (void *)__addr, MAX_INSN_SIZE);
		insn_get_length(&insn);

		/*
		 * Another debugging subsystem might insert this breakpoint.
		 * In that case, we can't recover it.
		 */
		if (insn.opcode.bytes[0] == BREAKPOINT_INSTRUCTION)
			return 0;
		addr += insn.length;
	}

	return (addr == paddr);
}

/*
 * Returns non-zero if opcode modifies the interrupt flag.
 */
static int is_IF_modifier(kprobe_opcode_t *insn)
{
	/* Skip prefixes */
	insn = skip_prefixes(insn);

	switch (*insn) {
	case 0xfa:		/* cli */
	case 0xfb:		/* sti */
	case 0xcf:		/* iret/iretd */
	case 0x9d:		/* popf/popfd */
		return 1;
	}

	return 0;
}

/*
 * Copy an instruction with recovering modified instruction by kprobes
 * and adjust the displacement if the instruction uses the %rip-relative
 * addressing mode. Note that since @real will be the final place of copied
 * instruction, displacement must be adjust by @real, not @dest.
 * This returns the length of copied instruction, or 0 if it has an error.
 */
int __copy_instruction(u8 *dest, u8 *src, u8 *real, struct insn *insn)
{
	kprobe_opcode_t buf[MAX_INSN_SIZE];
	unsigned long recovered_insn =
		recover_probed_instruction(buf, (unsigned long)src);

	if (!recovered_insn || !insn)
		return 0;

	/* This can access kernel text if given address is not recovered */
	if (probe_kernel_read(dest, (void *)recovered_insn, MAX_INSN_SIZE))
		return 0;

	kernel_insn_init(insn, dest, MAX_INSN_SIZE);
	insn_get_length(insn);

	/* Another subsystem puts a breakpoint, failed to recover */
	if (insn->opcode.bytes[0] == BREAKPOINT_INSTRUCTION)
		return 0;

	/* We should not singlestep on the exception masking instructions */
	if (insn_masking_exception(insn))
		return 0;

#ifdef CONFIG_X86_64
	/* Only x86_64 has RIP relative instructions */
	if (insn_rip_relative(insn)) {
		s64 newdisp;
		u8 *disp;
		/*
		 * The copied instruction uses the %rip-relative addressing
		 * mode.  Adjust the displacement for the difference between
		 * the original location of this instruction and the location
		 * of the copy that will actually be run.  The tricky bit here
		 * is making sure that the sign extension happens correctly in
		 * this calculation, since we need a signed 32-bit result to
		 * be sign-extended to 64 bits when it's added to the %rip
		 * value and yield the same 64-bit result that the sign-
		 * extension of the original signed 32-bit displacement would
		 * have given.
		 */
		newdisp = (u8 *) src + (s64) insn->displacement.value
			  - (u8 *) real;
		if ((s64) (s32) newdisp != newdisp) {
			pr_err("Kprobes error: new displacement does not fit into s32 (%llx)\n", newdisp);
			return 0;
		}
		disp = (u8 *) dest + insn_offset_displacement(insn);
		*(s32 *) disp = (s32) newdisp;
	}
#endif
	return insn->length;
}

/* Prepare reljump right after instruction to boost */
static int prepare_boost(kprobe_opcode_t *buf, struct kprobe *p,
			  struct insn *insn)
{
	int len = insn->length;

	if (can_boost(insn, p->addr) &&
	    MAX_INSN_SIZE - len >= RELATIVEJUMP_SIZE) {
		/*
		 * These instructions can be executed directly if it
		 * jumps back to correct address.
		 */
		synthesize_reljump(buf + len, p->ainsn.insn + len,
				   p->addr + insn->length);
		len += RELATIVEJUMP_SIZE;
		p->ainsn.boostable = true;
	} else {
		p->ainsn.boostable = false;
	}

	return len;
}

/* Make page to RO mode when allocate it */
void *alloc_insn_page(void)
{
	void *page;

	page = module_alloc(PAGE_SIZE);
	if (!page)
		return NULL;

	set_vm_flush_reset_perms(page);
	/*
	 * First make the page read-only, and only then make it executable to
	 * prevent it from being W+X in between.
	 */
	set_memory_ro((unsigned long)page, 1);

	/*
	 * TODO: Once additional kernel code protection mechanisms are set, ensure
	 * that the page was not maliciously altered and it is still zeroed.
	 */
	set_memory_x((unsigned long)page, 1);

	return page;
}

/* Recover page to RW mode before releasing it */
void free_insn_page(void *page)
{
	module_memfree(page);
}

static int arch_copy_kprobe(struct kprobe *p)
{
	struct insn insn;
	kprobe_opcode_t buf[MAX_INSN_SIZE];
	int len;

	/* Copy an instruction with recovering if other optprobe modifies it.*/
	len = __copy_instruction(buf, p->addr, p->ainsn.insn, &insn);
	if (!len)
		return -EINVAL;

	/*
	 * __copy_instruction can modify the displacement of the instruction,
	 * but it doesn't affect boostable check.
	 */
	len = prepare_boost(buf, p, &insn);

	/* Check whether the instruction modifies Interrupt Flag or not */
	p->ainsn.if_modifier = is_IF_modifier(buf);

	/* Also, displacement change doesn't affect the first byte */
	p->opcode = buf[0];

	/* OK, write back the instruction(s) into ROX insn buffer */
	text_poke(p->ainsn.insn, buf, len);

	return 0;
}

int arch_prepare_kprobe(struct kprobe *p)
{
	int ret;

	if (alternatives_text_reserved(p->addr, p->addr))
		return -EINVAL;

	if (!can_probe((unsigned long)p->addr))
		return -EILSEQ;
	/* insn: must be on special executable page on x86. */
	p->ainsn.insn = get_insn_slot();
	if (!p->ainsn.insn)
		return -ENOMEM;

	ret = arch_copy_kprobe(p);
	if (ret) {
		free_insn_slot(p->ainsn.insn, 0);
		p->ainsn.insn = NULL;
	}

	return ret;
}

void arch_arm_kprobe(struct kprobe *p)
{
	text_poke(p->addr, ((unsigned char []){BREAKPOINT_INSTRUCTION}), 1);
}

void arch_disarm_kprobe(struct kprobe *p)
{
	text_poke(p->addr, &p->opcode, 1);
}

void arch_remove_kprobe(struct kprobe *p)
{
	if (p->ainsn.insn) {
		free_insn_slot(p->ainsn.insn, p->ainsn.boostable);
		p->ainsn.insn = NULL;
	}
}

static nokprobe_inline void
save_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	kcb->prev_kprobe.kp = kprobe_running();
	kcb->prev_kprobe.status = kcb->kprobe_status;
	kcb->prev_kprobe.old_flags = kcb->kprobe_old_flags;
	kcb->prev_kprobe.saved_flags = kcb->kprobe_saved_flags;
}

static nokprobe_inline void
restore_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	__this_cpu_write(current_kprobe, kcb->prev_kprobe.kp);
	kcb->kprobe_status = kcb->prev_kprobe.status;
	kcb->kprobe_old_flags = kcb->prev_kprobe.old_flags;
	kcb->kprobe_saved_flags = kcb->prev_kprobe.saved_flags;
}

static nokprobe_inline void
set_current_kprobe(struct kprobe *p, struct pt_regs *regs,
		   struct kprobe_ctlblk *kcb)
{
	__this_cpu_write(current_kprobe, p);
	kcb->kprobe_saved_flags = kcb->kprobe_old_flags
		= (regs->flags & (X86_EFLAGS_TF | X86_EFLAGS_IF));
	if (p->ainsn.if_modifier)
		kcb->kprobe_saved_flags &= ~X86_EFLAGS_IF;
}

static nokprobe_inline void clear_btf(void)
{
	if (test_thread_flag(TIF_BLOCKSTEP)) {
		unsigned long debugctl = get_debugctlmsr();

		debugctl &= ~DEBUGCTLMSR_BTF;
		update_debugctlmsr(debugctl);
	}
}

static nokprobe_inline void restore_btf(void)
{
	if (test_thread_flag(TIF_BLOCKSTEP)) {
		unsigned long debugctl = get_debugctlmsr();

		debugctl |= DEBUGCTLMSR_BTF;
		update_debugctlmsr(debugctl);
	}
}

void arch_prepare_kretprobe(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned long *sara = stack_addr(regs);

	ri->ret_addr = (kprobe_opcode_t *) *sara;
	ri->fp = sara;

	/* Replace the return addr with trampoline addr */
	*sara = (unsigned long) &kretprobe_trampoline;
}
NOKPROBE_SYMBOL(arch_prepare_kretprobe);

static void setup_singlestep(struct kprobe *p, struct pt_regs *regs,
			     struct kprobe_ctlblk *kcb, int reenter)
{
	if (setup_detour_execution(p, regs, reenter))
		return;

#if !defined(CONFIG_PREEMPTION)
	if (p->ainsn.boostable && !p->post_handler) {
		/* Boost up -- we can execute copied instructions directly */
		if (!reenter)
			reset_current_kprobe();
		/*
		 * Reentering boosted probe doesn't reset current_kprobe,
		 * nor set current_kprobe, because it doesn't use single
		 * stepping.
		 */
		regs->ip = (unsigned long)p->ainsn.insn;
		return;
	}
#endif
	if (reenter) {
		save_previous_kprobe(kcb);
		set_current_kprobe(p, regs, kcb);
		kcb->kprobe_status = KPROBE_REENTER;
	} else
		kcb->kprobe_status = KPROBE_HIT_SS;
	/* Prepare real single stepping */
	clear_btf();
	regs->flags |= X86_EFLAGS_TF;
	regs->flags &= ~X86_EFLAGS_IF;
	/* single step inline if the instruction is an int3 */
	if (p->opcode == BREAKPOINT_INSTRUCTION)
		regs->ip = (unsigned long)p->addr;
	else
		regs->ip = (unsigned long)p->ainsn.insn;
}
NOKPROBE_SYMBOL(setup_singlestep);

/*
 * We have reentered the kprobe_handler(), since another probe was hit while
 * within the handler. We save the original kprobes variables and just single
 * step on the instruction of the new probe without calling any user handlers.
 */
static int reenter_kprobe(struct kprobe *p, struct pt_regs *regs,
			  struct kprobe_ctlblk *kcb)
{
	switch (kcb->kprobe_status) {
	case KPROBE_HIT_SSDONE:
	case KPROBE_HIT_ACTIVE:
	case KPROBE_HIT_SS:
		kprobes_inc_nmissed_count(p);
		setup_singlestep(p, regs, kcb, 1);
		break;
	case KPROBE_REENTER:
		/* A probe has been hit in the codepath leading up to, or just
		 * after, single-stepping of a probed instruction. This entire
		 * codepath should strictly reside in .kprobes.text section.
		 * Raise a BUG or we'll continue in an endless reentering loop
		 * and eventually a stack overflow.
		 */
		pr_err("Unrecoverable kprobe detected.\n");
		dump_kprobe(p);
		BUG();
	default:
		/* impossible cases */
		WARN_ON(1);
		return 0;
	}

	return 1;
}
NOKPROBE_SYMBOL(reenter_kprobe);

/*
 * Interrupts are disabled on entry as trap3 is an interrupt gate and they
 * remain disabled throughout this function.
 */
int kprobe_int3_handler(struct pt_regs *regs)
{
	kprobe_opcode_t *addr;
	struct kprobe *p;
	struct kprobe_ctlblk *kcb;

	if (user_mode(regs))
		return 0;

	addr = (kprobe_opcode_t *)(regs->ip - sizeof(kprobe_opcode_t));
	/*
	 * We don't want to be preempted for the entire duration of kprobe
	 * processing. Since int3 and debug trap disables irqs and we clear
	 * IF while singlestepping, it must be no preemptible.
	 */

	kcb = get_kprobe_ctlblk();
	p = get_kprobe(addr);

	if (p) {
		if (kprobe_running()) {
			if (reenter_kprobe(p, regs, kcb))
				return 1;
		} else {
			set_current_kprobe(p, regs, kcb);
			kcb->kprobe_status = KPROBE_HIT_ACTIVE;

			/*
			 * If we have no pre-handler or it returned 0, we
			 * continue with normal processing.  If we have a
			 * pre-handler and it returned non-zero, that means
			 * user handler setup registers to exit to another
			 * instruction, we must skip the single stepping.
			 */
			if (!p->pre_handler || !p->pre_handler(p, regs))
				setup_singlestep(p, regs, kcb, 0);
			else
				reset_current_kprobe();
			return 1;
		}
	} else if (*addr != BREAKPOINT_INSTRUCTION) {
		/*
		 * The breakpoint instruction was removed right
		 * after we hit it.  Another cpu has removed
		 * either a probepoint or a debugger breakpoint
		 * at this address.  In either case, no further
		 * handling of this interrupt is appropriate.
		 * Back up over the (now missing) int3 and run
		 * the original instruction.
		 */
		regs->ip = (unsigned long)addr;
		return 1;
	} /* else: not a kprobe fault; let the kernel handle it */

	return 0;
}
NOKPROBE_SYMBOL(kprobe_int3_handler);

/*
 * When a retprobed function returns, this code saves registers and
 * calls trampoline_handler() runs, which calls the kretprobe's handler.
 */
asm(
	".text\n"
	".global kretprobe_trampoline\n"
	".type kretprobe_trampoline, @function\n"
	"kretprobe_trampoline:\n"
	/* We don't bother saving the ss register */
#ifdef CONFIG_X86_64
	"	pushq %rsp\n"
	"	pushfq\n"
	SAVE_REGS_STRING
	"	movq %rsp, %rdi\n"
	"	call trampoline_handler\n"
	/* Replace saved sp with true return address. */
	"	movq %rax, 19*8(%rsp)\n"
	RESTORE_REGS_STRING
	"	popfq\n"
#else
	"	pushl %esp\n"
	"	pushfl\n"
	SAVE_REGS_STRING
	"	movl %esp, %eax\n"
	"	call trampoline_handler\n"
	/* Replace saved sp with true return address. */
	"	movl %eax, 15*4(%esp)\n"
	RESTORE_REGS_STRING
	"	popfl\n"
#endif
	"	ret\n"
	".size kretprobe_trampoline, .-kretprobe_trampoline\n"
);
NOKPROBE_SYMBOL(kretprobe_trampoline);
STACK_FRAME_NON_STANDARD(kretprobe_trampoline);

/*
 * Called from kretprobe_trampoline
 */
__used __visible void *trampoline_handler(struct pt_regs *regs)
{
	struct kretprobe_instance *ri = NULL;
	struct hlist_head *head, empty_rp;
	struct hlist_node *tmp;
	unsigned long flags, orig_ret_address = 0;
	unsigned long trampoline_address = (unsigned long)&kretprobe_trampoline;
	kprobe_opcode_t *correct_ret_addr = NULL;
	void *frame_pointer;
	bool skipped = false;

	/*
	 * Set a dummy kprobe for avoiding kretprobe recursion.
	 * Since kretprobe never run in kprobe handler, kprobe must not
	 * be running at this point.
	 */
	kprobe_busy_begin();

	INIT_HLIST_HEAD(&empty_rp);
	kretprobe_hash_lock(current, &head, &flags);
	/* fixup registers */
	regs->cs = __KERNEL_CS;
#ifdef CONFIG_X86_32
	regs->cs |= get_kernel_rpl();
	regs->gs = 0;
#endif
	/* We use pt_regs->sp for return address holder. */
	frame_pointer = &regs->sp;
	regs->ip = trampoline_address;
	regs->orig_ax = ~0UL;

	/*
	 * It is possible to have multiple instances associated with a given
	 * task either because multiple functions in the call path have
	 * return probes installed on them, and/or more than one
	 * return probe was registered for a target function.
	 *
	 * We can handle this because:
	 *     - instances are always pushed into the head of the list
	 *     - when multiple return probes are registered for the same
	 *	 function, the (chronologically) first instance's ret_addr
	 *	 will be the real return address, and all the rest will
	 *	 point to kretprobe_trampoline.
	 */
	hlist_for_each_entry(ri, head, hlist) {
		if (ri->task != current)
			/* another task is sharing our hash bucket */
			continue;
		/*
		 * Return probes must be pushed on this hash list correct
		 * order (same as return order) so that it can be popped
		 * correctly. However, if we find it is pushed it incorrect
		 * order, this means we find a function which should not be
		 * probed, because the wrong order entry is pushed on the
		 * path of processing other kretprobe itself.
		 */
		if (ri->fp != frame_pointer) {
			if (!skipped)
				pr_warn("kretprobe is stacked incorrectly. Trying to fixup.\n");
			skipped = true;
			continue;
		}

		orig_ret_address = (unsigned long)ri->ret_addr;
		if (skipped)
			pr_warn("%ps must be blacklisted because of incorrect kretprobe order\n",
				ri->rp->kp.addr);

		if (orig_ret_address != trampoline_address)
			/*
			 * This is the real return address. Any other
			 * instances associated with this task are for
			 * other calls deeper on the call stack
			 */
			break;
	}

	kretprobe_assert(ri, orig_ret_address, trampoline_address);

	correct_ret_addr = ri->ret_addr;
	hlist_for_each_entry_safe(ri, tmp, head, hlist) {
		if (ri->task != current)
			/* another task is sharing our hash bucket */
			continue;
		if (ri->fp != frame_pointer)
			continue;

		orig_ret_address = (unsigned long)ri->ret_addr;
		if (ri->rp && ri->rp->handler) {
			__this_cpu_write(current_kprobe, &ri->rp->kp);
			ri->ret_addr = correct_ret_addr;
			ri->rp->handler(ri, regs);
			__this_cpu_write(current_kprobe, &kprobe_busy);
		}

		recycle_rp_inst(ri, &empty_rp);

		if (orig_ret_address != trampoline_address)
			/*
			 * This is the real return address. Any other
			 * instances associated with this task are for
			 * other calls deeper on the call stack
			 */
			break;
	}

	kretprobe_hash_unlock(current, &flags);

	kprobe_busy_end();

	hlist_for_each_entry_safe(ri, tmp, &empty_rp, hlist) {
		hlist_del(&ri->hlist);
		kfree(ri);
	}
	return (void *)orig_ret_address;
}
NOKPROBE_SYMBOL(trampoline_handler);

/*
 * Called after single-stepping.  p->addr is the address of the
 * instruction whose first byte has been replaced by the "int 3"
 * instruction.  To avoid the SMP problems that can occur when we
 * temporarily put back the original opcode to single-step, we
 * single-stepped a copy of the instruction.  The address of this
 * copy is p->ainsn.insn.
 *
 * This function prepares to return from the post-single-step
 * interrupt.  We have to fix up the stack as follows:
 *
 * 0) Except in the case of absolute or indirect jump or call instructions,
 * the new ip is relative to the copied instruction.  We need to make
 * it relative to the original instruction.
 *
 * 1) If the single-stepped instruction was pushfl, then the TF and IF
 * flags are set in the just-pushed flags, and may need to be cleared.
 *
 * 2) If the single-stepped instruction was a call, the return address
 * that is atop the stack is the address following the copied instruction.
 * We need to make it the address following the original instruction.
 *
 * If this is the first time we've single-stepped the instruction at
 * this probepoint, and the instruction is boostable, boost it: add a
 * jump instruction after the copied instruction, that jumps to the next
 * instruction after the probepoint.
 */
static void resume_execution(struct kprobe *p, struct pt_regs *regs,
			     struct kprobe_ctlblk *kcb)
{
	unsigned long *tos = stack_addr(regs);
	unsigned long copy_ip = (unsigned long)p->ainsn.insn;
	unsigned long orig_ip = (unsigned long)p->addr;
	kprobe_opcode_t *insn = p->ainsn.insn;

	/* Skip prefixes */
	insn = skip_prefixes(insn);

	regs->flags &= ~X86_EFLAGS_TF;
	switch (*insn) {
	case 0x9c:	/* pushfl */
		*tos &= ~(X86_EFLAGS_TF | X86_EFLAGS_IF);
		*tos |= kcb->kprobe_old_flags;
		break;
	case 0xc2:	/* iret/ret/lret */
	case 0xc3:
	case 0xca:
	case 0xcb:
	case 0xcf:
	case 0xea:	/* jmp absolute -- ip is correct */
		/* ip is already adjusted, no more changes required */
		p->ainsn.boostable = true;
		goto no_change;
	case 0xe8:	/* call relative - Fix return addr */
		*tos = orig_ip + (*tos - copy_ip);
		break;
#ifdef CONFIG_X86_32
	case 0x9a:	/* call absolute -- same as call absolute, indirect */
		*tos = orig_ip + (*tos - copy_ip);
		goto no_change;
#endif
	case 0xff:
		if ((insn[1] & 0x30) == 0x10) {
			/*
			 * call absolute, indirect
			 * Fix return addr; ip is correct.
			 * But this is not boostable
			 */
			*tos = orig_ip + (*tos - copy_ip);
			goto no_change;
		} else if (((insn[1] & 0x31) == 0x20) ||
			   ((insn[1] & 0x31) == 0x21)) {
			/*
			 * jmp near and far, absolute indirect
			 * ip is correct. And this is boostable
			 */
			p->ainsn.boostable = true;
			goto no_change;
		}
	default:
		break;
	}

	regs->ip += orig_ip - copy_ip;

no_change:
	restore_btf();
}
NOKPROBE_SYMBOL(resume_execution);

/*
 * Interrupts are disabled on entry as trap1 is an interrupt gate and they
 * remain disabled throughout this function.
 */
int kprobe_debug_handler(struct pt_regs *regs)
{
	struct kprobe *cur = kprobe_running();
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	if (!cur)
		return 0;

	resume_execution(cur, regs, kcb);
	regs->flags |= kcb->kprobe_saved_flags;

	if ((kcb->kprobe_status != KPROBE_REENTER) && cur->post_handler) {
		kcb->kprobe_status = KPROBE_HIT_SSDONE;
		cur->post_handler(cur, regs, 0);
	}

	/* Restore back the original saved kprobes variables and continue. */
	if (kcb->kprobe_status == KPROBE_REENTER) {
		restore_previous_kprobe(kcb);
		goto out;
	}
	reset_current_kprobe();
out:
	/*
	 * if somebody else is singlestepping across a probe point, flags
	 * will have TF set, in which case, continue the remaining processing
	 * of do_debug, as if this is not a probe hit.
	 */
	if (regs->flags & X86_EFLAGS_TF)
		return 0;

	return 1;
}
NOKPROBE_SYMBOL(kprobe_debug_handler);

int kprobe_fault_handler(struct pt_regs *regs, int trapnr)
{
	struct kprobe *cur = kprobe_running();
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	if (unlikely(regs->ip == (unsigned long)cur->ainsn.insn)) {
		/* This must happen on single-stepping */
		WARN_ON(kcb->kprobe_status != KPROBE_HIT_SS &&
			kcb->kprobe_status != KPROBE_REENTER);
		/*
		 * We are here because the instruction being single
		 * stepped caused a page fault. We reset the current
		 * kprobe and the ip points back to the probe address
		 * and allow the page fault handler to continue as a
		 * normal page fault.
		 */
		regs->ip = (unsigned long)cur->addr;
		/*
		 * Trap flag (TF) has been set here because this fault
		 * happened where the single stepping will be done.
		 * So clear it by resetting the current kprobe:
		 */
		regs->flags &= ~X86_EFLAGS_TF;

		/*
		 * If the TF flag was set before the kprobe hit,
		 * don't touch it:
		 */
		regs->flags |= kcb->kprobe_old_flags;

		if (kcb->kprobe_status == KPROBE_REENTER)
			restore_previous_kprobe(kcb);
		else
			reset_current_kprobe();
	} else if (kcb->kprobe_status == KPROBE_HIT_ACTIVE ||
		   kcb->kprobe_status == KPROBE_HIT_SSDONE) {
		/*
		 * We increment the nmissed count for accounting,
		 * we can also use npre/npostfault count for accounting
		 * these specific fault cases.
		 */
		kprobes_inc_nmissed_count(cur);

		/*
		 * We come here because instructions in the pre/post
		 * handler caused the page_fault, this could happen
		 * if handler tries to access user space by
		 * copy_from_user(), get_user() etc. Let the
		 * user-specified handler try to fix it first.
		 */
		if (cur->fault_handler && cur->fault_handler(cur, regs, trapnr))
			return 1;
	}

	return 0;
}
NOKPROBE_SYMBOL(kprobe_fault_handler);

int __init arch_populate_kprobe_blacklist(void)
{
	int ret;

	ret = kprobe_add_area_blacklist((unsigned long)__irqentry_text_start,
					 (unsigned long)__irqentry_text_end);
	if (ret)
		return ret;

	return kprobe_add_area_blacklist((unsigned long)__entry_text_start,
					 (unsigned long)__entry_text_end);
}

int __init arch_init_kprobes(void)
{
	return 0;
}

int arch_trampoline_kprobe(struct kprobe *p)
{
	return 0;
}
