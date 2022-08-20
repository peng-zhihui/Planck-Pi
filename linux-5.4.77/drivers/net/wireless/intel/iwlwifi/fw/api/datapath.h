/******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * Copyright (C) 2018 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * The full GNU General Public License is included in this distribution
 * in the file called COPYING.
 *
 * Contact Information:
 *  Intel Linux Wireless <linuxwifi@intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 * BSD LICENSE
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * Copyright (C) 2018 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/

#ifndef __iwl_fw_api_datapath_h__
#define __iwl_fw_api_datapath_h__

/**
 * enum iwl_data_path_subcmd_ids - data path group commands
 */
enum iwl_data_path_subcmd_ids {
	/**
	 * @DQA_ENABLE_CMD: &struct iwl_dqa_enable_cmd
	 */
	DQA_ENABLE_CMD = 0x0,

	/**
	 * @UPDATE_MU_GROUPS_CMD: &struct iwl_mu_group_mgmt_cmd
	 */
	UPDATE_MU_GROUPS_CMD = 0x1,

	/**
	 * @TRIGGER_RX_QUEUES_NOTIF_CMD: &struct iwl_rxq_sync_cmd
	 */
	TRIGGER_RX_QUEUES_NOTIF_CMD = 0x2,

	/**
	 * @STA_HE_CTXT_CMD: &struct iwl_he_sta_context_cmd
	 */
	STA_HE_CTXT_CMD = 0x7,

	/**
	 * @RFH_QUEUE_CONFIG_CMD: &struct iwl_rfh_queue_config
	 */
	RFH_QUEUE_CONFIG_CMD = 0xD,

	/**
	 * @TLC_MNG_CONFIG_CMD: &struct iwl_tlc_config_cmd
	 */
	TLC_MNG_CONFIG_CMD = 0xF,

	/**
	 * @HE_AIR_SNIFFER_CONFIG_CMD: &struct iwl_he_monitor_cmd
	 */
	HE_AIR_SNIFFER_CONFIG_CMD = 0x13,

	/**
	 * @CHEST_COLLECTOR_FILTER_CONFIG_CMD: Configure the CSI
	 *	matrix collection, uses &struct iwl_channel_estimation_cfg
	 */
	CHEST_COLLECTOR_FILTER_CONFIG_CMD = 0x14,

	/**
	 * @RX_NO_DATA_NOTIF: &struct iwl_rx_no_data
	 */
	RX_NO_DATA_NOTIF = 0xF5,

	/**
	 * @TLC_MNG_UPDATE_NOTIF: &struct iwl_tlc_update_notif
	 */
	TLC_MNG_UPDATE_NOTIF = 0xF7,

	/**
	 * @STA_PM_NOTIF: &struct iwl_mvm_pm_state_notification
	 */
	STA_PM_NOTIF = 0xFD,

	/**
	 * @MU_GROUP_MGMT_NOTIF: &struct iwl_mu_group_mgmt_notif
	 */
	MU_GROUP_MGMT_NOTIF = 0xFE,

	/**
	 * @RX_QUEUES_NOTIFICATION: &struct iwl_rxq_sync_notification
	 */
	RX_QUEUES_NOTIFICATION = 0xFF,
};

/**
 * struct iwl_mu_group_mgmt_cmd - VHT MU-MIMO group configuration
 *
 * @reserved: reserved
 * @membership_status: a bitmap of MU groups
 * @user_position:the position of station in a group. If the station is in the
 *	group then bits (group * 2) is the position -1
 */
struct iwl_mu_group_mgmt_cmd {
	__le32 reserved;
	__le32 membership_status[2];
	__le32 user_position[4];
} __packed; /* MU_GROUP_ID_MNG_TABLE_API_S_VER_1 */

/**
 * struct iwl_mu_group_mgmt_notif - VHT MU-MIMO group id notification
 *
 * @membership_status: a bitmap of MU groups
 * @user_position: the position of station in a group. If the station is in the
 *	group then bits (group * 2) is the position -1
 */
struct iwl_mu_group_mgmt_notif {
	__le32 membership_status[2];
	__le32 user_position[4];
} __packed; /* MU_GROUP_MNG_NTFY_API_S_VER_1 */

enum iwl_channel_estimation_flags {
	IWL_CHANNEL_ESTIMATION_ENABLE	= BIT(0),
	IWL_CHANNEL_ESTIMATION_TIMER	= BIT(1),
	IWL_CHANNEL_ESTIMATION_COUNTER	= BIT(2),
};

/**
 * struct iwl_channel_estimation_cfg - channel estimation reporting config
 */
struct iwl_channel_estimation_cfg {
	/**
	 * @flags: flags, see &enum iwl_channel_estimation_flags
	 */
	__le32 flags;
	/**
	 * @timer: if enabled via flags, automatically disable after this many
	 *	microseconds
	 */
	__le32 timer;
	/**
	 * @count: if enabled via flags, automatically disable after this many
	 *	frames with channel estimation matrix were captured
	 */
	__le32 count;
	/**
	 * @rate_n_flags_mask: only try to record the channel estimation matrix
	 *	if the rate_n_flags value for the received frame (let's call
	 *	that rx_rnf) matches the mask/value given here like this:
	 *	(rx_rnf & rate_n_flags_mask) == rate_n_flags_val.
	 */
	__le32 rate_n_flags_mask;
	/**
	 * @rate_n_flags_val: see @rate_n_flags_mask
	 */
	__le32 rate_n_flags_val;
	/**
	 * @reserved: reserved (for alignment)
	 */
	__le32 reserved;
	/**
	 * @frame_types: bitmap of frame types to capture, the received frame's
	 *	subtype|type takes 6 bits in the frame and the corresponding bit
	 *	in this field must be set to 1 to capture channel estimation for
	 *	that frame type. Set to all-ones to enable capturing for all
	 *	frame types.
	 */
	__le64 frame_types;
} __packed; /* CHEST_COLLECTOR_FILTER_CMD_API_S_VER_1 */

#endif /* __iwl_fw_api_datapath_h__ */
