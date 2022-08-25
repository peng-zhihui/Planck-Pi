// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * net/dsa/dsa.c - Hardware switch handling
 * Copyright (c) 2008-2009 Marvell Semiconductor
 * Copyright (c) 2013 Florian Fainelli <florian@openwrt.org>
 */

#include <linux/device.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/netdevice.h>
#include <linux/sysfs.h>
#include <linux/phy_fixed.h>
#include <linux/ptp_classify.h>
#include <linux/etherdevice.h>

#include "dsa_priv.h"

static LIST_HEAD(dsa_tag_drivers_list);
static DEFINE_MUTEX(dsa_tag_drivers_lock);

static struct sk_buff *dsa_slave_notag_xmit(struct sk_buff *skb,
					    struct net_device *dev)
{
	/* Just return the original SKB */
	return skb;
}

static const struct dsa_device_ops none_ops = {
	.name	= "none",
	.proto	= DSA_TAG_PROTO_NONE,
	.xmit	= dsa_slave_notag_xmit,
	.rcv	= NULL,
};

DSA_TAG_DRIVER(none_ops);

static void dsa_tag_driver_register(struct dsa_tag_driver *dsa_tag_driver,
				    struct module *owner)
{
	dsa_tag_driver->owner = owner;

	mutex_lock(&dsa_tag_drivers_lock);
	list_add_tail(&dsa_tag_driver->list, &dsa_tag_drivers_list);
	mutex_unlock(&dsa_tag_drivers_lock);
}

void dsa_tag_drivers_register(struct dsa_tag_driver *dsa_tag_driver_array[],
			      unsigned int count, struct module *owner)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		dsa_tag_driver_register(dsa_tag_driver_array[i], owner);
}

static void dsa_tag_driver_unregister(struct dsa_tag_driver *dsa_tag_driver)
{
	mutex_lock(&dsa_tag_drivers_lock);
	list_del(&dsa_tag_driver->list);
	mutex_unlock(&dsa_tag_drivers_lock);
}
EXPORT_SYMBOL_GPL(dsa_tag_drivers_register);

void dsa_tag_drivers_unregister(struct dsa_tag_driver *dsa_tag_driver_array[],
				unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		dsa_tag_driver_unregister(dsa_tag_driver_array[i]);
}
EXPORT_SYMBOL_GPL(dsa_tag_drivers_unregister);

const char *dsa_tag_protocol_to_str(const struct dsa_device_ops *ops)
{
	return ops->name;
};

const struct dsa_device_ops *dsa_tag_driver_get(int tag_protocol)
{
	struct dsa_tag_driver *dsa_tag_driver;
	const struct dsa_device_ops *ops;
	char module_name[128];
	bool found = false;

	snprintf(module_name, 127, "%s%d", DSA_TAG_DRIVER_ALIAS,
		 tag_protocol);

	request_module(module_name);

	mutex_lock(&dsa_tag_drivers_lock);
	list_for_each_entry(dsa_tag_driver, &dsa_tag_drivers_list, list) {
		ops = dsa_tag_driver->ops;
		if (ops->proto == tag_protocol) {
			found = true;
			break;
		}
	}

	if (found) {
		if (!try_module_get(dsa_tag_driver->owner))
			ops = ERR_PTR(-ENOPROTOOPT);
	} else {
		ops = ERR_PTR(-ENOPROTOOPT);
	}

	mutex_unlock(&dsa_tag_drivers_lock);

	return ops;
}

void dsa_tag_driver_put(const struct dsa_device_ops *ops)
{
	struct dsa_tag_driver *dsa_tag_driver;

	mutex_lock(&dsa_tag_drivers_lock);
	list_for_each_entry(dsa_tag_driver, &dsa_tag_drivers_list, list) {
		if (dsa_tag_driver->ops == ops) {
			module_put(dsa_tag_driver->owner);
			break;
		}
	}
	mutex_unlock(&dsa_tag_drivers_lock);
}

static int dev_is_class(struct device *dev, void *class)
{
	if (dev->class != NULL && !strcmp(dev->class->name, class))
		return 1;

	return 0;
}

static struct device *dev_find_class(struct device *parent, char *class)
{
	if (dev_is_class(parent, class)) {
		get_device(parent);
		return parent;
	}

	return device_find_child(parent, class, dev_is_class);
}

struct net_device *dsa_dev_to_net_device(struct device *dev)
{
	struct device *d;

	d = dev_find_class(dev, "net");
	if (d != NULL) {
		struct net_device *nd;

		nd = to_net_dev(d);
		dev_hold(nd);
		put_device(d);

		return nd;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(dsa_dev_to_net_device);

/* Determine if we should defer delivery of skb until we have a rx timestamp.
 *
 * Called from dsa_switch_rcv. For now, this will only work if tagging is
 * enabled on the switch. Normally the MAC driver would retrieve the hardware
 * timestamp when it reads the packet out of the hardware. However in a DSA
 * switch, the DSA driver owning the interface to which the packet is
 * delivered is never notified unless we do so here.
 */
static bool dsa_skb_defer_rx_timestamp(struct dsa_slave_priv *p,
				       struct sk_buff *skb)
{
	struct dsa_switch *ds = p->dp->ds;
	unsigned int type;

	if (skb_headroom(skb) < ETH_HLEN)
		return false;

	__skb_push(skb, ETH_HLEN);

	type = ptp_classify_raw(skb);

	__skb_pull(skb, ETH_HLEN);

	if (type == PTP_CLASS_NONE)
		return false;

	if (likely(ds->ops->port_rxtstamp))
		return ds->ops->port_rxtstamp(ds, p->dp->index, skb, type);

	return false;
}

static int dsa_switch_rcv(struct sk_buff *skb, struct net_device *dev,
			  struct packet_type *pt, struct net_device *unused)
{
	struct dsa_port *cpu_dp = dev->dsa_ptr;
	struct sk_buff *nskb = NULL;
	struct pcpu_sw_netstats *s;
	struct dsa_slave_priv *p;

	if (unlikely(!cpu_dp)) {
		kfree_skb(skb);
		return 0;
	}

	skb = skb_unshare(skb, GFP_ATOMIC);
	if (!skb)
		return 0;

	nskb = cpu_dp->rcv(skb, dev, pt);
	if (!nskb) {
		kfree_skb(skb);
		return 0;
	}

	skb = nskb;
	p = netdev_priv(skb->dev);
	skb_push(skb, ETH_HLEN);
	skb->pkt_type = PACKET_HOST;
	skb->protocol = eth_type_trans(skb, skb->dev);

	s = this_cpu_ptr(p->stats64);
	u64_stats_update_begin(&s->syncp);
	s->rx_packets++;
	s->rx_bytes += skb->len;
	u64_stats_update_end(&s->syncp);

	if (dsa_skb_defer_rx_timestamp(p, skb))
		return 0;

	netif_receive_skb(skb);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static bool dsa_is_port_initialized(struct dsa_switch *ds, int p)
{
	return dsa_is_user_port(ds, p) && ds->ports[p].slave;
}

int dsa_switch_suspend(struct dsa_switch *ds)
{
	int i, ret = 0;

	/* Suspend slave network devices */
	for (i = 0; i < ds->num_ports; i++) {
		if (!dsa_is_port_initialized(ds, i))
			continue;

		ret = dsa_slave_suspend(ds->ports[i].slave);
		if (ret)
			return ret;
	}

	if (ds->ops->suspend)
		ret = ds->ops->suspend(ds);

	return ret;
}
EXPORT_SYMBOL_GPL(dsa_switch_suspend);

int dsa_switch_resume(struct dsa_switch *ds)
{
	int i, ret = 0;

	if (ds->ops->resume)
		ret = ds->ops->resume(ds);

	if (ret)
		return ret;

	/* Resume slave network devices */
	for (i = 0; i < ds->num_ports; i++) {
		if (!dsa_is_port_initialized(ds, i))
			continue;

		ret = dsa_slave_resume(ds->ports[i].slave);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(dsa_switch_resume);
#endif

static struct packet_type dsa_pack_type __read_mostly = {
	.type	= cpu_to_be16(ETH_P_XDSA),
	.func	= dsa_switch_rcv,
};

static struct workqueue_struct *dsa_owq;

bool dsa_schedule_work(struct work_struct *work)
{
	return queue_work(dsa_owq, work);
}

static ATOMIC_NOTIFIER_HEAD(dsa_notif_chain);

int register_dsa_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&dsa_notif_chain, nb);
}
EXPORT_SYMBOL_GPL(register_dsa_notifier);

int unregister_dsa_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&dsa_notif_chain, nb);
}
EXPORT_SYMBOL_GPL(unregister_dsa_notifier);

int call_dsa_notifiers(unsigned long val, struct net_device *dev,
		       struct dsa_notifier_info *info)
{
	info->dev = dev;
	return atomic_notifier_call_chain(&dsa_notif_chain, val, info);
}
EXPORT_SYMBOL_GPL(call_dsa_notifiers);

static int __init dsa_init_module(void)
{
	int rc;

	dsa_owq = alloc_ordered_workqueue("dsa_ordered",
					  WQ_MEM_RECLAIM);
	if (!dsa_owq)
		return -ENOMEM;

	rc = dsa_slave_register_notifier();
	if (rc)
		goto register_notifier_fail;

	dev_add_pack(&dsa_pack_type);

	dsa_tag_driver_register(&DSA_TAG_DRIVER_NAME(none_ops),
				THIS_MODULE);

	return 0;

register_notifier_fail:
	destroy_workqueue(dsa_owq);

	return rc;
}
module_init(dsa_init_module);

static void __exit dsa_cleanup_module(void)
{
	dsa_tag_driver_unregister(&DSA_TAG_DRIVER_NAME(none_ops));

	dsa_slave_unregister_notifier();
	dev_remove_pack(&dsa_pack_type);
	destroy_workqueue(dsa_owq);
}
module_exit(dsa_cleanup_module);

MODULE_AUTHOR("Lennert Buytenhek <buytenh@wantstofly.org>");
MODULE_DESCRIPTION("Driver for Distributed Switch Architecture switch chips");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:dsa");
