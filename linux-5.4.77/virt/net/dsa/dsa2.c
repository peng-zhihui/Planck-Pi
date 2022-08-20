// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * net/dsa/dsa2.c - Hardware switch handling, binding version 2
 * Copyright (c) 2008-2009 Marvell Semiconductor
 * Copyright (c) 2013 Florian Fainelli <florian@openwrt.org>
 * Copyright (c) 2016 Andrew Lunn <andrew@lunn.ch>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/rtnetlink.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <net/devlink.h>

#include "dsa_priv.h"

static LIST_HEAD(dsa_tree_list);
static DEFINE_MUTEX(dsa2_mutex);

static const struct devlink_ops dsa_devlink_ops = {
};

static struct dsa_switch_tree *dsa_tree_find(int index)
{
	struct dsa_switch_tree *dst;

	list_for_each_entry(dst, &dsa_tree_list, list)
		if (dst->index == index)
			return dst;

	return NULL;
}

static struct dsa_switch_tree *dsa_tree_alloc(int index)
{
	struct dsa_switch_tree *dst;

	dst = kzalloc(sizeof(*dst), GFP_KERNEL);
	if (!dst)
		return NULL;

	dst->index = index;

	INIT_LIST_HEAD(&dst->list);
	list_add_tail(&dst->list, &dsa_tree_list);

	kref_init(&dst->refcount);

	return dst;
}

static void dsa_tree_free(struct dsa_switch_tree *dst)
{
	list_del(&dst->list);
	kfree(dst);
}

static struct dsa_switch_tree *dsa_tree_get(struct dsa_switch_tree *dst)
{
	if (dst)
		kref_get(&dst->refcount);

	return dst;
}

static struct dsa_switch_tree *dsa_tree_touch(int index)
{
	struct dsa_switch_tree *dst;

	dst = dsa_tree_find(index);
	if (dst)
		return dsa_tree_get(dst);
	else
		return dsa_tree_alloc(index);
}

static void dsa_tree_release(struct kref *ref)
{
	struct dsa_switch_tree *dst;

	dst = container_of(ref, struct dsa_switch_tree, refcount);

	dsa_tree_free(dst);
}

static void dsa_tree_put(struct dsa_switch_tree *dst)
{
	if (dst)
		kref_put(&dst->refcount, dsa_tree_release);
}

static bool dsa_port_is_dsa(struct dsa_port *port)
{
	return port->type == DSA_PORT_TYPE_DSA;
}

static bool dsa_port_is_cpu(struct dsa_port *port)
{
	return port->type == DSA_PORT_TYPE_CPU;
}

static bool dsa_port_is_user(struct dsa_port *dp)
{
	return dp->type == DSA_PORT_TYPE_USER;
}

static struct dsa_port *dsa_tree_find_port_by_node(struct dsa_switch_tree *dst,
						   struct device_node *dn)
{
	struct dsa_switch *ds;
	struct dsa_port *dp;
	int device, port;

	for (device = 0; device < DSA_MAX_SWITCHES; device++) {
		ds = dst->ds[device];
		if (!ds)
			continue;

		for (port = 0; port < ds->num_ports; port++) {
			dp = &ds->ports[port];

			if (dp->dn == dn)
				return dp;
		}
	}

	return NULL;
}

static bool dsa_port_setup_routing_table(struct dsa_port *dp)
{
	struct dsa_switch *ds = dp->ds;
	struct dsa_switch_tree *dst = ds->dst;
	struct device_node *dn = dp->dn;
	struct of_phandle_iterator it;
	struct dsa_port *link_dp;
	int err;

	of_for_each_phandle(&it, err, dn, "link", NULL, 0) {
		link_dp = dsa_tree_find_port_by_node(dst, it.node);
		if (!link_dp) {
			of_node_put(it.node);
			return false;
		}

		ds->rtable[link_dp->ds->index] = dp->index;
	}

	return true;
}

static bool dsa_switch_setup_routing_table(struct dsa_switch *ds)
{
	bool complete = true;
	struct dsa_port *dp;
	int i;

	for (i = 0; i < DSA_MAX_SWITCHES; i++)
		ds->rtable[i] = DSA_RTABLE_NONE;

	for (i = 0; i < ds->num_ports; i++) {
		dp = &ds->ports[i];

		if (dsa_port_is_dsa(dp)) {
			complete = dsa_port_setup_routing_table(dp);
			if (!complete)
				break;
		}
	}

	return complete;
}

static bool dsa_tree_setup_routing_table(struct dsa_switch_tree *dst)
{
	struct dsa_switch *ds;
	bool complete = true;
	int device;

	for (device = 0; device < DSA_MAX_SWITCHES; device++) {
		ds = dst->ds[device];
		if (!ds)
			continue;

		complete = dsa_switch_setup_routing_table(ds);
		if (!complete)
			break;
	}

	return complete;
}

static struct dsa_port *dsa_tree_find_first_cpu(struct dsa_switch_tree *dst)
{
	struct dsa_switch *ds;
	struct dsa_port *dp;
	int device, port;

	for (device = 0; device < DSA_MAX_SWITCHES; device++) {
		ds = dst->ds[device];
		if (!ds)
			continue;

		for (port = 0; port < ds->num_ports; port++) {
			dp = &ds->ports[port];

			if (dsa_port_is_cpu(dp))
				return dp;
		}
	}

	return NULL;
}

static int dsa_tree_setup_default_cpu(struct dsa_switch_tree *dst)
{
	struct dsa_switch *ds;
	struct dsa_port *dp;
	int device, port;

	/* DSA currently only supports a single CPU port */
	dst->cpu_dp = dsa_tree_find_first_cpu(dst);
	if (!dst->cpu_dp) {
		pr_warn("Tree has no master device\n");
		return -EINVAL;
	}

	/* Assign the default CPU port to all ports of the fabric */
	for (device = 0; device < DSA_MAX_SWITCHES; device++) {
		ds = dst->ds[device];
		if (!ds)
			continue;

		for (port = 0; port < ds->num_ports; port++) {
			dp = &ds->ports[port];

			if (dsa_port_is_user(dp) || dsa_port_is_dsa(dp))
				dp->cpu_dp = dst->cpu_dp;
		}
	}

	return 0;
}

static void dsa_tree_teardown_default_cpu(struct dsa_switch_tree *dst)
{
	/* DSA currently only supports a single CPU port */
	dst->cpu_dp = NULL;
}

static int dsa_port_setup(struct dsa_port *dp)
{
	struct dsa_switch *ds = dp->ds;
	struct dsa_switch_tree *dst = ds->dst;
	const unsigned char *id = (const unsigned char *)&dst->index;
	const unsigned char len = sizeof(dst->index);
	struct devlink_port *dlp = &dp->devlink_port;
	bool dsa_port_link_registered = false;
	bool devlink_port_registered = false;
	struct devlink *dl = ds->devlink;
	bool dsa_port_enabled = false;
	int err = 0;

	switch (dp->type) {
	case DSA_PORT_TYPE_UNUSED:
		dsa_port_disable(dp);
		break;
	case DSA_PORT_TYPE_CPU:
		memset(dlp, 0, sizeof(*dlp));
		devlink_port_attrs_set(dlp, DEVLINK_PORT_FLAVOUR_CPU,
				       dp->index, false, 0, id, len);
		err = devlink_port_register(dl, dlp, dp->index);
		if (err)
			break;
		devlink_port_registered = true;

		err = dsa_port_link_register_of(dp);
		if (err)
			break;
		dsa_port_link_registered = true;

		err = dsa_port_enable(dp, NULL);
		if (err)
			break;
		dsa_port_enabled = true;

		break;
	case DSA_PORT_TYPE_DSA:
		memset(dlp, 0, sizeof(*dlp));
		devlink_port_attrs_set(dlp, DEVLINK_PORT_FLAVOUR_DSA,
				       dp->index, false, 0, id, len);
		err = devlink_port_register(dl, dlp, dp->index);
		if (err)
			break;
		devlink_port_registered = true;

		err = dsa_port_link_register_of(dp);
		if (err)
			break;
		dsa_port_link_registered = true;

		err = dsa_port_enable(dp, NULL);
		if (err)
			break;
		dsa_port_enabled = true;

		break;
	case DSA_PORT_TYPE_USER:
		memset(dlp, 0, sizeof(*dlp));
		devlink_port_attrs_set(dlp, DEVLINK_PORT_FLAVOUR_PHYSICAL,
				       dp->index, false, 0, id, len);
		err = devlink_port_register(dl, dlp, dp->index);
		if (err)
			break;
		devlink_port_registered = true;

		dp->mac = of_get_mac_address(dp->dn);
		err = dsa_slave_create(dp);
		if (err)
			break;

		devlink_port_type_eth_set(dlp, dp->slave);
		break;
	}

	if (err && dsa_port_enabled)
		dsa_port_disable(dp);
	if (err && dsa_port_link_registered)
		dsa_port_link_unregister_of(dp);
	if (err && devlink_port_registered)
		devlink_port_unregister(dlp);

	return err;
}

static void dsa_port_teardown(struct dsa_port *dp)
{
	struct devlink_port *dlp = &dp->devlink_port;

	switch (dp->type) {
	case DSA_PORT_TYPE_UNUSED:
		break;
	case DSA_PORT_TYPE_CPU:
		dsa_port_disable(dp);
		dsa_tag_driver_put(dp->tag_ops);
		devlink_port_unregister(dlp);
		dsa_port_link_unregister_of(dp);
		break;
	case DSA_PORT_TYPE_DSA:
		dsa_port_disable(dp);
		devlink_port_unregister(dlp);
		dsa_port_link_unregister_of(dp);
		break;
	case DSA_PORT_TYPE_USER:
		devlink_port_unregister(dlp);
		if (dp->slave) {
			dsa_slave_destroy(dp->slave);
			dp->slave = NULL;
		}
		break;
	}
}

static int dsa_switch_setup(struct dsa_switch *ds)
{
	int err = 0;

	/* Initialize ds->phys_mii_mask before registering the slave MDIO bus
	 * driver and before ops->setup() has run, since the switch drivers and
	 * the slave MDIO bus driver rely on these values for probing PHY
	 * devices or not
	 */
	ds->phys_mii_mask |= dsa_user_ports(ds);

	/* Add the switch to devlink before calling setup, so that setup can
	 * add dpipe tables
	 */
	ds->devlink = devlink_alloc(&dsa_devlink_ops, 0);
	if (!ds->devlink)
		return -ENOMEM;

	err = devlink_register(ds->devlink, ds->dev);
	if (err)
		goto free_devlink;

	err = dsa_switch_register_notifier(ds);
	if (err)
		goto unregister_devlink;

	err = ds->ops->setup(ds);
	if (err < 0)
		goto unregister_notifier;

	if (!ds->slave_mii_bus && ds->ops->phy_read) {
		ds->slave_mii_bus = devm_mdiobus_alloc(ds->dev);
		if (!ds->slave_mii_bus) {
			err = -ENOMEM;
			goto unregister_notifier;
		}

		dsa_slave_mii_bus_init(ds);

		err = mdiobus_register(ds->slave_mii_bus);
		if (err < 0)
			goto unregister_notifier;
	}

	return 0;

unregister_notifier:
	dsa_switch_unregister_notifier(ds);
unregister_devlink:
	devlink_unregister(ds->devlink);
free_devlink:
	devlink_free(ds->devlink);
	ds->devlink = NULL;

	return err;
}

static void dsa_switch_teardown(struct dsa_switch *ds)
{
	if (ds->slave_mii_bus && ds->ops->phy_read)
		mdiobus_unregister(ds->slave_mii_bus);

	dsa_switch_unregister_notifier(ds);

	if (ds->ops->teardown)
		ds->ops->teardown(ds);

	if (ds->devlink) {
		devlink_unregister(ds->devlink);
		devlink_free(ds->devlink);
		ds->devlink = NULL;
	}

}

static int dsa_tree_setup_switches(struct dsa_switch_tree *dst)
{
	struct dsa_switch *ds;
	struct dsa_port *dp;
	int device, port, i;
	int err = 0;

	for (device = 0; device < DSA_MAX_SWITCHES; device++) {
		ds = dst->ds[device];
		if (!ds)
			continue;

		err = dsa_switch_setup(ds);
		if (err)
			goto switch_teardown;

		for (port = 0; port < ds->num_ports; port++) {
			dp = &ds->ports[port];

			err = dsa_port_setup(dp);
			if (err)
				continue;
		}
	}

	return 0;

switch_teardown:
	for (i = 0; i < device; i++) {
		ds = dst->ds[i];
		if (!ds)
			continue;

		for (port = 0; port < ds->num_ports; port++) {
			dp = &ds->ports[port];

			dsa_port_teardown(dp);
		}

		dsa_switch_teardown(ds);
	}

	return err;
}

static void dsa_tree_teardown_switches(struct dsa_switch_tree *dst)
{
	struct dsa_switch *ds;
	struct dsa_port *dp;
	int device, port;

	for (device = 0; device < DSA_MAX_SWITCHES; device++) {
		ds = dst->ds[device];
		if (!ds)
			continue;

		for (port = 0; port < ds->num_ports; port++) {
			dp = &ds->ports[port];

			dsa_port_teardown(dp);
		}

		dsa_switch_teardown(ds);
	}
}

static int dsa_tree_setup_master(struct dsa_switch_tree *dst)
{
	struct dsa_port *cpu_dp = dst->cpu_dp;
	struct net_device *master = cpu_dp->master;

	/* DSA currently supports a single pair of CPU port and master device */
	return dsa_master_setup(master, cpu_dp);
}

static void dsa_tree_teardown_master(struct dsa_switch_tree *dst)
{
	struct dsa_port *cpu_dp = dst->cpu_dp;
	struct net_device *master = cpu_dp->master;

	return dsa_master_teardown(master);
}

static int dsa_tree_setup(struct dsa_switch_tree *dst)
{
	bool complete;
	int err;

	if (dst->setup) {
		pr_err("DSA: tree %d already setup! Disjoint trees?\n",
		       dst->index);
		return -EEXIST;
	}

	complete = dsa_tree_setup_routing_table(dst);
	if (!complete)
		return 0;

	err = dsa_tree_setup_default_cpu(dst);
	if (err)
		return err;

	err = dsa_tree_setup_switches(dst);
	if (err)
		goto teardown_default_cpu;

	err = dsa_tree_setup_master(dst);
	if (err)
		goto teardown_switches;

	dst->setup = true;

	pr_info("DSA: tree %d setup\n", dst->index);

	return 0;

teardown_switches:
	dsa_tree_teardown_switches(dst);
teardown_default_cpu:
	dsa_tree_teardown_default_cpu(dst);

	return err;
}

static void dsa_tree_teardown(struct dsa_switch_tree *dst)
{
	if (!dst->setup)
		return;

	dsa_tree_teardown_master(dst);

	dsa_tree_teardown_switches(dst);

	dsa_tree_teardown_default_cpu(dst);

	pr_info("DSA: tree %d torn down\n", dst->index);

	dst->setup = false;
}

static void dsa_tree_remove_switch(struct dsa_switch_tree *dst,
				   unsigned int index)
{
	dsa_tree_teardown(dst);

	dst->ds[index] = NULL;
	dsa_tree_put(dst);
}

static int dsa_tree_add_switch(struct dsa_switch_tree *dst,
			       struct dsa_switch *ds)
{
	unsigned int index = ds->index;
	int err;

	if (dst->ds[index])
		return -EBUSY;

	dsa_tree_get(dst);
	dst->ds[index] = ds;

	err = dsa_tree_setup(dst);
	if (err) {
		dst->ds[index] = NULL;
		dsa_tree_put(dst);
	}

	return err;
}

static int dsa_port_parse_user(struct dsa_port *dp, const char *name)
{
	if (!name)
		name = "eth%d";

	dp->type = DSA_PORT_TYPE_USER;
	dp->name = name;

	return 0;
}

static int dsa_port_parse_dsa(struct dsa_port *dp)
{
	dp->type = DSA_PORT_TYPE_DSA;

	return 0;
}

static int dsa_port_parse_cpu(struct dsa_port *dp, struct net_device *master)
{
	struct dsa_switch *ds = dp->ds;
	struct dsa_switch_tree *dst = ds->dst;
	const struct dsa_device_ops *tag_ops;
	enum dsa_tag_protocol tag_protocol;

	tag_protocol = ds->ops->get_tag_protocol(ds, dp->index);
	tag_ops = dsa_tag_driver_get(tag_protocol);
	if (IS_ERR(tag_ops)) {
		if (PTR_ERR(tag_ops) == -ENOPROTOOPT)
			return -EPROBE_DEFER;
		dev_warn(ds->dev, "No tagger for this switch\n");
		return PTR_ERR(tag_ops);
	}

	dp->type = DSA_PORT_TYPE_CPU;
	dp->filter = tag_ops->filter;
	dp->rcv = tag_ops->rcv;
	dp->tag_ops = tag_ops;
	dp->master = master;
	dp->dst = dst;

	return 0;
}

static int dsa_port_parse_of(struct dsa_port *dp, struct device_node *dn)
{
	struct device_node *ethernet = of_parse_phandle(dn, "ethernet", 0);
	const char *name = of_get_property(dn, "label", NULL);
	bool link = of_property_read_bool(dn, "link");

	dp->dn = dn;

	if (ethernet) {
		struct net_device *master;

		master = of_find_net_device_by_node(ethernet);
		if (!master)
			return -EPROBE_DEFER;

		return dsa_port_parse_cpu(dp, master);
	}

	if (link)
		return dsa_port_parse_dsa(dp);

	return dsa_port_parse_user(dp, name);
}

static int dsa_switch_parse_ports_of(struct dsa_switch *ds,
				     struct device_node *dn)
{
	struct device_node *ports, *port;
	struct dsa_port *dp;
	int err = 0;
	u32 reg;

	ports = of_get_child_by_name(dn, "ports");
	if (!ports) {
		dev_err(ds->dev, "no ports child node found\n");
		return -EINVAL;
	}

	for_each_available_child_of_node(ports, port) {
		err = of_property_read_u32(port, "reg", &reg);
		if (err)
			goto out_put_node;

		if (reg >= ds->num_ports) {
			err = -EINVAL;
			goto out_put_node;
		}

		dp = &ds->ports[reg];

		err = dsa_port_parse_of(dp, port);
		if (err)
			goto out_put_node;
	}

out_put_node:
	of_node_put(ports);
	return err;
}

static int dsa_switch_parse_member_of(struct dsa_switch *ds,
				      struct device_node *dn)
{
	u32 m[2] = { 0, 0 };
	int sz;

	/* Don't error out if this optional property isn't found */
	sz = of_property_read_variable_u32_array(dn, "dsa,member", m, 2, 2);
	if (sz < 0 && sz != -EINVAL)
		return sz;

	ds->index = m[1];
	if (ds->index >= DSA_MAX_SWITCHES)
		return -EINVAL;

	ds->dst = dsa_tree_touch(m[0]);
	if (!ds->dst)
		return -ENOMEM;

	return 0;
}

static int dsa_switch_parse_of(struct dsa_switch *ds, struct device_node *dn)
{
	int err;

	err = dsa_switch_parse_member_of(ds, dn);
	if (err)
		return err;

	return dsa_switch_parse_ports_of(ds, dn);
}

static int dsa_port_parse(struct dsa_port *dp, const char *name,
			  struct device *dev)
{
	if (!strcmp(name, "cpu")) {
		struct net_device *master;

		master = dsa_dev_to_net_device(dev);
		if (!master)
			return -EPROBE_DEFER;

		dev_put(master);

		return dsa_port_parse_cpu(dp, master);
	}

	if (!strcmp(name, "dsa"))
		return dsa_port_parse_dsa(dp);

	return dsa_port_parse_user(dp, name);
}

static int dsa_switch_parse_ports(struct dsa_switch *ds,
				  struct dsa_chip_data *cd)
{
	bool valid_name_found = false;
	struct dsa_port *dp;
	struct device *dev;
	const char *name;
	unsigned int i;
	int err;

	for (i = 0; i < DSA_MAX_PORTS; i++) {
		name = cd->port_names[i];
		dev = cd->netdev[i];
		dp = &ds->ports[i];

		if (!name)
			continue;

		err = dsa_port_parse(dp, name, dev);
		if (err)
			return err;

		valid_name_found = true;
	}

	if (!valid_name_found && i == DSA_MAX_PORTS)
		return -EINVAL;

	return 0;
}

static int dsa_switch_parse(struct dsa_switch *ds, struct dsa_chip_data *cd)
{
	ds->cd = cd;

	/* We don't support interconnected switches nor multiple trees via
	 * platform data, so this is the unique switch of the tree.
	 */
	ds->index = 0;
	ds->dst = dsa_tree_touch(0);
	if (!ds->dst)
		return -ENOMEM;

	return dsa_switch_parse_ports(ds, cd);
}

static int dsa_switch_add(struct dsa_switch *ds)
{
	struct dsa_switch_tree *dst = ds->dst;

	return dsa_tree_add_switch(dst, ds);
}

static int dsa_switch_probe(struct dsa_switch *ds)
{
	struct dsa_chip_data *pdata = ds->dev->platform_data;
	struct device_node *np = ds->dev->of_node;
	int err;

	if (np)
		err = dsa_switch_parse_of(ds, np);
	else if (pdata)
		err = dsa_switch_parse(ds, pdata);
	else
		err = -ENODEV;

	if (err)
		return err;

	return dsa_switch_add(ds);
}

struct dsa_switch *dsa_switch_alloc(struct device *dev, size_t n)
{
	struct dsa_switch *ds;
	int i;

	ds = devm_kzalloc(dev, struct_size(ds, ports, n), GFP_KERNEL);
	if (!ds)
		return NULL;

	ds->dev = dev;
	ds->num_ports = n;

	for (i = 0; i < ds->num_ports; ++i) {
		ds->ports[i].index = i;
		ds->ports[i].ds = ds;
	}

	return ds;
}
EXPORT_SYMBOL_GPL(dsa_switch_alloc);

int dsa_register_switch(struct dsa_switch *ds)
{
	int err;

	mutex_lock(&dsa2_mutex);
	err = dsa_switch_probe(ds);
	dsa_tree_put(ds->dst);
	mutex_unlock(&dsa2_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(dsa_register_switch);

static void dsa_switch_remove(struct dsa_switch *ds)
{
	struct dsa_switch_tree *dst = ds->dst;
	unsigned int index = ds->index;

	dsa_tree_remove_switch(dst, index);
}

void dsa_unregister_switch(struct dsa_switch *ds)
{
	mutex_lock(&dsa2_mutex);
	dsa_switch_remove(ds);
	mutex_unlock(&dsa2_mutex);
}
EXPORT_SYMBOL_GPL(dsa_unregister_switch);
