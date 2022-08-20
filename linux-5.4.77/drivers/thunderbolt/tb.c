// SPDX-License-Identifier: GPL-2.0
/*
 * Thunderbolt driver - bus logic (NHI independent)
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 * Copyright (C) 2019, Intel Corporation
 */

#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/platform_data/x86/apple.h>

#include "tb.h"
#include "tb_regs.h"
#include "tunnel.h"

/**
 * struct tb_cm - Simple Thunderbolt connection manager
 * @tunnel_list: List of active tunnels
 * @hotplug_active: tb_handle_hotplug will stop progressing plug
 *		    events and exit if this is not set (it needs to
 *		    acquire the lock one more time). Used to drain wq
 *		    after cfg has been paused.
 */
struct tb_cm {
	struct list_head tunnel_list;
	bool hotplug_active;
};

struct tb_hotplug_event {
	struct work_struct work;
	struct tb *tb;
	u64 route;
	u8 port;
	bool unplug;
};

static void tb_handle_hotplug(struct work_struct *work);

static void tb_queue_hotplug(struct tb *tb, u64 route, u8 port, bool unplug)
{
	struct tb_hotplug_event *ev;

	ev = kmalloc(sizeof(*ev), GFP_KERNEL);
	if (!ev)
		return;

	ev->tb = tb;
	ev->route = route;
	ev->port = port;
	ev->unplug = unplug;
	INIT_WORK(&ev->work, tb_handle_hotplug);
	queue_work(tb->wq, &ev->work);
}

/* enumeration & hot plug handling */

static void tb_discover_tunnels(struct tb_switch *sw)
{
	struct tb *tb = sw->tb;
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_port *port;
	int i;

	for (i = 1; i <= sw->config.max_port_number; i++) {
		struct tb_tunnel *tunnel = NULL;

		port = &sw->ports[i];
		switch (port->config.type) {
		case TB_TYPE_DP_HDMI_IN:
			tunnel = tb_tunnel_discover_dp(tb, port);
			break;

		case TB_TYPE_PCIE_DOWN:
			tunnel = tb_tunnel_discover_pci(tb, port);
			break;

		default:
			break;
		}

		if (!tunnel)
			continue;

		if (tb_tunnel_is_pci(tunnel)) {
			struct tb_switch *parent = tunnel->dst_port->sw;

			while (parent != tunnel->src_port->sw) {
				parent->boot = true;
				parent = tb_switch_parent(parent);
			}
		}

		list_add_tail(&tunnel->list, &tcm->tunnel_list);
	}

	for (i = 1; i <= sw->config.max_port_number; i++) {
		if (tb_port_has_remote(&sw->ports[i]))
			tb_discover_tunnels(sw->ports[i].remote->sw);
	}
}

static void tb_scan_xdomain(struct tb_port *port)
{
	struct tb_switch *sw = port->sw;
	struct tb *tb = sw->tb;
	struct tb_xdomain *xd;
	u64 route;

	route = tb_downstream_route(port);
	xd = tb_xdomain_find_by_route(tb, route);
	if (xd) {
		tb_xdomain_put(xd);
		return;
	}

	xd = tb_xdomain_alloc(tb, &sw->dev, route, tb->root_switch->uuid,
			      NULL);
	if (xd) {
		tb_port_at(route, sw)->xdomain = xd;
		tb_xdomain_add(xd);
	}
}

static void tb_scan_port(struct tb_port *port);

/**
 * tb_scan_switch() - scan for and initialize downstream switches
 */
static void tb_scan_switch(struct tb_switch *sw)
{
	int i;
	for (i = 1; i <= sw->config.max_port_number; i++)
		tb_scan_port(&sw->ports[i]);
}

/**
 * tb_scan_port() - check for and initialize switches below port
 */
static void tb_scan_port(struct tb_port *port)
{
	struct tb_cm *tcm = tb_priv(port->sw->tb);
	struct tb_port *upstream_port;
	struct tb_switch *sw;

	if (tb_is_upstream_port(port))
		return;

	if (tb_port_is_dpout(port) && tb_dp_port_hpd_is_active(port) == 1 &&
	    !tb_dp_port_is_enabled(port)) {
		tb_port_dbg(port, "DP adapter HPD set, queuing hotplug\n");
		tb_queue_hotplug(port->sw->tb, tb_route(port->sw), port->port,
				 false);
		return;
	}

	if (port->config.type != TB_TYPE_PORT)
		return;
	if (port->dual_link_port && port->link_nr)
		return; /*
			 * Downstream switch is reachable through two ports.
			 * Only scan on the primary port (link_nr == 0).
			 */
	if (tb_wait_for_port(port, false) <= 0)
		return;
	if (port->remote) {
		tb_port_dbg(port, "port already has a remote\n");
		return;
	}
	sw = tb_switch_alloc(port->sw->tb, &port->sw->dev,
			     tb_downstream_route(port));
	if (IS_ERR(sw)) {
		/*
		 * If there is an error accessing the connected switch
		 * it may be connected to another domain. Also we allow
		 * the other domain to be connected to a max depth switch.
		 */
		if (PTR_ERR(sw) == -EIO || PTR_ERR(sw) == -EADDRNOTAVAIL)
			tb_scan_xdomain(port);
		return;
	}

	if (tb_switch_configure(sw)) {
		tb_switch_put(sw);
		return;
	}

	/*
	 * If there was previously another domain connected remove it
	 * first.
	 */
	if (port->xdomain) {
		tb_xdomain_remove(port->xdomain);
		port->xdomain = NULL;
	}

	/*
	 * Do not send uevents until we have discovered all existing
	 * tunnels and know which switches were authorized already by
	 * the boot firmware.
	 */
	if (!tcm->hotplug_active)
		dev_set_uevent_suppress(&sw->dev, true);

	if (tb_switch_add(sw)) {
		tb_switch_put(sw);
		return;
	}

	/* Link the switches using both links if available */
	upstream_port = tb_upstream_port(sw);
	port->remote = upstream_port;
	upstream_port->remote = port;
	if (port->dual_link_port && upstream_port->dual_link_port) {
		port->dual_link_port->remote = upstream_port->dual_link_port;
		upstream_port->dual_link_port->remote = port->dual_link_port;
	}

	tb_scan_switch(sw);
}

static int tb_free_tunnel(struct tb *tb, enum tb_tunnel_type type,
			  struct tb_port *src_port, struct tb_port *dst_port)
{
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_tunnel *tunnel;

	list_for_each_entry(tunnel, &tcm->tunnel_list, list) {
		if (tunnel->type == type &&
		    ((src_port && src_port == tunnel->src_port) ||
		     (dst_port && dst_port == tunnel->dst_port))) {
			tb_tunnel_deactivate(tunnel);
			list_del(&tunnel->list);
			tb_tunnel_free(tunnel);
			return 0;
		}
	}

	return -ENODEV;
}

/**
 * tb_free_invalid_tunnels() - destroy tunnels of devices that have gone away
 */
static void tb_free_invalid_tunnels(struct tb *tb)
{
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_tunnel *tunnel;
	struct tb_tunnel *n;

	list_for_each_entry_safe(tunnel, n, &tcm->tunnel_list, list) {
		if (tb_tunnel_is_invalid(tunnel)) {
			tb_tunnel_deactivate(tunnel);
			list_del(&tunnel->list);
			tb_tunnel_free(tunnel);
		}
	}
}

/**
 * tb_free_unplugged_children() - traverse hierarchy and free unplugged switches
 */
static void tb_free_unplugged_children(struct tb_switch *sw)
{
	int i;
	for (i = 1; i <= sw->config.max_port_number; i++) {
		struct tb_port *port = &sw->ports[i];

		if (!tb_port_has_remote(port))
			continue;

		if (port->remote->sw->is_unplugged) {
			tb_switch_remove(port->remote->sw);
			port->remote = NULL;
			if (port->dual_link_port)
				port->dual_link_port->remote = NULL;
		} else {
			tb_free_unplugged_children(port->remote->sw);
		}
	}
}

/**
 * tb_find_port() - return the first port of @type on @sw or NULL
 * @sw: Switch to find the port from
 * @type: Port type to look for
 */
static struct tb_port *tb_find_port(struct tb_switch *sw,
				    enum tb_port_type type)
{
	int i;
	for (i = 1; i <= sw->config.max_port_number; i++)
		if (sw->ports[i].config.type == type)
			return &sw->ports[i];
	return NULL;
}

/**
 * tb_find_unused_port() - return the first inactive port on @sw
 * @sw: Switch to find the port on
 * @type: Port type to look for
 */
static struct tb_port *tb_find_unused_port(struct tb_switch *sw,
					   enum tb_port_type type)
{
	int i;

	for (i = 1; i <= sw->config.max_port_number; i++) {
		if (tb_is_upstream_port(&sw->ports[i]))
			continue;
		if (sw->ports[i].config.type != type)
			continue;
		if (!sw->ports[i].cap_adap)
			continue;
		if (tb_port_is_enabled(&sw->ports[i]))
			continue;
		return &sw->ports[i];
	}
	return NULL;
}

static struct tb_port *tb_find_pcie_down(struct tb_switch *sw,
					 const struct tb_port *port)
{
	/*
	 * To keep plugging devices consistently in the same PCIe
	 * hierarchy, do mapping here for root switch downstream PCIe
	 * ports.
	 */
	if (!tb_route(sw)) {
		int phy_port = tb_phy_port_from_link(port->port);
		int index;

		/*
		 * Hard-coded Thunderbolt port to PCIe down port mapping
		 * per controller.
		 */
		if (tb_switch_is_cr(sw))
			index = !phy_port ? 6 : 7;
		else if (tb_switch_is_fr(sw))
			index = !phy_port ? 6 : 8;
		else
			goto out;

		/* Validate the hard-coding */
		if (WARN_ON(index > sw->config.max_port_number))
			goto out;
		if (WARN_ON(!tb_port_is_pcie_down(&sw->ports[index])))
			goto out;
		if (WARN_ON(tb_pci_port_is_enabled(&sw->ports[index])))
			goto out;

		return &sw->ports[index];
	}

out:
	return tb_find_unused_port(sw, TB_TYPE_PCIE_DOWN);
}

static int tb_tunnel_dp(struct tb *tb, struct tb_port *out)
{
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_switch *sw = out->sw;
	struct tb_tunnel *tunnel;
	struct tb_port *in;

	if (tb_port_is_enabled(out))
		return 0;

	do {
		sw = tb_to_switch(sw->dev.parent);
		if (!sw)
			return 0;
		in = tb_find_unused_port(sw, TB_TYPE_DP_HDMI_IN);
	} while (!in);

	tunnel = tb_tunnel_alloc_dp(tb, in, out);
	if (!tunnel) {
		tb_port_dbg(out, "DP tunnel allocation failed\n");
		return -ENOMEM;
	}

	if (tb_tunnel_activate(tunnel)) {
		tb_port_info(out, "DP tunnel activation failed, aborting\n");
		tb_tunnel_free(tunnel);
		return -EIO;
	}

	list_add_tail(&tunnel->list, &tcm->tunnel_list);
	return 0;
}

static void tb_teardown_dp(struct tb *tb, struct tb_port *out)
{
	tb_free_tunnel(tb, TB_TUNNEL_DP, NULL, out);
}

static int tb_tunnel_pci(struct tb *tb, struct tb_switch *sw)
{
	struct tb_port *up, *down, *port;
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_switch *parent_sw;
	struct tb_tunnel *tunnel;

	up = tb_find_port(sw, TB_TYPE_PCIE_UP);
	if (!up)
		return 0;

	/*
	 * Look up available down port. Since we are chaining it should
	 * be found right above this switch.
	 */
	parent_sw = tb_to_switch(sw->dev.parent);
	port = tb_port_at(tb_route(sw), parent_sw);
	down = tb_find_pcie_down(parent_sw, port);
	if (!down)
		return 0;

	tunnel = tb_tunnel_alloc_pci(tb, up, down);
	if (!tunnel)
		return -ENOMEM;

	if (tb_tunnel_activate(tunnel)) {
		tb_port_info(up,
			     "PCIe tunnel activation failed, aborting\n");
		tb_tunnel_free(tunnel);
		return -EIO;
	}

	list_add_tail(&tunnel->list, &tcm->tunnel_list);
	return 0;
}

static int tb_approve_xdomain_paths(struct tb *tb, struct tb_xdomain *xd)
{
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_port *nhi_port, *dst_port;
	struct tb_tunnel *tunnel;
	struct tb_switch *sw;

	sw = tb_to_switch(xd->dev.parent);
	dst_port = tb_port_at(xd->route, sw);
	nhi_port = tb_find_port(tb->root_switch, TB_TYPE_NHI);

	mutex_lock(&tb->lock);
	tunnel = tb_tunnel_alloc_dma(tb, nhi_port, dst_port, xd->transmit_ring,
				     xd->transmit_path, xd->receive_ring,
				     xd->receive_path);
	if (!tunnel) {
		mutex_unlock(&tb->lock);
		return -ENOMEM;
	}

	if (tb_tunnel_activate(tunnel)) {
		tb_port_info(nhi_port,
			     "DMA tunnel activation failed, aborting\n");
		tb_tunnel_free(tunnel);
		mutex_unlock(&tb->lock);
		return -EIO;
	}

	list_add_tail(&tunnel->list, &tcm->tunnel_list);
	mutex_unlock(&tb->lock);
	return 0;
}

static void __tb_disconnect_xdomain_paths(struct tb *tb, struct tb_xdomain *xd)
{
	struct tb_port *dst_port;
	struct tb_switch *sw;

	sw = tb_to_switch(xd->dev.parent);
	dst_port = tb_port_at(xd->route, sw);

	/*
	 * It is possible that the tunnel was already teared down (in
	 * case of cable disconnect) so it is fine if we cannot find it
	 * here anymore.
	 */
	tb_free_tunnel(tb, TB_TUNNEL_DMA, NULL, dst_port);
}

static int tb_disconnect_xdomain_paths(struct tb *tb, struct tb_xdomain *xd)
{
	if (!xd->is_unplugged) {
		mutex_lock(&tb->lock);
		__tb_disconnect_xdomain_paths(tb, xd);
		mutex_unlock(&tb->lock);
	}
	return 0;
}

/* hotplug handling */

/**
 * tb_handle_hotplug() - handle hotplug event
 *
 * Executes on tb->wq.
 */
static void tb_handle_hotplug(struct work_struct *work)
{
	struct tb_hotplug_event *ev = container_of(work, typeof(*ev), work);
	struct tb *tb = ev->tb;
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_switch *sw;
	struct tb_port *port;
	mutex_lock(&tb->lock);
	if (!tcm->hotplug_active)
		goto out; /* during init, suspend or shutdown */

	sw = tb_switch_find_by_route(tb, ev->route);
	if (!sw) {
		tb_warn(tb,
			"hotplug event from non existent switch %llx:%x (unplug: %d)\n",
			ev->route, ev->port, ev->unplug);
		goto out;
	}
	if (ev->port > sw->config.max_port_number) {
		tb_warn(tb,
			"hotplug event from non existent port %llx:%x (unplug: %d)\n",
			ev->route, ev->port, ev->unplug);
		goto put_sw;
	}
	port = &sw->ports[ev->port];
	if (tb_is_upstream_port(port)) {
		tb_dbg(tb, "hotplug event for upstream port %llx:%x (unplug: %d)\n",
		       ev->route, ev->port, ev->unplug);
		goto put_sw;
	}
	if (ev->unplug) {
		if (tb_port_has_remote(port)) {
			tb_port_dbg(port, "switch unplugged\n");
			tb_sw_set_unplugged(port->remote->sw);
			tb_free_invalid_tunnels(tb);
			tb_switch_remove(port->remote->sw);
			port->remote = NULL;
			if (port->dual_link_port)
				port->dual_link_port->remote = NULL;
		} else if (port->xdomain) {
			struct tb_xdomain *xd = tb_xdomain_get(port->xdomain);

			tb_port_dbg(port, "xdomain unplugged\n");
			/*
			 * Service drivers are unbound during
			 * tb_xdomain_remove() so setting XDomain as
			 * unplugged here prevents deadlock if they call
			 * tb_xdomain_disable_paths(). We will tear down
			 * the path below.
			 */
			xd->is_unplugged = true;
			tb_xdomain_remove(xd);
			port->xdomain = NULL;
			__tb_disconnect_xdomain_paths(tb, xd);
			tb_xdomain_put(xd);
		} else if (tb_port_is_dpout(port)) {
			tb_teardown_dp(tb, port);
		} else {
			tb_port_dbg(port,
				   "got unplug event for disconnected port, ignoring\n");
		}
	} else if (port->remote) {
		tb_port_dbg(port, "got plug event for connected port, ignoring\n");
	} else {
		if (tb_port_is_null(port)) {
			tb_port_dbg(port, "hotplug: scanning\n");
			tb_scan_port(port);
			if (!port->remote)
				tb_port_dbg(port, "hotplug: no switch found\n");
		} else if (tb_port_is_dpout(port)) {
			tb_tunnel_dp(tb, port);
		}
	}

put_sw:
	tb_switch_put(sw);
out:
	mutex_unlock(&tb->lock);
	kfree(ev);
}

/**
 * tb_schedule_hotplug_handler() - callback function for the control channel
 *
 * Delegates to tb_handle_hotplug.
 */
static void tb_handle_event(struct tb *tb, enum tb_cfg_pkg_type type,
			    const void *buf, size_t size)
{
	const struct cfg_event_pkg *pkg = buf;
	u64 route;

	if (type != TB_CFG_PKG_EVENT) {
		tb_warn(tb, "unexpected event %#x, ignoring\n", type);
		return;
	}

	route = tb_cfg_get_route(&pkg->header);

	if (tb_cfg_error(tb->ctl, route, pkg->port,
			 TB_CFG_ERROR_ACK_PLUG_EVENT)) {
		tb_warn(tb, "could not ack plug event on %llx:%x\n", route,
			pkg->port);
	}

	tb_queue_hotplug(tb, route, pkg->port, pkg->unplug);
}

static void tb_stop(struct tb *tb)
{
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_tunnel *tunnel;
	struct tb_tunnel *n;

	/* tunnels are only present after everything has been initialized */
	list_for_each_entry_safe(tunnel, n, &tcm->tunnel_list, list) {
		/*
		 * DMA tunnels require the driver to be functional so we
		 * tear them down. Other protocol tunnels can be left
		 * intact.
		 */
		if (tb_tunnel_is_dma(tunnel))
			tb_tunnel_deactivate(tunnel);
		tb_tunnel_free(tunnel);
	}
	tb_switch_remove(tb->root_switch);
	tcm->hotplug_active = false; /* signal tb_handle_hotplug to quit */
}

static int tb_scan_finalize_switch(struct device *dev, void *data)
{
	if (tb_is_switch(dev)) {
		struct tb_switch *sw = tb_to_switch(dev);

		/*
		 * If we found that the switch was already setup by the
		 * boot firmware, mark it as authorized now before we
		 * send uevent to userspace.
		 */
		if (sw->boot)
			sw->authorized = 1;

		dev_set_uevent_suppress(dev, false);
		kobject_uevent(&dev->kobj, KOBJ_ADD);
		device_for_each_child(dev, NULL, tb_scan_finalize_switch);
	}

	return 0;
}

static int tb_start(struct tb *tb)
{
	struct tb_cm *tcm = tb_priv(tb);
	int ret;

	tb->root_switch = tb_switch_alloc(tb, &tb->dev, 0);
	if (IS_ERR(tb->root_switch))
		return PTR_ERR(tb->root_switch);

	/*
	 * ICM firmware upgrade needs running firmware and in native
	 * mode that is not available so disable firmware upgrade of the
	 * root switch.
	 */
	tb->root_switch->no_nvm_upgrade = true;

	ret = tb_switch_configure(tb->root_switch);
	if (ret) {
		tb_switch_put(tb->root_switch);
		return ret;
	}

	/* Announce the switch to the world */
	ret = tb_switch_add(tb->root_switch);
	if (ret) {
		tb_switch_put(tb->root_switch);
		return ret;
	}

	/* Full scan to discover devices added before the driver was loaded. */
	tb_scan_switch(tb->root_switch);
	/* Find out tunnels created by the boot firmware */
	tb_discover_tunnels(tb->root_switch);
	/* Make the discovered switches available to the userspace */
	device_for_each_child(&tb->root_switch->dev, NULL,
			      tb_scan_finalize_switch);

	/* Allow tb_handle_hotplug to progress events */
	tcm->hotplug_active = true;
	return 0;
}

static int tb_suspend_noirq(struct tb *tb)
{
	struct tb_cm *tcm = tb_priv(tb);

	tb_dbg(tb, "suspending...\n");
	tb_switch_suspend(tb->root_switch);
	tcm->hotplug_active = false; /* signal tb_handle_hotplug to quit */
	tb_dbg(tb, "suspend finished\n");

	return 0;
}

static int tb_resume_noirq(struct tb *tb)
{
	struct tb_cm *tcm = tb_priv(tb);
	struct tb_tunnel *tunnel, *n;

	tb_dbg(tb, "resuming...\n");

	/* remove any pci devices the firmware might have setup */
	tb_switch_reset(tb, 0);

	tb_switch_resume(tb->root_switch);
	tb_free_invalid_tunnels(tb);
	tb_free_unplugged_children(tb->root_switch);
	list_for_each_entry_safe(tunnel, n, &tcm->tunnel_list, list)
		tb_tunnel_restart(tunnel);
	if (!list_empty(&tcm->tunnel_list)) {
		/*
		 * the pcie links need some time to get going.
		 * 100ms works for me...
		 */
		tb_dbg(tb, "tunnels restarted, sleeping for 100ms\n");
		msleep(100);
	}
	 /* Allow tb_handle_hotplug to progress events */
	tcm->hotplug_active = true;
	tb_dbg(tb, "resume finished\n");

	return 0;
}

static int tb_free_unplugged_xdomains(struct tb_switch *sw)
{
	int i, ret = 0;

	for (i = 1; i <= sw->config.max_port_number; i++) {
		struct tb_port *port = &sw->ports[i];

		if (tb_is_upstream_port(port))
			continue;
		if (port->xdomain && port->xdomain->is_unplugged) {
			tb_xdomain_remove(port->xdomain);
			port->xdomain = NULL;
			ret++;
		} else if (port->remote) {
			ret += tb_free_unplugged_xdomains(port->remote->sw);
		}
	}

	return ret;
}

static void tb_complete(struct tb *tb)
{
	/*
	 * Release any unplugged XDomains and if there is a case where
	 * another domain is swapped in place of unplugged XDomain we
	 * need to run another rescan.
	 */
	mutex_lock(&tb->lock);
	if (tb_free_unplugged_xdomains(tb->root_switch))
		tb_scan_switch(tb->root_switch);
	mutex_unlock(&tb->lock);
}

static const struct tb_cm_ops tb_cm_ops = {
	.start = tb_start,
	.stop = tb_stop,
	.suspend_noirq = tb_suspend_noirq,
	.resume_noirq = tb_resume_noirq,
	.complete = tb_complete,
	.handle_event = tb_handle_event,
	.approve_switch = tb_tunnel_pci,
	.approve_xdomain_paths = tb_approve_xdomain_paths,
	.disconnect_xdomain_paths = tb_disconnect_xdomain_paths,
};

struct tb *tb_probe(struct tb_nhi *nhi)
{
	struct tb_cm *tcm;
	struct tb *tb;

	if (!x86_apple_machine)
		return NULL;

	tb = tb_domain_alloc(nhi, sizeof(*tcm));
	if (!tb)
		return NULL;

	tb->security_level = TB_SECURITY_USER;
	tb->cm_ops = &tb_cm_ops;

	tcm = tb_priv(tb);
	INIT_LIST_HEAD(&tcm->tunnel_list);

	return tb;
}
