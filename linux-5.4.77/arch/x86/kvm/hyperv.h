/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * KVM Microsoft Hyper-V emulation
 *
 * derived from arch/x86/kvm/x86.c
 *
 * Copyright (C) 2006 Qumranet, Inc.
 * Copyright (C) 2008 Qumranet, Inc.
 * Copyright IBM Corporation, 2008
 * Copyright 2010 Red Hat, Inc. and/or its affiliates.
 * Copyright (C) 2015 Andrey Smetanin <asmetanin@virtuozzo.com>
 *
 * Authors:
 *   Avi Kivity   <avi@qumranet.com>
 *   Yaniv Kamay  <yaniv@qumranet.com>
 *   Amit Shah    <amit.shah@qumranet.com>
 *   Ben-Ami Yassour <benami@il.ibm.com>
 *   Andrey Smetanin <asmetanin@virtuozzo.com>
 */

#ifndef __ARCH_X86_KVM_HYPERV_H__
#define __ARCH_X86_KVM_HYPERV_H__

#include <linux/kvm_host.h>

static inline struct kvm_vcpu_hv *vcpu_to_hv_vcpu(struct kvm_vcpu *vcpu)
{
	return &vcpu->arch.hyperv;
}

static inline struct kvm_vcpu *hv_vcpu_to_vcpu(struct kvm_vcpu_hv *hv_vcpu)
{
	struct kvm_vcpu_arch *arch;

	arch = container_of(hv_vcpu, struct kvm_vcpu_arch, hyperv);
	return container_of(arch, struct kvm_vcpu, arch);
}

static inline struct kvm_vcpu_hv_synic *vcpu_to_synic(struct kvm_vcpu *vcpu)
{
	return &vcpu->arch.hyperv.synic;
}

static inline struct kvm_vcpu *synic_to_vcpu(struct kvm_vcpu_hv_synic *synic)
{
	return hv_vcpu_to_vcpu(container_of(synic, struct kvm_vcpu_hv, synic));
}

int kvm_hv_set_msr_common(struct kvm_vcpu *vcpu, u32 msr, u64 data, bool host);
int kvm_hv_get_msr_common(struct kvm_vcpu *vcpu, u32 msr, u64 *pdata, bool host);

bool kvm_hv_hypercall_enabled(struct kvm *kvm);
int kvm_hv_hypercall(struct kvm_vcpu *vcpu);

void kvm_hv_irq_routing_update(struct kvm *kvm);
int kvm_hv_synic_set_irq(struct kvm *kvm, u32 vcpu_id, u32 sint);
void kvm_hv_synic_send_eoi(struct kvm_vcpu *vcpu, int vector);
int kvm_hv_activate_synic(struct kvm_vcpu *vcpu, bool dont_zero_synic_pages);

void kvm_hv_vcpu_init(struct kvm_vcpu *vcpu);
void kvm_hv_vcpu_postcreate(struct kvm_vcpu *vcpu);
void kvm_hv_vcpu_uninit(struct kvm_vcpu *vcpu);

bool kvm_hv_assist_page_enabled(struct kvm_vcpu *vcpu);
bool kvm_hv_get_assist_page(struct kvm_vcpu *vcpu,
			    struct hv_vp_assist_page *assist_page);

static inline struct kvm_vcpu_hv_stimer *vcpu_to_stimer(struct kvm_vcpu *vcpu,
							int timer_index)
{
	return &vcpu_to_hv_vcpu(vcpu)->stimer[timer_index];
}

static inline struct kvm_vcpu *stimer_to_vcpu(struct kvm_vcpu_hv_stimer *stimer)
{
	struct kvm_vcpu_hv *hv_vcpu;

	hv_vcpu = container_of(stimer - stimer->index, struct kvm_vcpu_hv,
			       stimer[0]);
	return hv_vcpu_to_vcpu(hv_vcpu);
}

static inline bool kvm_hv_has_stimer_pending(struct kvm_vcpu *vcpu)
{
	return !bitmap_empty(vcpu->arch.hyperv.stimer_pending_bitmap,
			     HV_SYNIC_STIMER_COUNT);
}

void kvm_hv_process_stimers(struct kvm_vcpu *vcpu);

void kvm_hv_setup_tsc_page(struct kvm *kvm,
			   struct pvclock_vcpu_time_info *hv_clock);

void kvm_hv_init_vm(struct kvm *kvm);
void kvm_hv_destroy_vm(struct kvm *kvm);
int kvm_vm_ioctl_hv_eventfd(struct kvm *kvm, struct kvm_hyperv_eventfd *args);
int kvm_vcpu_ioctl_get_hv_cpuid(struct kvm_vcpu *vcpu, struct kvm_cpuid2 *cpuid,
				struct kvm_cpuid_entry2 __user *entries);

#endif
