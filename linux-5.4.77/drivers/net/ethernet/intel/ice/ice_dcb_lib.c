// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Intel Corporation. */

#include "ice_dcb_lib.h"

/**
 * ice_vsi_cfg_netdev_tc - Setup the netdev TC configuration
 * @vsi: the VSI being configured
 * @ena_tc: TC map to be enabled
 */
void ice_vsi_cfg_netdev_tc(struct ice_vsi *vsi, u8 ena_tc)
{
	struct net_device *netdev = vsi->netdev;
	struct ice_pf *pf = vsi->back;
	struct ice_dcbx_cfg *dcbcfg;
	u8 netdev_tc;
	int i;

	if (!netdev)
		return;

	if (!ena_tc) {
		netdev_reset_tc(netdev);
		return;
	}

	if (netdev_set_num_tc(netdev, vsi->tc_cfg.numtc))
		return;

	dcbcfg = &pf->hw.port_info->local_dcbx_cfg;

	ice_for_each_traffic_class(i)
		if (vsi->tc_cfg.ena_tc & BIT(i))
			netdev_set_tc_queue(netdev,
					    vsi->tc_cfg.tc_info[i].netdev_tc,
					    vsi->tc_cfg.tc_info[i].qcount_tx,
					    vsi->tc_cfg.tc_info[i].qoffset);

	for (i = 0; i < ICE_MAX_USER_PRIORITY; i++) {
		u8 ets_tc = dcbcfg->etscfg.prio_table[i];

		/* Get the mapped netdev TC# for the UP */
		netdev_tc = vsi->tc_cfg.tc_info[ets_tc].netdev_tc;
		netdev_set_prio_tc_map(netdev, i, netdev_tc);
	}
}

/**
 * ice_dcb_get_ena_tc - return bitmap of enabled TCs
 * @dcbcfg: DCB config to evaluate for enabled TCs
 */
u8 ice_dcb_get_ena_tc(struct ice_dcbx_cfg *dcbcfg)
{
	u8 i, num_tc, ena_tc = 1;

	num_tc = ice_dcb_get_num_tc(dcbcfg);

	for (i = 0; i < num_tc; i++)
		ena_tc |= BIT(i);

	return ena_tc;
}

/**
 * ice_dcb_get_num_tc - Get the number of TCs from DCBX config
 * @dcbcfg: config to retrieve number of TCs from
 */
u8 ice_dcb_get_num_tc(struct ice_dcbx_cfg *dcbcfg)
{
	bool tc_unused = false;
	u8 num_tc = 0;
	u8 ret = 0;
	int i;

	/* Scan the ETS Config Priority Table to find traffic classes
	 * enabled and create a bitmask of enabled TCs
	 */
	for (i = 0; i < CEE_DCBX_MAX_PRIO; i++)
		num_tc |= BIT(dcbcfg->etscfg.prio_table[i]);

	/* Scan bitmask for contiguous TCs starting with TC0 */
	for (i = 0; i < IEEE_8021QAZ_MAX_TCS; i++) {
		if (num_tc & BIT(i)) {
			if (!tc_unused) {
				ret++;
			} else {
				pr_err("Non-contiguous TCs - Disabling DCB\n");
				return 1;
			}
		} else {
			tc_unused = true;
		}
	}

	/* There is always at least 1 TC */
	if (!ret)
		ret = 1;

	return ret;
}

/**
 * ice_vsi_cfg_dcb_rings - Update rings to reflect DCB TC
 * @vsi: VSI owner of rings being updated
 */
void ice_vsi_cfg_dcb_rings(struct ice_vsi *vsi)
{
	struct ice_ring *tx_ring, *rx_ring;
	u16 qoffset, qcount;
	int i, n;

	if (!test_bit(ICE_FLAG_DCB_ENA, vsi->back->flags)) {
		/* Reset the TC information */
		for (i = 0; i < vsi->num_txq; i++) {
			tx_ring = vsi->tx_rings[i];
			tx_ring->dcb_tc = 0;
		}
		for (i = 0; i < vsi->num_rxq; i++) {
			rx_ring = vsi->rx_rings[i];
			rx_ring->dcb_tc = 0;
		}
		return;
	}

	ice_for_each_traffic_class(n) {
		if (!(vsi->tc_cfg.ena_tc & BIT(n)))
			break;

		qoffset = vsi->tc_cfg.tc_info[n].qoffset;
		qcount = vsi->tc_cfg.tc_info[n].qcount_tx;
		for (i = qoffset; i < (qoffset + qcount); i++) {
			tx_ring = vsi->tx_rings[i];
			rx_ring = vsi->rx_rings[i];
			tx_ring->dcb_tc = n;
			rx_ring->dcb_tc = n;
		}
	}
}

/**
 * ice_pf_dcb_recfg - Reconfigure all VEBs and VSIs
 * @pf: pointer to the PF struct
 *
 * Assumed caller has already disabled all VSIs before
 * calling this function. Reconfiguring DCB based on
 * local_dcbx_cfg.
 */
static void ice_pf_dcb_recfg(struct ice_pf *pf)
{
	struct ice_dcbx_cfg *dcbcfg = &pf->hw.port_info->local_dcbx_cfg;
	u8 tc_map = 0;
	int v, ret;

	/* Update each VSI */
	ice_for_each_vsi(pf, v) {
		if (!pf->vsi[v])
			continue;

		if (pf->vsi[v]->type == ICE_VSI_PF)
			tc_map = ice_dcb_get_ena_tc(dcbcfg);
		else
			tc_map = ICE_DFLT_TRAFFIC_CLASS;

		ret = ice_vsi_cfg_tc(pf->vsi[v], tc_map);
		if (ret) {
			dev_err(&pf->pdev->dev,
				"Failed to config TC for VSI index: %d\n",
				pf->vsi[v]->idx);
			continue;
		}

		ice_vsi_map_rings_to_vectors(pf->vsi[v]);
	}
}

/**
 * ice_pf_dcb_cfg - Apply new DCB configuration
 * @pf: pointer to the PF struct
 * @new_cfg: DCBX config to apply
 * @locked: is the RTNL held
 */
static
int ice_pf_dcb_cfg(struct ice_pf *pf, struct ice_dcbx_cfg *new_cfg, bool locked)
{
	struct ice_dcbx_cfg *old_cfg, *curr_cfg;
	struct ice_aqc_port_ets_elem buf = { 0 };
	int ret = 0;

	curr_cfg = &pf->hw.port_info->local_dcbx_cfg;

	/* Enable DCB tagging only when more than one TC */
	if (ice_dcb_get_num_tc(new_cfg) > 1) {
		dev_dbg(&pf->pdev->dev, "DCB tagging enabled (num TC > 1)\n");
		set_bit(ICE_FLAG_DCB_ENA, pf->flags);
	} else {
		dev_dbg(&pf->pdev->dev, "DCB tagging disabled (num TC = 1)\n");
		clear_bit(ICE_FLAG_DCB_ENA, pf->flags);
	}

	if (!memcmp(new_cfg, curr_cfg, sizeof(*new_cfg))) {
		dev_dbg(&pf->pdev->dev, "No change in DCB config required\n");
		return ret;
	}

	/* Store old config in case FW config fails */
	old_cfg = devm_kzalloc(&pf->pdev->dev, sizeof(*old_cfg), GFP_KERNEL);
	memcpy(old_cfg, curr_cfg, sizeof(*old_cfg));

	/* avoid race conditions by holding the lock while disabling and
	 * re-enabling the VSI
	 */
	if (!locked)
		rtnl_lock();
	ice_pf_dis_all_vsi(pf, true);

	memcpy(curr_cfg, new_cfg, sizeof(*curr_cfg));
	memcpy(&curr_cfg->etsrec, &curr_cfg->etscfg, sizeof(curr_cfg->etsrec));

	/* Only send new config to HW if we are in SW LLDP mode. Otherwise,
	 * the new config came from the HW in the first place.
	 */
	if (pf->hw.port_info->is_sw_lldp) {
		ret = ice_set_dcb_cfg(pf->hw.port_info);
		if (ret) {
			dev_err(&pf->pdev->dev, "Set DCB Config failed\n");
			/* Restore previous settings to local config */
			memcpy(curr_cfg, old_cfg, sizeof(*curr_cfg));
			goto out;
		}
	}

	ret = ice_query_port_ets(pf->hw.port_info, &buf, sizeof(buf), NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Query Port ETS failed\n");
		goto out;
	}

	ice_pf_dcb_recfg(pf);

out:
	ice_pf_ena_all_vsi(pf, true);
	if (!locked)
		rtnl_unlock();
	devm_kfree(&pf->pdev->dev, old_cfg);
	return ret;
}

/**
 * ice_cfg_etsrec_defaults - Set default ETS recommended DCB config
 * @pi: port information structure
 */
static void ice_cfg_etsrec_defaults(struct ice_port_info *pi)
{
	struct ice_dcbx_cfg *dcbcfg = &pi->local_dcbx_cfg;
	u8 i;

	/* Ensure ETS recommended DCB configuration is not already set */
	if (dcbcfg->etsrec.maxtcs)
		return;

	/* In CEE mode, set the default to 1 TC */
	dcbcfg->etsrec.maxtcs = 1;
	for (i = 0; i < ICE_MAX_TRAFFIC_CLASS; i++) {
		dcbcfg->etsrec.tcbwtable[i] = i ? 0 : 100;
		dcbcfg->etsrec.tsatable[i] = i ? ICE_IEEE_TSA_STRICT :
						 ICE_IEEE_TSA_ETS;
	}
}

/**
 * ice_dcb_need_recfg - Check if DCB needs reconfig
 * @pf: board private structure
 * @old_cfg: current DCB config
 * @new_cfg: new DCB config
 */
static bool
ice_dcb_need_recfg(struct ice_pf *pf, struct ice_dcbx_cfg *old_cfg,
		   struct ice_dcbx_cfg *new_cfg)
{
	bool need_reconfig = false;

	/* Check if ETS configuration has changed */
	if (memcmp(&new_cfg->etscfg, &old_cfg->etscfg,
		   sizeof(new_cfg->etscfg))) {
		/* If Priority Table has changed reconfig is needed */
		if (memcmp(&new_cfg->etscfg.prio_table,
			   &old_cfg->etscfg.prio_table,
			   sizeof(new_cfg->etscfg.prio_table))) {
			need_reconfig = true;
			dev_dbg(&pf->pdev->dev, "ETS UP2TC changed.\n");
		}

		if (memcmp(&new_cfg->etscfg.tcbwtable,
			   &old_cfg->etscfg.tcbwtable,
			   sizeof(new_cfg->etscfg.tcbwtable)))
			dev_dbg(&pf->pdev->dev, "ETS TC BW Table changed.\n");

		if (memcmp(&new_cfg->etscfg.tsatable,
			   &old_cfg->etscfg.tsatable,
			   sizeof(new_cfg->etscfg.tsatable)))
			dev_dbg(&pf->pdev->dev, "ETS TSA Table changed.\n");
	}

	/* Check if PFC configuration has changed */
	if (memcmp(&new_cfg->pfc, &old_cfg->pfc, sizeof(new_cfg->pfc))) {
		need_reconfig = true;
		dev_dbg(&pf->pdev->dev, "PFC config change detected.\n");
	}

	/* Check if APP Table has changed */
	if (memcmp(&new_cfg->app, &old_cfg->app, sizeof(new_cfg->app))) {
		need_reconfig = true;
		dev_dbg(&pf->pdev->dev, "APP Table change detected.\n");
	}

	dev_dbg(&pf->pdev->dev, "dcb need_reconfig=%d\n", need_reconfig);
	return need_reconfig;
}

/**
 * ice_dcb_rebuild - rebuild DCB post reset
 * @pf: physical function instance
 */
void ice_dcb_rebuild(struct ice_pf *pf)
{
	struct ice_dcbx_cfg *local_dcbx_cfg, *desired_dcbx_cfg, *prev_cfg;
	struct ice_aqc_port_ets_elem buf = { 0 };
	enum ice_status ret;

	ret = ice_query_port_ets(pf->hw.port_info, &buf, sizeof(buf), NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Query Port ETS failed\n");
		goto dcb_error;
	}

	/* If DCB was not enabled previously, we are done */
	if (!test_bit(ICE_FLAG_DCB_ENA, pf->flags))
		return;

	local_dcbx_cfg = &pf->hw.port_info->local_dcbx_cfg;
	desired_dcbx_cfg = &pf->hw.port_info->desired_dcbx_cfg;

	/* Save current willing state and force FW to unwilling */
	local_dcbx_cfg->etscfg.willing = 0x0;
	local_dcbx_cfg->pfc.willing = 0x0;
	local_dcbx_cfg->app_mode = ICE_DCBX_APPS_NON_WILLING;

	ice_cfg_etsrec_defaults(pf->hw.port_info);
	ret = ice_set_dcb_cfg(pf->hw.port_info);
	if (ret) {
		dev_err(&pf->pdev->dev, "Failed to set DCB to unwilling\n");
		goto dcb_error;
	}

	/* Retrieve DCB config and ensure same as current in SW */
	prev_cfg = devm_kmemdup(&pf->pdev->dev, local_dcbx_cfg,
				sizeof(*prev_cfg), GFP_KERNEL);
	if (!prev_cfg) {
		dev_err(&pf->pdev->dev, "Failed to alloc space for DCB cfg\n");
		goto dcb_error;
	}

	ice_init_dcb(&pf->hw, true);
	if (pf->hw.port_info->dcbx_status == ICE_DCBX_STATUS_DIS)
		pf->hw.port_info->is_sw_lldp = true;
	else
		pf->hw.port_info->is_sw_lldp = false;

	if (ice_dcb_need_recfg(pf, prev_cfg, local_dcbx_cfg)) {
		/* difference in cfg detected - disable DCB till next MIB */
		dev_err(&pf->pdev->dev, "Set local MIB not accurate\n");
		goto dcb_error;
	}

	/* fetched config congruent to previous configuration */
	devm_kfree(&pf->pdev->dev, prev_cfg);

	/* Set the local desired config */
	if (local_dcbx_cfg->dcbx_mode == ICE_DCBX_MODE_CEE)
		memcpy(local_dcbx_cfg, desired_dcbx_cfg,
		       sizeof(*local_dcbx_cfg));

	ice_cfg_etsrec_defaults(pf->hw.port_info);
	ret = ice_set_dcb_cfg(pf->hw.port_info);
	if (ret) {
		dev_err(&pf->pdev->dev, "Failed to set desired config\n");
		goto dcb_error;
	}
	dev_info(&pf->pdev->dev, "DCB restored after reset\n");
	ret = ice_query_port_ets(pf->hw.port_info, &buf, sizeof(buf), NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Query Port ETS failed\n");
		goto dcb_error;
	}

	return;

dcb_error:
	dev_err(&pf->pdev->dev, "Disabling DCB until new settings occur\n");
	prev_cfg = devm_kzalloc(&pf->pdev->dev, sizeof(*prev_cfg), GFP_KERNEL);
	prev_cfg->etscfg.willing = true;
	prev_cfg->etscfg.tcbwtable[0] = ICE_TC_MAX_BW;
	prev_cfg->etscfg.tsatable[0] = ICE_IEEE_TSA_ETS;
	memcpy(&prev_cfg->etsrec, &prev_cfg->etscfg, sizeof(prev_cfg->etsrec));
	ice_pf_dcb_cfg(pf, prev_cfg, false);
	devm_kfree(&pf->pdev->dev, prev_cfg);
}

/**
 * ice_dcb_init_cfg - set the initial DCB config in SW
 * @pf: PF to apply config to
 * @locked: Is the RTNL held
 */
static int ice_dcb_init_cfg(struct ice_pf *pf, bool locked)
{
	struct ice_dcbx_cfg *newcfg;
	struct ice_port_info *pi;
	int ret = 0;

	pi = pf->hw.port_info;
	newcfg = devm_kzalloc(&pf->pdev->dev, sizeof(*newcfg), GFP_KERNEL);
	if (!newcfg)
		return -ENOMEM;

	memcpy(newcfg, &pi->local_dcbx_cfg, sizeof(*newcfg));
	memset(&pi->local_dcbx_cfg, 0, sizeof(*newcfg));

	dev_info(&pf->pdev->dev, "Configuring initial DCB values\n");
	if (ice_pf_dcb_cfg(pf, newcfg, locked))
		ret = -EINVAL;

	devm_kfree(&pf->pdev->dev, newcfg);

	return ret;
}

/**
 * ice_dcb_sw_default_config - Apply a default DCB config
 * @pf: PF to apply config to
 * @locked: was this function called with RTNL held
 */
static int ice_dcb_sw_dflt_cfg(struct ice_pf *pf, bool locked)
{
	struct ice_aqc_port_ets_elem buf = { 0 };
	struct ice_dcbx_cfg *dcbcfg;
	struct ice_port_info *pi;
	struct ice_hw *hw;
	int ret;

	hw = &pf->hw;
	pi = hw->port_info;
	dcbcfg = devm_kzalloc(&pf->pdev->dev, sizeof(*dcbcfg), GFP_KERNEL);

	memset(dcbcfg, 0, sizeof(*dcbcfg));
	memset(&pi->local_dcbx_cfg, 0, sizeof(*dcbcfg));

	dcbcfg->etscfg.willing = 1;
	dcbcfg->etscfg.maxtcs = hw->func_caps.common_cap.maxtc;
	dcbcfg->etscfg.tcbwtable[0] = 100;
	dcbcfg->etscfg.tsatable[0] = ICE_IEEE_TSA_ETS;

	memcpy(&dcbcfg->etsrec, &dcbcfg->etscfg,
	       sizeof(dcbcfg->etsrec));
	dcbcfg->etsrec.willing = 0;

	dcbcfg->pfc.willing = 1;
	dcbcfg->pfc.pfccap = hw->func_caps.common_cap.maxtc;

	dcbcfg->numapps = 1;
	dcbcfg->app[0].selector = ICE_APP_SEL_ETHTYPE;
	dcbcfg->app[0].priority = 3;
	dcbcfg->app[0].prot_id = ICE_APP_PROT_ID_FCOE;

	ret = ice_pf_dcb_cfg(pf, dcbcfg, locked);
	devm_kfree(&pf->pdev->dev, dcbcfg);
	if (ret)
		return ret;

	return ice_query_port_ets(pi, &buf, sizeof(buf), NULL);
}

/**
 * ice_init_pf_dcb - initialize DCB for a PF
 * @pf: PF to initialize DCB for
 * @locked: Was function called with RTNL held
 */
int ice_init_pf_dcb(struct ice_pf *pf, bool locked)
{
	struct device *dev = &pf->pdev->dev;
	struct ice_port_info *port_info;
	struct ice_hw *hw = &pf->hw;
	int err;

	port_info = hw->port_info;

	err = ice_init_dcb(hw, false);
	if (err && !port_info->is_sw_lldp) {
		dev_err(&pf->pdev->dev, "Error initializing DCB %d\n", err);
		goto dcb_init_err;
	}

	dev_info(&pf->pdev->dev,
		 "DCB is enabled in the hardware, max number of TCs supported on this port are %d\n",
		 pf->hw.func_caps.common_cap.maxtc);
	if (err) {
		/* FW LLDP is disabled, activate SW DCBX/LLDP mode */
		dev_info(&pf->pdev->dev,
			 "FW LLDP is disabled, DCBx/LLDP in SW mode.\n");
		clear_bit(ICE_FLAG_FW_LLDP_AGENT, pf->flags);
		err = ice_dcb_sw_dflt_cfg(pf, locked);
		if (err) {
			dev_err(&pf->pdev->dev,
				"Failed to set local DCB config %d\n", err);
			err = -EIO;
			goto dcb_init_err;
		}

		pf->dcbx_cap = DCB_CAP_DCBX_HOST | DCB_CAP_DCBX_VER_IEEE;
		return 0;
	}

	set_bit(ICE_FLAG_FW_LLDP_AGENT, pf->flags);

	/* DCBX in FW and LLDP enabled in FW */
	pf->dcbx_cap = DCB_CAP_DCBX_LLD_MANAGED | DCB_CAP_DCBX_VER_IEEE;

	err = ice_dcb_init_cfg(pf, locked);
	if (err)
		goto dcb_init_err;

	return err;

dcb_init_err:
	dev_err(dev, "DCB init failed\n");
	return err;
}

/**
 * ice_update_dcb_stats - Update DCB stats counters
 * @pf: PF whose stats needs to be updated
 */
void ice_update_dcb_stats(struct ice_pf *pf)
{
	struct ice_hw_port_stats *prev_ps, *cur_ps;
	struct ice_hw *hw = &pf->hw;
	u8 port;
	int i;

	port = hw->port_info->lport;
	prev_ps = &pf->stats_prev;
	cur_ps = &pf->stats;

	for (i = 0; i < 8; i++) {
		ice_stat_update32(hw, GLPRT_PXOFFRXC(port, i),
				  pf->stat_prev_loaded,
				  &prev_ps->priority_xoff_rx[i],
				  &cur_ps->priority_xoff_rx[i]);
		ice_stat_update32(hw, GLPRT_PXONRXC(port, i),
				  pf->stat_prev_loaded,
				  &prev_ps->priority_xon_rx[i],
				  &cur_ps->priority_xon_rx[i]);
		ice_stat_update32(hw, GLPRT_PXONTXC(port, i),
				  pf->stat_prev_loaded,
				  &prev_ps->priority_xon_tx[i],
				  &cur_ps->priority_xon_tx[i]);
		ice_stat_update32(hw, GLPRT_PXOFFTXC(port, i),
				  pf->stat_prev_loaded,
				  &prev_ps->priority_xoff_tx[i],
				  &cur_ps->priority_xoff_tx[i]);
		ice_stat_update32(hw, GLPRT_RXON2OFFCNT(port, i),
				  pf->stat_prev_loaded,
				  &prev_ps->priority_xon_2_xoff[i],
				  &cur_ps->priority_xon_2_xoff[i]);
	}
}

/**
 * ice_tx_prepare_vlan_flags_dcb - prepare VLAN tagging for DCB
 * @tx_ring: ring to send buffer on
 * @first: pointer to struct ice_tx_buf
 */
int
ice_tx_prepare_vlan_flags_dcb(struct ice_ring *tx_ring,
			      struct ice_tx_buf *first)
{
	struct sk_buff *skb = first->skb;

	if (!test_bit(ICE_FLAG_DCB_ENA, tx_ring->vsi->back->flags))
		return 0;

	/* Insert 802.1p priority into VLAN header */
	if ((first->tx_flags & (ICE_TX_FLAGS_HW_VLAN | ICE_TX_FLAGS_SW_VLAN)) ||
	    skb->priority != TC_PRIO_CONTROL) {
		first->tx_flags &= ~ICE_TX_FLAGS_VLAN_PR_M;
		/* Mask the lower 3 bits to set the 802.1p priority */
		first->tx_flags |= (skb->priority & 0x7) <<
				   ICE_TX_FLAGS_VLAN_PR_S;
		if (first->tx_flags & ICE_TX_FLAGS_SW_VLAN) {
			struct vlan_ethhdr *vhdr;
			int rc;

			rc = skb_cow_head(skb, 0);
			if (rc < 0)
				return rc;
			vhdr = (struct vlan_ethhdr *)skb->data;
			vhdr->h_vlan_TCI = htons(first->tx_flags >>
						 ICE_TX_FLAGS_VLAN_S);
		} else {
			first->tx_flags |= ICE_TX_FLAGS_HW_VLAN;
		}
	}

	return 0;
}

/**
 * ice_dcb_process_lldp_set_mib_change - Process MIB change
 * @pf: ptr to ice_pf
 * @event: pointer to the admin queue receive event
 */
void
ice_dcb_process_lldp_set_mib_change(struct ice_pf *pf,
				    struct ice_rq_event_info *event)
{
	struct ice_aqc_port_ets_elem buf = { 0 };
	struct ice_aqc_lldp_get_mib *mib;
	struct ice_dcbx_cfg tmp_dcbx_cfg;
	bool need_reconfig = false;
	struct ice_port_info *pi;
	u8 type;
	int ret;

	/* Not DCB capable or capability disabled */
	if (!(test_bit(ICE_FLAG_DCB_CAPABLE, pf->flags)))
		return;

	if (pf->dcbx_cap & DCB_CAP_DCBX_HOST) {
		dev_dbg(&pf->pdev->dev,
			"MIB Change Event in HOST mode\n");
		return;
	}

	pi = pf->hw.port_info;
	mib = (struct ice_aqc_lldp_get_mib *)&event->desc.params.raw;
	/* Ignore if event is not for Nearest Bridge */
	type = ((mib->type >> ICE_AQ_LLDP_BRID_TYPE_S) &
		ICE_AQ_LLDP_BRID_TYPE_M);
	dev_dbg(&pf->pdev->dev, "LLDP event MIB bridge type 0x%x\n", type);
	if (type != ICE_AQ_LLDP_BRID_TYPE_NEAREST_BRID)
		return;

	/* Check MIB Type and return if event for Remote MIB update */
	type = mib->type & ICE_AQ_LLDP_MIB_TYPE_M;
	dev_dbg(&pf->pdev->dev,
		"LLDP event mib type %s\n", type ? "remote" : "local");
	if (type == ICE_AQ_LLDP_MIB_REMOTE) {
		/* Update the remote cached instance and return */
		ret = ice_aq_get_dcb_cfg(pi->hw, ICE_AQ_LLDP_MIB_REMOTE,
					 ICE_AQ_LLDP_BRID_TYPE_NEAREST_BRID,
					 &pi->remote_dcbx_cfg);
		if (ret) {
			dev_err(&pf->pdev->dev, "Failed to get remote DCB config\n");
			return;
		}
	}

	/* store the old configuration */
	tmp_dcbx_cfg = pf->hw.port_info->local_dcbx_cfg;

	/* Reset the old DCBX configuration data */
	memset(&pi->local_dcbx_cfg, 0, sizeof(pi->local_dcbx_cfg));

	/* Get updated DCBX data from firmware */
	ret = ice_get_dcb_cfg(pf->hw.port_info);
	if (ret) {
		dev_err(&pf->pdev->dev, "Failed to get DCB config\n");
		return;
	}

	/* No change detected in DCBX configs */
	if (!memcmp(&tmp_dcbx_cfg, &pi->local_dcbx_cfg, sizeof(tmp_dcbx_cfg))) {
		dev_dbg(&pf->pdev->dev,
			"No change detected in DCBX configuration.\n");
		return;
	}

	need_reconfig = ice_dcb_need_recfg(pf, &tmp_dcbx_cfg,
					   &pi->local_dcbx_cfg);
	if (!need_reconfig)
		return;

	/* Enable DCB tagging only when more than one TC */
	if (ice_dcb_get_num_tc(&pi->local_dcbx_cfg) > 1) {
		dev_dbg(&pf->pdev->dev, "DCB tagging enabled (num TC > 1)\n");
		set_bit(ICE_FLAG_DCB_ENA, pf->flags);
	} else {
		dev_dbg(&pf->pdev->dev, "DCB tagging disabled (num TC = 1)\n");
		clear_bit(ICE_FLAG_DCB_ENA, pf->flags);
	}

	rtnl_lock();
	ice_pf_dis_all_vsi(pf, true);

	ret = ice_query_port_ets(pf->hw.port_info, &buf, sizeof(buf), NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Query Port ETS failed\n");
		rtnl_unlock();
		return;
	}

	/* changes in configuration update VSI */
	ice_pf_dcb_recfg(pf);

	ice_pf_ena_all_vsi(pf, true);
	rtnl_unlock();
}
