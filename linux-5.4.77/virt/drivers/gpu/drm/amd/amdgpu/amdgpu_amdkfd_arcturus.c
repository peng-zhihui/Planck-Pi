/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#undef pr_fmt
#define pr_fmt(fmt) "kfd2kgd: " fmt

#include <linux/module.h>
#include <linux/fdtable.h>
#include <linux/uaccess.h>
#include <linux/mmu_context.h>
#include <linux/firmware.h>
#include "amdgpu.h"
#include "amdgpu_amdkfd.h"
#include "sdma0/sdma0_4_2_2_offset.h"
#include "sdma0/sdma0_4_2_2_sh_mask.h"
#include "sdma1/sdma1_4_2_2_offset.h"
#include "sdma1/sdma1_4_2_2_sh_mask.h"
#include "sdma2/sdma2_4_2_2_offset.h"
#include "sdma2/sdma2_4_2_2_sh_mask.h"
#include "sdma3/sdma3_4_2_2_offset.h"
#include "sdma3/sdma3_4_2_2_sh_mask.h"
#include "sdma4/sdma4_4_2_2_offset.h"
#include "sdma4/sdma4_4_2_2_sh_mask.h"
#include "sdma5/sdma5_4_2_2_offset.h"
#include "sdma5/sdma5_4_2_2_sh_mask.h"
#include "sdma6/sdma6_4_2_2_offset.h"
#include "sdma6/sdma6_4_2_2_sh_mask.h"
#include "sdma7/sdma7_4_2_2_offset.h"
#include "sdma7/sdma7_4_2_2_sh_mask.h"
#include "v9_structs.h"
#include "soc15.h"
#include "soc15d.h"
#include "amdgpu_amdkfd_gfx_v9.h"

#define HQD_N_REGS 56
#define DUMP_REG(addr) do {				\
		if (WARN_ON_ONCE(i >= HQD_N_REGS))	\
			break;				\
		(*dump)[i][0] = (addr) << 2;		\
		(*dump)[i++][1] = RREG32(addr);		\
	} while (0)

static inline struct amdgpu_device *get_amdgpu_device(struct kgd_dev *kgd)
{
	return (struct amdgpu_device *)kgd;
}

static inline struct v9_sdma_mqd *get_sdma_mqd(void *mqd)
{
	return (struct v9_sdma_mqd *)mqd;
}

static uint32_t get_sdma_base_addr(struct amdgpu_device *adev,
				unsigned int engine_id,
				unsigned int queue_id)
{
	uint32_t base[8] = {
		SOC15_REG_OFFSET(SDMA0, 0,
				 mmSDMA0_RLC0_RB_CNTL) - mmSDMA0_RLC0_RB_CNTL,
		SOC15_REG_OFFSET(SDMA1, 0,
				 mmSDMA1_RLC0_RB_CNTL) - mmSDMA1_RLC0_RB_CNTL,
		SOC15_REG_OFFSET(SDMA2, 0,
				 mmSDMA2_RLC0_RB_CNTL) - mmSDMA2_RLC0_RB_CNTL,
		SOC15_REG_OFFSET(SDMA3, 0,
				 mmSDMA3_RLC0_RB_CNTL) - mmSDMA3_RLC0_RB_CNTL,
		SOC15_REG_OFFSET(SDMA4, 0,
				 mmSDMA4_RLC0_RB_CNTL) - mmSDMA4_RLC0_RB_CNTL,
		SOC15_REG_OFFSET(SDMA5, 0,
				 mmSDMA5_RLC0_RB_CNTL) - mmSDMA5_RLC0_RB_CNTL,
		SOC15_REG_OFFSET(SDMA6, 0,
				 mmSDMA6_RLC0_RB_CNTL) - mmSDMA6_RLC0_RB_CNTL,
		SOC15_REG_OFFSET(SDMA7, 0,
				 mmSDMA7_RLC0_RB_CNTL) - mmSDMA7_RLC0_RB_CNTL
	};
	uint32_t retval;

	retval = base[engine_id] + queue_id * (mmSDMA0_RLC1_RB_CNTL -
					       mmSDMA0_RLC0_RB_CNTL);

	pr_debug("sdma base address: 0x%x\n", retval);

	return retval;
}

static u32 sdma_v4_0_get_reg_offset(struct amdgpu_device *adev,
		u32 instance, u32 offset)
{
	switch (instance) {
	case 0:
		return (adev->reg_offset[SDMA0_HWIP][0][0] + offset);
	case 1:
		return (adev->reg_offset[SDMA1_HWIP][0][1] + offset);
	case 2:
		return (adev->reg_offset[SDMA2_HWIP][0][1] + offset);
	case 3:
		return (adev->reg_offset[SDMA3_HWIP][0][1] + offset);
	case 4:
		return (adev->reg_offset[SDMA4_HWIP][0][1] + offset);
	case 5:
		return (adev->reg_offset[SDMA5_HWIP][0][1] + offset);
	case 6:
		return (adev->reg_offset[SDMA6_HWIP][0][1] + offset);
	case 7:
		return (adev->reg_offset[SDMA7_HWIP][0][1] + offset);
	default:
		break;
	}
	return 0;
}

static int kgd_hqd_sdma_load(struct kgd_dev *kgd, void *mqd,
			     uint32_t __user *wptr, struct mm_struct *mm)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	struct v9_sdma_mqd *m;
	uint32_t sdma_base_addr, sdmax_gfx_context_cntl;
	unsigned long end_jiffies;
	uint32_t data;
	uint64_t data64;
	uint64_t __user *wptr64 = (uint64_t __user *)wptr;

	m = get_sdma_mqd(mqd);
	sdma_base_addr = get_sdma_base_addr(adev, m->sdma_engine_id,
					    m->sdma_queue_id);
	sdmax_gfx_context_cntl = sdma_v4_0_get_reg_offset(adev,
			m->sdma_engine_id, mmSDMA0_GFX_CONTEXT_CNTL);

	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL,
		m->sdmax_rlcx_rb_cntl & (~SDMA0_RLC0_RB_CNTL__RB_ENABLE_MASK));

	end_jiffies = msecs_to_jiffies(2000) + jiffies;
	while (true) {
		data = RREG32(sdma_base_addr + mmSDMA0_RLC0_CONTEXT_STATUS);
		if (data & SDMA0_RLC0_CONTEXT_STATUS__IDLE_MASK)
			break;
		if (time_after(jiffies, end_jiffies))
			return -ETIME;
		usleep_range(500, 1000);
	}
	data = RREG32(sdmax_gfx_context_cntl);
	data = REG_SET_FIELD(data, SDMA0_GFX_CONTEXT_CNTL,
			     RESUME_CTX, 0);
	WREG32(sdmax_gfx_context_cntl, data);

	WREG32(sdma_base_addr + mmSDMA0_RLC0_DOORBELL_OFFSET,
	       m->sdmax_rlcx_doorbell_offset);

	data = REG_SET_FIELD(m->sdmax_rlcx_doorbell, SDMA0_RLC0_DOORBELL,
			     ENABLE, 1);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_DOORBELL, data);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR, m->sdmax_rlcx_rb_rptr);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR_HI,
				m->sdmax_rlcx_rb_rptr_hi);

	WREG32(sdma_base_addr + mmSDMA0_RLC0_MINOR_PTR_UPDATE, 1);
	if (read_user_wptr(mm, wptr64, data64)) {
		WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_WPTR,
		       lower_32_bits(data64));
		WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_WPTR_HI,
		       upper_32_bits(data64));
	} else {
		WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_WPTR,
		       m->sdmax_rlcx_rb_rptr);
		WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_WPTR_HI,
		       m->sdmax_rlcx_rb_rptr_hi);
	}
	WREG32(sdma_base_addr + mmSDMA0_RLC0_MINOR_PTR_UPDATE, 0);

	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_BASE, m->sdmax_rlcx_rb_base);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_BASE_HI,
			m->sdmax_rlcx_rb_base_hi);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR_ADDR_LO,
			m->sdmax_rlcx_rb_rptr_addr_lo);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR_ADDR_HI,
			m->sdmax_rlcx_rb_rptr_addr_hi);

	data = REG_SET_FIELD(m->sdmax_rlcx_rb_cntl, SDMA0_RLC0_RB_CNTL,
			     RB_ENABLE, 1);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL, data);

	return 0;
}

static int kgd_hqd_sdma_dump(struct kgd_dev *kgd,
			     uint32_t engine_id, uint32_t queue_id,
			     uint32_t (**dump)[2], uint32_t *n_regs)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	uint32_t sdma_base_addr = get_sdma_base_addr(adev, engine_id, queue_id);
	uint32_t i = 0, reg;
#undef HQD_N_REGS
#define HQD_N_REGS (19+6+7+10)

	*dump = kmalloc_array(HQD_N_REGS * 2, sizeof(uint32_t), GFP_KERNEL);
	if (*dump == NULL)
		return -ENOMEM;

	for (reg = mmSDMA0_RLC0_RB_CNTL; reg <= mmSDMA0_RLC0_DOORBELL; reg++)
		DUMP_REG(sdma_base_addr + reg);
	for (reg = mmSDMA0_RLC0_STATUS; reg <= mmSDMA0_RLC0_CSA_ADDR_HI; reg++)
		DUMP_REG(sdma_base_addr + reg);
	for (reg = mmSDMA0_RLC0_IB_SUB_REMAIN;
	     reg <= mmSDMA0_RLC0_MINOR_PTR_UPDATE; reg++)
		DUMP_REG(sdma_base_addr + reg);
	for (reg = mmSDMA0_RLC0_MIDCMD_DATA0;
	     reg <= mmSDMA0_RLC0_MIDCMD_CNTL; reg++)
		DUMP_REG(sdma_base_addr + reg);

	WARN_ON_ONCE(i != HQD_N_REGS);
	*n_regs = i;

	return 0;
}

static bool kgd_hqd_sdma_is_occupied(struct kgd_dev *kgd, void *mqd)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	struct v9_sdma_mqd *m;
	uint32_t sdma_base_addr;
	uint32_t sdma_rlc_rb_cntl;

	m = get_sdma_mqd(mqd);
	sdma_base_addr = get_sdma_base_addr(adev, m->sdma_engine_id,
					    m->sdma_queue_id);

	sdma_rlc_rb_cntl = RREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL);

	if (sdma_rlc_rb_cntl & SDMA0_RLC0_RB_CNTL__RB_ENABLE_MASK)
		return true;

	return false;
}

static int kgd_hqd_sdma_destroy(struct kgd_dev *kgd, void *mqd,
				unsigned int utimeout)
{
	struct amdgpu_device *adev = get_amdgpu_device(kgd);
	struct v9_sdma_mqd *m;
	uint32_t sdma_base_addr;
	uint32_t temp;
	unsigned long end_jiffies = (utimeout * HZ / 1000) + jiffies;

	m = get_sdma_mqd(mqd);
	sdma_base_addr = get_sdma_base_addr(adev, m->sdma_engine_id,
					    m->sdma_queue_id);

	temp = RREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL);
	temp = temp & ~SDMA0_RLC0_RB_CNTL__RB_ENABLE_MASK;
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL, temp);

	while (true) {
		temp = RREG32(sdma_base_addr + mmSDMA0_RLC0_CONTEXT_STATUS);
		if (temp & SDMA0_RLC0_CONTEXT_STATUS__IDLE_MASK)
			break;
		if (time_after(jiffies, end_jiffies))
			return -ETIME;
		usleep_range(500, 1000);
	}

	WREG32(sdma_base_addr + mmSDMA0_RLC0_DOORBELL, 0);
	WREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL,
		RREG32(sdma_base_addr + mmSDMA0_RLC0_RB_CNTL) |
		SDMA0_RLC0_RB_CNTL__RB_ENABLE_MASK);

	m->sdmax_rlcx_rb_rptr = RREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR);
	m->sdmax_rlcx_rb_rptr_hi =
		RREG32(sdma_base_addr + mmSDMA0_RLC0_RB_RPTR_HI);

	return 0;
}

static const struct kfd2kgd_calls kfd2kgd = {
	.program_sh_mem_settings = kgd_gfx_v9_program_sh_mem_settings,
	.set_pasid_vmid_mapping = kgd_gfx_v9_set_pasid_vmid_mapping,
	.init_interrupts = kgd_gfx_v9_init_interrupts,
	.hqd_load = kgd_gfx_v9_hqd_load,
	.hqd_sdma_load = kgd_hqd_sdma_load,
	.hqd_dump = kgd_gfx_v9_hqd_dump,
	.hqd_sdma_dump = kgd_hqd_sdma_dump,
	.hqd_is_occupied = kgd_gfx_v9_hqd_is_occupied,
	.hqd_sdma_is_occupied = kgd_hqd_sdma_is_occupied,
	.hqd_destroy = kgd_gfx_v9_hqd_destroy,
	.hqd_sdma_destroy = kgd_hqd_sdma_destroy,
	.address_watch_disable = kgd_gfx_v9_address_watch_disable,
	.address_watch_execute = kgd_gfx_v9_address_watch_execute,
	.wave_control_execute = kgd_gfx_v9_wave_control_execute,
	.address_watch_get_offset = kgd_gfx_v9_address_watch_get_offset,
	.get_atc_vmid_pasid_mapping_pasid =
			kgd_gfx_v9_get_atc_vmid_pasid_mapping_pasid,
	.get_atc_vmid_pasid_mapping_valid =
			kgd_gfx_v9_get_atc_vmid_pasid_mapping_valid,
	.set_scratch_backing_va = kgd_gfx_v9_set_scratch_backing_va,
	.get_tile_config = kgd_gfx_v9_get_tile_config,
	.set_vm_context_page_table_base = kgd_gfx_v9_set_vm_context_page_table_base,
	.invalidate_tlbs = kgd_gfx_v9_invalidate_tlbs,
	.invalidate_tlbs_vmid = kgd_gfx_v9_invalidate_tlbs_vmid,
	.get_hive_id = amdgpu_amdkfd_get_hive_id,
};

struct kfd2kgd_calls *amdgpu_amdkfd_arcturus_get_functions(void)
{
	return (struct kfd2kgd_calls *)&kfd2kgd;
}

