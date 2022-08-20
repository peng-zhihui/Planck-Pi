/******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2012 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2017        Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
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
 * Copyright(c) 2017        Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
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

#include <linux/leds.h>
#include "iwl-io.h"
#include "iwl-csr.h"
#include "mvm.h"

static void iwl_mvm_send_led_fw_cmd(struct iwl_mvm *mvm, bool on)
{
	struct iwl_led_cmd led_cmd = {
		.status = cpu_to_le32(on),
	};
	struct iwl_host_cmd cmd = {
		.id = WIDE_ID(LONG_GROUP, LEDS_CMD),
		.len = { sizeof(led_cmd), },
		.data = { &led_cmd, },
		.flags = CMD_ASYNC,
	};
	int err;

	if (!iwl_mvm_firmware_running(mvm))
		return;

	err = iwl_mvm_send_cmd(mvm, &cmd);

	if (err)
		IWL_WARN(mvm, "LED command failed: %d\n", err);
}

static void iwl_mvm_led_set(struct iwl_mvm *mvm, bool on)
{
	if (fw_has_capa(&mvm->fw->ucode_capa,
			IWL_UCODE_TLV_CAPA_LED_CMD_SUPPORT)) {
		iwl_mvm_send_led_fw_cmd(mvm, on);
		return;
	}

	iwl_write32(mvm->trans, CSR_LED_REG,
		    on ? CSR_LED_REG_TURN_ON : CSR_LED_REG_TURN_OFF);
}

static void iwl_led_brightness_set(struct led_classdev *led_cdev,
				   enum led_brightness brightness)
{
	struct iwl_mvm *mvm = container_of(led_cdev, struct iwl_mvm, led);

	iwl_mvm_led_set(mvm, brightness > 0);
}

int iwl_mvm_leds_init(struct iwl_mvm *mvm)
{
	int mode = iwlwifi_mod_params.led_mode;
	int ret;

	switch (mode) {
	case IWL_LED_BLINK:
		IWL_ERR(mvm, "Blink led mode not supported, used default\n");
		/* fall through */
	case IWL_LED_DEFAULT:
	case IWL_LED_RF_STATE:
		mode = IWL_LED_RF_STATE;
		break;
	case IWL_LED_DISABLE:
		IWL_INFO(mvm, "Led disabled\n");
		return 0;
	default:
		return -EINVAL;
	}

	mvm->led.name = kasprintf(GFP_KERNEL, "%s-led",
				   wiphy_name(mvm->hw->wiphy));
	if (!mvm->led.name)
		return -ENOMEM;

	mvm->led.brightness_set = iwl_led_brightness_set;
	mvm->led.max_brightness = 1;

	if (mode == IWL_LED_RF_STATE)
		mvm->led.default_trigger =
			ieee80211_get_radio_led_name(mvm->hw);

	ret = led_classdev_register(mvm->trans->dev, &mvm->led);
	if (ret) {
		kfree(mvm->led.name);
		IWL_INFO(mvm, "Failed to enable led\n");
		return ret;
	}

	mvm->init_status |= IWL_MVM_INIT_STATUS_LEDS_INIT_COMPLETE;
	return 0;
}

void iwl_mvm_leds_sync(struct iwl_mvm *mvm)
{
	if (!(mvm->init_status & IWL_MVM_INIT_STATUS_LEDS_INIT_COMPLETE))
		return;

	/*
	 * if we control through the register, we're doing it
	 * even when the firmware isn't up, so no need to sync
	 */
	if (mvm->trans->trans_cfg->device_family < IWL_DEVICE_FAMILY_8000)
		return;

	iwl_mvm_led_set(mvm, mvm->led.brightness > 0);
}

void iwl_mvm_leds_exit(struct iwl_mvm *mvm)
{
	if (!(mvm->init_status & IWL_MVM_INIT_STATUS_LEDS_INIT_COMPLETE))
		return;

	led_classdev_unregister(&mvm->led);
	kfree(mvm->led.name);
	mvm->init_status &= ~IWL_MVM_INIT_STATUS_LEDS_INIT_COMPLETE;
}
