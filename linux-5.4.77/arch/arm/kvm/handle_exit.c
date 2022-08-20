// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 - Virtual Open Systems and Columbia University
 * Author: Christoffer Dall <c.dall@virtualopensystems.com>
 */

#include <linux/kvm.h>
#include <linux/kvm_host.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_coproc.h>
#include <asm/kvm_mmu.h>
#include <kvm/arm_psci.h>
#include <trace/events/kvm.h>

#include "trace.h"

typedef int (*exit_handle_fn)(struct kvm_vcpu *, struct kvm_run *);

static int handle_hvc(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	int ret;

	trace_kvm_hvc(*vcpu_pc(vcpu), *vcpu_reg(vcpu, 0),
		      kvm_vcpu_hvc_get_imm(vcpu));
	vcpu->stat.hvc_exit_stat++;

	ret = kvm_hvc_call_handler(vcpu);
	if (ret < 0) {
		vcpu_set_reg(vcpu, 0, ~0UL);
		return 1;
	}

	return ret;
}

static int handle_smc(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	/*
	 * "If an SMC instruction executed at Non-secure EL1 is
	 * trapped to EL2 because HCR_EL2.TSC is 1, the exception is a
	 * Trap exception, not a Secure Monitor Call exception [...]"
	 *
	 * We need to advance the PC after the trap, as it would
	 * otherwise return to the same address...
	 */
	vcpu_set_reg(vcpu, 0, ~0UL);
	kvm_skip_instr(vcpu, kvm_vcpu_trap_il_is32bit(vcpu));
	return 1;
}

/**
 * kvm_handle_wfx - handle a WFI or WFE instructions trapped in guests
 * @vcpu:	the vcpu pointer
 * @run:	the kvm_run structure pointer
 *
 * WFE: Yield the CPU and come back to this vcpu when the scheduler
 * decides to.
 * WFI: Simply call kvm_vcpu_block(), which will halt execution of
 * world-switches and schedule other host processes until there is an
 * incoming IRQ or FIQ to the VM.
 */
static int kvm_handle_wfx(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	if (kvm_vcpu_get_hsr(vcpu) & HSR_WFI_IS_WFE) {
		trace_kvm_wfx(*vcpu_pc(vcpu), true);
		vcpu->stat.wfe_exit_stat++;
		kvm_vcpu_on_spin(vcpu, vcpu_mode_priv(vcpu));
	} else {
		trace_kvm_wfx(*vcpu_pc(vcpu), false);
		vcpu->stat.wfi_exit_stat++;
		kvm_vcpu_block(vcpu);
		kvm_clear_request(KVM_REQ_UNHALT, vcpu);
	}

	kvm_skip_instr(vcpu, kvm_vcpu_trap_il_is32bit(vcpu));

	return 1;
}

static int kvm_handle_unknown_ec(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
	u32 hsr = kvm_vcpu_get_hsr(vcpu);

	kvm_pr_unimpl("Unknown exception class: hsr: %#08x\n",
		      hsr);

	kvm_inject_undefined(vcpu);
	return 1;
}

static exit_handle_fn arm_exit_handlers[] = {
	[0 ... HSR_EC_MAX]	= kvm_handle_unknown_ec,
	[HSR_EC_WFI]		= kvm_handle_wfx,
	[HSR_EC_CP15_32]	= kvm_handle_cp15_32,
	[HSR_EC_CP15_64]	= kvm_handle_cp15_64,
	[HSR_EC_CP14_MR]	= kvm_handle_cp14_32,
	[HSR_EC_CP14_LS]	= kvm_handle_cp14_load_store,
	[HSR_EC_CP14_64]	= kvm_handle_cp14_64,
	[HSR_EC_CP_0_13]	= kvm_handle_cp_0_13_access,
	[HSR_EC_CP10_ID]	= kvm_handle_cp10_id,
	[HSR_EC_HVC]		= handle_hvc,
	[HSR_EC_SMC]		= handle_smc,
	[HSR_EC_IABT]		= kvm_handle_guest_abort,
	[HSR_EC_DABT]		= kvm_handle_guest_abort,
};

static exit_handle_fn kvm_get_exit_handler(struct kvm_vcpu *vcpu)
{
	u8 hsr_ec = kvm_vcpu_trap_get_class(vcpu);

	return arm_exit_handlers[hsr_ec];
}

/*
 * Return > 0 to return to guest, < 0 on error, 0 (and set exit_reason) on
 * proper exit to userspace.
 */
int handle_exit(struct kvm_vcpu *vcpu, struct kvm_run *run,
		       int exception_index)
{
	exit_handle_fn exit_handler;

	if (ARM_ABORT_PENDING(exception_index)) {
		u8 hsr_ec = kvm_vcpu_trap_get_class(vcpu);

		/*
		 * HVC/SMC already have an adjusted PC, which we need
		 * to correct in order to return to after having
		 * injected the abort.
		 */
		if (hsr_ec == HSR_EC_HVC || hsr_ec == HSR_EC_SMC) {
			u32 adj =  kvm_vcpu_trap_il_is32bit(vcpu) ? 4 : 2;
			*vcpu_pc(vcpu) -= adj;
		}

		kvm_inject_vabt(vcpu);
		return 1;
	}

	exception_index = ARM_EXCEPTION_CODE(exception_index);

	switch (exception_index) {
	case ARM_EXCEPTION_IRQ:
		return 1;
	case ARM_EXCEPTION_HVC:
		/*
		 * See ARM ARM B1.14.1: "Hyp traps on instructions
		 * that fail their condition code check"
		 */
		if (!kvm_condition_valid(vcpu)) {
			kvm_skip_instr(vcpu, kvm_vcpu_trap_il_is32bit(vcpu));
			return 1;
		}

		exit_handler = kvm_get_exit_handler(vcpu);

		return exit_handler(vcpu, run);
	case ARM_EXCEPTION_DATA_ABORT:
		kvm_inject_vabt(vcpu);
		return 1;
	case ARM_EXCEPTION_HYP_GONE:
		/*
		 * HYP has been reset to the hyp-stub. This happens
		 * when a guest is pre-empted by kvm_reboot()'s
		 * shutdown call.
		 */
		run->exit_reason = KVM_EXIT_FAIL_ENTRY;
		return 0;
	default:
		kvm_pr_unimpl("Unsupported exception type: %d",
			      exception_index);
		run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		return 0;
	}
}
