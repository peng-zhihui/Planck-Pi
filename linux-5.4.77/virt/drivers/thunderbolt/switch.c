// SPDX-License-Identifier: GPL-2.0
/*
 * Thunderbolt driver - switch/port utility functions
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 * Copyright (C) 2018, Intel Corporation
 */

#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/nvmem-provider.h>
#include <linux/pm_runtime.h>
#include <linux/sched/signal.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "tb.h"

/* Switch NVM support */

#define NVM_DEVID		0x05
#define NVM_VERSION		0x08
#define NVM_CSS			0x10
#define NVM_FLASH_SIZE		0x45

#define NVM_MIN_SIZE		SZ_32K
#define NVM_MAX_SIZE		SZ_512K

static DEFINE_IDA(nvm_ida);

struct nvm_auth_status {
	struct list_head list;
	uuid_t uuid;
	u32 status;
};

/*
 * Hold NVM authentication failure status per switch This information
 * needs to stay around even when the switch gets power cycled so we
 * keep it separately.
 */
static LIST_HEAD(nvm_auth_status_cache);
static DEFINE_MUTEX(nvm_auth_status_lock);

static struct nvm_auth_status *__nvm_get_auth_status(const struct tb_switch *sw)
{
	struct nvm_auth_status *st;

	list_for_each_entry(st, &nvm_auth_status_cache, list) {
		if (uuid_equal(&st->uuid, sw->uuid))
			return st;
	}

	return NULL;
}

static void nvm_get_auth_status(const struct tb_switch *sw, u32 *status)
{
	struct nvm_auth_status *st;

	mutex_lock(&nvm_auth_status_lock);
	st = __nvm_get_auth_status(sw);
	mutex_unlock(&nvm_auth_status_lock);

	*status = st ? st->status : 0;
}

static void nvm_set_auth_status(const struct tb_switch *sw, u32 status)
{
	struct nvm_auth_status *st;

	if (WARN_ON(!sw->uuid))
		return;

	mutex_lock(&nvm_auth_status_lock);
	st = __nvm_get_auth_status(sw);

	if (!st) {
		st = kzalloc(sizeof(*st), GFP_KERNEL);
		if (!st)
			goto unlock;

		memcpy(&st->uuid, sw->uuid, sizeof(st->uuid));
		INIT_LIST_HEAD(&st->list);
		list_add_tail(&st->list, &nvm_auth_status_cache);
	}

	st->status = status;
unlock:
	mutex_unlock(&nvm_auth_status_lock);
}

static void nvm_clear_auth_status(const struct tb_switch *sw)
{
	struct nvm_auth_status *st;

	mutex_lock(&nvm_auth_status_lock);
	st = __nvm_get_auth_status(sw);
	if (st) {
		list_del(&st->list);
		kfree(st);
	}
	mutex_unlock(&nvm_auth_status_lock);
}

static int nvm_validate_and_write(struct tb_switch *sw)
{
	unsigned int image_size, hdr_size;
	const u8 *buf = sw->nvm->buf;
	u16 ds_size;
	int ret;

	if (!buf)
		return -EINVAL;

	image_size = sw->nvm->buf_data_size;
	if (image_size < NVM_MIN_SIZE || image_size > NVM_MAX_SIZE)
		return -EINVAL;

	/*
	 * FARB pointer must point inside the image and must at least
	 * contain parts of the digital section we will be reading here.
	 */
	hdr_size = (*(u32 *)buf) & 0xffffff;
	if (hdr_size + NVM_DEVID + 2 >= image_size)
		return -EINVAL;

	/* Digital section start should be aligned to 4k page */
	if (!IS_ALIGNED(hdr_size, SZ_4K))
		return -EINVAL;

	/*
	 * Read digital section size and check that it also fits inside
	 * the image.
	 */
	ds_size = *(u16 *)(buf + hdr_size);
	if (ds_size >= image_size)
		return -EINVAL;

	if (!sw->safe_mode) {
		u16 device_id;

		/*
		 * Make sure the device ID in the image matches the one
		 * we read from the switch config space.
		 */
		device_id = *(u16 *)(buf + hdr_size + NVM_DEVID);
		if (device_id != sw->config.device_id)
			return -EINVAL;

		if (sw->generation < 3) {
			/* Write CSS headers first */
			ret = dma_port_flash_write(sw->dma_port,
				DMA_PORT_CSS_ADDRESS, buf + NVM_CSS,
				DMA_PORT_CSS_MAX_SIZE);
			if (ret)
				return ret;
		}

		/* Skip headers in the image */
		buf += hdr_size;
		image_size -= hdr_size;
	}

	return dma_port_flash_write(sw->dma_port, 0, buf, image_size);
}

static int nvm_authenticate_host(struct tb_switch *sw)
{
	int ret = 0;

	/*
	 * Root switch NVM upgrade requires that we disconnect the
	 * existing paths first (in case it is not in safe mode
	 * already).
	 */
	if (!sw->safe_mode) {
		u32 status;

		ret = tb_domain_disconnect_all_paths(sw->tb);
		if (ret)
			return ret;
		/*
		 * The host controller goes away pretty soon after this if
		 * everything goes well so getting timeout is expected.
		 */
		ret = dma_port_flash_update_auth(sw->dma_port);
		if (!ret || ret == -ETIMEDOUT)
			return 0;

		/*
		 * Any error from update auth operation requires power
		 * cycling of the host router.
		 */
		tb_sw_warn(sw, "failed to authenticate NVM, power cycling\n");
		if (dma_port_flash_update_auth_status(sw->dma_port, &status) > 0)
			nvm_set_auth_status(sw, status);
	}

	/*
	 * From safe mode we can get out by just power cycling the
	 * switch.
	 */
	dma_port_power_cycle(sw->dma_port);
	return ret;
}

static int nvm_authenticate_device(struct tb_switch *sw)
{
	int ret, retries = 10;

	ret = dma_port_flash_update_auth(sw->dma_port);
	switch (ret) {
	case 0:
	case -ETIMEDOUT:
	case -EACCES:
	case -EINVAL:
		/* Power cycle is required */
		break;
	default:
		return ret;
	}

	/*
	 * Poll here for the authentication status. It takes some time
	 * for the device to respond (we get timeout for a while). Once
	 * we get response the device needs to be power cycled in order
	 * to the new NVM to be taken into use.
	 */
	do {
		u32 status;

		ret = dma_port_flash_update_auth_status(sw->dma_port, &status);
		if (ret < 0 && ret != -ETIMEDOUT)
			return ret;
		if (ret > 0) {
			if (status) {
				tb_sw_warn(sw, "failed to authenticate NVM\n");
				nvm_set_auth_status(sw, status);
			}

			tb_sw_info(sw, "power cycling the switch now\n");
			dma_port_power_cycle(sw->dma_port);
			return 0;
		}

		msleep(500);
	} while (--retries);

	return -ETIMEDOUT;
}

static int tb_switch_nvm_read(void *priv, unsigned int offset, void *val,
			      size_t bytes)
{
	struct tb_switch *sw = priv;
	int ret;

	pm_runtime_get_sync(&sw->dev);

	if (!mutex_trylock(&sw->tb->lock)) {
		ret = restart_syscall();
		goto out;
	}

	ret = dma_port_flash_read(sw->dma_port, offset, val, bytes);
	mutex_unlock(&sw->tb->lock);

out:
	pm_runtime_mark_last_busy(&sw->dev);
	pm_runtime_put_autosuspend(&sw->dev);

	return ret;
}

static int tb_switch_nvm_no_read(void *priv, unsigned int offset, void *val,
				 size_t bytes)
{
	return -EPERM;
}

static int tb_switch_nvm_write(void *priv, unsigned int offset, void *val,
			       size_t bytes)
{
	struct tb_switch *sw = priv;
	int ret = 0;

	if (!mutex_trylock(&sw->tb->lock))
		return restart_syscall();

	/*
	 * Since writing the NVM image might require some special steps,
	 * for example when CSS headers are written, we cache the image
	 * locally here and handle the special cases when the user asks
	 * us to authenticate the image.
	 */
	if (!sw->nvm->buf) {
		sw->nvm->buf = vmalloc(NVM_MAX_SIZE);
		if (!sw->nvm->buf) {
			ret = -ENOMEM;
			goto unlock;
		}
	}

	sw->nvm->buf_data_size = offset + bytes;
	memcpy(sw->nvm->buf + offset, val, bytes);

unlock:
	mutex_unlock(&sw->tb->lock);

	return ret;
}

static struct nvmem_device *register_nvmem(struct tb_switch *sw, int id,
					   size_t size, bool active)
{
	struct nvmem_config config;

	memset(&config, 0, sizeof(config));

	if (active) {
		config.name = "nvm_active";
		config.reg_read = tb_switch_nvm_read;
		config.read_only = true;
	} else {
		config.name = "nvm_non_active";
		config.reg_read = tb_switch_nvm_no_read;
		config.reg_write = tb_switch_nvm_write;
		config.root_only = true;
	}

	config.id = id;
	config.stride = 4;
	config.word_size = 4;
	config.size = size;
	config.dev = &sw->dev;
	config.owner = THIS_MODULE;
	config.priv = sw;

	return nvmem_register(&config);
}

static int tb_switch_nvm_add(struct tb_switch *sw)
{
	struct nvmem_device *nvm_dev;
	struct tb_switch_nvm *nvm;
	u32 val;
	int ret;

	if (!sw->dma_port)
		return 0;

	nvm = kzalloc(sizeof(*nvm), GFP_KERNEL);
	if (!nvm)
		return -ENOMEM;

	nvm->id = ida_simple_get(&nvm_ida, 0, 0, GFP_KERNEL);

	/*
	 * If the switch is in safe-mode the only accessible portion of
	 * the NVM is the non-active one where userspace is expected to
	 * write new functional NVM.
	 */
	if (!sw->safe_mode) {
		u32 nvm_size, hdr_size;

		ret = dma_port_flash_read(sw->dma_port, NVM_FLASH_SIZE, &val,
					  sizeof(val));
		if (ret)
			goto err_ida;

		hdr_size = sw->generation < 3 ? SZ_8K : SZ_16K;
		nvm_size = (SZ_1M << (val & 7)) / 8;
		nvm_size = (nvm_size - hdr_size) / 2;

		ret = dma_port_flash_read(sw->dma_port, NVM_VERSION, &val,
					  sizeof(val));
		if (ret)
			goto err_ida;

		nvm->major = val >> 16;
		nvm->minor = val >> 8;

		nvm_dev = register_nvmem(sw, nvm->id, nvm_size, true);
		if (IS_ERR(nvm_dev)) {
			ret = PTR_ERR(nvm_dev);
			goto err_ida;
		}
		nvm->active = nvm_dev;
	}

	if (!sw->no_nvm_upgrade) {
		nvm_dev = register_nvmem(sw, nvm->id, NVM_MAX_SIZE, false);
		if (IS_ERR(nvm_dev)) {
			ret = PTR_ERR(nvm_dev);
			goto err_nvm_active;
		}
		nvm->non_active = nvm_dev;
	}

	sw->nvm = nvm;
	return 0;

err_nvm_active:
	if (nvm->active)
		nvmem_unregister(nvm->active);
err_ida:
	ida_simple_remove(&nvm_ida, nvm->id);
	kfree(nvm);

	return ret;
}

static void tb_switch_nvm_remove(struct tb_switch *sw)
{
	struct tb_switch_nvm *nvm;

	nvm = sw->nvm;
	sw->nvm = NULL;

	if (!nvm)
		return;

	/* Remove authentication status in case the switch is unplugged */
	if (!nvm->authenticating)
		nvm_clear_auth_status(sw);

	if (nvm->non_active)
		nvmem_unregister(nvm->non_active);
	if (nvm->active)
		nvmem_unregister(nvm->active);
	ida_simple_remove(&nvm_ida, nvm->id);
	vfree(nvm->buf);
	kfree(nvm);
}

/* port utility functions */

static const char *tb_port_type(struct tb_regs_port_header *port)
{
	switch (port->type >> 16) {
	case 0:
		switch ((u8) port->type) {
		case 0:
			return "Inactive";
		case 1:
			return "Port";
		case 2:
			return "NHI";
		default:
			return "unknown";
		}
	case 0x2:
		return "Ethernet";
	case 0x8:
		return "SATA";
	case 0xe:
		return "DP/HDMI";
	case 0x10:
		return "PCIe";
	case 0x20:
		return "USB";
	default:
		return "unknown";
	}
}

static void tb_dump_port(struct tb *tb, struct tb_regs_port_header *port)
{
	tb_dbg(tb,
	       " Port %d: %x:%x (Revision: %d, TB Version: %d, Type: %s (%#x))\n",
	       port->port_number, port->vendor_id, port->device_id,
	       port->revision, port->thunderbolt_version, tb_port_type(port),
	       port->type);
	tb_dbg(tb, "  Max hop id (in/out): %d/%d\n",
	       port->max_in_hop_id, port->max_out_hop_id);
	tb_dbg(tb, "  Max counters: %d\n", port->max_counters);
	tb_dbg(tb, "  NFC Credits: %#x\n", port->nfc_credits);
}

/**
 * tb_port_state() - get connectedness state of a port
 *
 * The port must have a TB_CAP_PHY (i.e. it should be a real port).
 *
 * Return: Returns an enum tb_port_state on success or an error code on failure.
 */
static int tb_port_state(struct tb_port *port)
{
	struct tb_cap_phy phy;
	int res;
	if (port->cap_phy == 0) {
		tb_port_WARN(port, "does not have a PHY\n");
		return -EINVAL;
	}
	res = tb_port_read(port, &phy, TB_CFG_PORT, port->cap_phy, 2);
	if (res)
		return res;
	return phy.state;
}

/**
 * tb_wait_for_port() - wait for a port to become ready
 *
 * Wait up to 1 second for a port to reach state TB_PORT_UP. If
 * wait_if_unplugged is set then we also wait if the port is in state
 * TB_PORT_UNPLUGGED (it takes a while for the device to be registered after
 * switch resume). Otherwise we only wait if a device is registered but the link
 * has not yet been established.
 *
 * Return: Returns an error code on failure. Returns 0 if the port is not
 * connected or failed to reach state TB_PORT_UP within one second. Returns 1
 * if the port is connected and in state TB_PORT_UP.
 */
int tb_wait_for_port(struct tb_port *port, bool wait_if_unplugged)
{
	int retries = 10;
	int state;
	if (!port->cap_phy) {
		tb_port_WARN(port, "does not have PHY\n");
		return -EINVAL;
	}
	if (tb_is_upstream_port(port)) {
		tb_port_WARN(port, "is the upstream port\n");
		return -EINVAL;
	}

	while (retries--) {
		state = tb_port_state(port);
		if (state < 0)
			return state;
		if (state == TB_PORT_DISABLED) {
			tb_port_dbg(port, "is disabled (state: 0)\n");
			return 0;
		}
		if (state == TB_PORT_UNPLUGGED) {
			if (wait_if_unplugged) {
				/* used during resume */
				tb_port_dbg(port,
					    "is unplugged (state: 7), retrying...\n");
				msleep(100);
				continue;
			}
			tb_port_dbg(port, "is unplugged (state: 7)\n");
			return 0;
		}
		if (state == TB_PORT_UP) {
			tb_port_dbg(port, "is connected, link is up (state: 2)\n");
			return 1;
		}

		/*
		 * After plug-in the state is TB_PORT_CONNECTING. Give it some
		 * time.
		 */
		tb_port_dbg(port,
			    "is connected, link is not up (state: %d), retrying...\n",
			    state);
		msleep(100);
	}
	tb_port_warn(port,
		     "failed to reach state TB_PORT_UP. Ignoring port...\n");
	return 0;
}

/**
 * tb_port_add_nfc_credits() - add/remove non flow controlled credits to port
 *
 * Change the number of NFC credits allocated to @port by @credits. To remove
 * NFC credits pass a negative amount of credits.
 *
 * Return: Returns 0 on success or an error code on failure.
 */
int tb_port_add_nfc_credits(struct tb_port *port, int credits)
{
	u32 nfc_credits;

	if (credits == 0 || port->sw->is_unplugged)
		return 0;

	nfc_credits = port->config.nfc_credits & TB_PORT_NFC_CREDITS_MASK;
	nfc_credits += credits;

	tb_port_dbg(port, "adding %d NFC credits to %lu",
		    credits, port->config.nfc_credits & TB_PORT_NFC_CREDITS_MASK);

	port->config.nfc_credits &= ~TB_PORT_NFC_CREDITS_MASK;
	port->config.nfc_credits |= nfc_credits;

	return tb_port_write(port, &port->config.nfc_credits,
			     TB_CFG_PORT, 4, 1);
}

/**
 * tb_port_set_initial_credits() - Set initial port link credits allocated
 * @port: Port to set the initial credits
 * @credits: Number of credits to to allocate
 *
 * Set initial credits value to be used for ingress shared buffering.
 */
int tb_port_set_initial_credits(struct tb_port *port, u32 credits)
{
	u32 data;
	int ret;

	ret = tb_port_read(port, &data, TB_CFG_PORT, 5, 1);
	if (ret)
		return ret;

	data &= ~TB_PORT_LCA_MASK;
	data |= (credits << TB_PORT_LCA_SHIFT) & TB_PORT_LCA_MASK;

	return tb_port_write(port, &data, TB_CFG_PORT, 5, 1);
}

/**
 * tb_port_clear_counter() - clear a counter in TB_CFG_COUNTER
 *
 * Return: Returns 0 on success or an error code on failure.
 */
int tb_port_clear_counter(struct tb_port *port, int counter)
{
	u32 zero[3] = { 0, 0, 0 };
	tb_port_dbg(port, "clearing counter %d\n", counter);
	return tb_port_write(port, zero, TB_CFG_COUNTERS, 3 * counter, 3);
}

/**
 * tb_init_port() - initialize a port
 *
 * This is a helper method for tb_switch_alloc. Does not check or initialize
 * any downstream switches.
 *
 * Return: Returns 0 on success or an error code on failure.
 */
static int tb_init_port(struct tb_port *port)
{
	int res;
	int cap;

	res = tb_port_read(port, &port->config, TB_CFG_PORT, 0, 8);
	if (res) {
		if (res == -ENODEV) {
			tb_dbg(port->sw->tb, " Port %d: not implemented\n",
			       port->port);
			return 0;
		}
		return res;
	}

	/* Port 0 is the switch itself and has no PHY. */
	if (port->config.type == TB_TYPE_PORT && port->port != 0) {
		cap = tb_port_find_cap(port, TB_PORT_CAP_PHY);

		if (cap > 0)
			port->cap_phy = cap;
		else
			tb_port_WARN(port, "non switch port without a PHY\n");
	} else if (port->port != 0) {
		cap = tb_port_find_cap(port, TB_PORT_CAP_ADAP);
		if (cap > 0)
			port->cap_adap = cap;
	}

	tb_dump_port(port->sw->tb, &port->config);

	/* Control port does not need HopID allocation */
	if (port->port) {
		ida_init(&port->in_hopids);
		ida_init(&port->out_hopids);
	}

	return 0;

}

static int tb_port_alloc_hopid(struct tb_port *port, bool in, int min_hopid,
			       int max_hopid)
{
	int port_max_hopid;
	struct ida *ida;

	if (in) {
		port_max_hopid = port->config.max_in_hop_id;
		ida = &port->in_hopids;
	} else {
		port_max_hopid = port->config.max_out_hop_id;
		ida = &port->out_hopids;
	}

	/* HopIDs 0-7 are reserved */
	if (min_hopid < TB_PATH_MIN_HOPID)
		min_hopid = TB_PATH_MIN_HOPID;

	if (max_hopid < 0 || max_hopid > port_max_hopid)
		max_hopid = port_max_hopid;

	return ida_simple_get(ida, min_hopid, max_hopid + 1, GFP_KERNEL);
}

/**
 * tb_port_alloc_in_hopid() - Allocate input HopID from port
 * @port: Port to allocate HopID for
 * @min_hopid: Minimum acceptable input HopID
 * @max_hopid: Maximum acceptable input HopID
 *
 * Return: HopID between @min_hopid and @max_hopid or negative errno in
 * case of error.
 */
int tb_port_alloc_in_hopid(struct tb_port *port, int min_hopid, int max_hopid)
{
	return tb_port_alloc_hopid(port, true, min_hopid, max_hopid);
}

/**
 * tb_port_alloc_out_hopid() - Allocate output HopID from port
 * @port: Port to allocate HopID for
 * @min_hopid: Minimum acceptable output HopID
 * @max_hopid: Maximum acceptable output HopID
 *
 * Return: HopID between @min_hopid and @max_hopid or negative errno in
 * case of error.
 */
int tb_port_alloc_out_hopid(struct tb_port *port, int min_hopid, int max_hopid)
{
	return tb_port_alloc_hopid(port, false, min_hopid, max_hopid);
}

/**
 * tb_port_release_in_hopid() - Release allocated input HopID from port
 * @port: Port whose HopID to release
 * @hopid: HopID to release
 */
void tb_port_release_in_hopid(struct tb_port *port, int hopid)
{
	ida_simple_remove(&port->in_hopids, hopid);
}

/**
 * tb_port_release_out_hopid() - Release allocated output HopID from port
 * @port: Port whose HopID to release
 * @hopid: HopID to release
 */
void tb_port_release_out_hopid(struct tb_port *port, int hopid)
{
	ida_simple_remove(&port->out_hopids, hopid);
}

/**
 * tb_next_port_on_path() - Return next port for given port on a path
 * @start: Start port of the walk
 * @end: End port of the walk
 * @prev: Previous port (%NULL if this is the first)
 *
 * This function can be used to walk from one port to another if they
 * are connected through zero or more switches. If the @prev is dual
 * link port, the function follows that link and returns another end on
 * that same link.
 *
 * If the @end port has been reached, return %NULL.
 *
 * Domain tb->lock must be held when this function is called.
 */
struct tb_port *tb_next_port_on_path(struct tb_port *start, struct tb_port *end,
				     struct tb_port *prev)
{
	struct tb_port *next;

	if (!prev)
		return start;

	if (prev->sw == end->sw) {
		if (prev == end)
			return NULL;
		return end;
	}

	if (start->sw->config.depth < end->sw->config.depth) {
		if (prev->remote &&
		    prev->remote->sw->config.depth > prev->sw->config.depth)
			next = prev->remote;
		else
			next = tb_port_at(tb_route(end->sw), prev->sw);
	} else {
		if (tb_is_upstream_port(prev)) {
			next = prev->remote;
		} else {
			next = tb_upstream_port(prev->sw);
			/*
			 * Keep the same link if prev and next are both
			 * dual link ports.
			 */
			if (next->dual_link_port &&
			    next->link_nr != prev->link_nr) {
				next = next->dual_link_port;
			}
		}
	}

	return next;
}

/**
 * tb_port_is_enabled() - Is the adapter port enabled
 * @port: Port to check
 */
bool tb_port_is_enabled(struct tb_port *port)
{
	switch (port->config.type) {
	case TB_TYPE_PCIE_UP:
	case TB_TYPE_PCIE_DOWN:
		return tb_pci_port_is_enabled(port);

	case TB_TYPE_DP_HDMI_IN:
	case TB_TYPE_DP_HDMI_OUT:
		return tb_dp_port_is_enabled(port);

	default:
		return false;
	}
}

/**
 * tb_pci_port_is_enabled() - Is the PCIe adapter port enabled
 * @port: PCIe port to check
 */
bool tb_pci_port_is_enabled(struct tb_port *port)
{
	u32 data;

	if (tb_port_read(port, &data, TB_CFG_PORT, port->cap_adap, 1))
		return false;

	return !!(data & TB_PCI_EN);
}

/**
 * tb_pci_port_enable() - Enable PCIe adapter port
 * @port: PCIe port to enable
 * @enable: Enable/disable the PCIe adapter
 */
int tb_pci_port_enable(struct tb_port *port, bool enable)
{
	u32 word = enable ? TB_PCI_EN : 0x0;
	if (!port->cap_adap)
		return -ENXIO;
	return tb_port_write(port, &word, TB_CFG_PORT, port->cap_adap, 1);
}

/**
 * tb_dp_port_hpd_is_active() - Is HPD already active
 * @port: DP out port to check
 *
 * Checks if the DP OUT adapter port has HDP bit already set.
 */
int tb_dp_port_hpd_is_active(struct tb_port *port)
{
	u32 data;
	int ret;

	ret = tb_port_read(port, &data, TB_CFG_PORT, port->cap_adap + 2, 1);
	if (ret)
		return ret;

	return !!(data & TB_DP_HDP);
}

/**
 * tb_dp_port_hpd_clear() - Clear HPD from DP IN port
 * @port: Port to clear HPD
 *
 * If the DP IN port has HDP set, this function can be used to clear it.
 */
int tb_dp_port_hpd_clear(struct tb_port *port)
{
	u32 data;
	int ret;

	ret = tb_port_read(port, &data, TB_CFG_PORT, port->cap_adap + 3, 1);
	if (ret)
		return ret;

	data |= TB_DP_HPDC;
	return tb_port_write(port, &data, TB_CFG_PORT, port->cap_adap + 3, 1);
}

/**
 * tb_dp_port_set_hops() - Set video/aux Hop IDs for DP port
 * @port: DP IN/OUT port to set hops
 * @video: Video Hop ID
 * @aux_tx: AUX TX Hop ID
 * @aux_rx: AUX RX Hop ID
 *
 * Programs specified Hop IDs for DP IN/OUT port.
 */
int tb_dp_port_set_hops(struct tb_port *port, unsigned int video,
			unsigned int aux_tx, unsigned int aux_rx)
{
	u32 data[2];
	int ret;

	ret = tb_port_read(port, data, TB_CFG_PORT, port->cap_adap,
			   ARRAY_SIZE(data));
	if (ret)
		return ret;

	data[0] &= ~TB_DP_VIDEO_HOPID_MASK;
	data[1] &= ~(TB_DP_AUX_RX_HOPID_MASK | TB_DP_AUX_TX_HOPID_MASK);

	data[0] |= (video << TB_DP_VIDEO_HOPID_SHIFT) & TB_DP_VIDEO_HOPID_MASK;
	data[1] |= aux_tx & TB_DP_AUX_TX_HOPID_MASK;
	data[1] |= (aux_rx << TB_DP_AUX_RX_HOPID_SHIFT) & TB_DP_AUX_RX_HOPID_MASK;

	return tb_port_write(port, data, TB_CFG_PORT, port->cap_adap,
			     ARRAY_SIZE(data));
}

/**
 * tb_dp_port_is_enabled() - Is DP adapter port enabled
 * @port: DP adapter port to check
 */
bool tb_dp_port_is_enabled(struct tb_port *port)
{
	u32 data[2];

	if (tb_port_read(port, data, TB_CFG_PORT, port->cap_adap,
			 ARRAY_SIZE(data)))
		return false;

	return !!(data[0] & (TB_DP_VIDEO_EN | TB_DP_AUX_EN));
}

/**
 * tb_dp_port_enable() - Enables/disables DP paths of a port
 * @port: DP IN/OUT port
 * @enable: Enable/disable DP path
 *
 * Once Hop IDs are programmed DP paths can be enabled or disabled by
 * calling this function.
 */
int tb_dp_port_enable(struct tb_port *port, bool enable)
{
	u32 data[2];
	int ret;

	ret = tb_port_read(port, data, TB_CFG_PORT, port->cap_adap,
			   ARRAY_SIZE(data));
	if (ret)
		return ret;

	if (enable)
		data[0] |= TB_DP_VIDEO_EN | TB_DP_AUX_EN;
	else
		data[0] &= ~(TB_DP_VIDEO_EN | TB_DP_AUX_EN);

	return tb_port_write(port, data, TB_CFG_PORT, port->cap_adap,
			     ARRAY_SIZE(data));
}

/* switch utility functions */

static void tb_dump_switch(struct tb *tb, struct tb_regs_switch_header *sw)
{
	tb_dbg(tb, " Switch: %x:%x (Revision: %d, TB Version: %d)\n",
	       sw->vendor_id, sw->device_id, sw->revision,
	       sw->thunderbolt_version);
	tb_dbg(tb, "  Max Port Number: %d\n", sw->max_port_number);
	tb_dbg(tb, "  Config:\n");
	tb_dbg(tb,
		"   Upstream Port Number: %d Depth: %d Route String: %#llx Enabled: %d, PlugEventsDelay: %dms\n",
	       sw->upstream_port_number, sw->depth,
	       (((u64) sw->route_hi) << 32) | sw->route_lo,
	       sw->enabled, sw->plug_events_delay);
	tb_dbg(tb, "   unknown1: %#x unknown4: %#x\n",
	       sw->__unknown1, sw->__unknown4);
}

/**
 * reset_switch() - reconfigure route, enable and send TB_CFG_PKG_RESET
 *
 * Return: Returns 0 on success or an error code on failure.
 */
int tb_switch_reset(struct tb *tb, u64 route)
{
	struct tb_cfg_result res;
	struct tb_regs_switch_header header = {
		header.route_hi = route >> 32,
		header.route_lo = route,
		header.enabled = true,
	};
	tb_dbg(tb, "resetting switch at %llx\n", route);
	res.err = tb_cfg_write(tb->ctl, ((u32 *) &header) + 2, route,
			0, 2, 2, 2);
	if (res.err)
		return res.err;
	res = tb_cfg_reset(tb->ctl, route, TB_CFG_DEFAULT_TIMEOUT);
	if (res.err > 0)
		return -EIO;
	return res.err;
}

/**
 * tb_plug_events_active() - enable/disable plug events on a switch
 *
 * Also configures a sane plug_events_delay of 255ms.
 *
 * Return: Returns 0 on success or an error code on failure.
 */
static int tb_plug_events_active(struct tb_switch *sw, bool active)
{
	u32 data;
	int res;

	if (!sw->config.enabled)
		return 0;

	sw->config.plug_events_delay = 0xff;
	res = tb_sw_write(sw, ((u32 *) &sw->config) + 4, TB_CFG_SWITCH, 4, 1);
	if (res)
		return res;

	res = tb_sw_read(sw, &data, TB_CFG_SWITCH, sw->cap_plug_events + 1, 1);
	if (res)
		return res;

	if (active) {
		data = data & 0xFFFFFF83;
		switch (sw->config.device_id) {
		case PCI_DEVICE_ID_INTEL_LIGHT_RIDGE:
		case PCI_DEVICE_ID_INTEL_EAGLE_RIDGE:
		case PCI_DEVICE_ID_INTEL_PORT_RIDGE:
			break;
		default:
			data |= 4;
		}
	} else {
		data = data | 0x7c;
	}
	return tb_sw_write(sw, &data, TB_CFG_SWITCH,
			   sw->cap_plug_events + 1, 1);
}

static ssize_t authorized_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);

	return sprintf(buf, "%u\n", sw->authorized);
}

static int tb_switch_set_authorized(struct tb_switch *sw, unsigned int val)
{
	int ret = -EINVAL;

	if (!mutex_trylock(&sw->tb->lock))
		return restart_syscall();

	if (sw->authorized)
		goto unlock;

	switch (val) {
	/* Approve switch */
	case 1:
		if (sw->key)
			ret = tb_domain_approve_switch_key(sw->tb, sw);
		else
			ret = tb_domain_approve_switch(sw->tb, sw);
		break;

	/* Challenge switch */
	case 2:
		if (sw->key)
			ret = tb_domain_challenge_switch_key(sw->tb, sw);
		break;

	default:
		break;
	}

	if (!ret) {
		sw->authorized = val;
		/* Notify status change to the userspace */
		kobject_uevent(&sw->dev.kobj, KOBJ_CHANGE);
	}

unlock:
	mutex_unlock(&sw->tb->lock);
	return ret;
}

static ssize_t authorized_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct tb_switch *sw = tb_to_switch(dev);
	unsigned int val;
	ssize_t ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 2)
		return -EINVAL;

	pm_runtime_get_sync(&sw->dev);
	ret = tb_switch_set_authorized(sw, val);
	pm_runtime_mark_last_busy(&sw->dev);
	pm_runtime_put_autosuspend(&sw->dev);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(authorized);

static ssize_t boot_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);

	return sprintf(buf, "%u\n", sw->boot);
}
static DEVICE_ATTR_RO(boot);

static ssize_t device_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);

	return sprintf(buf, "%#x\n", sw->device);
}
static DEVICE_ATTR_RO(device);

static ssize_t
device_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);

	return sprintf(buf, "%s\n", sw->device_name ? sw->device_name : "");
}
static DEVICE_ATTR_RO(device_name);

static ssize_t key_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);
	ssize_t ret;

	if (!mutex_trylock(&sw->tb->lock))
		return restart_syscall();

	if (sw->key)
		ret = sprintf(buf, "%*phN\n", TB_SWITCH_KEY_SIZE, sw->key);
	else
		ret = sprintf(buf, "\n");

	mutex_unlock(&sw->tb->lock);
	return ret;
}

static ssize_t key_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct tb_switch *sw = tb_to_switch(dev);
	u8 key[TB_SWITCH_KEY_SIZE];
	ssize_t ret = count;
	bool clear = false;

	if (!strcmp(buf, "\n"))
		clear = true;
	else if (hex2bin(key, buf, sizeof(key)))
		return -EINVAL;

	if (!mutex_trylock(&sw->tb->lock))
		return restart_syscall();

	if (sw->authorized) {
		ret = -EBUSY;
	} else {
		kfree(sw->key);
		if (clear) {
			sw->key = NULL;
		} else {
			sw->key = kmemdup(key, sizeof(key), GFP_KERNEL);
			if (!sw->key)
				ret = -ENOMEM;
		}
	}

	mutex_unlock(&sw->tb->lock);
	return ret;
}
static DEVICE_ATTR(key, 0600, key_show, key_store);

static void nvm_authenticate_start(struct tb_switch *sw)
{
	struct pci_dev *root_port;

	/*
	 * During host router NVM upgrade we should not allow root port to
	 * go into D3cold because some root ports cannot trigger PME
	 * itself. To be on the safe side keep the root port in D0 during
	 * the whole upgrade process.
	 */
	root_port = pci_find_pcie_root_port(sw->tb->nhi->pdev);
	if (root_port)
		pm_runtime_get_noresume(&root_port->dev);
}

static void nvm_authenticate_complete(struct tb_switch *sw)
{
	struct pci_dev *root_port;

	root_port = pci_find_pcie_root_port(sw->tb->nhi->pdev);
	if (root_port)
		pm_runtime_put(&root_port->dev);
}

static ssize_t nvm_authenticate_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);
	u32 status;

	nvm_get_auth_status(sw, &status);
	return sprintf(buf, "%#x\n", status);
}

static ssize_t nvm_authenticate_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct tb_switch *sw = tb_to_switch(dev);
	bool val;
	int ret;

	pm_runtime_get_sync(&sw->dev);

	if (!mutex_trylock(&sw->tb->lock)) {
		ret = restart_syscall();
		goto exit_rpm;
	}

	/* If NVMem devices are not yet added */
	if (!sw->nvm) {
		ret = -EAGAIN;
		goto exit_unlock;
	}

	ret = kstrtobool(buf, &val);
	if (ret)
		goto exit_unlock;

	/* Always clear the authentication status */
	nvm_clear_auth_status(sw);

	if (val) {
		if (!sw->nvm->buf) {
			ret = -EINVAL;
			goto exit_unlock;
		}

		ret = nvm_validate_and_write(sw);
		if (ret)
			goto exit_unlock;

		sw->nvm->authenticating = true;

		if (!tb_route(sw)) {
			/*
			 * Keep root port from suspending as long as the
			 * NVM upgrade process is running.
			 */
			nvm_authenticate_start(sw);
			ret = nvm_authenticate_host(sw);
		} else {
			ret = nvm_authenticate_device(sw);
		}
	}

exit_unlock:
	mutex_unlock(&sw->tb->lock);
exit_rpm:
	pm_runtime_mark_last_busy(&sw->dev);
	pm_runtime_put_autosuspend(&sw->dev);

	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_RW(nvm_authenticate);

static ssize_t nvm_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);
	int ret;

	if (!mutex_trylock(&sw->tb->lock))
		return restart_syscall();

	if (sw->safe_mode)
		ret = -ENODATA;
	else if (!sw->nvm)
		ret = -EAGAIN;
	else
		ret = sprintf(buf, "%x.%x\n", sw->nvm->major, sw->nvm->minor);

	mutex_unlock(&sw->tb->lock);

	return ret;
}
static DEVICE_ATTR_RO(nvm_version);

static ssize_t vendor_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);

	return sprintf(buf, "%#x\n", sw->vendor);
}
static DEVICE_ATTR_RO(vendor);

static ssize_t
vendor_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);

	return sprintf(buf, "%s\n", sw->vendor_name ? sw->vendor_name : "");
}
static DEVICE_ATTR_RO(vendor_name);

static ssize_t unique_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct tb_switch *sw = tb_to_switch(dev);

	return sprintf(buf, "%pUb\n", sw->uuid);
}
static DEVICE_ATTR_RO(unique_id);

static struct attribute *switch_attrs[] = {
	&dev_attr_authorized.attr,
	&dev_attr_boot.attr,
	&dev_attr_device.attr,
	&dev_attr_device_name.attr,
	&dev_attr_key.attr,
	&dev_attr_nvm_authenticate.attr,
	&dev_attr_nvm_version.attr,
	&dev_attr_vendor.attr,
	&dev_attr_vendor_name.attr,
	&dev_attr_unique_id.attr,
	NULL,
};

static umode_t switch_attr_is_visible(struct kobject *kobj,
				      struct attribute *attr, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct tb_switch *sw = tb_to_switch(dev);

	if (attr == &dev_attr_device.attr) {
		if (!sw->device)
			return 0;
	} else if (attr == &dev_attr_device_name.attr) {
		if (!sw->device_name)
			return 0;
	} else if (attr == &dev_attr_vendor.attr)  {
		if (!sw->vendor)
			return 0;
	} else if (attr == &dev_attr_vendor_name.attr)  {
		if (!sw->vendor_name)
			return 0;
	} else if (attr == &dev_attr_key.attr) {
		if (tb_route(sw) &&
		    sw->tb->security_level == TB_SECURITY_SECURE &&
		    sw->security_level == TB_SECURITY_SECURE)
			return attr->mode;
		return 0;
	} else if (attr == &dev_attr_nvm_authenticate.attr) {
		if (sw->dma_port && !sw->no_nvm_upgrade)
			return attr->mode;
		return 0;
	} else if (attr == &dev_attr_nvm_version.attr) {
		if (sw->dma_port)
			return attr->mode;
		return 0;
	} else if (attr == &dev_attr_boot.attr) {
		if (tb_route(sw))
			return attr->mode;
		return 0;
	}

	return sw->safe_mode ? 0 : attr->mode;
}

static struct attribute_group switch_group = {
	.is_visible = switch_attr_is_visible,
	.attrs = switch_attrs,
};

static const struct attribute_group *switch_groups[] = {
	&switch_group,
	NULL,
};

static void tb_switch_release(struct device *dev)
{
	struct tb_switch *sw = tb_to_switch(dev);
	int i;

	dma_port_free(sw->dma_port);

	for (i = 1; i <= sw->config.max_port_number; i++) {
		if (!sw->ports[i].disabled) {
			ida_destroy(&sw->ports[i].in_hopids);
			ida_destroy(&sw->ports[i].out_hopids);
		}
	}

	kfree(sw->uuid);
	kfree(sw->device_name);
	kfree(sw->vendor_name);
	kfree(sw->ports);
	kfree(sw->drom);
	kfree(sw->key);
	kfree(sw);
}

/*
 * Currently only need to provide the callbacks. Everything else is handled
 * in the connection manager.
 */
static int __maybe_unused tb_switch_runtime_suspend(struct device *dev)
{
	struct tb_switch *sw = tb_to_switch(dev);
	const struct tb_cm_ops *cm_ops = sw->tb->cm_ops;

	if (cm_ops->runtime_suspend_switch)
		return cm_ops->runtime_suspend_switch(sw);

	return 0;
}

static int __maybe_unused tb_switch_runtime_resume(struct device *dev)
{
	struct tb_switch *sw = tb_to_switch(dev);
	const struct tb_cm_ops *cm_ops = sw->tb->cm_ops;

	if (cm_ops->runtime_resume_switch)
		return cm_ops->runtime_resume_switch(sw);
	return 0;
}

static const struct dev_pm_ops tb_switch_pm_ops = {
	SET_RUNTIME_PM_OPS(tb_switch_runtime_suspend, tb_switch_runtime_resume,
			   NULL)
};

struct device_type tb_switch_type = {
	.name = "thunderbolt_device",
	.release = tb_switch_release,
	.pm = &tb_switch_pm_ops,
};

static int tb_switch_get_generation(struct tb_switch *sw)
{
	switch (sw->config.device_id) {
	case PCI_DEVICE_ID_INTEL_LIGHT_RIDGE:
	case PCI_DEVICE_ID_INTEL_EAGLE_RIDGE:
	case PCI_DEVICE_ID_INTEL_LIGHT_PEAK:
	case PCI_DEVICE_ID_INTEL_CACTUS_RIDGE_2C:
	case PCI_DEVICE_ID_INTEL_CACTUS_RIDGE_4C:
	case PCI_DEVICE_ID_INTEL_PORT_RIDGE:
	case PCI_DEVICE_ID_INTEL_REDWOOD_RIDGE_2C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_REDWOOD_RIDGE_4C_BRIDGE:
		return 1;

	case PCI_DEVICE_ID_INTEL_WIN_RIDGE_2C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_FALCON_RIDGE_2C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_FALCON_RIDGE_4C_BRIDGE:
		return 2;

	case PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_LP_BRIDGE:
	case PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_2C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_4C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_C_2C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_C_4C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_TITAN_RIDGE_2C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_TITAN_RIDGE_4C_BRIDGE:
	case PCI_DEVICE_ID_INTEL_TITAN_RIDGE_DD_BRIDGE:
	case PCI_DEVICE_ID_INTEL_ICL_NHI0:
	case PCI_DEVICE_ID_INTEL_ICL_NHI1:
		return 3;

	default:
		/*
		 * For unknown switches assume generation to be 1 to be
		 * on the safe side.
		 */
		tb_sw_warn(sw, "unsupported switch device id %#x\n",
			   sw->config.device_id);
		return 1;
	}
}

/**
 * tb_switch_alloc() - allocate a switch
 * @tb: Pointer to the owning domain
 * @parent: Parent device for this switch
 * @route: Route string for this switch
 *
 * Allocates and initializes a switch. Will not upload configuration to
 * the switch. For that you need to call tb_switch_configure()
 * separately. The returned switch should be released by calling
 * tb_switch_put().
 *
 * Return: Pointer to the allocated switch or ERR_PTR() in case of
 * failure.
 */
struct tb_switch *tb_switch_alloc(struct tb *tb, struct device *parent,
				  u64 route)
{
	struct tb_switch *sw;
	int upstream_port;
	int i, ret, depth;

	/* Make sure we do not exceed maximum topology limit */
	depth = tb_route_length(route);
	if (depth > TB_SWITCH_MAX_DEPTH)
		return ERR_PTR(-EADDRNOTAVAIL);

	upstream_port = tb_cfg_get_upstream_port(tb->ctl, route);
	if (upstream_port < 0)
		return ERR_PTR(upstream_port);

	sw = kzalloc(sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return ERR_PTR(-ENOMEM);

	sw->tb = tb;
	ret = tb_cfg_read(tb->ctl, &sw->config, route, 0, TB_CFG_SWITCH, 0, 5);
	if (ret)
		goto err_free_sw_ports;

	tb_dbg(tb, "current switch config:\n");
	tb_dump_switch(tb, &sw->config);

	/* configure switch */
	sw->config.upstream_port_number = upstream_port;
	sw->config.depth = depth;
	sw->config.route_hi = upper_32_bits(route);
	sw->config.route_lo = lower_32_bits(route);
	sw->config.enabled = 0;

	/* initialize ports */
	sw->ports = kcalloc(sw->config.max_port_number + 1, sizeof(*sw->ports),
				GFP_KERNEL);
	if (!sw->ports) {
		ret = -ENOMEM;
		goto err_free_sw_ports;
	}

	for (i = 0; i <= sw->config.max_port_number; i++) {
		/* minimum setup for tb_find_cap and tb_drom_read to work */
		sw->ports[i].sw = sw;
		sw->ports[i].port = i;
	}

	sw->generation = tb_switch_get_generation(sw);

	ret = tb_switch_find_vse_cap(sw, TB_VSE_CAP_PLUG_EVENTS);
	if (ret < 0) {
		tb_sw_warn(sw, "cannot find TB_VSE_CAP_PLUG_EVENTS aborting\n");
		goto err_free_sw_ports;
	}
	sw->cap_plug_events = ret;

	ret = tb_switch_find_vse_cap(sw, TB_VSE_CAP_LINK_CONTROLLER);
	if (ret > 0)
		sw->cap_lc = ret;

	/* Root switch is always authorized */
	if (!route)
		sw->authorized = true;

	device_initialize(&sw->dev);
	sw->dev.parent = parent;
	sw->dev.bus = &tb_bus_type;
	sw->dev.type = &tb_switch_type;
	sw->dev.groups = switch_groups;
	dev_set_name(&sw->dev, "%u-%llx", tb->index, tb_route(sw));

	return sw;

err_free_sw_ports:
	kfree(sw->ports);
	kfree(sw);

	return ERR_PTR(ret);
}

/**
 * tb_switch_alloc_safe_mode() - allocate a switch that is in safe mode
 * @tb: Pointer to the owning domain
 * @parent: Parent device for this switch
 * @route: Route string for this switch
 *
 * This creates a switch in safe mode. This means the switch pretty much
 * lacks all capabilities except DMA configuration port before it is
 * flashed with a valid NVM firmware.
 *
 * The returned switch must be released by calling tb_switch_put().
 *
 * Return: Pointer to the allocated switch or ERR_PTR() in case of failure
 */
struct tb_switch *
tb_switch_alloc_safe_mode(struct tb *tb, struct device *parent, u64 route)
{
	struct tb_switch *sw;

	sw = kzalloc(sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return ERR_PTR(-ENOMEM);

	sw->tb = tb;
	sw->config.depth = tb_route_length(route);
	sw->config.route_hi = upper_32_bits(route);
	sw->config.route_lo = lower_32_bits(route);
	sw->safe_mode = true;

	device_initialize(&sw->dev);
	sw->dev.parent = parent;
	sw->dev.bus = &tb_bus_type;
	sw->dev.type = &tb_switch_type;
	sw->dev.groups = switch_groups;
	dev_set_name(&sw->dev, "%u-%llx", tb->index, tb_route(sw));

	return sw;
}

/**
 * tb_switch_configure() - Uploads configuration to the switch
 * @sw: Switch to configure
 *
 * Call this function before the switch is added to the system. It will
 * upload configuration to the switch and makes it available for the
 * connection manager to use.
 *
 * Return: %0 in case of success and negative errno in case of failure
 */
int tb_switch_configure(struct tb_switch *sw)
{
	struct tb *tb = sw->tb;
	u64 route;
	int ret;

	route = tb_route(sw);
	tb_dbg(tb, "initializing Switch at %#llx (depth: %d, up port: %d)\n",
	       route, tb_route_length(route), sw->config.upstream_port_number);

	if (sw->config.vendor_id != PCI_VENDOR_ID_INTEL)
		tb_sw_warn(sw, "unknown switch vendor id %#x\n",
			   sw->config.vendor_id);

	sw->config.enabled = 1;

	/* upload configuration */
	ret = tb_sw_write(sw, 1 + (u32 *)&sw->config, TB_CFG_SWITCH, 1, 3);
	if (ret)
		return ret;

	ret = tb_lc_configure_link(sw);
	if (ret)
		return ret;

	return tb_plug_events_active(sw, true);
}

static int tb_switch_set_uuid(struct tb_switch *sw)
{
	u32 uuid[4];
	int ret;

	if (sw->uuid)
		return 0;

	/*
	 * The newer controllers include fused UUID as part of link
	 * controller specific registers
	 */
	ret = tb_lc_read_uuid(sw, uuid);
	if (ret) {
		/*
		 * ICM generates UUID based on UID and fills the upper
		 * two words with ones. This is not strictly following
		 * UUID format but we want to be compatible with it so
		 * we do the same here.
		 */
		uuid[0] = sw->uid & 0xffffffff;
		uuid[1] = (sw->uid >> 32) & 0xffffffff;
		uuid[2] = 0xffffffff;
		uuid[3] = 0xffffffff;
	}

	sw->uuid = kmemdup(uuid, sizeof(uuid), GFP_KERNEL);
	if (!sw->uuid)
		return -ENOMEM;
	return 0;
}

static int tb_switch_add_dma_port(struct tb_switch *sw)
{
	u32 status;
	int ret;

	switch (sw->generation) {
	case 2:
		/* Only root switch can be upgraded */
		if (tb_route(sw))
			return 0;

		/* fallthrough */
	case 3:
		ret = tb_switch_set_uuid(sw);
		if (ret)
			return ret;
		break;

	default:
		/*
		 * DMA port is the only thing available when the switch
		 * is in safe mode.
		 */
		if (!sw->safe_mode)
			return 0;
		break;
	}

	/* Root switch DMA port requires running firmware */
	if (!tb_route(sw) && sw->config.enabled)
		return 0;

	sw->dma_port = dma_port_alloc(sw);
	if (!sw->dma_port)
		return 0;

	if (sw->no_nvm_upgrade)
		return 0;

	/*
	 * If there is status already set then authentication failed
	 * when the dma_port_flash_update_auth() returned. Power cycling
	 * is not needed (it was done already) so only thing we do here
	 * is to unblock runtime PM of the root port.
	 */
	nvm_get_auth_status(sw, &status);
	if (status) {
		if (!tb_route(sw))
			nvm_authenticate_complete(sw);
		return 0;
	}

	/*
	 * Check status of the previous flash authentication. If there
	 * is one we need to power cycle the switch in any case to make
	 * it functional again.
	 */
	ret = dma_port_flash_update_auth_status(sw->dma_port, &status);
	if (ret <= 0)
		return ret;

	/* Now we can allow root port to suspend again */
	if (!tb_route(sw))
		nvm_authenticate_complete(sw);

	if (status) {
		tb_sw_info(sw, "switch flash authentication failed\n");
		nvm_set_auth_status(sw, status);
	}

	tb_sw_info(sw, "power cycling the switch now\n");
	dma_port_power_cycle(sw->dma_port);

	/*
	 * We return error here which causes the switch adding failure.
	 * It should appear back after power cycle is complete.
	 */
	return -ESHUTDOWN;
}

/**
 * tb_switch_add() - Add a switch to the domain
 * @sw: Switch to add
 *
 * This is the last step in adding switch to the domain. It will read
 * identification information from DROM and initializes ports so that
 * they can be used to connect other switches. The switch will be
 * exposed to the userspace when this function successfully returns. To
 * remove and release the switch, call tb_switch_remove().
 *
 * Return: %0 in case of success and negative errno in case of failure
 */
int tb_switch_add(struct tb_switch *sw)
{
	int i, ret;

	/*
	 * Initialize DMA control port now before we read DROM. Recent
	 * host controllers have more complete DROM on NVM that includes
	 * vendor and model identification strings which we then expose
	 * to the userspace. NVM can be accessed through DMA
	 * configuration based mailbox.
	 */
	ret = tb_switch_add_dma_port(sw);
	if (ret)
		return ret;

	if (!sw->safe_mode) {
		/* read drom */
		ret = tb_drom_read(sw);
		if (ret) {
			tb_sw_warn(sw, "tb_eeprom_read_rom failed\n");
			return ret;
		}
		tb_sw_dbg(sw, "uid: %#llx\n", sw->uid);

		ret = tb_switch_set_uuid(sw);
		if (ret)
			return ret;

		for (i = 0; i <= sw->config.max_port_number; i++) {
			if (sw->ports[i].disabled) {
				tb_port_dbg(&sw->ports[i], "disabled by eeprom\n");
				continue;
			}
			ret = tb_init_port(&sw->ports[i]);
			if (ret)
				return ret;
		}
	}

	ret = device_add(&sw->dev);
	if (ret)
		return ret;

	if (tb_route(sw)) {
		dev_info(&sw->dev, "new device found, vendor=%#x device=%#x\n",
			 sw->vendor, sw->device);
		if (sw->vendor_name && sw->device_name)
			dev_info(&sw->dev, "%s %s\n", sw->vendor_name,
				 sw->device_name);
	}

	ret = tb_switch_nvm_add(sw);
	if (ret) {
		device_del(&sw->dev);
		return ret;
	}

	pm_runtime_set_active(&sw->dev);
	if (sw->rpm) {
		pm_runtime_set_autosuspend_delay(&sw->dev, TB_AUTOSUSPEND_DELAY);
		pm_runtime_use_autosuspend(&sw->dev);
		pm_runtime_mark_last_busy(&sw->dev);
		pm_runtime_enable(&sw->dev);
		pm_request_autosuspend(&sw->dev);
	}

	return 0;
}

/**
 * tb_switch_remove() - Remove and release a switch
 * @sw: Switch to remove
 *
 * This will remove the switch from the domain and release it after last
 * reference count drops to zero. If there are switches connected below
 * this switch, they will be removed as well.
 */
void tb_switch_remove(struct tb_switch *sw)
{
	int i;

	if (sw->rpm) {
		pm_runtime_get_sync(&sw->dev);
		pm_runtime_disable(&sw->dev);
	}

	/* port 0 is the switch itself and never has a remote */
	for (i = 1; i <= sw->config.max_port_number; i++) {
		if (tb_port_has_remote(&sw->ports[i])) {
			tb_switch_remove(sw->ports[i].remote->sw);
			sw->ports[i].remote = NULL;
		} else if (sw->ports[i].xdomain) {
			tb_xdomain_remove(sw->ports[i].xdomain);
			sw->ports[i].xdomain = NULL;
		}
	}

	if (!sw->is_unplugged)
		tb_plug_events_active(sw, false);
	tb_lc_unconfigure_link(sw);

	tb_switch_nvm_remove(sw);

	if (tb_route(sw))
		dev_info(&sw->dev, "device disconnected\n");
	device_unregister(&sw->dev);
}

/**
 * tb_sw_set_unplugged() - set is_unplugged on switch and downstream switches
 */
void tb_sw_set_unplugged(struct tb_switch *sw)
{
	int i;
	if (sw == sw->tb->root_switch) {
		tb_sw_WARN(sw, "cannot unplug root switch\n");
		return;
	}
	if (sw->is_unplugged) {
		tb_sw_WARN(sw, "is_unplugged already set\n");
		return;
	}
	sw->is_unplugged = true;
	for (i = 0; i <= sw->config.max_port_number; i++) {
		if (tb_port_has_remote(&sw->ports[i]))
			tb_sw_set_unplugged(sw->ports[i].remote->sw);
		else if (sw->ports[i].xdomain)
			sw->ports[i].xdomain->is_unplugged = true;
	}
}

int tb_switch_resume(struct tb_switch *sw)
{
	int i, err;
	tb_sw_dbg(sw, "resuming switch\n");

	/*
	 * Check for UID of the connected switches except for root
	 * switch which we assume cannot be removed.
	 */
	if (tb_route(sw)) {
		u64 uid;

		/*
		 * Check first that we can still read the switch config
		 * space. It may be that there is now another domain
		 * connected.
		 */
		err = tb_cfg_get_upstream_port(sw->tb->ctl, tb_route(sw));
		if (err < 0) {
			tb_sw_info(sw, "switch not present anymore\n");
			return err;
		}

		err = tb_drom_read_uid_only(sw, &uid);
		if (err) {
			tb_sw_warn(sw, "uid read failed\n");
			return err;
		}
		if (sw->uid != uid) {
			tb_sw_info(sw,
				"changed while suspended (uid %#llx -> %#llx)\n",
				sw->uid, uid);
			return -ENODEV;
		}
	}

	/* upload configuration */
	err = tb_sw_write(sw, 1 + (u32 *) &sw->config, TB_CFG_SWITCH, 1, 3);
	if (err)
		return err;

	err = tb_lc_configure_link(sw);
	if (err)
		return err;

	err = tb_plug_events_active(sw, true);
	if (err)
		return err;

	/* check for surviving downstream switches */
	for (i = 1; i <= sw->config.max_port_number; i++) {
		struct tb_port *port = &sw->ports[i];

		if (!tb_port_has_remote(port) && !port->xdomain)
			continue;

		if (tb_wait_for_port(port, true) <= 0) {
			tb_port_warn(port,
				     "lost during suspend, disconnecting\n");
			if (tb_port_has_remote(port))
				tb_sw_set_unplugged(port->remote->sw);
			else if (port->xdomain)
				port->xdomain->is_unplugged = true;
		} else if (tb_port_has_remote(port)) {
			if (tb_switch_resume(port->remote->sw)) {
				tb_port_warn(port,
					     "lost during suspend, disconnecting\n");
				tb_sw_set_unplugged(port->remote->sw);
			}
		}
	}
	return 0;
}

void tb_switch_suspend(struct tb_switch *sw)
{
	int i, err;
	err = tb_plug_events_active(sw, false);
	if (err)
		return;

	for (i = 1; i <= sw->config.max_port_number; i++) {
		if (tb_port_has_remote(&sw->ports[i]))
			tb_switch_suspend(sw->ports[i].remote->sw);
	}

	tb_lc_set_sleep(sw);
}

struct tb_sw_lookup {
	struct tb *tb;
	u8 link;
	u8 depth;
	const uuid_t *uuid;
	u64 route;
};

static int tb_switch_match(struct device *dev, const void *data)
{
	struct tb_switch *sw = tb_to_switch(dev);
	const struct tb_sw_lookup *lookup = data;

	if (!sw)
		return 0;
	if (sw->tb != lookup->tb)
		return 0;

	if (lookup->uuid)
		return !memcmp(sw->uuid, lookup->uuid, sizeof(*lookup->uuid));

	if (lookup->route) {
		return sw->config.route_lo == lower_32_bits(lookup->route) &&
		       sw->config.route_hi == upper_32_bits(lookup->route);
	}

	/* Root switch is matched only by depth */
	if (!lookup->depth)
		return !sw->depth;

	return sw->link == lookup->link && sw->depth == lookup->depth;
}

/**
 * tb_switch_find_by_link_depth() - Find switch by link and depth
 * @tb: Domain the switch belongs
 * @link: Link number the switch is connected
 * @depth: Depth of the switch in link
 *
 * Returned switch has reference count increased so the caller needs to
 * call tb_switch_put() when done with the switch.
 */
struct tb_switch *tb_switch_find_by_link_depth(struct tb *tb, u8 link, u8 depth)
{
	struct tb_sw_lookup lookup;
	struct device *dev;

	memset(&lookup, 0, sizeof(lookup));
	lookup.tb = tb;
	lookup.link = link;
	lookup.depth = depth;

	dev = bus_find_device(&tb_bus_type, NULL, &lookup, tb_switch_match);
	if (dev)
		return tb_to_switch(dev);

	return NULL;
}

/**
 * tb_switch_find_by_uuid() - Find switch by UUID
 * @tb: Domain the switch belongs
 * @uuid: UUID to look for
 *
 * Returned switch has reference count increased so the caller needs to
 * call tb_switch_put() when done with the switch.
 */
struct tb_switch *tb_switch_find_by_uuid(struct tb *tb, const uuid_t *uuid)
{
	struct tb_sw_lookup lookup;
	struct device *dev;

	memset(&lookup, 0, sizeof(lookup));
	lookup.tb = tb;
	lookup.uuid = uuid;

	dev = bus_find_device(&tb_bus_type, NULL, &lookup, tb_switch_match);
	if (dev)
		return tb_to_switch(dev);

	return NULL;
}

/**
 * tb_switch_find_by_route() - Find switch by route string
 * @tb: Domain the switch belongs
 * @route: Route string to look for
 *
 * Returned switch has reference count increased so the caller needs to
 * call tb_switch_put() when done with the switch.
 */
struct tb_switch *tb_switch_find_by_route(struct tb *tb, u64 route)
{
	struct tb_sw_lookup lookup;
	struct device *dev;

	if (!route)
		return tb_switch_get(tb->root_switch);

	memset(&lookup, 0, sizeof(lookup));
	lookup.tb = tb;
	lookup.route = route;

	dev = bus_find_device(&tb_bus_type, NULL, &lookup, tb_switch_match);
	if (dev)
		return tb_to_switch(dev);

	return NULL;
}

void tb_switch_exit(void)
{
	ida_destroy(&nvm_ida);
}
