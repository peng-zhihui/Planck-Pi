// SPDX-License-Identifier: GPL-2.0
/*
 * Test for x86 KVM_CAP_HYPERV_CPUID
 *
 * Copyright (C) 2018, Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 */

#define _GNU_SOURCE /* for program_invocation_short_name */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "vmx.h"

#define VCPU_ID 0

static void guest_code(void)
{
}

static int smt_possible(void)
{
	char buf[16];
	FILE *f;
	bool res = 1;

	f = fopen("/sys/devices/system/cpu/smt/control", "r");
	if (f) {
		if (fread(buf, sizeof(*buf), sizeof(buf), f) > 0) {
			if (!strncmp(buf, "forceoff", 8) ||
			    !strncmp(buf, "notsupported", 12))
				res = 0;
		}
		fclose(f);
	}

	return res;
}

static void test_hv_cpuid(struct kvm_cpuid2 *hv_cpuid_entries,
			  int evmcs_enabled)
{
	int i;

	if (!evmcs_enabled)
		TEST_ASSERT(hv_cpuid_entries->nent == 6,
			    "KVM_GET_SUPPORTED_HV_CPUID should return 6 entries"
			    " when Enlightened VMCS is disabled (returned %d)",
			    hv_cpuid_entries->nent);
	else
		TEST_ASSERT(hv_cpuid_entries->nent == 7,
			    "KVM_GET_SUPPORTED_HV_CPUID should return 7 entries"
			    " when Enlightened VMCS is enabled (returned %d)",
			    hv_cpuid_entries->nent);

	for (i = 0; i < hv_cpuid_entries->nent; i++) {
		struct kvm_cpuid_entry2 *entry = &hv_cpuid_entries->entries[i];

		TEST_ASSERT((entry->function >= 0x40000000) &&
			    (entry->function <= 0x4000000A),
			    "function %lx is our of supported range",
			    entry->function);

		TEST_ASSERT(entry->index == 0,
			    ".index field should be zero");

		TEST_ASSERT(entry->flags == 0,
			    ".flags field should be zero");

		TEST_ASSERT(!entry->padding[0] && !entry->padding[1] &&
			    !entry->padding[2], "padding should be zero");

		if (entry->function == 0x40000004) {
			int nononarchcs = !!(entry->eax & (1UL << 18));

			TEST_ASSERT(nononarchcs == !smt_possible(),
				    "NoNonArchitecturalCoreSharing bit"
				    " doesn't reflect SMT setting");
		}

		/*
		 * If needed for debug:
		 * fprintf(stdout,
		 *	"CPUID%lx EAX=0x%lx EBX=0x%lx ECX=0x%lx EDX=0x%lx\n",
		 *	entry->function, entry->eax, entry->ebx, entry->ecx,
		 *	entry->edx);
		 */
	}

}

void test_hv_cpuid_e2big(struct kvm_vm *vm)
{
	static struct kvm_cpuid2 cpuid = {.nent = 0};
	int ret;

	ret = _vcpu_ioctl(vm, VCPU_ID, KVM_GET_SUPPORTED_HV_CPUID, &cpuid);

	TEST_ASSERT(ret == -1 && errno == E2BIG,
		    "KVM_GET_SUPPORTED_HV_CPUID didn't fail with -E2BIG when"
		    " it should have: %d %d", ret, errno);
}


struct kvm_cpuid2 *kvm_get_supported_hv_cpuid(struct kvm_vm *vm)
{
	int nent = 20; /* should be enough */
	static struct kvm_cpuid2 *cpuid;

	cpuid = malloc(sizeof(*cpuid) + nent * sizeof(struct kvm_cpuid_entry2));

	if (!cpuid) {
		perror("malloc");
		abort();
	}

	cpuid->nent = nent;

	vcpu_ioctl(vm, VCPU_ID, KVM_GET_SUPPORTED_HV_CPUID, cpuid);

	return cpuid;
}


int main(int argc, char *argv[])
{
	struct kvm_vm *vm;
	int rv;
	struct kvm_cpuid2 *hv_cpuid_entries;

	/* Tell stdout not to buffer its content */
	setbuf(stdout, NULL);

	rv = kvm_check_cap(KVM_CAP_HYPERV_CPUID);
	if (!rv) {
		fprintf(stderr,
			"KVM_CAP_HYPERV_CPUID not supported, skip test\n");
		exit(KSFT_SKIP);
	}

	/* Create VM */
	vm = vm_create_default(VCPU_ID, 0, guest_code);

	test_hv_cpuid_e2big(vm);

	hv_cpuid_entries = kvm_get_supported_hv_cpuid(vm);
	if (!hv_cpuid_entries)
		return 1;

	test_hv_cpuid(hv_cpuid_entries, 0);

	free(hv_cpuid_entries);

	if (!kvm_check_cap(KVM_CAP_HYPERV_ENLIGHTENED_VMCS)) {
		fprintf(stderr,
			"Enlightened VMCS is unsupported, skip related test\n");
		goto vm_free;
	}

	vcpu_enable_evmcs(vm, VCPU_ID);

	hv_cpuid_entries = kvm_get_supported_hv_cpuid(vm);
	if (!hv_cpuid_entries)
		return 1;

	test_hv_cpuid(hv_cpuid_entries, 1);

	free(hv_cpuid_entries);

vm_free:
	kvm_vm_free(vm);

	return 0;
}
