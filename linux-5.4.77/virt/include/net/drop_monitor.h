/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _NET_DROP_MONITOR_H_
#define _NET_DROP_MONITOR_H_

#include <linux/ktime.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>

/**
 * struct net_dm_hw_metadata - Hardware-supplied packet metadata.
 * @trap_group_name: Hardware trap group name.
 * @trap_name: Hardware trap name.
 * @input_dev: Input netdevice.
 */
struct net_dm_hw_metadata {
	const char *trap_group_name;
	const char *trap_name;
	struct net_device *input_dev;
};

#if IS_REACHABLE(CONFIG_NET_DROP_MONITOR)
void net_dm_hw_report(struct sk_buff *skb,
		      const struct net_dm_hw_metadata *hw_metadata);
#else
static inline void
net_dm_hw_report(struct sk_buff *skb,
		 const struct net_dm_hw_metadata *hw_metadata)
{
}
#endif

#endif /* _NET_DROP_MONITOR_H_ */
