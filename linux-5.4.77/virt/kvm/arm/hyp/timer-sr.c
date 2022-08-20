// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012-2015 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#include <clocksource/arm_arch_timer.h>
#include <linux/compiler.h>
#include <linux/kvm_host.h>

#include <asm/kvm_hyp.h>

void __hyp_text __kvm_timer_set_cntvoff(u32 cntvoff_low, u32 cntvoff_high)
{
	u64 cntvoff = (u64)cntvoff_high << 32 | cntvoff_low;
	write_sysreg(cntvoff, cntvoff_el2);
}

/*
 * Should only be called on non-VHE systems.
 * VHE systems use EL2 timers and configure EL1 timers in kvm_timer_init_vhe().
 */
void __hyp_text __timer_disable_traps(struct kvm_vcpu *vcpu)
{
	u64 val;

	/* Allow physical timer/counter access for the host */
	val = read_sysreg(cnthctl_el2);
	val |= CNTHCTL_EL1PCTEN | CNTHCTL_EL1PCEN;
	write_sysreg(val, cnthctl_el2);
}

/*
 * Should only be called on non-VHE systems.
 * VHE systems use EL2 timers and configure EL1 timers in kvm_timer_init_vhe().
 */
void __hyp_text __timer_enable_traps(struct kvm_vcpu *vcpu)
{
	u64 val;

	/*
	 * Disallow physical timer access for the guest
	 * Physical counter access is allowed
	 */
	val = read_sysreg(cnthctl_el2);
	val &= ~CNTHCTL_EL1PCEN;
	val |= CNTHCTL_EL1PCTEN;
	write_sysreg(val, cnthctl_el2);
}
