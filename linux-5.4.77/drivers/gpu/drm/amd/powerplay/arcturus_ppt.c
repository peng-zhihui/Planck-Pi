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
 *
 */

#include "pp_debug.h"
#include <linux/firmware.h>
#include "amdgpu.h"
#include "amdgpu_smu.h"
#include "atomfirmware.h"
#include "amdgpu_atomfirmware.h"
#include "smu_v11_0.h"
#include "smu11_driver_if_arcturus.h"
#include "soc15_common.h"
#include "atom.h"
#include "power_state.h"
#include "arcturus_ppt.h"
#include "smu_v11_0_pptable.h"
#include "arcturus_ppsmc.h"
#include "nbio/nbio_7_4_sh_mask.h"

#define CTF_OFFSET_EDGE			5
#define CTF_OFFSET_HOTSPOT		5
#define CTF_OFFSET_HBM			5

#define MSG_MAP(msg, index) \
	[SMU_MSG_##msg] = {1, (index)}
#define ARCTURUS_FEA_MAP(smu_feature, arcturus_feature) \
	[smu_feature] = {1, (arcturus_feature)}

#define SMU_FEATURES_LOW_MASK        0x00000000FFFFFFFF
#define SMU_FEATURES_LOW_SHIFT       0
#define SMU_FEATURES_HIGH_MASK       0xFFFFFFFF00000000
#define SMU_FEATURES_HIGH_SHIFT      32

#define SMC_DPM_FEATURE ( \
	FEATURE_DPM_PREFETCHER_MASK | \
	FEATURE_DPM_GFXCLK_MASK | \
	FEATURE_DPM_UCLK_MASK | \
	FEATURE_DPM_SOCCLK_MASK | \
	FEATURE_DPM_MP0CLK_MASK | \
	FEATURE_DPM_FCLK_MASK | \
	FEATURE_DPM_XGMI_MASK)

/* possible frequency drift (1Mhz) */
#define EPSILON				1

static struct smu_11_0_cmn2aisc_mapping arcturus_message_map[SMU_MSG_MAX_COUNT] = {
	MSG_MAP(TestMessage,			     PPSMC_MSG_TestMessage),
	MSG_MAP(GetSmuVersion,			     PPSMC_MSG_GetSmuVersion),
	MSG_MAP(GetDriverIfVersion,		     PPSMC_MSG_GetDriverIfVersion),
	MSG_MAP(SetAllowedFeaturesMaskLow,	     PPSMC_MSG_SetAllowedFeaturesMaskLow),
	MSG_MAP(SetAllowedFeaturesMaskHigh,	     PPSMC_MSG_SetAllowedFeaturesMaskHigh),
	MSG_MAP(EnableAllSmuFeatures,		     PPSMC_MSG_EnableAllSmuFeatures),
	MSG_MAP(DisableAllSmuFeatures,		     PPSMC_MSG_DisableAllSmuFeatures),
	MSG_MAP(EnableSmuFeaturesLow,		     PPSMC_MSG_EnableSmuFeaturesLow),
	MSG_MAP(EnableSmuFeaturesHigh,		     PPSMC_MSG_EnableSmuFeaturesHigh),
	MSG_MAP(DisableSmuFeaturesLow,		     PPSMC_MSG_DisableSmuFeaturesLow),
	MSG_MAP(DisableSmuFeaturesHigh,		     PPSMC_MSG_DisableSmuFeaturesHigh),
	MSG_MAP(GetEnabledSmuFeaturesLow,	     PPSMC_MSG_GetEnabledSmuFeaturesLow),
	MSG_MAP(GetEnabledSmuFeaturesHigh,	     PPSMC_MSG_GetEnabledSmuFeaturesHigh),
	MSG_MAP(SetDriverDramAddrHigh,		     PPSMC_MSG_SetDriverDramAddrHigh),
	MSG_MAP(SetDriverDramAddrLow,		     PPSMC_MSG_SetDriverDramAddrLow),
	MSG_MAP(SetToolsDramAddrHigh,		     PPSMC_MSG_SetToolsDramAddrHigh),
	MSG_MAP(SetToolsDramAddrLow,		     PPSMC_MSG_SetToolsDramAddrLow),
	MSG_MAP(TransferTableSmu2Dram,		     PPSMC_MSG_TransferTableSmu2Dram),
	MSG_MAP(TransferTableDram2Smu,		     PPSMC_MSG_TransferTableDram2Smu),
	MSG_MAP(UseDefaultPPTable,		     PPSMC_MSG_UseDefaultPPTable),
	MSG_MAP(UseBackupPPTable,		     PPSMC_MSG_UseBackupPPTable),
	MSG_MAP(SetSystemVirtualDramAddrHigh,	     PPSMC_MSG_SetSystemVirtualDramAddrHigh),
	MSG_MAP(SetSystemVirtualDramAddrLow,	     PPSMC_MSG_SetSystemVirtualDramAddrLow),
	MSG_MAP(EnterBaco,			     PPSMC_MSG_EnterBaco),
	MSG_MAP(ExitBaco,			     PPSMC_MSG_ExitBaco),
	MSG_MAP(ArmD3,				     PPSMC_MSG_ArmD3),
	MSG_MAP(SetSoftMinByFreq,		     PPSMC_MSG_SetSoftMinByFreq),
	MSG_MAP(SetSoftMaxByFreq,		     PPSMC_MSG_SetSoftMaxByFreq),
	MSG_MAP(SetHardMinByFreq,		     PPSMC_MSG_SetHardMinByFreq),
	MSG_MAP(SetHardMaxByFreq,		     PPSMC_MSG_SetHardMaxByFreq),
	MSG_MAP(GetMinDpmFreq,			     PPSMC_MSG_GetMinDpmFreq),
	MSG_MAP(GetMaxDpmFreq,			     PPSMC_MSG_GetMaxDpmFreq),
	MSG_MAP(GetDpmFreqByIndex,		     PPSMC_MSG_GetDpmFreqByIndex),
	MSG_MAP(SetWorkloadMask,		     PPSMC_MSG_SetWorkloadMask),
	MSG_MAP(SetDfSwitchType,		     PPSMC_MSG_SetDfSwitchType),
	MSG_MAP(GetVoltageByDpm,		     PPSMC_MSG_GetVoltageByDpm),
	MSG_MAP(GetVoltageByDpmOverdrive,	     PPSMC_MSG_GetVoltageByDpmOverdrive),
	MSG_MAP(SetPptLimit,			     PPSMC_MSG_SetPptLimit),
	MSG_MAP(GetPptLimit,			     PPSMC_MSG_GetPptLimit),
	MSG_MAP(PowerUpVcn0,			     PPSMC_MSG_PowerUpVcn0),
	MSG_MAP(PowerDownVcn0,			     PPSMC_MSG_PowerDownVcn0),
	MSG_MAP(PowerUpVcn1,			     PPSMC_MSG_PowerUpVcn1),
	MSG_MAP(PowerDownVcn1,			     PPSMC_MSG_PowerDownVcn1),
	MSG_MAP(PrepareMp1ForUnload,		     PPSMC_MSG_PrepareMp1ForUnload),
	MSG_MAP(PrepareMp1ForReset,		     PPSMC_MSG_PrepareMp1ForReset),
	MSG_MAP(PrepareMp1ForShutdown,		     PPSMC_MSG_PrepareMp1ForShutdown),
	MSG_MAP(SoftReset,			     PPSMC_MSG_SoftReset),
	MSG_MAP(RunAfllBtc,			     PPSMC_MSG_RunAfllBtc),
	MSG_MAP(RunGfxDcBtc,			     PPSMC_MSG_RunGfxDcBtc),
	MSG_MAP(RunSocDcBtc,			     PPSMC_MSG_RunSocDcBtc),
	MSG_MAP(DramLogSetDramAddrHigh,		     PPSMC_MSG_DramLogSetDramAddrHigh),
	MSG_MAP(DramLogSetDramAddrLow,		     PPSMC_MSG_DramLogSetDramAddrLow),
	MSG_MAP(DramLogSetDramSize,		     PPSMC_MSG_DramLogSetDramSize),
	MSG_MAP(GetDebugData,			     PPSMC_MSG_GetDebugData),
	MSG_MAP(WaflTest,			     PPSMC_MSG_WaflTest),
	MSG_MAP(SetXgmiMode,			     PPSMC_MSG_SetXgmiMode),
	MSG_MAP(SetMemoryChannelEnable,		     PPSMC_MSG_SetMemoryChannelEnable),
};

static struct smu_11_0_cmn2aisc_mapping arcturus_clk_map[SMU_CLK_COUNT] = {
	CLK_MAP(GFXCLK, PPCLK_GFXCLK),
	CLK_MAP(SCLK,	PPCLK_GFXCLK),
	CLK_MAP(SOCCLK, PPCLK_SOCCLK),
	CLK_MAP(FCLK, PPCLK_FCLK),
	CLK_MAP(UCLK, PPCLK_UCLK),
	CLK_MAP(MCLK, PPCLK_UCLK),
	CLK_MAP(DCLK, PPCLK_DCLK),
	CLK_MAP(VCLK, PPCLK_VCLK),
};

static struct smu_11_0_cmn2aisc_mapping arcturus_feature_mask_map[SMU_FEATURE_COUNT] = {
	FEA_MAP(DPM_PREFETCHER),
	FEA_MAP(DPM_GFXCLK),
	FEA_MAP(DPM_UCLK),
	FEA_MAP(DPM_SOCCLK),
	FEA_MAP(DPM_FCLK),
	FEA_MAP(DPM_MP0CLK),
	ARCTURUS_FEA_MAP(SMU_FEATURE_XGMI_BIT, FEATURE_DPM_XGMI_BIT),
	FEA_MAP(DS_GFXCLK),
	FEA_MAP(DS_SOCCLK),
	FEA_MAP(DS_LCLK),
	FEA_MAP(DS_FCLK),
	FEA_MAP(DS_UCLK),
	FEA_MAP(GFX_ULV),
	ARCTURUS_FEA_MAP(SMU_FEATURE_VCN_PG_BIT, FEATURE_DPM_VCN_BIT),
	FEA_MAP(RSMU_SMN_CG),
	FEA_MAP(WAFL_CG),
	FEA_MAP(PPT),
	FEA_MAP(TDC),
	FEA_MAP(APCC_PLUS),
	FEA_MAP(VR0HOT),
	FEA_MAP(VR1HOT),
	FEA_MAP(FW_CTF),
	FEA_MAP(FAN_CONTROL),
	FEA_MAP(THERMAL),
	FEA_MAP(OUT_OF_BAND_MONITOR),
	FEA_MAP(TEMP_DEPENDENT_VMIN),
};

static struct smu_11_0_cmn2aisc_mapping arcturus_table_map[SMU_TABLE_COUNT] = {
	TAB_MAP(PPTABLE),
	TAB_MAP(AVFS),
	TAB_MAP(AVFS_PSM_DEBUG),
	TAB_MAP(AVFS_FUSE_OVERRIDE),
	TAB_MAP(PMSTATUSLOG),
	TAB_MAP(SMU_METRICS),
	TAB_MAP(DRIVER_SMU_CONFIG),
	TAB_MAP(OVERDRIVE),
};

static struct smu_11_0_cmn2aisc_mapping arcturus_pwr_src_map[SMU_POWER_SOURCE_COUNT] = {
	PWR_MAP(AC),
	PWR_MAP(DC),
};

static struct smu_11_0_cmn2aisc_mapping arcturus_workload_map[PP_SMC_POWER_PROFILE_COUNT] = {
	WORKLOAD_MAP(PP_SMC_POWER_PROFILE_BOOTUP_DEFAULT,	WORKLOAD_PPLIB_DEFAULT_BIT),
	WORKLOAD_MAP(PP_SMC_POWER_PROFILE_POWERSAVING,		WORKLOAD_PPLIB_POWER_SAVING_BIT),
	WORKLOAD_MAP(PP_SMC_POWER_PROFILE_VIDEO,		WORKLOAD_PPLIB_VIDEO_BIT),
	WORKLOAD_MAP(PP_SMC_POWER_PROFILE_COMPUTE,		WORKLOAD_PPLIB_COMPUTE_BIT),
	WORKLOAD_MAP(PP_SMC_POWER_PROFILE_CUSTOM,		WORKLOAD_PPLIB_CUSTOM_BIT),
};

static int arcturus_get_smu_msg_index(struct smu_context *smc, uint32_t index)
{
	struct smu_11_0_cmn2aisc_mapping mapping;

	if (index >= SMU_MSG_MAX_COUNT)
		return -EINVAL;

	mapping = arcturus_message_map[index];
	if (!(mapping.valid_mapping))
		return -EINVAL;

	return mapping.map_to;
}

static int arcturus_get_smu_clk_index(struct smu_context *smc, uint32_t index)
{
	struct smu_11_0_cmn2aisc_mapping mapping;

	if (index >= SMU_CLK_COUNT)
		return -EINVAL;

	mapping = arcturus_clk_map[index];
	if (!(mapping.valid_mapping)) {
		pr_warn("Unsupported SMU clk: %d\n", index);
		return -EINVAL;
	}

	return mapping.map_to;
}

static int arcturus_get_smu_feature_index(struct smu_context *smc, uint32_t index)
{
	struct smu_11_0_cmn2aisc_mapping mapping;

	if (index >= SMU_FEATURE_COUNT)
		return -EINVAL;

	mapping = arcturus_feature_mask_map[index];
	if (!(mapping.valid_mapping)) {
		return -EINVAL;
	}

	return mapping.map_to;
}

static int arcturus_get_smu_table_index(struct smu_context *smc, uint32_t index)
{
	struct smu_11_0_cmn2aisc_mapping mapping;

	if (index >= SMU_TABLE_COUNT)
		return -EINVAL;

	mapping = arcturus_table_map[index];
	if (!(mapping.valid_mapping)) {
		pr_warn("Unsupported SMU table: %d\n", index);
		return -EINVAL;
	}

	return mapping.map_to;
}

static int arcturus_get_pwr_src_index(struct smu_context *smc, uint32_t index)
{
	struct smu_11_0_cmn2aisc_mapping mapping;

	if (index >= SMU_POWER_SOURCE_COUNT)
		return -EINVAL;

	mapping = arcturus_pwr_src_map[index];
	if (!(mapping.valid_mapping)) {
		pr_warn("Unsupported SMU power source: %d\n", index);
		return -EINVAL;
	}

	return mapping.map_to;
}


static int arcturus_get_workload_type(struct smu_context *smu, enum PP_SMC_POWER_PROFILE profile)
{
	struct smu_11_0_cmn2aisc_mapping mapping;

	if (profile > PP_SMC_POWER_PROFILE_CUSTOM)
		return -EINVAL;

	mapping = arcturus_workload_map[profile];
	if (!(mapping.valid_mapping)) {
		pr_warn("Unsupported SMU power source: %d\n", profile);
		return -EINVAL;
	}

	return mapping.map_to;
}

static int arcturus_tables_init(struct smu_context *smu, struct smu_table *tables)
{
	struct smu_table_context *smu_table = &smu->smu_table;

	SMU_TABLE_INIT(tables, SMU_TABLE_PPTABLE, sizeof(PPTable_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);

	SMU_TABLE_INIT(tables, SMU_TABLE_PMSTATUSLOG, SMU11_TOOL_SIZE,
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);

	SMU_TABLE_INIT(tables, SMU_TABLE_SMU_METRICS, sizeof(SmuMetrics_t),
		       PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);

	smu_table->metrics_table = kzalloc(sizeof(SmuMetrics_t), GFP_KERNEL);
	if (!smu_table->metrics_table)
		return -ENOMEM;
	smu_table->metrics_time = 0;

	return 0;
}

static int arcturus_allocate_dpm_context(struct smu_context *smu)
{
	struct smu_dpm_context *smu_dpm = &smu->smu_dpm;

	if (smu_dpm->dpm_context)
		return -EINVAL;

	smu_dpm->dpm_context = kzalloc(sizeof(struct arcturus_dpm_table),
				       GFP_KERNEL);
	if (!smu_dpm->dpm_context)
		return -ENOMEM;

	if (smu_dpm->golden_dpm_context)
		return -EINVAL;

	smu_dpm->golden_dpm_context = kzalloc(sizeof(struct arcturus_dpm_table),
					      GFP_KERNEL);
	if (!smu_dpm->golden_dpm_context)
		return -ENOMEM;

	smu_dpm->dpm_context_size = sizeof(struct arcturus_dpm_table);

	smu_dpm->dpm_current_power_state = kzalloc(sizeof(struct smu_power_state),
				       GFP_KERNEL);
	if (!smu_dpm->dpm_current_power_state)
		return -ENOMEM;

	smu_dpm->dpm_request_power_state = kzalloc(sizeof(struct smu_power_state),
				       GFP_KERNEL);
	if (!smu_dpm->dpm_request_power_state)
		return -ENOMEM;

	return 0;
}

static int
arcturus_get_allowed_feature_mask(struct smu_context *smu,
				  uint32_t *feature_mask, uint32_t num)
{
	if (num > 2)
		return -EINVAL;

	/* pptable will handle the features to enable */
	memset(feature_mask, 0xFF, sizeof(uint32_t) * num);

	return 0;
}

static int
arcturus_set_single_dpm_table(struct smu_context *smu,
			    struct arcturus_single_dpm_table *single_dpm_table,
			    PPCLK_e clk_id)
{
	int ret = 0;
	uint32_t i, num_of_levels = 0, clk;

	ret = smu_send_smc_msg_with_param(smu,
			SMU_MSG_GetDpmFreqByIndex,
			(clk_id << 16 | 0xFF));
	if (ret) {
		pr_err("[%s] failed to get dpm levels!\n", __func__);
		return ret;
	}

	smu_read_smc_arg(smu, &num_of_levels);
	if (!num_of_levels) {
		pr_err("[%s] number of clk levels is invalid!\n", __func__);
		return -EINVAL;
	}

	single_dpm_table->count = num_of_levels;
	for (i = 0; i < num_of_levels; i++) {
		ret = smu_send_smc_msg_with_param(smu,
				SMU_MSG_GetDpmFreqByIndex,
				(clk_id << 16 | i));
		if (ret) {
			pr_err("[%s] failed to get dpm freq by index!\n", __func__);
			return ret;
		}
		smu_read_smc_arg(smu, &clk);
		if (!clk) {
			pr_err("[%s] clk value is invalid!\n", __func__);
			return -EINVAL;
		}
		single_dpm_table->dpm_levels[i].value = clk;
		single_dpm_table->dpm_levels[i].enabled = true;
	}
	return 0;
}

static void arcturus_init_single_dpm_state(struct arcturus_dpm_state *dpm_state)
{
	dpm_state->soft_min_level = 0x0;
	dpm_state->soft_max_level = 0xffff;
        dpm_state->hard_min_level = 0x0;
        dpm_state->hard_max_level = 0xffff;
}

static int arcturus_set_default_dpm_table(struct smu_context *smu)
{
	int ret;

	struct smu_dpm_context *smu_dpm = &smu->smu_dpm;
	struct arcturus_dpm_table *dpm_table = NULL;
	struct arcturus_single_dpm_table *single_dpm_table;

	dpm_table = smu_dpm->dpm_context;

	/* socclk */
	single_dpm_table = &(dpm_table->soc_table);
	if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_SOCCLK_BIT)) {
		ret = arcturus_set_single_dpm_table(smu, single_dpm_table,
						  PPCLK_SOCCLK);
		if (ret) {
			pr_err("[%s] failed to get socclk dpm levels!\n", __func__);
			return ret;
		}
	} else {
		single_dpm_table->count = 1;
		single_dpm_table->dpm_levels[0].value = smu->smu_table.boot_values.socclk / 100;
	}
	arcturus_init_single_dpm_state(&(single_dpm_table->dpm_state));

	/* gfxclk */
	single_dpm_table = &(dpm_table->gfx_table);
	if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_GFXCLK_BIT)) {
		ret = arcturus_set_single_dpm_table(smu, single_dpm_table,
						  PPCLK_GFXCLK);
		if (ret) {
			pr_err("[SetupDefaultDpmTable] failed to get gfxclk dpm levels!");
			return ret;
		}
	} else {
		single_dpm_table->count = 1;
		single_dpm_table->dpm_levels[0].value = smu->smu_table.boot_values.gfxclk / 100;
	}
	arcturus_init_single_dpm_state(&(single_dpm_table->dpm_state));

	/* memclk */
	single_dpm_table = &(dpm_table->mem_table);
	if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_UCLK_BIT)) {
		ret = arcturus_set_single_dpm_table(smu, single_dpm_table,
						  PPCLK_UCLK);
		if (ret) {
			pr_err("[SetupDefaultDpmTable] failed to get memclk dpm levels!");
			return ret;
		}
	} else {
		single_dpm_table->count = 1;
		single_dpm_table->dpm_levels[0].value = smu->smu_table.boot_values.uclk / 100;
	}
	arcturus_init_single_dpm_state(&(single_dpm_table->dpm_state));

	/* fclk */
	single_dpm_table = &(dpm_table->fclk_table);
	if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_FCLK_BIT)) {
		ret = arcturus_set_single_dpm_table(smu, single_dpm_table,
						  PPCLK_FCLK);
		if (ret) {
			pr_err("[SetupDefaultDpmTable] failed to get fclk dpm levels!");
			return ret;
		}
	} else {
		single_dpm_table->count = 1;
		single_dpm_table->dpm_levels[0].value = smu->smu_table.boot_values.fclk / 100;
	}
	arcturus_init_single_dpm_state(&(single_dpm_table->dpm_state));

	memcpy(smu_dpm->golden_dpm_context, dpm_table,
	       sizeof(struct arcturus_dpm_table));

	return 0;
}

static int arcturus_check_powerplay_table(struct smu_context *smu)
{
	return 0;
}

static int arcturus_store_powerplay_table(struct smu_context *smu)
{
	struct smu_11_0_powerplay_table *powerplay_table = NULL;
	struct smu_table_context *table_context = &smu->smu_table;
	int ret = 0;

	if (!table_context->power_play_table)
		return -EINVAL;

	powerplay_table = table_context->power_play_table;

	memcpy(table_context->driver_pptable, &powerplay_table->smc_pptable,
	       sizeof(PPTable_t));

	table_context->thermal_controller_type = powerplay_table->thermal_controller_type;

	return ret;
}

static int arcturus_append_powerplay_table(struct smu_context *smu)
{
	struct smu_table_context *table_context = &smu->smu_table;
	PPTable_t *smc_pptable = table_context->driver_pptable;
	struct atom_smc_dpm_info_v4_6 *smc_dpm_table;
	int index, ret;

	index = get_index_into_master_table(atom_master_list_of_data_tables_v2_1,
					   smc_dpm_info);

	ret = smu_get_atom_data_table(smu, index, NULL, NULL, NULL,
				      (uint8_t **)&smc_dpm_table);
	if (ret)
		return ret;

	pr_info("smc_dpm_info table revision(format.content): %d.%d\n",
			smc_dpm_table->table_header.format_revision,
			smc_dpm_table->table_header.content_revision);

	if ((smc_dpm_table->table_header.format_revision == 4) &&
	    (smc_dpm_table->table_header.content_revision == 6))
		memcpy(&smc_pptable->MaxVoltageStepGfx,
		       &smc_dpm_table->maxvoltagestepgfx,
		       sizeof(*smc_dpm_table) - offsetof(struct atom_smc_dpm_info_v4_6, maxvoltagestepgfx));

	return 0;
}

static int arcturus_run_btc_afll(struct smu_context *smu)
{
	return smu_send_smc_msg(smu, SMU_MSG_RunAfllBtc);
}

static int arcturus_populate_umd_state_clk(struct smu_context *smu)
{
	struct smu_dpm_context *smu_dpm = &smu->smu_dpm;
	struct arcturus_dpm_table *dpm_table = NULL;
	struct arcturus_single_dpm_table *gfx_table = NULL;
	struct arcturus_single_dpm_table *mem_table = NULL;

	dpm_table = smu_dpm->dpm_context;
	gfx_table = &(dpm_table->gfx_table);
	mem_table = &(dpm_table->mem_table);

	smu->pstate_sclk = gfx_table->dpm_levels[0].value;
	smu->pstate_mclk = mem_table->dpm_levels[0].value;

	if (gfx_table->count > ARCTURUS_UMD_PSTATE_GFXCLK_LEVEL &&
	    mem_table->count > ARCTURUS_UMD_PSTATE_MCLK_LEVEL) {
		smu->pstate_sclk = gfx_table->dpm_levels[ARCTURUS_UMD_PSTATE_GFXCLK_LEVEL].value;
		smu->pstate_mclk = mem_table->dpm_levels[ARCTURUS_UMD_PSTATE_MCLK_LEVEL].value;
	}

	smu->pstate_sclk = smu->pstate_sclk * 100;
	smu->pstate_mclk = smu->pstate_mclk * 100;

	return 0;
}

static int arcturus_get_clk_table(struct smu_context *smu,
			struct pp_clock_levels_with_latency *clocks,
			struct arcturus_single_dpm_table *dpm_table)
{
	int i, count;

	count = (dpm_table->count > MAX_NUM_CLOCKS) ? MAX_NUM_CLOCKS : dpm_table->count;
	clocks->num_levels = count;

	for (i = 0; i < count; i++) {
		clocks->data[i].clocks_in_khz =
			dpm_table->dpm_levels[i].value * 1000;
		clocks->data[i].latency_in_us = 0;
	}

	return 0;
}

static int arcturus_freqs_in_same_level(int32_t frequency1,
					int32_t frequency2)
{
	return (abs(frequency1 - frequency2) <= EPSILON);
}

static int arcturus_print_clk_levels(struct smu_context *smu,
			enum smu_clk_type type, char *buf)
{
	int i, now, size = 0;
	int ret = 0;
	struct pp_clock_levels_with_latency clocks;
	struct arcturus_single_dpm_table *single_dpm_table;
	struct smu_dpm_context *smu_dpm = &smu->smu_dpm;
	struct arcturus_dpm_table *dpm_table = NULL;

	dpm_table = smu_dpm->dpm_context;

	switch (type) {
	case SMU_SCLK:
		ret = smu_get_current_clk_freq(smu, SMU_GFXCLK, &now);
		if (ret) {
			pr_err("Attempt to get current gfx clk Failed!");
			return ret;
		}

		single_dpm_table = &(dpm_table->gfx_table);
		ret = arcturus_get_clk_table(smu, &clocks, single_dpm_table);
		if (ret) {
			pr_err("Attempt to get gfx clk levels Failed!");
			return ret;
		}

		for (i = 0; i < clocks.num_levels; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n", i,
					clocks.data[i].clocks_in_khz / 1000,
					arcturus_freqs_in_same_level(
					clocks.data[i].clocks_in_khz / 1000,
					now / 100) ? "*" : "");
		break;

	case SMU_MCLK:
		ret = smu_get_current_clk_freq(smu, SMU_UCLK, &now);
		if (ret) {
			pr_err("Attempt to get current mclk Failed!");
			return ret;
		}

		single_dpm_table = &(dpm_table->mem_table);
		ret = arcturus_get_clk_table(smu, &clocks, single_dpm_table);
		if (ret) {
			pr_err("Attempt to get memory clk levels Failed!");
			return ret;
		}

		for (i = 0; i < clocks.num_levels; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n",
				i, clocks.data[i].clocks_in_khz / 1000,
				arcturus_freqs_in_same_level(
				clocks.data[i].clocks_in_khz / 1000,
				now / 100) ? "*" : "");
		break;

	case SMU_SOCCLK:
		ret = smu_get_current_clk_freq(smu, SMU_SOCCLK, &now);
		if (ret) {
			pr_err("Attempt to get current socclk Failed!");
			return ret;
		}

		single_dpm_table = &(dpm_table->soc_table);
		ret = arcturus_get_clk_table(smu, &clocks, single_dpm_table);
		if (ret) {
			pr_err("Attempt to get socclk levels Failed!");
			return ret;
		}

		for (i = 0; i < clocks.num_levels; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n",
				i, clocks.data[i].clocks_in_khz / 1000,
				arcturus_freqs_in_same_level(
				clocks.data[i].clocks_in_khz / 1000,
				now / 100) ? "*" : "");
		break;

	case SMU_FCLK:
		ret = smu_get_current_clk_freq(smu, SMU_FCLK, &now);
		if (ret) {
			pr_err("Attempt to get current fclk Failed!");
			return ret;
		}

		single_dpm_table = &(dpm_table->fclk_table);
		ret = arcturus_get_clk_table(smu, &clocks, single_dpm_table);
		if (ret) {
			pr_err("Attempt to get fclk levels Failed!");
			return ret;
		}

		for (i = 0; i < single_dpm_table->count; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n",
				i, single_dpm_table->dpm_levels[i].value,
				arcturus_freqs_in_same_level(
				clocks.data[i].clocks_in_khz / 1000,
				now / 100) ? "*" : "");
		break;

	default:
		break;
	}

	return size;
}

static int arcturus_upload_dpm_level(struct smu_context *smu, bool max,
				     uint32_t feature_mask)
{
	struct arcturus_single_dpm_table *single_dpm_table;
	struct arcturus_dpm_table *dpm_table =
			smu->smu_dpm.dpm_context;
	uint32_t freq;
	int ret = 0;

	if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_GFXCLK_BIT) &&
	    (feature_mask & FEATURE_DPM_GFXCLK_MASK)) {
		single_dpm_table = &(dpm_table->gfx_table);
		freq = max ? single_dpm_table->dpm_state.soft_max_level :
			single_dpm_table->dpm_state.soft_min_level;
		ret = smu_send_smc_msg_with_param(smu,
			(max ? SMU_MSG_SetSoftMaxByFreq : SMU_MSG_SetSoftMinByFreq),
			(PPCLK_GFXCLK << 16) | (freq & 0xffff));
		if (ret) {
			pr_err("Failed to set soft %s gfxclk !\n",
						max ? "max" : "min");
			return ret;
		}
	}

	if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_UCLK_BIT) &&
	    (feature_mask & FEATURE_DPM_UCLK_MASK)) {
		single_dpm_table = &(dpm_table->mem_table);
		freq = max ? single_dpm_table->dpm_state.soft_max_level :
			single_dpm_table->dpm_state.soft_min_level;
		ret = smu_send_smc_msg_with_param(smu,
			(max ? SMU_MSG_SetSoftMaxByFreq : SMU_MSG_SetSoftMinByFreq),
			(PPCLK_UCLK << 16) | (freq & 0xffff));
		if (ret) {
			pr_err("Failed to set soft %s memclk !\n",
						max ? "max" : "min");
			return ret;
		}
	}

	if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_SOCCLK_BIT) &&
	    (feature_mask & FEATURE_DPM_SOCCLK_MASK)) {
		single_dpm_table = &(dpm_table->soc_table);
		freq = max ? single_dpm_table->dpm_state.soft_max_level :
			single_dpm_table->dpm_state.soft_min_level;
		ret = smu_send_smc_msg_with_param(smu,
			(max ? SMU_MSG_SetSoftMaxByFreq : SMU_MSG_SetSoftMinByFreq),
			(PPCLK_SOCCLK << 16) | (freq & 0xffff));
		if (ret) {
			pr_err("Failed to set soft %s socclk !\n",
						max ? "max" : "min");
			return ret;
		}
	}

	return ret;
}

static int arcturus_force_clk_levels(struct smu_context *smu,
			enum smu_clk_type type, uint32_t mask)
{
	struct arcturus_dpm_table *dpm_table;
	struct arcturus_single_dpm_table *single_dpm_table;
	uint32_t soft_min_level, soft_max_level;
	int ret = 0;

	mutex_lock(&(smu->mutex));

	soft_min_level = mask ? (ffs(mask) - 1) : 0;
	soft_max_level = mask ? (fls(mask) - 1) : 0;

	dpm_table = smu->smu_dpm.dpm_context;

	switch (type) {
	case SMU_SCLK:
		single_dpm_table = &(dpm_table->gfx_table);

		if (soft_max_level >= single_dpm_table->count) {
			pr_err("Clock level specified %d is over max allowed %d\n",
					soft_max_level, single_dpm_table->count - 1);
			ret = -EINVAL;
			break;
		}

		single_dpm_table->dpm_state.soft_min_level =
			single_dpm_table->dpm_levels[soft_min_level].value;
		single_dpm_table->dpm_state.soft_max_level =
			single_dpm_table->dpm_levels[soft_max_level].value;

		ret = arcturus_upload_dpm_level(smu, false, FEATURE_DPM_GFXCLK_MASK);
		if (ret) {
			pr_err("Failed to upload boot level to lowest!\n");
			break;
		}

		ret = arcturus_upload_dpm_level(smu, true, FEATURE_DPM_GFXCLK_MASK);
		if (ret)
			pr_err("Failed to upload dpm max level to highest!\n");

		break;

	case SMU_MCLK:
		single_dpm_table = &(dpm_table->mem_table);

		if (soft_max_level >= single_dpm_table->count) {
			pr_err("Clock level specified %d is over max allowed %d\n",
					soft_max_level, single_dpm_table->count - 1);
			ret = -EINVAL;
			break;
		}

		single_dpm_table->dpm_state.soft_min_level =
			single_dpm_table->dpm_levels[soft_min_level].value;
		single_dpm_table->dpm_state.soft_max_level =
			single_dpm_table->dpm_levels[soft_max_level].value;

		ret = arcturus_upload_dpm_level(smu, false, FEATURE_DPM_UCLK_MASK);
		if (ret) {
			pr_err("Failed to upload boot level to lowest!\n");
			break;
		}

		ret = arcturus_upload_dpm_level(smu, true, FEATURE_DPM_UCLK_MASK);
		if (ret)
			pr_err("Failed to upload dpm max level to highest!\n");

		break;

	case SMU_SOCCLK:
		single_dpm_table = &(dpm_table->soc_table);

		if (soft_max_level >= single_dpm_table->count) {
			pr_err("Clock level specified %d is over max allowed %d\n",
					soft_max_level, single_dpm_table->count - 1);
			ret = -EINVAL;
			break;
		}

		single_dpm_table->dpm_state.soft_min_level =
			single_dpm_table->dpm_levels[soft_min_level].value;
		single_dpm_table->dpm_state.soft_max_level =
			single_dpm_table->dpm_levels[soft_max_level].value;

		ret = arcturus_upload_dpm_level(smu, false, FEATURE_DPM_SOCCLK_MASK);
		if (ret) {
			pr_err("Failed to upload boot level to lowest!\n");
			break;
		}

		ret = arcturus_upload_dpm_level(smu, true, FEATURE_DPM_SOCCLK_MASK);
		if (ret)
			pr_err("Failed to upload dpm max level to highest!\n");

		break;

	case SMU_FCLK:
		single_dpm_table = &(dpm_table->fclk_table);

		if (soft_max_level >= single_dpm_table->count) {
			pr_err("Clock level specified %d is over max allowed %d\n",
					soft_max_level, single_dpm_table->count - 1);
			ret = -EINVAL;
			break;
		}

		single_dpm_table->dpm_state.soft_min_level =
			single_dpm_table->dpm_levels[soft_min_level].value;
		single_dpm_table->dpm_state.soft_max_level =
			single_dpm_table->dpm_levels[soft_max_level].value;

		ret = arcturus_upload_dpm_level(smu, false, FEATURE_DPM_FCLK_MASK);
		if (ret) {
			pr_err("Failed to upload boot level to lowest!\n");
			break;
		}

		ret = arcturus_upload_dpm_level(smu, true, FEATURE_DPM_FCLK_MASK);
		if (ret)
			pr_err("Failed to upload dpm max level to highest!\n");

		break;

	default:
		break;
	}

	mutex_unlock(&(smu->mutex));
	return ret;
}

static int arcturus_get_thermal_temperature_range(struct smu_context *smu,
						struct smu_temperature_range *range)
{
	PPTable_t *pptable = smu->smu_table.driver_pptable;

	if (!range)
		return -EINVAL;

	range->max = pptable->TedgeLimit *
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->edge_emergency_max = (pptable->TedgeLimit + CTF_OFFSET_EDGE) *
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->hotspot_crit_max = pptable->ThotspotLimit *
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->hotspot_emergency_max = (pptable->ThotspotLimit + CTF_OFFSET_HOTSPOT) *
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->mem_crit_max = pptable->TmemLimit *
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
	range->mem_emergency_max = (pptable->TmemLimit + CTF_OFFSET_HBM)*
		SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;

	return 0;
}

static int arcturus_get_metrics_table(struct smu_context *smu,
				      SmuMetrics_t *metrics_table)
{
	struct smu_table_context *smu_table= &smu->smu_table;
	int ret = 0;

	mutex_lock(&smu->metrics_lock);
	if (!smu_table->metrics_time ||
	     time_after(jiffies, smu_table->metrics_time + HZ / 1000)) {
		ret = smu_update_table(smu, SMU_TABLE_SMU_METRICS, 0,
				(void *)smu_table->metrics_table, false);
		if (ret) {
			pr_info("Failed to export SMU metrics table!\n");
			mutex_unlock(&smu->metrics_lock);
			return ret;
		}
		smu_table->metrics_time = jiffies;
	}

	memcpy(metrics_table, smu_table->metrics_table, sizeof(SmuMetrics_t));
	mutex_unlock(&smu->metrics_lock);

	return ret;
}

static int arcturus_get_current_activity_percent(struct smu_context *smu,
						 enum amd_pp_sensors sensor,
						 uint32_t *value)
{
	SmuMetrics_t metrics;
	int ret = 0;

	if (!value)
		return -EINVAL;

	ret = arcturus_get_metrics_table(smu, &metrics);
	if (ret)
		return ret;

	switch (sensor) {
	case AMDGPU_PP_SENSOR_GPU_LOAD:
		*value = metrics.AverageGfxActivity;
		break;
	case AMDGPU_PP_SENSOR_MEM_LOAD:
		*value = metrics.AverageUclkActivity;
		break;
	default:
		pr_err("Invalid sensor for retrieving clock activity\n");
		return -EINVAL;
	}

	return 0;
}

static int arcturus_get_gpu_power(struct smu_context *smu, uint32_t *value)
{
	SmuMetrics_t metrics;
	int ret = 0;

	if (!value)
		return -EINVAL;

	ret = arcturus_get_metrics_table(smu, &metrics);
	if (ret)
		return ret;

	*value = metrics.AverageSocketPower << 8;

	return 0;
}

static int arcturus_thermal_get_temperature(struct smu_context *smu,
					    enum amd_pp_sensors sensor,
					    uint32_t *value)
{
	SmuMetrics_t metrics;
	int ret = 0;

	if (!value)
		return -EINVAL;

	ret = arcturus_get_metrics_table(smu, &metrics);
	if (ret)
		return ret;

	switch (sensor) {
	case AMDGPU_PP_SENSOR_HOTSPOT_TEMP:
		*value = metrics.TemperatureHotspot *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case AMDGPU_PP_SENSOR_EDGE_TEMP:
		*value = metrics.TemperatureEdge *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	case AMDGPU_PP_SENSOR_MEM_TEMP:
		*value = metrics.TemperatureHBM *
			SMU_TEMPERATURE_UNITS_PER_CENTIGRADES;
		break;
	default:
		pr_err("Invalid sensor for retrieving temp\n");
		return -EINVAL;
	}

	return 0;
}

static int arcturus_read_sensor(struct smu_context *smu,
				enum amd_pp_sensors sensor,
				void *data, uint32_t *size)
{
	struct smu_table_context *table_context = &smu->smu_table;
	PPTable_t *pptable = table_context->driver_pptable;
	int ret = 0;

	if (!data || !size)
		return -EINVAL;

	mutex_lock(&smu->sensor_lock);
	switch (sensor) {
	case AMDGPU_PP_SENSOR_MAX_FAN_RPM:
		*(uint32_t *)data = pptable->FanMaximumRpm;
		*size = 4;
		break;
	case AMDGPU_PP_SENSOR_MEM_LOAD:
	case AMDGPU_PP_SENSOR_GPU_LOAD:
		ret = arcturus_get_current_activity_percent(smu,
							    sensor,
						(uint32_t *)data);
		*size = 4;
		break;
	case AMDGPU_PP_SENSOR_GPU_POWER:
		ret = arcturus_get_gpu_power(smu, (uint32_t *)data);
		*size = 4;
		break;
	case AMDGPU_PP_SENSOR_HOTSPOT_TEMP:
	case AMDGPU_PP_SENSOR_EDGE_TEMP:
	case AMDGPU_PP_SENSOR_MEM_TEMP:
		ret = arcturus_thermal_get_temperature(smu, sensor,
						(uint32_t *)data);
		*size = 4;
		break;
	default:
		ret = smu_smc_read_sensor(smu, sensor, data, size);
	}
	mutex_unlock(&smu->sensor_lock);

	return ret;
}

static int arcturus_get_fan_speed_rpm(struct smu_context *smu,
				      uint32_t *speed)
{
	SmuMetrics_t metrics;
	int ret = 0;

	if (!speed)
		return -EINVAL;

	ret = arcturus_get_metrics_table(smu, &metrics);
	if (ret)
		return ret;

	*speed = metrics.CurrFanSpeed;

	return ret;
}

static int arcturus_get_fan_speed_percent(struct smu_context *smu,
					  uint32_t *speed)
{
	PPTable_t *pptable = smu->smu_table.driver_pptable;
	uint32_t percent, current_rpm;
	int ret = 0;

	if (!speed)
		return -EINVAL;

	ret = arcturus_get_fan_speed_rpm(smu, &current_rpm);
	if (ret)
		return ret;

	percent = current_rpm * 100 / pptable->FanMaximumRpm;
	*speed = percent > 100 ? 100 : percent;

	return ret;
}

static int arcturus_get_current_clk_freq_by_table(struct smu_context *smu,
				       enum smu_clk_type clk_type,
				       uint32_t *value)
{
	static SmuMetrics_t metrics;
	int ret = 0, clk_id = 0;

	if (!value)
		return -EINVAL;

	clk_id = smu_clk_get_index(smu, clk_type);
	if (clk_id < 0)
		return -EINVAL;

	ret = arcturus_get_metrics_table(smu, &metrics);
	if (ret)
		return ret;

	switch (clk_id) {
	case PPCLK_GFXCLK:
		/*
		 * CurrClock[clk_id] can provide accurate
		 *   output only when the dpm feature is enabled.
		 * We can use Average_* for dpm disabled case.
		 *   But this is available for gfxclk/uclk/socclk.
		 */
		if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_GFXCLK_BIT))
			*value = metrics.CurrClock[PPCLK_GFXCLK];
		else
			*value = metrics.AverageGfxclkFrequency;
		break;
	case PPCLK_UCLK:
		if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_UCLK_BIT))
			*value = metrics.CurrClock[PPCLK_UCLK];
		else
			*value = metrics.AverageUclkFrequency;
		break;
	case PPCLK_SOCCLK:
		if (smu_feature_is_enabled(smu, SMU_FEATURE_DPM_SOCCLK_BIT))
			*value = metrics.CurrClock[PPCLK_SOCCLK];
		else
			*value = metrics.AverageSocclkFrequency;
		break;
	default:
		*value = metrics.CurrClock[clk_id];
		break;
	}

	return ret;
}

static uint32_t arcturus_find_lowest_dpm_level(struct arcturus_single_dpm_table *table)
{
	uint32_t i;

	for (i = 0; i < table->count; i++) {
		if (table->dpm_levels[i].enabled)
			break;
	}
	if (i >= table->count) {
		i = 0;
		table->dpm_levels[i].enabled = true;
	}

	return i;
}

static uint32_t arcturus_find_highest_dpm_level(struct arcturus_single_dpm_table *table)
{
	int i = 0;

	if (table->count <= 0) {
		pr_err("[%s] DPM Table has no entry!", __func__);
		return 0;
	}
	if (table->count > MAX_DPM_NUMBER) {
		pr_err("[%s] DPM Table has too many entries!", __func__);
		return MAX_DPM_NUMBER - 1;
	}

	for (i = table->count - 1; i >= 0; i--) {
		if (table->dpm_levels[i].enabled)
			break;
	}
	if (i < 0) {
		i = 0;
		table->dpm_levels[i].enabled = true;
	}

	return i;
}



static int arcturus_force_dpm_limit_value(struct smu_context *smu, bool highest)
{
	struct arcturus_dpm_table *dpm_table =
		(struct arcturus_dpm_table *)smu->smu_dpm.dpm_context;
	uint32_t soft_level;
	int ret = 0;

	/* gfxclk */
	if (highest)
		soft_level = arcturus_find_highest_dpm_level(&(dpm_table->gfx_table));
	else
		soft_level = arcturus_find_lowest_dpm_level(&(dpm_table->gfx_table));

	dpm_table->gfx_table.dpm_state.soft_min_level =
		dpm_table->gfx_table.dpm_state.soft_max_level =
		dpm_table->gfx_table.dpm_levels[soft_level].value;

	/* uclk */
	if (highest)
		soft_level = arcturus_find_highest_dpm_level(&(dpm_table->mem_table));
	else
		soft_level = arcturus_find_lowest_dpm_level(&(dpm_table->mem_table));

	dpm_table->mem_table.dpm_state.soft_min_level =
		dpm_table->mem_table.dpm_state.soft_max_level =
		dpm_table->mem_table.dpm_levels[soft_level].value;

	/* socclk */
	if (highest)
		soft_level = arcturus_find_highest_dpm_level(&(dpm_table->soc_table));
	else
		soft_level = arcturus_find_lowest_dpm_level(&(dpm_table->soc_table));

	dpm_table->soc_table.dpm_state.soft_min_level =
		dpm_table->soc_table.dpm_state.soft_max_level =
		dpm_table->soc_table.dpm_levels[soft_level].value;

	ret = arcturus_upload_dpm_level(smu, false, 0xFFFFFFFF);
	if (ret) {
		pr_err("Failed to upload boot level to %s!\n",
				highest ? "highest" : "lowest");
		return ret;
	}

	ret = arcturus_upload_dpm_level(smu, true, 0xFFFFFFFF);
	if (ret) {
		pr_err("Failed to upload dpm max level to %s!\n!",
				highest ? "highest" : "lowest");
		return ret;
	}

	return ret;
}

static int arcturus_unforce_dpm_levels(struct smu_context *smu)
{
	struct arcturus_dpm_table *dpm_table =
		(struct arcturus_dpm_table *)smu->smu_dpm.dpm_context;
	uint32_t soft_min_level, soft_max_level;
	int ret = 0;

	/* gfxclk */
	soft_min_level = arcturus_find_lowest_dpm_level(&(dpm_table->gfx_table));
	soft_max_level = arcturus_find_highest_dpm_level(&(dpm_table->gfx_table));
	dpm_table->gfx_table.dpm_state.soft_min_level =
		dpm_table->gfx_table.dpm_levels[soft_min_level].value;
	dpm_table->gfx_table.dpm_state.soft_max_level =
		dpm_table->gfx_table.dpm_levels[soft_max_level].value;

	/* uclk */
	soft_min_level = arcturus_find_lowest_dpm_level(&(dpm_table->mem_table));
	soft_max_level = arcturus_find_highest_dpm_level(&(dpm_table->mem_table));
	dpm_table->mem_table.dpm_state.soft_min_level =
		dpm_table->gfx_table.dpm_levels[soft_min_level].value;
	dpm_table->mem_table.dpm_state.soft_max_level =
		dpm_table->gfx_table.dpm_levels[soft_max_level].value;

	/* socclk */
	soft_min_level = arcturus_find_lowest_dpm_level(&(dpm_table->soc_table));
	soft_max_level = arcturus_find_highest_dpm_level(&(dpm_table->soc_table));
	dpm_table->soc_table.dpm_state.soft_min_level =
		dpm_table->soc_table.dpm_levels[soft_min_level].value;
	dpm_table->soc_table.dpm_state.soft_max_level =
		dpm_table->soc_table.dpm_levels[soft_max_level].value;

	ret = arcturus_upload_dpm_level(smu, false, 0xFFFFFFFF);
	if (ret) {
		pr_err("Failed to upload DPM Bootup Levels!");
		return ret;
	}

	ret = arcturus_upload_dpm_level(smu, true, 0xFFFFFFFF);
	if (ret) {
		pr_err("Failed to upload DPM Max Levels!");
		return ret;
	}

	return ret;
}

static int
arcturus_get_profiling_clk_mask(struct smu_context *smu,
				enum amd_dpm_forced_level level,
				uint32_t *sclk_mask,
				uint32_t *mclk_mask,
				uint32_t *soc_mask)
{
	struct arcturus_dpm_table *dpm_table =
		(struct arcturus_dpm_table *)smu->smu_dpm.dpm_context;
	struct arcturus_single_dpm_table *gfx_dpm_table;
	struct arcturus_single_dpm_table *mem_dpm_table;
	struct arcturus_single_dpm_table *soc_dpm_table;

	if (!smu->smu_dpm.dpm_context)
		return -EINVAL;

	gfx_dpm_table = &dpm_table->gfx_table;
	mem_dpm_table = &dpm_table->mem_table;
	soc_dpm_table = &dpm_table->soc_table;

	*sclk_mask = 0;
	*mclk_mask = 0;
	*soc_mask  = 0;

	if (gfx_dpm_table->count > ARCTURUS_UMD_PSTATE_GFXCLK_LEVEL &&
	    mem_dpm_table->count > ARCTURUS_UMD_PSTATE_MCLK_LEVEL &&
	    soc_dpm_table->count > ARCTURUS_UMD_PSTATE_SOCCLK_LEVEL) {
		*sclk_mask = ARCTURUS_UMD_PSTATE_GFXCLK_LEVEL;
		*mclk_mask = ARCTURUS_UMD_PSTATE_MCLK_LEVEL;
		*soc_mask  = ARCTURUS_UMD_PSTATE_SOCCLK_LEVEL;
	}

	if (level == AMD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK) {
		*sclk_mask = 0;
	} else if (level == AMD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK) {
		*mclk_mask = 0;
	} else if (level == AMD_DPM_FORCED_LEVEL_PROFILE_PEAK) {
		*sclk_mask = gfx_dpm_table->count - 1;
		*mclk_mask = mem_dpm_table->count - 1;
		*soc_mask  = soc_dpm_table->count - 1;
	}

	return 0;
}

static int arcturus_get_power_limit(struct smu_context *smu,
				     uint32_t *limit,
				     bool asic_default)
{
	PPTable_t *pptable = smu->smu_table.driver_pptable;
	uint32_t asic_default_power_limit = 0;
	int ret = 0;
	int power_src;

	if (!smu->default_power_limit ||
	    !smu->power_limit) {
		if (smu_feature_is_enabled(smu, SMU_FEATURE_PPT_BIT)) {
			power_src = smu_power_get_index(smu, SMU_POWER_SOURCE_AC);
			if (power_src < 0)
				return -EINVAL;

			ret = smu_send_smc_msg_with_param(smu, SMU_MSG_GetPptLimit,
				power_src << 16);
			if (ret) {
				pr_err("[%s] get PPT limit failed!", __func__);
				return ret;
			}
			smu_read_smc_arg(smu, &asic_default_power_limit);
		} else {
			/* the last hope to figure out the ppt limit */
			if (!pptable) {
				pr_err("Cannot get PPT limit due to pptable missing!");
				return -EINVAL;
			}
			asic_default_power_limit =
				pptable->SocketPowerLimitAc[PPT_THROTTLER_PPT0];
		}

		if (smu->od_enabled) {
			asic_default_power_limit *= (100 + smu->smu_table.TDPODLimit);
			asic_default_power_limit /= 100;
		}

		smu->default_power_limit = asic_default_power_limit;
		smu->power_limit = asic_default_power_limit;
	}

	if (asic_default)
		*limit = smu->default_power_limit;
	else
		*limit = smu->power_limit;

	return 0;
}

static int arcturus_get_power_profile_mode(struct smu_context *smu,
					   char *buf)
{
	static const char *profile_name[] = {
					"BOOTUP_DEFAULT",
					"3D_FULL_SCREEN",
					"POWER_SAVING",
					"VIDEO",
					"VR",
					"COMPUTE",
					"CUSTOM"};
	static const char *title[] = {
			"PROFILE_INDEX(NAME)"};
	uint32_t i, size = 0;
	int16_t workload_type = 0;

	if (!smu->pm_enabled || !buf)
		return -EINVAL;

	size += sprintf(buf + size, "%16s\n",
			title[0]);

	for (i = 0; i <= PP_SMC_POWER_PROFILE_CUSTOM; i++) {
		/*
		 * Conv PP_SMC_POWER_PROFILE* to WORKLOAD_PPLIB_*_BIT
		 * Not all profile modes are supported on arcturus.
		 */
		workload_type = smu_workload_get_type(smu, i);
		if (workload_type < 0)
			continue;

		size += sprintf(buf + size, "%2d %14s%s\n",
			i, profile_name[i], (i == smu->power_profile_mode) ? "*" : " ");
	}

	return size;
}

static int arcturus_set_power_profile_mode(struct smu_context *smu,
					   long *input,
					   uint32_t size)
{
	int workload_type = 0;
	uint32_t profile_mode = input[size];
	int ret = 0;

	if (!smu->pm_enabled)
		return -EINVAL;

	if (profile_mode > PP_SMC_POWER_PROFILE_CUSTOM) {
		pr_err("Invalid power profile mode %d\n", profile_mode);
		return -EINVAL;
	}

	/*
	 * Conv PP_SMC_POWER_PROFILE* to WORKLOAD_PPLIB_*_BIT
	 * Not all profile modes are supported on arcturus.
	 */
	workload_type = smu_workload_get_type(smu, profile_mode);
	if (workload_type < 0) {
		pr_err("Unsupported power profile mode %d on arcturus\n", profile_mode);
		return -EINVAL;
	}

	ret = smu_send_smc_msg_with_param(smu,
					  SMU_MSG_SetWorkloadMask,
					  1 << workload_type);
	if (ret) {
		pr_err("Fail to set workload type %d\n", workload_type);
		return ret;
	}

	smu->power_profile_mode = profile_mode;

	return 0;
}

static void arcturus_dump_pptable(struct smu_context *smu)
{
	struct smu_table_context *table_context = &smu->smu_table;
	PPTable_t *pptable = table_context->driver_pptable;
	int i;

	pr_info("Dumped PPTable:\n");

	pr_info("Version = 0x%08x\n", pptable->Version);

	pr_info("FeaturesToRun[0] = 0x%08x\n", pptable->FeaturesToRun[0]);
	pr_info("FeaturesToRun[1] = 0x%08x\n", pptable->FeaturesToRun[1]);

	for (i = 0; i < PPT_THROTTLER_COUNT; i++) {
		pr_info("SocketPowerLimitAc[%d] = %d\n", i, pptable->SocketPowerLimitAc[i]);
		pr_info("SocketPowerLimitAcTau[%d] = %d\n", i, pptable->SocketPowerLimitAcTau[i]);
	}

	pr_info("TdcLimitSoc = %d\n", pptable->TdcLimitSoc);
	pr_info("TdcLimitSocTau = %d\n", pptable->TdcLimitSocTau);
	pr_info("TdcLimitGfx = %d\n", pptable->TdcLimitGfx);
	pr_info("TdcLimitGfxTau = %d\n", pptable->TdcLimitGfxTau);

	pr_info("TedgeLimit = %d\n", pptable->TedgeLimit);
	pr_info("ThotspotLimit = %d\n", pptable->ThotspotLimit);
	pr_info("TmemLimit = %d\n", pptable->TmemLimit);
	pr_info("Tvr_gfxLimit = %d\n", pptable->Tvr_gfxLimit);
	pr_info("Tvr_memLimit = %d\n", pptable->Tvr_memLimit);
	pr_info("Tvr_socLimit = %d\n", pptable->Tvr_socLimit);
	pr_info("FitLimit = %d\n", pptable->FitLimit);

	pr_info("PpmPowerLimit = %d\n", pptable->PpmPowerLimit);
	pr_info("PpmTemperatureThreshold = %d\n", pptable->PpmTemperatureThreshold);

	pr_info("ThrottlerControlMask = %d\n", pptable->ThrottlerControlMask);

	pr_info("UlvVoltageOffsetGfx = %d\n", pptable->UlvVoltageOffsetGfx);
	pr_info("UlvPadding = 0x%08x\n", pptable->UlvPadding);

	pr_info("UlvGfxclkBypass = %d\n", pptable->UlvGfxclkBypass);
	pr_info("Padding234[0] = 0x%02x\n", pptable->Padding234[0]);
	pr_info("Padding234[1] = 0x%02x\n", pptable->Padding234[1]);
	pr_info("Padding234[2] = 0x%02x\n", pptable->Padding234[2]);

	pr_info("MinVoltageGfx = %d\n", pptable->MinVoltageGfx);
	pr_info("MinVoltageSoc = %d\n", pptable->MinVoltageSoc);
	pr_info("MaxVoltageGfx = %d\n", pptable->MaxVoltageGfx);
	pr_info("MaxVoltageSoc = %d\n", pptable->MaxVoltageSoc);

	pr_info("LoadLineResistanceGfx = %d\n", pptable->LoadLineResistanceGfx);
	pr_info("LoadLineResistanceSoc = %d\n", pptable->LoadLineResistanceSoc);

	pr_info("[PPCLK_GFXCLK]\n"
			"  .VoltageMode          = 0x%02x\n"
			"  .SnapToDiscrete       = 0x%02x\n"
			"  .NumDiscreteLevels    = 0x%02x\n"
			"  .padding              = 0x%02x\n"
			"  .ConversionToAvfsClk{m = 0x%08x b = 0x%08x}\n"
			"  .SsCurve            {a = 0x%08x b = 0x%08x c = 0x%08x}\n"
			"  .SsFmin               = 0x%04x\n"
			"  .Padding_16           = 0x%04x\n",
			pptable->DpmDescriptor[PPCLK_GFXCLK].VoltageMode,
			pptable->DpmDescriptor[PPCLK_GFXCLK].SnapToDiscrete,
			pptable->DpmDescriptor[PPCLK_GFXCLK].NumDiscreteLevels,
			pptable->DpmDescriptor[PPCLK_GFXCLK].padding,
			pptable->DpmDescriptor[PPCLK_GFXCLK].ConversionToAvfsClk.m,
			pptable->DpmDescriptor[PPCLK_GFXCLK].ConversionToAvfsClk.b,
			pptable->DpmDescriptor[PPCLK_GFXCLK].SsCurve.a,
			pptable->DpmDescriptor[PPCLK_GFXCLK].SsCurve.b,
			pptable->DpmDescriptor[PPCLK_GFXCLK].SsCurve.c,
			pptable->DpmDescriptor[PPCLK_GFXCLK].SsFmin,
			pptable->DpmDescriptor[PPCLK_GFXCLK].Padding16);

	pr_info("[PPCLK_VCLK]\n"
			"  .VoltageMode          = 0x%02x\n"
			"  .SnapToDiscrete       = 0x%02x\n"
			"  .NumDiscreteLevels    = 0x%02x\n"
			"  .padding              = 0x%02x\n"
			"  .ConversionToAvfsClk{m = 0x%08x b = 0x%08x}\n"
			"  .SsCurve            {a = 0x%08x b = 0x%08x c = 0x%08x}\n"
			"  .SsFmin               = 0x%04x\n"
			"  .Padding_16           = 0x%04x\n",
			pptable->DpmDescriptor[PPCLK_VCLK].VoltageMode,
			pptable->DpmDescriptor[PPCLK_VCLK].SnapToDiscrete,
			pptable->DpmDescriptor[PPCLK_VCLK].NumDiscreteLevels,
			pptable->DpmDescriptor[PPCLK_VCLK].padding,
			pptable->DpmDescriptor[PPCLK_VCLK].ConversionToAvfsClk.m,
			pptable->DpmDescriptor[PPCLK_VCLK].ConversionToAvfsClk.b,
			pptable->DpmDescriptor[PPCLK_VCLK].SsCurve.a,
			pptable->DpmDescriptor[PPCLK_VCLK].SsCurve.b,
			pptable->DpmDescriptor[PPCLK_VCLK].SsCurve.c,
			pptable->DpmDescriptor[PPCLK_VCLK].SsFmin,
			pptable->DpmDescriptor[PPCLK_VCLK].Padding16);

	pr_info("[PPCLK_DCLK]\n"
			"  .VoltageMode          = 0x%02x\n"
			"  .SnapToDiscrete       = 0x%02x\n"
			"  .NumDiscreteLevels    = 0x%02x\n"
			"  .padding              = 0x%02x\n"
			"  .ConversionToAvfsClk{m = 0x%08x b = 0x%08x}\n"
			"  .SsCurve            {a = 0x%08x b = 0x%08x c = 0x%08x}\n"
			"  .SsFmin               = 0x%04x\n"
			"  .Padding_16           = 0x%04x\n",
			pptable->DpmDescriptor[PPCLK_DCLK].VoltageMode,
			pptable->DpmDescriptor[PPCLK_DCLK].SnapToDiscrete,
			pptable->DpmDescriptor[PPCLK_DCLK].NumDiscreteLevels,
			pptable->DpmDescriptor[PPCLK_DCLK].padding,
			pptable->DpmDescriptor[PPCLK_DCLK].ConversionToAvfsClk.m,
			pptable->DpmDescriptor[PPCLK_DCLK].ConversionToAvfsClk.b,
			pptable->DpmDescriptor[PPCLK_DCLK].SsCurve.a,
			pptable->DpmDescriptor[PPCLK_DCLK].SsCurve.b,
			pptable->DpmDescriptor[PPCLK_DCLK].SsCurve.c,
			pptable->DpmDescriptor[PPCLK_DCLK].SsFmin,
			pptable->DpmDescriptor[PPCLK_DCLK].Padding16);

	pr_info("[PPCLK_SOCCLK]\n"
			"  .VoltageMode          = 0x%02x\n"
			"  .SnapToDiscrete       = 0x%02x\n"
			"  .NumDiscreteLevels    = 0x%02x\n"
			"  .padding              = 0x%02x\n"
			"  .ConversionToAvfsClk{m = 0x%08x b = 0x%08x}\n"
			"  .SsCurve            {a = 0x%08x b = 0x%08x c = 0x%08x}\n"
			"  .SsFmin               = 0x%04x\n"
			"  .Padding_16           = 0x%04x\n",
			pptable->DpmDescriptor[PPCLK_SOCCLK].VoltageMode,
			pptable->DpmDescriptor[PPCLK_SOCCLK].SnapToDiscrete,
			pptable->DpmDescriptor[PPCLK_SOCCLK].NumDiscreteLevels,
			pptable->DpmDescriptor[PPCLK_SOCCLK].padding,
			pptable->DpmDescriptor[PPCLK_SOCCLK].ConversionToAvfsClk.m,
			pptable->DpmDescriptor[PPCLK_SOCCLK].ConversionToAvfsClk.b,
			pptable->DpmDescriptor[PPCLK_SOCCLK].SsCurve.a,
			pptable->DpmDescriptor[PPCLK_SOCCLK].SsCurve.b,
			pptable->DpmDescriptor[PPCLK_SOCCLK].SsCurve.c,
			pptable->DpmDescriptor[PPCLK_SOCCLK].SsFmin,
			pptable->DpmDescriptor[PPCLK_SOCCLK].Padding16);

	pr_info("[PPCLK_UCLK]\n"
			"  .VoltageMode          = 0x%02x\n"
			"  .SnapToDiscrete       = 0x%02x\n"
			"  .NumDiscreteLevels    = 0x%02x\n"
			"  .padding              = 0x%02x\n"
			"  .ConversionToAvfsClk{m = 0x%08x b = 0x%08x}\n"
			"  .SsCurve            {a = 0x%08x b = 0x%08x c = 0x%08x}\n"
			"  .SsFmin               = 0x%04x\n"
			"  .Padding_16           = 0x%04x\n",
			pptable->DpmDescriptor[PPCLK_UCLK].VoltageMode,
			pptable->DpmDescriptor[PPCLK_UCLK].SnapToDiscrete,
			pptable->DpmDescriptor[PPCLK_UCLK].NumDiscreteLevels,
			pptable->DpmDescriptor[PPCLK_UCLK].padding,
			pptable->DpmDescriptor[PPCLK_UCLK].ConversionToAvfsClk.m,
			pptable->DpmDescriptor[PPCLK_UCLK].ConversionToAvfsClk.b,
			pptable->DpmDescriptor[PPCLK_UCLK].SsCurve.a,
			pptable->DpmDescriptor[PPCLK_UCLK].SsCurve.b,
			pptable->DpmDescriptor[PPCLK_UCLK].SsCurve.c,
			pptable->DpmDescriptor[PPCLK_UCLK].SsFmin,
			pptable->DpmDescriptor[PPCLK_UCLK].Padding16);

	pr_info("[PPCLK_FCLK]\n"
			"  .VoltageMode          = 0x%02x\n"
			"  .SnapToDiscrete       = 0x%02x\n"
			"  .NumDiscreteLevels    = 0x%02x\n"
			"  .padding              = 0x%02x\n"
			"  .ConversionToAvfsClk{m = 0x%08x b = 0x%08x}\n"
			"  .SsCurve            {a = 0x%08x b = 0x%08x c = 0x%08x}\n"
			"  .SsFmin               = 0x%04x\n"
			"  .Padding_16           = 0x%04x\n",
			pptable->DpmDescriptor[PPCLK_FCLK].VoltageMode,
			pptable->DpmDescriptor[PPCLK_FCLK].SnapToDiscrete,
			pptable->DpmDescriptor[PPCLK_FCLK].NumDiscreteLevels,
			pptable->DpmDescriptor[PPCLK_FCLK].padding,
			pptable->DpmDescriptor[PPCLK_FCLK].ConversionToAvfsClk.m,
			pptable->DpmDescriptor[PPCLK_FCLK].ConversionToAvfsClk.b,
			pptable->DpmDescriptor[PPCLK_FCLK].SsCurve.a,
			pptable->DpmDescriptor[PPCLK_FCLK].SsCurve.b,
			pptable->DpmDescriptor[PPCLK_FCLK].SsCurve.c,
			pptable->DpmDescriptor[PPCLK_FCLK].SsFmin,
			pptable->DpmDescriptor[PPCLK_FCLK].Padding16);


	pr_info("FreqTableGfx\n");
	for (i = 0; i < NUM_GFXCLK_DPM_LEVELS; i++)
		pr_info("  .[%02d] = %d\n", i, pptable->FreqTableGfx[i]);

	pr_info("FreqTableVclk\n");
	for (i = 0; i < NUM_VCLK_DPM_LEVELS; i++)
		pr_info("  .[%02d] = %d\n", i, pptable->FreqTableVclk[i]);

	pr_info("FreqTableDclk\n");
	for (i = 0; i < NUM_DCLK_DPM_LEVELS; i++)
		pr_info("  .[%02d] = %d\n", i, pptable->FreqTableDclk[i]);

	pr_info("FreqTableSocclk\n");
	for (i = 0; i < NUM_SOCCLK_DPM_LEVELS; i++)
		pr_info("  .[%02d] = %d\n", i, pptable->FreqTableSocclk[i]);

	pr_info("FreqTableUclk\n");
	for (i = 0; i < NUM_UCLK_DPM_LEVELS; i++)
		pr_info("  .[%02d] = %d\n", i, pptable->FreqTableUclk[i]);

	pr_info("FreqTableFclk\n");
	for (i = 0; i < NUM_FCLK_DPM_LEVELS; i++)
		pr_info("  .[%02d] = %d\n", i, pptable->FreqTableFclk[i]);

	pr_info("Mp0clkFreq\n");
	for (i = 0; i < NUM_MP0CLK_DPM_LEVELS; i++)
		pr_info("  .[%d] = %d\n", i, pptable->Mp0clkFreq[i]);

	pr_info("Mp0DpmVoltage\n");
	for (i = 0; i < NUM_MP0CLK_DPM_LEVELS; i++)
		pr_info("  .[%d] = %d\n", i, pptable->Mp0DpmVoltage[i]);

	pr_info("GfxclkFidle = 0x%x\n", pptable->GfxclkFidle);
	pr_info("GfxclkSlewRate = 0x%x\n", pptable->GfxclkSlewRate);
	pr_info("Padding567[0] = 0x%x\n", pptable->Padding567[0]);
	pr_info("Padding567[1] = 0x%x\n", pptable->Padding567[1]);
	pr_info("Padding567[2] = 0x%x\n", pptable->Padding567[2]);
	pr_info("Padding567[3] = 0x%x\n", pptable->Padding567[3]);
	pr_info("GfxclkDsMaxFreq = %d\n", pptable->GfxclkDsMaxFreq);
	pr_info("GfxclkSource = 0x%x\n", pptable->GfxclkSource);
	pr_info("Padding456 = 0x%x\n", pptable->Padding456);

	pr_info("EnableTdpm = %d\n", pptable->EnableTdpm);
	pr_info("TdpmHighHystTemperature = %d\n", pptable->TdpmHighHystTemperature);
	pr_info("TdpmLowHystTemperature = %d\n", pptable->TdpmLowHystTemperature);
	pr_info("GfxclkFreqHighTempLimit = %d\n", pptable->GfxclkFreqHighTempLimit);

	pr_info("FanStopTemp = %d\n", pptable->FanStopTemp);
	pr_info("FanStartTemp = %d\n", pptable->FanStartTemp);

	pr_info("FanGainEdge = %d\n", pptable->FanGainEdge);
	pr_info("FanGainHotspot = %d\n", pptable->FanGainHotspot);
	pr_info("FanGainVrGfx = %d\n", pptable->FanGainVrGfx);
	pr_info("FanGainVrSoc = %d\n", pptable->FanGainVrSoc);
	pr_info("FanGainVrMem = %d\n", pptable->FanGainVrMem);
	pr_info("FanGainHbm = %d\n", pptable->FanGainHbm);

	pr_info("FanPwmMin = %d\n", pptable->FanPwmMin);
	pr_info("FanAcousticLimitRpm = %d\n", pptable->FanAcousticLimitRpm);
	pr_info("FanThrottlingRpm = %d\n", pptable->FanThrottlingRpm);
	pr_info("FanMaximumRpm = %d\n", pptable->FanMaximumRpm);
	pr_info("FanTargetTemperature = %d\n", pptable->FanTargetTemperature);
	pr_info("FanTargetGfxclk = %d\n", pptable->FanTargetGfxclk);
	pr_info("FanZeroRpmEnable = %d\n", pptable->FanZeroRpmEnable);
	pr_info("FanTachEdgePerRev = %d\n", pptable->FanTachEdgePerRev);
	pr_info("FanTempInputSelect = %d\n", pptable->FanTempInputSelect);

	pr_info("FuzzyFan_ErrorSetDelta = %d\n", pptable->FuzzyFan_ErrorSetDelta);
	pr_info("FuzzyFan_ErrorRateSetDelta = %d\n", pptable->FuzzyFan_ErrorRateSetDelta);
	pr_info("FuzzyFan_PwmSetDelta = %d\n", pptable->FuzzyFan_PwmSetDelta);
	pr_info("FuzzyFan_Reserved = %d\n", pptable->FuzzyFan_Reserved);

	pr_info("OverrideAvfsGb[AVFS_VOLTAGE_GFX] = 0x%x\n", pptable->OverrideAvfsGb[AVFS_VOLTAGE_GFX]);
	pr_info("OverrideAvfsGb[AVFS_VOLTAGE_SOC] = 0x%x\n", pptable->OverrideAvfsGb[AVFS_VOLTAGE_SOC]);
	pr_info("Padding8_Avfs[0] = %d\n", pptable->Padding8_Avfs[0]);
	pr_info("Padding8_Avfs[1] = %d\n", pptable->Padding8_Avfs[1]);

	pr_info("dBtcGbGfxPll{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->dBtcGbGfxPll.a,
			pptable->dBtcGbGfxPll.b,
			pptable->dBtcGbGfxPll.c);
	pr_info("dBtcGbGfxAfll{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->dBtcGbGfxAfll.a,
			pptable->dBtcGbGfxAfll.b,
			pptable->dBtcGbGfxAfll.c);
	pr_info("dBtcGbSoc{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->dBtcGbSoc.a,
			pptable->dBtcGbSoc.b,
			pptable->dBtcGbSoc.c);

	pr_info("qAgingGb[AVFS_VOLTAGE_GFX]{m = 0x%x b = 0x%x}\n",
			pptable->qAgingGb[AVFS_VOLTAGE_GFX].m,
			pptable->qAgingGb[AVFS_VOLTAGE_GFX].b);
	pr_info("qAgingGb[AVFS_VOLTAGE_SOC]{m = 0x%x b = 0x%x}\n",
			pptable->qAgingGb[AVFS_VOLTAGE_SOC].m,
			pptable->qAgingGb[AVFS_VOLTAGE_SOC].b);

	pr_info("qStaticVoltageOffset[AVFS_VOLTAGE_GFX]{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->qStaticVoltageOffset[AVFS_VOLTAGE_GFX].a,
			pptable->qStaticVoltageOffset[AVFS_VOLTAGE_GFX].b,
			pptable->qStaticVoltageOffset[AVFS_VOLTAGE_GFX].c);
	pr_info("qStaticVoltageOffset[AVFS_VOLTAGE_SOC]{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->qStaticVoltageOffset[AVFS_VOLTAGE_SOC].a,
			pptable->qStaticVoltageOffset[AVFS_VOLTAGE_SOC].b,
			pptable->qStaticVoltageOffset[AVFS_VOLTAGE_SOC].c);

	pr_info("DcTol[AVFS_VOLTAGE_GFX] = 0x%x\n", pptable->DcTol[AVFS_VOLTAGE_GFX]);
	pr_info("DcTol[AVFS_VOLTAGE_SOC] = 0x%x\n", pptable->DcTol[AVFS_VOLTAGE_SOC]);

	pr_info("DcBtcEnabled[AVFS_VOLTAGE_GFX] = 0x%x\n", pptable->DcBtcEnabled[AVFS_VOLTAGE_GFX]);
	pr_info("DcBtcEnabled[AVFS_VOLTAGE_SOC] = 0x%x\n", pptable->DcBtcEnabled[AVFS_VOLTAGE_SOC]);
	pr_info("Padding8_GfxBtc[0] = 0x%x\n", pptable->Padding8_GfxBtc[0]);
	pr_info("Padding8_GfxBtc[1] = 0x%x\n", pptable->Padding8_GfxBtc[1]);

	pr_info("DcBtcMin[AVFS_VOLTAGE_GFX] = 0x%x\n", pptable->DcBtcMin[AVFS_VOLTAGE_GFX]);
	pr_info("DcBtcMin[AVFS_VOLTAGE_SOC] = 0x%x\n", pptable->DcBtcMin[AVFS_VOLTAGE_SOC]);
	pr_info("DcBtcMax[AVFS_VOLTAGE_GFX] = 0x%x\n", pptable->DcBtcMax[AVFS_VOLTAGE_GFX]);
	pr_info("DcBtcMax[AVFS_VOLTAGE_SOC] = 0x%x\n", pptable->DcBtcMax[AVFS_VOLTAGE_SOC]);

	pr_info("DcBtcGb[AVFS_VOLTAGE_GFX] = 0x%x\n", pptable->DcBtcGb[AVFS_VOLTAGE_GFX]);
	pr_info("DcBtcGb[AVFS_VOLTAGE_SOC] = 0x%x\n", pptable->DcBtcGb[AVFS_VOLTAGE_SOC]);

	pr_info("XgmiDpmPstates\n");
	for (i = 0; i < NUM_XGMI_LEVELS; i++)
		pr_info("  .[%d] = %d\n", i, pptable->XgmiDpmPstates[i]);
	pr_info("XgmiDpmSpare[0] = 0x%02x\n", pptable->XgmiDpmSpare[0]);
	pr_info("XgmiDpmSpare[1] = 0x%02x\n", pptable->XgmiDpmSpare[1]);

	pr_info("VDDGFX_TVmin = %d\n", pptable->VDDGFX_TVmin);
	pr_info("VDDSOC_TVmin = %d\n", pptable->VDDSOC_TVmin);
	pr_info("VDDGFX_Vmin_HiTemp = %d\n", pptable->VDDGFX_Vmin_HiTemp);
	pr_info("VDDGFX_Vmin_LoTemp = %d\n", pptable->VDDGFX_Vmin_LoTemp);
	pr_info("VDDSOC_Vmin_HiTemp = %d\n", pptable->VDDSOC_Vmin_HiTemp);
	pr_info("VDDSOC_Vmin_LoTemp = %d\n", pptable->VDDSOC_Vmin_LoTemp);
	pr_info("VDDGFX_TVminHystersis = %d\n", pptable->VDDGFX_TVminHystersis);
	pr_info("VDDSOC_TVminHystersis = %d\n", pptable->VDDSOC_TVminHystersis);

	pr_info("DebugOverrides = 0x%x\n", pptable->DebugOverrides);
	pr_info("ReservedEquation0{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->ReservedEquation0.a,
			pptable->ReservedEquation0.b,
			pptable->ReservedEquation0.c);
	pr_info("ReservedEquation1{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->ReservedEquation1.a,
			pptable->ReservedEquation1.b,
			pptable->ReservedEquation1.c);
	pr_info("ReservedEquation2{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->ReservedEquation2.a,
			pptable->ReservedEquation2.b,
			pptable->ReservedEquation2.c);
	pr_info("ReservedEquation3{a = 0x%x b = 0x%x c = 0x%x}\n",
			pptable->ReservedEquation3.a,
			pptable->ReservedEquation3.b,
			pptable->ReservedEquation3.c);

	pr_info("MinVoltageUlvGfx = %d\n", pptable->MinVoltageUlvGfx);
	pr_info("PaddingUlv = %d\n", pptable->PaddingUlv);

	pr_info("TotalPowerConfig = %d\n", pptable->TotalPowerConfig);
	pr_info("TotalPowerSpare1 = %d\n", pptable->TotalPowerSpare1);
	pr_info("TotalPowerSpare2 = %d\n", pptable->TotalPowerSpare2);

	pr_info("PccThresholdLow = %d\n", pptable->PccThresholdLow);
	pr_info("PccThresholdHigh = %d\n", pptable->PccThresholdHigh);

	pr_info("Board Parameters:\n");
	pr_info("MaxVoltageStepGfx = 0x%x\n", pptable->MaxVoltageStepGfx);
	pr_info("MaxVoltageStepSoc = 0x%x\n", pptable->MaxVoltageStepSoc);

	pr_info("VddGfxVrMapping = 0x%x\n", pptable->VddGfxVrMapping);
	pr_info("VddSocVrMapping = 0x%x\n", pptable->VddSocVrMapping);
	pr_info("VddMemVrMapping = 0x%x\n", pptable->VddMemVrMapping);
	pr_info("BoardVrMapping = 0x%x\n", pptable->BoardVrMapping);

	pr_info("GfxUlvPhaseSheddingMask = 0x%x\n", pptable->GfxUlvPhaseSheddingMask);
	pr_info("ExternalSensorPresent = 0x%x\n", pptable->ExternalSensorPresent);

	pr_info("GfxMaxCurrent = 0x%x\n", pptable->GfxMaxCurrent);
	pr_info("GfxOffset = 0x%x\n", pptable->GfxOffset);
	pr_info("Padding_TelemetryGfx = 0x%x\n", pptable->Padding_TelemetryGfx);

	pr_info("SocMaxCurrent = 0x%x\n", pptable->SocMaxCurrent);
	pr_info("SocOffset = 0x%x\n", pptable->SocOffset);
	pr_info("Padding_TelemetrySoc = 0x%x\n", pptable->Padding_TelemetrySoc);

	pr_info("MemMaxCurrent = 0x%x\n", pptable->MemMaxCurrent);
	pr_info("MemOffset = 0x%x\n", pptable->MemOffset);
	pr_info("Padding_TelemetryMem = 0x%x\n", pptable->Padding_TelemetryMem);

	pr_info("BoardMaxCurrent = 0x%x\n", pptable->BoardMaxCurrent);
	pr_info("BoardOffset = 0x%x\n", pptable->BoardOffset);
	pr_info("Padding_TelemetryBoardInput = 0x%x\n", pptable->Padding_TelemetryBoardInput);

	pr_info("VR0HotGpio = %d\n", pptable->VR0HotGpio);
	pr_info("VR0HotPolarity = %d\n", pptable->VR0HotPolarity);
	pr_info("VR1HotGpio = %d\n", pptable->VR1HotGpio);
	pr_info("VR1HotPolarity = %d\n", pptable->VR1HotPolarity);

	pr_info("PllGfxclkSpreadEnabled = %d\n", pptable->PllGfxclkSpreadEnabled);
	pr_info("PllGfxclkSpreadPercent = %d\n", pptable->PllGfxclkSpreadPercent);
	pr_info("PllGfxclkSpreadFreq = %d\n", pptable->PllGfxclkSpreadFreq);

	pr_info("UclkSpreadEnabled = %d\n", pptable->UclkSpreadEnabled);
	pr_info("UclkSpreadPercent = %d\n", pptable->UclkSpreadPercent);
	pr_info("UclkSpreadFreq = %d\n", pptable->UclkSpreadFreq);

	pr_info("FclkSpreadEnabled = %d\n", pptable->FclkSpreadEnabled);
	pr_info("FclkSpreadPercent = %d\n", pptable->FclkSpreadPercent);
	pr_info("FclkSpreadFreq = %d\n", pptable->FclkSpreadFreq);

	pr_info("FllGfxclkSpreadEnabled = %d\n", pptable->FllGfxclkSpreadEnabled);
	pr_info("FllGfxclkSpreadPercent = %d\n", pptable->FllGfxclkSpreadPercent);
	pr_info("FllGfxclkSpreadFreq = %d\n", pptable->FllGfxclkSpreadFreq);

	for (i = 0; i < NUM_I2C_CONTROLLERS; i++) {
		pr_info("I2cControllers[%d]:\n", i);
		pr_info("                   .Enabled = %d\n",
				pptable->I2cControllers[i].Enabled);
		pr_info("                   .SlaveAddress = 0x%x\n",
				pptable->I2cControllers[i].SlaveAddress);
		pr_info("                   .ControllerPort = %d\n",
				pptable->I2cControllers[i].ControllerPort);
		pr_info("                   .ControllerName = %d\n",
				pptable->I2cControllers[i].ControllerName);
		pr_info("                   .ThermalThrottler = %d\n",
				pptable->I2cControllers[i].ThermalThrotter);
		pr_info("                   .I2cProtocol = %d\n",
				pptable->I2cControllers[i].I2cProtocol);
		pr_info("                   .Speed = %d\n",
				pptable->I2cControllers[i].Speed);
	}

	pr_info("MemoryChannelEnabled = %d\n", pptable->MemoryChannelEnabled);
	pr_info("DramBitWidth = %d\n", pptable->DramBitWidth);

	pr_info("TotalBoardPower = %d\n", pptable->TotalBoardPower);

	pr_info("XgmiLinkSpeed\n");
	for (i = 0; i < NUM_XGMI_PSTATE_LEVELS; i++)
		pr_info("  .[%d] = %d\n", i, pptable->XgmiLinkSpeed[i]);
	pr_info("XgmiLinkWidth\n");
	for (i = 0; i < NUM_XGMI_PSTATE_LEVELS; i++)
		pr_info("  .[%d] = %d\n", i, pptable->XgmiLinkWidth[i]);
	pr_info("XgmiFclkFreq\n");
	for (i = 0; i < NUM_XGMI_PSTATE_LEVELS; i++)
		pr_info("  .[%d] = %d\n", i, pptable->XgmiFclkFreq[i]);
	pr_info("XgmiSocVoltage\n");
	for (i = 0; i < NUM_XGMI_PSTATE_LEVELS; i++)
		pr_info("  .[%d] = %d\n", i, pptable->XgmiSocVoltage[i]);

}

static bool arcturus_is_dpm_running(struct smu_context *smu)
{
	int ret = 0;
	uint32_t feature_mask[2];
	unsigned long feature_enabled;
	ret = smu_feature_get_enabled_mask(smu, feature_mask, 2);
	feature_enabled = (unsigned long)((uint64_t)feature_mask[0] |
			   ((uint64_t)feature_mask[1] << 32));
	return !!(feature_enabled & SMC_DPM_FEATURE);
}

static const struct pptable_funcs arcturus_ppt_funcs = {
	/* translate smu index into arcturus specific index */
	.get_smu_msg_index = arcturus_get_smu_msg_index,
	.get_smu_clk_index = arcturus_get_smu_clk_index,
	.get_smu_feature_index = arcturus_get_smu_feature_index,
	.get_smu_table_index = arcturus_get_smu_table_index,
	.get_smu_power_index= arcturus_get_pwr_src_index,
	.get_workload_type = arcturus_get_workload_type,
	/* internal structurs allocations */
	.tables_init = arcturus_tables_init,
	.alloc_dpm_context = arcturus_allocate_dpm_context,
	/* pptable related */
	.check_powerplay_table = arcturus_check_powerplay_table,
	.store_powerplay_table = arcturus_store_powerplay_table,
	.append_powerplay_table = arcturus_append_powerplay_table,
	/* init dpm */
	.get_allowed_feature_mask = arcturus_get_allowed_feature_mask,
	/* btc */
	.run_afll_btc = arcturus_run_btc_afll,
	/* dpm/clk tables */
	.set_default_dpm_table = arcturus_set_default_dpm_table,
	.populate_umd_state_clk = arcturus_populate_umd_state_clk,
	.get_thermal_temperature_range = arcturus_get_thermal_temperature_range,
	.get_current_clk_freq_by_table = arcturus_get_current_clk_freq_by_table,
	.print_clk_levels = arcturus_print_clk_levels,
	.force_clk_levels = arcturus_force_clk_levels,
	.read_sensor = arcturus_read_sensor,
	.get_fan_speed_percent = arcturus_get_fan_speed_percent,
	.get_fan_speed_rpm = arcturus_get_fan_speed_rpm,
	.force_dpm_limit_value = arcturus_force_dpm_limit_value,
	.unforce_dpm_levels = arcturus_unforce_dpm_levels,
	.get_profiling_clk_mask = arcturus_get_profiling_clk_mask,
	.get_power_profile_mode = arcturus_get_power_profile_mode,
	.set_power_profile_mode = arcturus_set_power_profile_mode,
	/* debug (internal used) */
	.dump_pptable = arcturus_dump_pptable,
	.get_power_limit = arcturus_get_power_limit,
	.is_dpm_running = arcturus_is_dpm_running,
};

void arcturus_set_ppt_funcs(struct smu_context *smu)
{
	struct smu_table_context *smu_table = &smu->smu_table;

	smu->ppt_funcs = &arcturus_ppt_funcs;
	smu_table->table_count = TABLE_COUNT;
}
