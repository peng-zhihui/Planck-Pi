// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2016-2017 Hisilicon Limited.

#include "hclge_main.h"
#include "hclge_tm.h"
#include "hnae3.h"

#define BW_PERCENT	100

static int hclge_ieee_ets_to_tm_info(struct hclge_dev *hdev,
				     struct ieee_ets *ets)
{
	u8 i;

	for (i = 0; i < HNAE3_MAX_TC; i++) {
		switch (ets->tc_tsa[i]) {
		case IEEE_8021QAZ_TSA_STRICT:
			hdev->tm_info.tc_info[i].tc_sch_mode =
				HCLGE_SCH_MODE_SP;
			hdev->tm_info.pg_info[0].tc_dwrr[i] = 0;
			break;
		case IEEE_8021QAZ_TSA_ETS:
			hdev->tm_info.tc_info[i].tc_sch_mode =
				HCLGE_SCH_MODE_DWRR;
			hdev->tm_info.pg_info[0].tc_dwrr[i] =
				ets->tc_tx_bw[i];
			break;
		default:
			/* Hardware only supports SP (strict priority)
			 * or ETS (enhanced transmission selection)
			 * algorithms, if we receive some other value
			 * from dcbnl, then throw an error.
			 */
			return -EINVAL;
		}
	}

	hclge_tm_prio_tc_info_update(hdev, ets->prio_tc);

	return 0;
}

static void hclge_tm_info_to_ieee_ets(struct hclge_dev *hdev,
				      struct ieee_ets *ets)
{
	u32 i;

	memset(ets, 0, sizeof(*ets));
	ets->willing = 1;
	ets->ets_cap = hdev->tc_max;

	for (i = 0; i < HNAE3_MAX_TC; i++) {
		ets->prio_tc[i] = hdev->tm_info.prio_tc[i];
		ets->tc_tx_bw[i] = hdev->tm_info.pg_info[0].tc_dwrr[i];

		if (hdev->tm_info.tc_info[i].tc_sch_mode ==
		    HCLGE_SCH_MODE_SP)
			ets->tc_tsa[i] = IEEE_8021QAZ_TSA_STRICT;
		else
			ets->tc_tsa[i] = IEEE_8021QAZ_TSA_ETS;
	}
}

/* IEEE std */
static int hclge_ieee_getets(struct hnae3_handle *h, struct ieee_ets *ets)
{
	struct hclge_vport *vport = hclge_get_vport(h);
	struct hclge_dev *hdev = vport->back;

	hclge_tm_info_to_ieee_ets(hdev, ets);

	return 0;
}

static int hclge_dcb_common_validate(struct hclge_dev *hdev, u8 num_tc,
				     u8 *prio_tc)
{
	int i;

	if (num_tc > hdev->tc_max) {
		dev_err(&hdev->pdev->dev,
			"tc num checking failed, %u > tc_max(%u)\n",
			num_tc, hdev->tc_max);
		return -EINVAL;
	}

	for (i = 0; i < HNAE3_MAX_USER_PRIO; i++) {
		if (prio_tc[i] >= num_tc) {
			dev_err(&hdev->pdev->dev,
				"prio_tc[%u] checking failed, %u >= num_tc(%u)\n",
				i, prio_tc[i], num_tc);
			return -EINVAL;
		}
	}

	if (num_tc > hdev->vport[0].alloc_tqps) {
		dev_err(&hdev->pdev->dev,
			"allocated tqp checking failed, %u > tqp(%u)\n",
			num_tc, hdev->vport[0].alloc_tqps);
		return -EINVAL;
	}

	return 0;
}

static int hclge_ets_validate(struct hclge_dev *hdev, struct ieee_ets *ets,
			      u8 *tc, bool *changed)
{
	bool has_ets_tc = false;
	u32 total_ets_bw = 0;
	u8 max_tc = 0;
	int ret;
	u8 i;

	for (i = 0; i < HNAE3_MAX_USER_PRIO; i++) {
		if (ets->prio_tc[i] != hdev->tm_info.prio_tc[i])
			*changed = true;

		if (ets->prio_tc[i] > max_tc)
			max_tc = ets->prio_tc[i];
	}

	ret = hclge_dcb_common_validate(hdev, max_tc + 1, ets->prio_tc);
	if (ret)
		return ret;

	for (i = 0; i < hdev->tc_max; i++) {
		switch (ets->tc_tsa[i]) {
		case IEEE_8021QAZ_TSA_STRICT:
			if (hdev->tm_info.tc_info[i].tc_sch_mode !=
				HCLGE_SCH_MODE_SP)
				*changed = true;
			break;
		case IEEE_8021QAZ_TSA_ETS:
			if (hdev->tm_info.tc_info[i].tc_sch_mode !=
				HCLGE_SCH_MODE_DWRR)
				*changed = true;

			total_ets_bw += ets->tc_tx_bw[i];
			has_ets_tc = true;
			break;
		default:
			return -EINVAL;
		}
	}

	if (has_ets_tc && total_ets_bw != BW_PERCENT)
		return -EINVAL;

	*tc = max_tc + 1;
	if (*tc != hdev->tm_info.num_tc)
		*changed = true;

	return 0;
}

static int hclge_map_update(struct hclge_dev *hdev)
{
	int ret;

	ret = hclge_tm_schd_setup_hw(hdev);
	if (ret)
		return ret;

	ret = hclge_pause_setup_hw(hdev, false);
	if (ret)
		return ret;

	ret = hclge_buffer_alloc(hdev);
	if (ret)
		return ret;

	hclge_rss_indir_init_cfg(hdev);

	return hclge_rss_init_hw(hdev);
}

static int hclge_client_setup_tc(struct hclge_dev *hdev)
{
	struct hclge_vport *vport = hdev->vport;
	struct hnae3_client *client;
	struct hnae3_handle *handle;
	int ret;
	u32 i;

	for (i = 0; i < hdev->num_vmdq_vport + 1; i++) {
		handle = &vport[i].nic;
		client = handle->client;

		if (!client || !client->ops || !client->ops->setup_tc)
			continue;

		ret = client->ops->setup_tc(handle, hdev->tm_info.num_tc);
		if (ret)
			return ret;
	}

	return 0;
}

static int hclge_notify_down_uinit(struct hclge_dev *hdev)
{
	int ret;

	ret = hclge_notify_client(hdev, HNAE3_DOWN_CLIENT);
	if (ret)
		return ret;

	return hclge_notify_client(hdev, HNAE3_UNINIT_CLIENT);
}

static int hclge_notify_init_up(struct hclge_dev *hdev)
{
	int ret;

	ret = hclge_notify_client(hdev, HNAE3_INIT_CLIENT);
	if (ret)
		return ret;

	return hclge_notify_client(hdev, HNAE3_UP_CLIENT);
}

static int hclge_ieee_setets(struct hnae3_handle *h, struct ieee_ets *ets)
{
	struct hclge_vport *vport = hclge_get_vport(h);
	struct net_device *netdev = h->kinfo.netdev;
	struct hclge_dev *hdev = vport->back;
	bool map_changed = false;
	u8 num_tc = 0;
	int ret;

	if (!(hdev->dcbx_cap & DCB_CAP_DCBX_VER_IEEE) ||
	    hdev->flag & HCLGE_FLAG_MQPRIO_ENABLE)
		return -EINVAL;

	ret = hclge_ets_validate(hdev, ets, &num_tc, &map_changed);
	if (ret)
		return ret;

	if (map_changed) {
		netif_dbg(h, drv, netdev, "set ets\n");

		ret = hclge_notify_down_uinit(hdev);
		if (ret)
			return ret;
	}

	hclge_tm_schd_info_update(hdev, num_tc);

	ret = hclge_ieee_ets_to_tm_info(hdev, ets);
	if (ret)
		goto err_out;

	if (map_changed) {
		ret = hclge_map_update(hdev);
		if (ret)
			goto err_out;

		ret = hclge_client_setup_tc(hdev);
		if (ret)
			goto err_out;

		ret = hclge_notify_init_up(hdev);
		if (ret)
			return ret;
	}

	return hclge_tm_dwrr_cfg(hdev);

err_out:
	if (!map_changed)
		return ret;

	hclge_notify_init_up(hdev);

	return ret;
}

static int hclge_ieee_getpfc(struct hnae3_handle *h, struct ieee_pfc *pfc)
{
	u64 requests[HNAE3_MAX_TC], indications[HNAE3_MAX_TC];
	struct hclge_vport *vport = hclge_get_vport(h);
	struct hclge_dev *hdev = vport->back;
	u8 i, j, pfc_map, *prio_tc;
	int ret;

	memset(pfc, 0, sizeof(*pfc));
	pfc->pfc_cap = hdev->pfc_max;
	prio_tc = hdev->tm_info.prio_tc;
	pfc_map = hdev->tm_info.hw_pfc_map;

	/* Pfc setting is based on TC */
	for (i = 0; i < hdev->tm_info.num_tc; i++) {
		for (j = 0; j < HNAE3_MAX_USER_PRIO; j++) {
			if ((prio_tc[j] == i) && (pfc_map & BIT(i)))
				pfc->pfc_en |= BIT(j);
		}
	}

	ret = hclge_pfc_tx_stats_get(hdev, requests);
	if (ret)
		return ret;

	ret = hclge_pfc_rx_stats_get(hdev, indications);
	if (ret)
		return ret;

	for (i = 0; i < HCLGE_MAX_TC_NUM; i++) {
		pfc->requests[i] = requests[i];
		pfc->indications[i] = indications[i];
	}
	return 0;
}

static int hclge_ieee_setpfc(struct hnae3_handle *h, struct ieee_pfc *pfc)
{
	struct hclge_vport *vport = hclge_get_vport(h);
	struct net_device *netdev = h->kinfo.netdev;
	struct hclge_dev *hdev = vport->back;
	u8 i, j, pfc_map, *prio_tc;
	int ret;

	if (!(hdev->dcbx_cap & DCB_CAP_DCBX_VER_IEEE) ||
	    hdev->flag & HCLGE_FLAG_MQPRIO_ENABLE)
		return -EINVAL;

	if (pfc->pfc_en == hdev->tm_info.pfc_en)
		return 0;

	prio_tc = hdev->tm_info.prio_tc;
	pfc_map = 0;

	for (i = 0; i < hdev->tm_info.num_tc; i++) {
		for (j = 0; j < HNAE3_MAX_USER_PRIO; j++) {
			if ((prio_tc[j] == i) && (pfc->pfc_en & BIT(j))) {
				pfc_map |= BIT(i);
				break;
			}
		}
	}

	hdev->tm_info.hw_pfc_map = pfc_map;
	hdev->tm_info.pfc_en = pfc->pfc_en;

	netif_dbg(h, drv, netdev,
		  "set pfc: pfc_en=%x, pfc_map=%x, num_tc=%u\n",
		  pfc->pfc_en, pfc_map, hdev->tm_info.num_tc);

	hclge_tm_pfc_info_update(hdev);

	ret = hclge_pause_setup_hw(hdev, false);
	if (ret)
		return ret;

	ret = hclge_notify_client(hdev, HNAE3_DOWN_CLIENT);
	if (ret)
		return ret;

	ret = hclge_buffer_alloc(hdev);
	if (ret) {
		hclge_notify_client(hdev, HNAE3_UP_CLIENT);
		return ret;
	}

	return hclge_notify_client(hdev, HNAE3_UP_CLIENT);
}

/* DCBX configuration */
static u8 hclge_getdcbx(struct hnae3_handle *h)
{
	struct hclge_vport *vport = hclge_get_vport(h);
	struct hclge_dev *hdev = vport->back;

	if (hdev->flag & HCLGE_FLAG_MQPRIO_ENABLE)
		return 0;

	return hdev->dcbx_cap;
}

static u8 hclge_setdcbx(struct hnae3_handle *h, u8 mode)
{
	struct hclge_vport *vport = hclge_get_vport(h);
	struct net_device *netdev = h->kinfo.netdev;
	struct hclge_dev *hdev = vport->back;

	netif_dbg(h, drv, netdev, "set dcbx: mode=%u\n", mode);

	/* No support for LLD_MANAGED modes or CEE */
	if ((mode & DCB_CAP_DCBX_LLD_MANAGED) ||
	    (mode & DCB_CAP_DCBX_VER_CEE) ||
	    !(mode & DCB_CAP_DCBX_HOST))
		return 1;

	hdev->dcbx_cap = mode;

	return 0;
}

/* Set up TC for hardware offloaded mqprio in channel mode */
static int hclge_setup_tc(struct hnae3_handle *h, u8 tc, u8 *prio_tc)
{
	struct hclge_vport *vport = hclge_get_vport(h);
	struct hclge_dev *hdev = vport->back;
	int ret;

	if (hdev->flag & HCLGE_FLAG_DCB_ENABLE)
		return -EINVAL;

	ret = hclge_dcb_common_validate(hdev, tc, prio_tc);
	if (ret)
		return -EINVAL;

	ret = hclge_notify_down_uinit(hdev);
	if (ret)
		return ret;

	hclge_tm_schd_info_update(hdev, tc);
	hclge_tm_prio_tc_info_update(hdev, prio_tc);

	ret = hclge_tm_init_hw(hdev, false);
	if (ret)
		goto err_out;

	ret = hclge_client_setup_tc(hdev);
	if (ret)
		goto err_out;

	hdev->flag &= ~HCLGE_FLAG_DCB_ENABLE;

	if (tc > 1)
		hdev->flag |= HCLGE_FLAG_MQPRIO_ENABLE;
	else
		hdev->flag &= ~HCLGE_FLAG_MQPRIO_ENABLE;

	return hclge_notify_init_up(hdev);

err_out:
	hclge_notify_init_up(hdev);

	return ret;
}

static const struct hnae3_dcb_ops hns3_dcb_ops = {
	.ieee_getets	= hclge_ieee_getets,
	.ieee_setets	= hclge_ieee_setets,
	.ieee_getpfc	= hclge_ieee_getpfc,
	.ieee_setpfc	= hclge_ieee_setpfc,
	.getdcbx	= hclge_getdcbx,
	.setdcbx	= hclge_setdcbx,
	.setup_tc	= hclge_setup_tc,
};

void hclge_dcb_ops_set(struct hclge_dev *hdev)
{
	struct hclge_vport *vport = hdev->vport;
	struct hnae3_knic_private_info *kinfo;

	/* Hdev does not support DCB or vport is
	 * not a pf, then dcb_ops is not set.
	 */
	if (!hnae3_dev_dcb_supported(hdev) ||
	    vport->vport_id != 0)
		return;

	kinfo = &vport->nic.kinfo;
	kinfo->dcb_ops = &hns3_dcb_ops;
	hdev->dcbx_cap = DCB_CAP_DCBX_VER_IEEE | DCB_CAP_DCBX_HOST;
}
