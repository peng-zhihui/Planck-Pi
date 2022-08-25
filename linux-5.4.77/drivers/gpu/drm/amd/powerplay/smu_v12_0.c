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

#include "pp_debug.h"
#include <linux/firmware.h>
#include "amdgpu.h"
#include "amdgpu_smu.h"
#include "atomfirmware.h"
#include "amdgpu_atomfirmware.h"
#include "smu_v12_0.h"
#include "soc15_common.h"
#include "atom.h"
#include "renoir_ppt.h"

#include "asic_reg/mp/mp_12_0_0_offset.h"
#include "asic_reg/mp/mp_12_0_0_sh_mask.h"

#define smnMP1_FIRMWARE_FLAGS                                0x3010024

#define mmSMUIO_GFX_MISC_CNTL                                0x00c8
#define mmSMUIO_GFX_MISC_CNTL_BASE_IDX                       0
#define SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_MASK          0x00000006L
#define SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS__SHIFT        0x1

static int smu_v12_0_send_msg_without_waiting(struct smu_context *smu,
					      uint16_t msg)
{
	struct amdgpu_device *adev = smu->adev;

	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_66, msg);
	return 0;
}

static int smu_v12_0_read_arg(struct smu_context *smu, uint32_t *arg)
{
	struct amdgpu_device *adev = smu->adev;

	*arg = RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_82);
	return 0;
}

static int smu_v12_0_wait_for_response(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;
	uint32_t cur_value, i;

	for (i = 0; i < adev->usec_timeout; i++) {
		cur_value = RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90);
		if ((cur_value & MP1_C2PMSG_90__CONTENT_MASK) != 0)
			break;
		udelay(1);
	}

	/* timeout means wrong logic */
	if (i == adev->usec_timeout)
		return -ETIME;

	return RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90) == 0x1 ? 0 : -EIO;
}

static int smu_v12_0_send_msg(struct smu_context *smu, uint16_t msg)
{
	struct amdgpu_device *adev = smu->adev;
	int ret = 0, index = 0;

	index = smu_msg_get_index(smu, msg);
	if (index < 0)
		return index;

	smu_v12_0_wait_for_response(smu);

	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90, 0);

	smu_v12_0_send_msg_without_waiting(smu, (uint16_t)index);

	ret = smu_v12_0_wait_for_response(smu);

	if (ret)
		pr_err("Failed to send message 0x%x, response 0x%x\n", index,
		       ret);

	return ret;

}

static int
smu_v12_0_send_msg_with_param(struct smu_context *smu, uint16_t msg,
			      uint32_t param)
{
	struct amdgpu_device *adev = smu->adev;
	int ret = 0, index = 0;

	index = smu_msg_get_index(smu, msg);
	if (index < 0)
		return index;

	ret = smu_v12_0_wait_for_response(smu);
	if (ret)
		pr_err("Failed to send message 0x%x, response 0x%x, param 0x%x\n",
		       index, ret, param);

	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90, 0);

	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_82, param);

	smu_v12_0_send_msg_without_waiting(smu, (uint16_t)index);

	ret = smu_v12_0_wait_for_response(smu);
	if (ret)
		pr_err("Failed to send message 0x%x, response 0x%x param 0x%x\n",
		       index, ret, param);

	return ret;
}

static int smu_v12_0_check_fw_status(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;
	uint32_t mp1_fw_flags;

	mp1_fw_flags = RREG32_PCIE(MP1_Public |
		(smnMP1_FIRMWARE_FLAGS & 0xffffffff));

	if ((mp1_fw_flags & MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK) >>
		MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED__SHIFT)
		return 0;

	return -EIO;
}

static int smu_v12_0_check_fw_version(struct smu_context *smu)
{
	uint32_t if_version = 0xff, smu_version = 0xff;
	uint16_t smu_major;
	uint8_t smu_minor, smu_debug;
	int ret = 0;

	ret = smu_get_smc_version(smu, &if_version, &smu_version);
	if (ret)
		return ret;

	smu_major = (smu_version >> 16) & 0xffff;
	smu_minor = (smu_version >> 8) & 0xff;
	smu_debug = (smu_version >> 0) & 0xff;

	/*
	 * 1. if_version mismatch is not critical as our fw is designed
	 * to be backward compatible.
	 * 2. New fw usually brings some optimizations. But that's visible
	 * only on the paired driver.
	 * Considering above, we just leave user a warning message instead
	 * of halt driver loading.
	 */
	if (if_version != smu->smc_if_version) {
		pr_info("smu driver if version = 0x%08x, smu fw if version = 0x%08x, "
			"smu fw version = 0x%08x (%d.%d.%d)\n",
			smu->smc_if_version, if_version,
			smu_version, smu_major, smu_minor, smu_debug);
		pr_warn("SMU driver if version not matched\n");
	}

	return ret;
}

static int smu_v12_0_powergate_sdma(struct smu_context *smu, bool gate)
{
	if (!(smu->adev->flags & AMD_IS_APU))
		return 0;

	if (gate)
		return smu_send_smc_msg(smu, SMU_MSG_PowerDownSdma);
	else
		return smu_send_smc_msg(smu, SMU_MSG_PowerUpSdma);
}

static int smu_v12_0_powergate_vcn(struct smu_context *smu, bool gate)
{
	if (!(smu->adev->flags & AMD_IS_APU))
		return 0;

	if (gate)
		return smu_send_smc_msg(smu, SMU_MSG_PowerDownVcn);
	else
		return smu_send_smc_msg(smu, SMU_MSG_PowerUpVcn);
}

static int smu_v12_0_set_gfx_cgpg(struct smu_context *smu, bool enable)
{
	if (!(smu->adev->pg_flags & AMD_PG_SUPPORT_GFX_PG))
		return 0;

	return smu_v12_0_send_msg_with_param(smu,
		SMU_MSG_SetGfxCGPG, enable ? 1 : 0);
}

/**
 * smu_v12_0_get_gfxoff_status - get gfxoff status
 *
 * @smu: amdgpu_device pointer
 *
 * This function will be used to get gfxoff status
 *
 * Returns 0=GFXOFF(default).
 * Returns 1=Transition out of GFX State.
 * Returns 2=Not in GFXOFF.
 * Returns 3=Transition into GFXOFF.
 */
static uint32_t smu_v12_0_get_gfxoff_status(struct smu_context *smu)
{
	uint32_t reg;
	uint32_t gfxOff_Status = 0;
	struct amdgpu_device *adev = smu->adev;

	reg = RREG32_SOC15(SMUIO, 0, mmSMUIO_GFX_MISC_CNTL);
	gfxOff_Status = (reg & SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_MASK)
		>> SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS__SHIFT;

	return gfxOff_Status;
}

static int smu_v12_0_gfx_off_control(struct smu_context *smu, bool enable)
{
	int ret = 0, timeout = 500;

	if (enable) {
		ret = smu_send_smc_msg(smu, SMU_MSG_AllowGfxOff);

		/* confirm gfx is back to "off" state, timeout is 5 seconds */
		while (!(smu_v12_0_get_gfxoff_status(smu) == 0)) {
			msleep(10);
			timeout--;
			if (timeout == 0) {
				DRM_ERROR("enable gfxoff timeout and failed!\n");
				break;
			}
		}
	} else {
		ret = smu_send_smc_msg(smu, SMU_MSG_DisallowGfxOff);

		/* confirm gfx is back to "on" state, timeout is 0.5 second */
		while (!(smu_v12_0_get_gfxoff_status(smu) == 2)) {
			msleep(1);
			timeout--;
			if (timeout == 0) {
				DRM_ERROR("disable gfxoff timeout and failed!\n");
				break;
			}
		}
	}

	return ret;
}

static int smu_v12_0_init_smc_tables(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *tables = NULL;

	if (smu_table->tables || smu_table->table_count == 0)
		return -EINVAL;

	tables = kcalloc(SMU_TABLE_COUNT, sizeof(struct smu_table),
			 GFP_KERNEL);
	if (!tables)
		return -ENOMEM;

	smu_table->tables = tables;

	return smu_tables_init(smu, tables);
}

static int smu_v12_0_fini_smc_tables(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;

	if (!smu_table->tables || smu_table->table_count == 0)
		return -EINVAL;

	kfree(smu_table->clocks_table);
	kfree(smu_table->tables);

	smu_table->clocks_table = NULL;
	smu_table->tables = NULL;

	return 0;
}

static int smu_v12_0_populate_smc_tables(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;
	struct smu_table *table = NULL;

	table = &smu_table->tables[SMU_TABLE_DPMCLOCKS];
	if (!table)
		return -EINVAL;

	if (!table->cpu_addr)
		return -EINVAL;

	return smu_update_table(smu, SMU_TABLE_DPMCLOCKS, 0, smu_table->clocks_table, false);
}

static int smu_v12_0_get_dpm_ultimate_freq(struct smu_context *smu, enum smu_clk_type clk_type,
						 uint32_t *min, uint32_t *max)
{
	int ret = 0;

	mutex_lock(&smu->mutex);

	if (max) {
		switch (clk_type) {
		case SMU_GFXCLK:
		case SMU_SCLK:
			ret = smu_send_smc_msg(smu, SMU_MSG_GetMaxGfxclkFrequency);
			if (ret) {
				pr_err("Attempt to get max GX frequency from SMC Failed !\n");
				goto failed;
			}
			ret = smu_read_smc_arg(smu, max);
			if (ret)
				goto failed;
			break;
		case SMU_UCLK:
			ret = smu_get_dpm_uclk_limited(smu, max, true);
			if (ret)
				goto failed;
			break;
		default:
			ret = -EINVAL;
			goto failed;

		}
	}

	if (min) {
		switch (clk_type) {
		case SMU_GFXCLK:
		case SMU_SCLK:
			ret = smu_send_smc_msg(smu, SMU_MSG_GetMinGfxclkFrequency);
			if (ret) {
				pr_err("Attempt to get min GX frequency from SMC Failed !\n");
				goto failed;
			}
			ret = smu_read_smc_arg(smu, min);
			if (ret)
				goto failed;
			break;
		case SMU_UCLK:
			ret = smu_get_dpm_uclk_limited(smu, min, false);
			if (ret)
				goto failed;
			break;
		default:
			ret = -EINVAL;
			goto failed;
		}

	}
failed:
	mutex_unlock(&smu->mutex);
	return ret;
}

static const struct smu_funcs smu_v12_0_funcs = {
	.check_fw_status = smu_v12_0_check_fw_status,
	.check_fw_version = smu_v12_0_check_fw_version,
	.powergate_sdma = smu_v12_0_powergate_sdma,
	.powergate_vcn = smu_v12_0_powergate_vcn,
	.send_smc_msg = smu_v12_0_send_msg,
	.send_smc_msg_with_param = smu_v12_0_send_msg_with_param,
	.read_smc_arg = smu_v12_0_read_arg,
	.set_gfx_cgpg = smu_v12_0_set_gfx_cgpg,
	.gfx_off_control = smu_v12_0_gfx_off_control,
	.init_smc_tables = smu_v12_0_init_smc_tables,
	.fini_smc_tables = smu_v12_0_fini_smc_tables,
	.populate_smc_tables = smu_v12_0_populate_smc_tables,
	.get_dpm_ultimate_freq = smu_v12_0_get_dpm_ultimate_freq,
};

void smu_v12_0_set_smu_funcs(struct smu_context *smu)
{
	struct amdgpu_device *adev = smu->adev;

	smu->funcs = &smu_v12_0_funcs;

	switch (adev->asic_type) {
	case CHIP_RENOIR:
		renoir_set_ppt_funcs(smu);
		break;
	default:
		pr_warn("Unknown asic for smu12\n");
	}
}
