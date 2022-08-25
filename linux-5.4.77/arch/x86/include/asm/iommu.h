/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_IOMMU_H
#define _ASM_X86_IOMMU_H

extern int force_iommu, no_iommu;
extern int iommu_detected;

/* 10 seconds */
#define DMAR_OPERATION_TIMEOUT ((cycles_t) tsc_khz*10*1000)

#endif /* _ASM_X86_IOMMU_H */
