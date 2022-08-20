/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2012 - 2018 Microchip Technology Inc., and its subsidiaries.
 * All rights reserved.
 */

#ifndef NM_WFI_CFGOPERATIONS
#define NM_WFI_CFGOPERATIONS
#include "wilc_wfi_netdevice.h"

struct wiphy *wilc_cfg_alloc(void);
int wilc_cfg80211_init(struct wilc **wilc, struct device *dev, int io_type,
		       const struct wilc_hif_func *ops);
struct wilc *wilc_create_wiphy(struct device *dev);
void wilc_deinit_host_int(struct net_device *net);
int wilc_init_host_int(struct net_device *net);
void wilc_wfi_monitor_rx(struct net_device *mon_dev, u8 *buff, u32 size);
struct wilc_vif *wilc_netdev_interface(struct wilc *wl, const char *name,
				       enum nl80211_iftype type);
void wilc_wfi_deinit_mon_interface(struct wilc *wl, bool rtnl_locked);
struct net_device *wilc_wfi_init_mon_interface(struct wilc *wl,
					       const char *name,
					       struct net_device *real_dev);
void wilc_mgmt_frame_register(struct wiphy *wiphy, struct wireless_dev *wdev,
			      u16 frame_type, bool reg);
struct wilc_vif *wilc_get_interface(struct wilc *wl);
#endif
