/*
 * Copyright 2012-16 Advanced Micro Devices, Inc.
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
 * Authors: AMD
 *
 */

#include "core_types.h"
#include "clk_mgr_internal.h"
#include "reg_helper.h"

#include "renoir_ip_offset.h"

#include "mp/mp_12_0_0_offset.h"
#include "mp/mp_12_0_0_sh_mask.h"

#define REG(reg_name) \
	(MP0_BASE.instance[0].segment[mm ## reg_name ## _BASE_IDX] + mm ## reg_name)

#define FN(reg_name, field) \
	FD(reg_name##__##field)

#define VBIOSSMC_MSG_TestMessage                  0x1
#define VBIOSSMC_MSG_GetSmuVersion                0x2
#define VBIOSSMC_MSG_PowerUpGfx                   0x3
#define VBIOSSMC_MSG_SetDispclkFreq               0x4
#define VBIOSSMC_MSG_SetDprefclkFreq              0x5
#define VBIOSSMC_MSG_PowerDownGfx                 0x6
#define VBIOSSMC_MSG_SetDppclkFreq                0x7
#define VBIOSSMC_MSG_SetHardMinDcfclkByFreq       0x8
#define VBIOSSMC_MSG_SetMinDeepSleepDcfclk        0x9
#define VBIOSSMC_MSG_SetPhyclkVoltageByFreq       0xA
#define VBIOSSMC_MSG_GetFclkFrequency             0xB
#define VBIOSSMC_MSG_SetDisplayCount              0xC
#define VBIOSSMC_MSG_EnableTmdp48MHzRefclkPwrDown 0xD
#define VBIOSSMC_MSG_UpdatePmeRestore			  0xE

int rn_vbios_smu_send_msg_with_param(struct clk_mgr_internal *clk_mgr, unsigned int msg_id, unsigned int param)
{
	/* First clear response register */
	REG_WRITE(MP1_SMN_C2PMSG_91, 0);

	/* Set the parameter register for the SMU message, unit is Mhz */
	REG_WRITE(MP1_SMN_C2PMSG_83, param);

	/* Trigger the message transaction by writing the message ID */
	REG_WRITE(MP1_SMN_C2PMSG_67, msg_id);

	REG_WAIT(MP1_SMN_C2PMSG_91, CONTENT, 1, 10, 200000);

	/* Actual dispclk set is returned in the parameter register */
	return REG_READ(MP1_SMN_C2PMSG_83);
}

int rn_vbios_smu_get_smu_version(struct clk_mgr_internal *clk_mgr)
{
	return rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_GetSmuVersion,
			0);
}


int rn_vbios_smu_set_dispclk(struct clk_mgr_internal *clk_mgr, int requested_dispclk_khz)
{
	int actual_dispclk_set_mhz = -1;
	struct dc *core_dc = clk_mgr->base.ctx->dc;
	struct dmcu *dmcu = core_dc->res_pool->dmcu;
	uint32_t clk = requested_dispclk_khz / 1000;

	if (clk <= 100)
		clk = 101;

	/*  Unit of SMU msg parameter is Mhz */
	actual_dispclk_set_mhz = rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_SetDispclkFreq,
			clk);

	if (!IS_FPGA_MAXIMUS_DC(core_dc->ctx->dce_environment)) {
		if (dmcu && dmcu->funcs->is_dmcu_initialized(dmcu)) {
			if (clk_mgr->dfs_bypass_disp_clk != actual_dispclk_set_mhz)
				dmcu->funcs->set_psr_wait_loop(dmcu,
						actual_dispclk_set_mhz / 7);
		}
	}

	return actual_dispclk_set_mhz * 1000;
}

int rn_vbios_smu_set_dprefclk(struct clk_mgr_internal *clk_mgr)
{
	int actual_dprefclk_set_mhz = -1;

	actual_dprefclk_set_mhz = rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_SetDprefclkFreq,
			clk_mgr->base.dprefclk_khz / 1000);

	/* TODO: add code for programing DP DTO, currently this is down by command table */

	return actual_dprefclk_set_mhz * 1000;
}

int rn_vbios_smu_set_hard_min_dcfclk(struct clk_mgr_internal *clk_mgr, int requested_dcfclk_khz)
{
	int actual_dcfclk_set_mhz = -1;

	if (clk_mgr->smu_ver < 0xFFFFFFFF)
		return actual_dcfclk_set_mhz;

	actual_dcfclk_set_mhz = rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_SetHardMinDcfclkByFreq,
			requested_dcfclk_khz / 1000);

	return actual_dcfclk_set_mhz * 1000;
}

int rn_vbios_smu_set_min_deep_sleep_dcfclk(struct clk_mgr_internal *clk_mgr, int requested_min_ds_dcfclk_khz)
{
	int actual_min_ds_dcfclk_mhz = -1;

	if (clk_mgr->smu_ver < 0xFFFFFFFF)
		return actual_min_ds_dcfclk_mhz;

	actual_min_ds_dcfclk_mhz = rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_SetMinDeepSleepDcfclk,
			requested_min_ds_dcfclk_khz / 1000);

	return actual_min_ds_dcfclk_mhz * 1000;
}

void rn_vbios_smu_set_phyclk(struct clk_mgr_internal *clk_mgr, int requested_phyclk_khz)
{
	rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_SetPhyclkVoltageByFreq,
			requested_phyclk_khz / 1000);
}

int rn_vbios_smu_set_dppclk(struct clk_mgr_internal *clk_mgr, int requested_dpp_khz)
{
	int actual_dppclk_set_mhz = -1;

	uint32_t clk = requested_dpp_khz / 1000;

	if (clk <= 100)
		clk = 101;

	actual_dppclk_set_mhz = rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_SetDppclkFreq,
			clk);

	return actual_dppclk_set_mhz * 1000;
}

void rn_vbios_smu_set_display_count(struct clk_mgr_internal *clk_mgr, int display_count)
{
	rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_SetDisplayCount,
			display_count);
}

void rn_vbios_smu_enable_48mhz_tmdp_refclk_pwrdwn(struct clk_mgr_internal *clk_mgr)
{
	rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_EnableTmdp48MHzRefclkPwrDown,
			0);
}

void rn_vbios_smu_enable_pme_wa(struct clk_mgr_internal *clk_mgr)
{
	rn_vbios_smu_send_msg_with_param(
			clk_mgr,
			VBIOSSMC_MSG_UpdatePmeRestore,
			0);
}
