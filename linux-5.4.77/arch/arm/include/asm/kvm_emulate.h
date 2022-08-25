/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 - Virtual Open Systems and Columbia University
 * Author: Christoffer Dall <c.dall@virtualopensystems.com>
 */

#ifndef __ARM_KVM_EMULATE_H__
#define __ARM_KVM_EMULATE_H__

#include <linux/kvm_host.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_mmio.h>
#include <asm/kvm_arm.h>
#include <asm/cputype.h>

/* arm64 compatibility macros */
#define PSR_AA32_MODE_FIQ	FIQ_MODE
#define PSR_AA32_MODE_SVC	SVC_MODE
#define PSR_AA32_MODE_ABT	ABT_MODE
#define PSR_AA32_MODE_UND	UND_MODE
#define PSR_AA32_T_BIT		PSR_T_BIT
#define PSR_AA32_F_BIT		PSR_F_BIT
#define PSR_AA32_I_BIT		PSR_I_BIT
#define PSR_AA32_A_BIT		PSR_A_BIT
#define PSR_AA32_E_BIT		PSR_E_BIT
#define PSR_AA32_IT_MASK	PSR_IT_MASK
#define PSR_AA32_GE_MASK	0x000f0000
#define PSR_AA32_DIT_BIT	0x00200000
#define PSR_AA32_PAN_BIT	0x00400000
#define PSR_AA32_SSBS_BIT	0x00800000
#define PSR_AA32_Q_BIT		PSR_Q_BIT
#define PSR_AA32_V_BIT		PSR_V_BIT
#define PSR_AA32_C_BIT		PSR_C_BIT
#define PSR_AA32_Z_BIT		PSR_Z_BIT
#define PSR_AA32_N_BIT		PSR_N_BIT

unsigned long *vcpu_reg(struct kvm_vcpu *vcpu, u8 reg_num);

static inline unsigned long *vcpu_reg32(struct kvm_vcpu *vcpu, u8 reg_num)
{
	return vcpu_reg(vcpu, reg_num);
}

unsigned long *__vcpu_spsr(struct kvm_vcpu *vcpu);

static inline unsigned long vpcu_read_spsr(struct kvm_vcpu *vcpu)
{
	return *__vcpu_spsr(vcpu);
}

static inline void vcpu_write_spsr(struct kvm_vcpu *vcpu, unsigned long v)
{
	*__vcpu_spsr(vcpu) = v;
}

static inline unsigned long host_spsr_to_spsr32(unsigned long spsr)
{
	return spsr;
}

static inline unsigned long vcpu_get_reg(struct kvm_vcpu *vcpu,
					 u8 reg_num)
{
	return *vcpu_reg(vcpu, reg_num);
}

static inline void vcpu_set_reg(struct kvm_vcpu *vcpu, u8 reg_num,
				unsigned long val)
{
	*vcpu_reg(vcpu, reg_num) = val;
}

bool kvm_condition_valid32(const struct kvm_vcpu *vcpu);
void kvm_skip_instr32(struct kvm_vcpu *vcpu, bool is_wide_instr);
void kvm_inject_undef32(struct kvm_vcpu *vcpu);
void kvm_inject_dabt32(struct kvm_vcpu *vcpu, unsigned long addr);
void kvm_inject_pabt32(struct kvm_vcpu *vcpu, unsigned long addr);
void kvm_inject_vabt(struct kvm_vcpu *vcpu);

static inline void kvm_inject_undefined(struct kvm_vcpu *vcpu)
{
	kvm_inject_undef32(vcpu);
}

static inline void kvm_inject_dabt(struct kvm_vcpu *vcpu, unsigned long addr)
{
	kvm_inject_dabt32(vcpu, addr);
}

static inline void kvm_inject_pabt(struct kvm_vcpu *vcpu, unsigned long addr)
{
	kvm_inject_pabt32(vcpu, addr);
}

static inline bool kvm_condition_valid(const struct kvm_vcpu *vcpu)
{
	return kvm_condition_valid32(vcpu);
}

static inline void kvm_skip_instr(struct kvm_vcpu *vcpu, bool is_wide_instr)
{
	kvm_skip_instr32(vcpu, is_wide_instr);
}

static inline void vcpu_reset_hcr(struct kvm_vcpu *vcpu)
{
	vcpu->arch.hcr = HCR_GUEST_MASK;
}

static inline unsigned long *vcpu_hcr(const struct kvm_vcpu *vcpu)
{
	return (unsigned long *)&vcpu->arch.hcr;
}

static inline void vcpu_clear_wfe_traps(struct kvm_vcpu *vcpu)
{
	vcpu->arch.hcr &= ~HCR_TWE;
}

static inline void vcpu_set_wfe_traps(struct kvm_vcpu *vcpu)
{
	vcpu->arch.hcr |= HCR_TWE;
}

static inline bool vcpu_mode_is_32bit(const struct kvm_vcpu *vcpu)
{
	return true;
}

static inline unsigned long *vcpu_pc(struct kvm_vcpu *vcpu)
{
	return &vcpu->arch.ctxt.gp_regs.usr_regs.ARM_pc;
}

static inline unsigned long *vcpu_cpsr(const struct kvm_vcpu *vcpu)
{
	return (unsigned long *)&vcpu->arch.ctxt.gp_regs.usr_regs.ARM_cpsr;
}

static inline void vcpu_set_thumb(struct kvm_vcpu *vcpu)
{
	*vcpu_cpsr(vcpu) |= PSR_T_BIT;
}

static inline bool mode_has_spsr(struct kvm_vcpu *vcpu)
{
	unsigned long cpsr_mode = vcpu->arch.ctxt.gp_regs.usr_regs.ARM_cpsr & MODE_MASK;
	return (cpsr_mode > USR_MODE && cpsr_mode < SYSTEM_MODE);
}

static inline bool vcpu_mode_priv(struct kvm_vcpu *vcpu)
{
	unsigned long cpsr_mode = vcpu->arch.ctxt.gp_regs.usr_regs.ARM_cpsr & MODE_MASK;
	return cpsr_mode > USR_MODE;
}

static inline u32 kvm_vcpu_get_hsr(const struct kvm_vcpu *vcpu)
{
	return vcpu->arch.fault.hsr;
}

static inline int kvm_vcpu_get_condition(const struct kvm_vcpu *vcpu)
{
	u32 hsr = kvm_vcpu_get_hsr(vcpu);

	if (hsr & HSR_CV)
		return (hsr & HSR_COND) >> HSR_COND_SHIFT;

	return -1;
}

static inline unsigned long kvm_vcpu_get_hfar(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.fault.hxfar;
}

static inline phys_addr_t kvm_vcpu_get_fault_ipa(struct kvm_vcpu *vcpu)
{
	return ((phys_addr_t)vcpu->arch.fault.hpfar & HPFAR_MASK) << 8;
}

static inline bool kvm_vcpu_dabt_isvalid(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) & HSR_ISV;
}

static inline bool kvm_vcpu_dabt_iswrite(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) & HSR_WNR;
}

static inline bool kvm_vcpu_dabt_issext(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) & HSR_SSE;
}

static inline bool kvm_vcpu_dabt_issf(const struct kvm_vcpu *vcpu)
{
	return false;
}

static inline int kvm_vcpu_dabt_get_rd(struct kvm_vcpu *vcpu)
{
	return (kvm_vcpu_get_hsr(vcpu) & HSR_SRT_MASK) >> HSR_SRT_SHIFT;
}

static inline bool kvm_vcpu_abt_iss1tw(const struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) & HSR_DABT_S1PTW;
}

static inline bool kvm_vcpu_dabt_is_cm(struct kvm_vcpu *vcpu)
{
	return !!(kvm_vcpu_get_hsr(vcpu) & HSR_DABT_CM);
}

/* Get Access Size from a data abort */
static inline int kvm_vcpu_dabt_get_as(struct kvm_vcpu *vcpu)
{
	switch ((kvm_vcpu_get_hsr(vcpu) >> 22) & 0x3) {
	case 0:
		return 1;
	case 1:
		return 2;
	case 2:
		return 4;
	default:
		kvm_err("Hardware is weird: SAS 0b11 is reserved\n");
		return -EFAULT;
	}
}

/* This one is not specific to Data Abort */
static inline bool kvm_vcpu_trap_il_is32bit(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) & HSR_IL;
}

static inline u8 kvm_vcpu_trap_get_class(const struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) >> HSR_EC_SHIFT;
}

static inline bool kvm_vcpu_trap_is_iabt(const struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_trap_get_class(vcpu) == HSR_EC_IABT;
}

static inline bool kvm_vcpu_trap_is_exec_fault(const struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_trap_is_iabt(vcpu) && !kvm_vcpu_abt_iss1tw(vcpu);
}

static inline u8 kvm_vcpu_trap_get_fault(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) & HSR_FSC;
}

static inline u8 kvm_vcpu_trap_get_fault_type(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) & HSR_FSC_TYPE;
}

static inline bool kvm_vcpu_dabt_isextabt(struct kvm_vcpu *vcpu)
{
	switch (kvm_vcpu_trap_get_fault(vcpu)) {
	case FSC_SEA:
	case FSC_SEA_TTW0:
	case FSC_SEA_TTW1:
	case FSC_SEA_TTW2:
	case FSC_SEA_TTW3:
	case FSC_SECC:
	case FSC_SECC_TTW0:
	case FSC_SECC_TTW1:
	case FSC_SECC_TTW2:
	case FSC_SECC_TTW3:
		return true;
	default:
		return false;
	}
}

static inline bool kvm_is_write_fault(struct kvm_vcpu *vcpu)
{
	if (kvm_vcpu_trap_is_iabt(vcpu))
		return false;

	return kvm_vcpu_dabt_iswrite(vcpu);
}

static inline u32 kvm_vcpu_hvc_get_imm(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_get_hsr(vcpu) & HSR_HVC_IMM_MASK;
}

static inline unsigned long kvm_vcpu_get_mpidr_aff(struct kvm_vcpu *vcpu)
{
	return vcpu_cp15(vcpu, c0_MPIDR) & MPIDR_HWID_BITMASK;
}

static inline bool kvm_arm_get_vcpu_workaround_2_flag(struct kvm_vcpu *vcpu)
{
	return false;
}

static inline void kvm_arm_set_vcpu_workaround_2_flag(struct kvm_vcpu *vcpu,
						      bool flag)
{
}

static inline void kvm_vcpu_set_be(struct kvm_vcpu *vcpu)
{
	*vcpu_cpsr(vcpu) |= PSR_E_BIT;
}

static inline bool kvm_vcpu_is_be(struct kvm_vcpu *vcpu)
{
	return !!(*vcpu_cpsr(vcpu) & PSR_E_BIT);
}

static inline unsigned long vcpu_data_guest_to_host(struct kvm_vcpu *vcpu,
						    unsigned long data,
						    unsigned int len)
{
	if (kvm_vcpu_is_be(vcpu)) {
		switch (len) {
		case 1:
			return data & 0xff;
		case 2:
			return be16_to_cpu(data & 0xffff);
		default:
			return be32_to_cpu(data);
		}
	} else {
		switch (len) {
		case 1:
			return data & 0xff;
		case 2:
			return le16_to_cpu(data & 0xffff);
		default:
			return le32_to_cpu(data);
		}
	}
}

static inline unsigned long vcpu_data_host_to_guest(struct kvm_vcpu *vcpu,
						    unsigned long data,
						    unsigned int len)
{
	if (kvm_vcpu_is_be(vcpu)) {
		switch (len) {
		case 1:
			return data & 0xff;
		case 2:
			return cpu_to_be16(data & 0xffff);
		default:
			return cpu_to_be32(data);
		}
	} else {
		switch (len) {
		case 1:
			return data & 0xff;
		case 2:
			return cpu_to_le16(data & 0xffff);
		default:
			return cpu_to_le32(data);
		}
	}
}

static inline bool vcpu_has_ptrauth(struct kvm_vcpu *vcpu) { return false; }
static inline void vcpu_ptrauth_disable(struct kvm_vcpu *vcpu) { }

#endif /* __ARM_KVM_EMULATE_H__ */
