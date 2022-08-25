// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include "main.h"
#include "fw.h"
#include "ps.h"
#include "mac.h"
#include "coex.h"
#include "debug.h"

static int rtw_ips_pwr_up(struct rtw_dev *rtwdev)
{
	int ret;

	ret = rtw_core_start(rtwdev);
	if (ret)
		rtw_err(rtwdev, "leave idle state failed\n");

	rtw_set_channel(rtwdev);
	rtw_flag_clear(rtwdev, RTW_FLAG_INACTIVE_PS);

	return ret;
}

int rtw_enter_ips(struct rtw_dev *rtwdev)
{
	rtw_flag_set(rtwdev, RTW_FLAG_INACTIVE_PS);

	rtw_coex_ips_notify(rtwdev, COEX_IPS_ENTER);

	rtw_core_stop(rtwdev);

	return 0;
}

static void rtw_restore_port_cfg_iter(void *data, u8 *mac,
				      struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = data;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	u32 config = ~0;

	rtw_vif_port_config(rtwdev, rtwvif, config);
}

int rtw_leave_ips(struct rtw_dev *rtwdev)
{
	int ret;

	ret = rtw_ips_pwr_up(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to leave ips state\n");
		return ret;
	}

	rtw_iterate_vifs_atomic(rtwdev, rtw_restore_port_cfg_iter, rtwdev);

	rtw_coex_ips_notify(rtwdev, COEX_IPS_LEAVE);

	return 0;
}

static void rtw_leave_lps_core(struct rtw_dev *rtwdev)
{
	struct rtw_lps_conf *conf = &rtwdev->lps_conf;

	conf->state = RTW_ALL_ON;
	conf->awake_interval = 1;
	conf->rlbm = 0;
	conf->smart_ps = 0;

	rtw_fw_set_pwr_mode(rtwdev);
	rtw_flag_clear(rtwdev, RTW_FLAG_LEISURE_PS);

	rtw_coex_lps_notify(rtwdev, COEX_LPS_DISABLE);
}

static void rtw_enter_lps_core(struct rtw_dev *rtwdev)
{
	struct rtw_lps_conf *conf = &rtwdev->lps_conf;

	conf->state = RTW_RF_OFF;
	conf->awake_interval = 1;
	conf->rlbm = 1;
	conf->smart_ps = 2;

	rtw_coex_lps_notify(rtwdev, COEX_LPS_ENABLE);

	rtw_fw_set_pwr_mode(rtwdev);
	rtw_flag_set(rtwdev, RTW_FLAG_LEISURE_PS);
}

void rtw_lps_work(struct work_struct *work)
{
	struct rtw_dev *rtwdev = container_of(work, struct rtw_dev,
					      lps_work.work);
	struct rtw_lps_conf *conf = &rtwdev->lps_conf;
	struct rtw_vif *rtwvif = conf->rtwvif;

	if (WARN_ON(!rtwvif))
		return;

	if (conf->mode == RTW_MODE_LPS)
		rtw_enter_lps_core(rtwdev);
	else
		rtw_leave_lps_core(rtwdev);
}

void rtw_enter_lps_irqsafe(struct rtw_dev *rtwdev, struct rtw_vif *rtwvif)
{
	struct rtw_lps_conf *conf = &rtwdev->lps_conf;

	if (rtwvif->in_lps)
		return;

	conf->mode = RTW_MODE_LPS;
	conf->rtwvif = rtwvif;
	rtwvif->in_lps = true;

	ieee80211_queue_delayed_work(rtwdev->hw, &rtwdev->lps_work, 0);
}

void rtw_leave_lps_irqsafe(struct rtw_dev *rtwdev, struct rtw_vif *rtwvif)
{
	struct rtw_lps_conf *conf = &rtwdev->lps_conf;

	if (!rtwvif->in_lps)
		return;

	conf->mode = RTW_MODE_ACTIVE;
	conf->rtwvif = rtwvif;
	rtwvif->in_lps = false;

	ieee80211_queue_delayed_work(rtwdev->hw, &rtwdev->lps_work, 0);
}

bool rtw_in_lps(struct rtw_dev *rtwdev)
{
	return rtw_flag_check(rtwdev, RTW_FLAG_LEISURE_PS);
}

void rtw_enter_lps(struct rtw_dev *rtwdev, struct rtw_vif *rtwvif)
{
	struct rtw_lps_conf *conf = &rtwdev->lps_conf;

	if (WARN_ON(!rtwvif))
		return;

	if (rtwvif->in_lps)
		return;

	conf->mode = RTW_MODE_LPS;
	conf->rtwvif = rtwvif;
	rtwvif->in_lps = true;

	rtw_enter_lps_core(rtwdev);
}

void rtw_leave_lps(struct rtw_dev *rtwdev, struct rtw_vif *rtwvif)
{
	struct rtw_lps_conf *conf = &rtwdev->lps_conf;

	if (WARN_ON(!rtwvif))
		return;

	if (!rtwvif->in_lps)
		return;

	conf->mode = RTW_MODE_ACTIVE;
	conf->rtwvif = rtwvif;
	rtwvif->in_lps = false;

	rtw_leave_lps_core(rtwdev);
}
