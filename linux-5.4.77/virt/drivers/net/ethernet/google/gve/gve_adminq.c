// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2019 Google, Inc.
 */

#include <linux/etherdevice.h>
#include <linux/pci.h>
#include "gve.h"
#include "gve_adminq.h"
#include "gve_register.h"

#define GVE_MAX_ADMINQ_RELEASE_CHECK	500
#define GVE_ADMINQ_SLEEP_LEN		20
#define GVE_MAX_ADMINQ_EVENT_COUNTER_CHECK	100

int gve_adminq_alloc(struct device *dev, struct gve_priv *priv)
{
	priv->adminq = dma_alloc_coherent(dev, PAGE_SIZE,
					  &priv->adminq_bus_addr, GFP_KERNEL);
	if (unlikely(!priv->adminq))
		return -ENOMEM;

	priv->adminq_mask = (PAGE_SIZE / sizeof(union gve_adminq_command)) - 1;
	priv->adminq_prod_cnt = 0;

	/* Setup Admin queue with the device */
	iowrite32be(priv->adminq_bus_addr / PAGE_SIZE,
		    &priv->reg_bar0->adminq_pfn);

	gve_set_admin_queue_ok(priv);
	return 0;
}

void gve_adminq_release(struct gve_priv *priv)
{
	int i = 0;

	/* Tell the device the adminq is leaving */
	iowrite32be(0x0, &priv->reg_bar0->adminq_pfn);
	while (ioread32be(&priv->reg_bar0->adminq_pfn)) {
		/* If this is reached the device is unrecoverable and still
		 * holding memory. Continue looping to avoid memory corruption,
		 * but WARN so it is visible what is going on.
		 */
		if (i == GVE_MAX_ADMINQ_RELEASE_CHECK)
			WARN(1, "Unrecoverable platform error!");
		i++;
		msleep(GVE_ADMINQ_SLEEP_LEN);
	}
	gve_clear_device_rings_ok(priv);
	gve_clear_device_resources_ok(priv);
	gve_clear_admin_queue_ok(priv);
}

void gve_adminq_free(struct device *dev, struct gve_priv *priv)
{
	if (!gve_get_admin_queue_ok(priv))
		return;
	gve_adminq_release(priv);
	dma_free_coherent(dev, PAGE_SIZE, priv->adminq, priv->adminq_bus_addr);
	gve_clear_admin_queue_ok(priv);
}

static void gve_adminq_kick_cmd(struct gve_priv *priv, u32 prod_cnt)
{
	iowrite32be(prod_cnt, &priv->reg_bar0->adminq_doorbell);
}

static bool gve_adminq_wait_for_cmd(struct gve_priv *priv, u32 prod_cnt)
{
	int i;

	for (i = 0; i < GVE_MAX_ADMINQ_EVENT_COUNTER_CHECK; i++) {
		if (ioread32be(&priv->reg_bar0->adminq_event_counter)
		    == prod_cnt)
			return true;
		msleep(GVE_ADMINQ_SLEEP_LEN);
	}

	return false;
}

static int gve_adminq_parse_err(struct device *dev, u32 status)
{
	if (status != GVE_ADMINQ_COMMAND_PASSED &&
	    status != GVE_ADMINQ_COMMAND_UNSET)
		dev_err(dev, "AQ command failed with status %d\n", status);

	switch (status) {
	case GVE_ADMINQ_COMMAND_PASSED:
		return 0;
	case GVE_ADMINQ_COMMAND_UNSET:
		dev_err(dev, "parse_aq_err: err and status both unset, this should not be possible.\n");
		return -EINVAL;
	case GVE_ADMINQ_COMMAND_ERROR_ABORTED:
	case GVE_ADMINQ_COMMAND_ERROR_CANCELLED:
	case GVE_ADMINQ_COMMAND_ERROR_DATALOSS:
	case GVE_ADMINQ_COMMAND_ERROR_FAILED_PRECONDITION:
	case GVE_ADMINQ_COMMAND_ERROR_UNAVAILABLE:
		return -EAGAIN;
	case GVE_ADMINQ_COMMAND_ERROR_ALREADY_EXISTS:
	case GVE_ADMINQ_COMMAND_ERROR_INTERNAL_ERROR:
	case GVE_ADMINQ_COMMAND_ERROR_INVALID_ARGUMENT:
	case GVE_ADMINQ_COMMAND_ERROR_NOT_FOUND:
	case GVE_ADMINQ_COMMAND_ERROR_OUT_OF_RANGE:
	case GVE_ADMINQ_COMMAND_ERROR_UNKNOWN_ERROR:
		return -EINVAL;
	case GVE_ADMINQ_COMMAND_ERROR_DEADLINE_EXCEEDED:
		return -ETIME;
	case GVE_ADMINQ_COMMAND_ERROR_PERMISSION_DENIED:
	case GVE_ADMINQ_COMMAND_ERROR_UNAUTHENTICATED:
		return -EACCES;
	case GVE_ADMINQ_COMMAND_ERROR_RESOURCE_EXHAUSTED:
		return -ENOMEM;
	case GVE_ADMINQ_COMMAND_ERROR_UNIMPLEMENTED:
		return -ENOTSUPP;
	default:
		dev_err(dev, "parse_aq_err: unknown status code %d\n", status);
		return -EINVAL;
	}
}

/* This function is not threadsafe - the caller is responsible for any
 * necessary locks.
 */
int gve_adminq_execute_cmd(struct gve_priv *priv,
			   union gve_adminq_command *cmd_orig)
{
	union gve_adminq_command *cmd;
	u32 status = 0;
	u32 prod_cnt;

	cmd = &priv->adminq[priv->adminq_prod_cnt & priv->adminq_mask];
	priv->adminq_prod_cnt++;
	prod_cnt = priv->adminq_prod_cnt;

	memcpy(cmd, cmd_orig, sizeof(*cmd_orig));

	gve_adminq_kick_cmd(priv, prod_cnt);
	if (!gve_adminq_wait_for_cmd(priv, prod_cnt)) {
		dev_err(&priv->pdev->dev, "AQ command timed out, need to reset AQ\n");
		return -ENOTRECOVERABLE;
	}

	memcpy(cmd_orig, cmd, sizeof(*cmd));
	status = be32_to_cpu(READ_ONCE(cmd->status));
	return gve_adminq_parse_err(&priv->pdev->dev, status);
}

/* The device specifies that the management vector can either be the first irq
 * or the last irq. ntfy_blk_msix_base_idx indicates the first irq assigned to
 * the ntfy blks. It if is 0 then the management vector is last, if it is 1 then
 * the management vector is first.
 *
 * gve arranges the msix vectors so that the management vector is last.
 */
#define GVE_NTFY_BLK_BASE_MSIX_IDX	0
int gve_adminq_configure_device_resources(struct gve_priv *priv,
					  dma_addr_t counter_array_bus_addr,
					  u32 num_counters,
					  dma_addr_t db_array_bus_addr,
					  u32 num_ntfy_blks)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_CONFIGURE_DEVICE_RESOURCES);
	cmd.configure_device_resources =
		(struct gve_adminq_configure_device_resources) {
		.counter_array = cpu_to_be64(counter_array_bus_addr),
		.num_counters = cpu_to_be32(num_counters),
		.irq_db_addr = cpu_to_be64(db_array_bus_addr),
		.num_irq_dbs = cpu_to_be32(num_ntfy_blks),
		.irq_db_stride = cpu_to_be32(sizeof(priv->ntfy_blocks[0])),
		.ntfy_blk_msix_base_idx =
					cpu_to_be32(GVE_NTFY_BLK_BASE_MSIX_IDX),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_deconfigure_device_resources(struct gve_priv *priv)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_DECONFIGURE_DEVICE_RESOURCES);

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_create_tx_queue(struct gve_priv *priv, u32 queue_index)
{
	struct gve_tx_ring *tx = &priv->tx[queue_index];
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_CREATE_TX_QUEUE);
	cmd.create_tx_queue = (struct gve_adminq_create_tx_queue) {
		.queue_id = cpu_to_be32(queue_index),
		.reserved = 0,
		.queue_resources_addr = cpu_to_be64(tx->q_resources_bus),
		.tx_ring_addr = cpu_to_be64(tx->bus),
		.queue_page_list_id = cpu_to_be32(tx->tx_fifo.qpl->id),
		.ntfy_id = cpu_to_be32(tx->ntfy_id),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_create_rx_queue(struct gve_priv *priv, u32 queue_index)
{
	struct gve_rx_ring *rx = &priv->rx[queue_index];
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_CREATE_RX_QUEUE);
	cmd.create_rx_queue = (struct gve_adminq_create_rx_queue) {
		.queue_id = cpu_to_be32(queue_index),
		.index = cpu_to_be32(queue_index),
		.reserved = 0,
		.ntfy_id = cpu_to_be32(rx->ntfy_id),
		.queue_resources_addr = cpu_to_be64(rx->q_resources_bus),
		.rx_desc_ring_addr = cpu_to_be64(rx->desc.bus),
		.rx_data_ring_addr = cpu_to_be64(rx->data.data_bus),
		.queue_page_list_id = cpu_to_be32(rx->data.qpl->id),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_destroy_tx_queue(struct gve_priv *priv, u32 queue_index)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_DESTROY_TX_QUEUE);
	cmd.destroy_tx_queue = (struct gve_adminq_destroy_tx_queue) {
		.queue_id = cpu_to_be32(queue_index),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_destroy_rx_queue(struct gve_priv *priv, u32 queue_index)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_DESTROY_RX_QUEUE);
	cmd.destroy_rx_queue = (struct gve_adminq_destroy_rx_queue) {
		.queue_id = cpu_to_be32(queue_index),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_describe_device(struct gve_priv *priv)
{
	struct gve_device_descriptor *descriptor;
	union gve_adminq_command cmd;
	dma_addr_t descriptor_bus;
	int err = 0;
	u8 *mac;
	u16 mtu;

	memset(&cmd, 0, sizeof(cmd));
	descriptor = dma_alloc_coherent(&priv->pdev->dev, PAGE_SIZE,
					&descriptor_bus, GFP_KERNEL);
	if (!descriptor)
		return -ENOMEM;
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_DESCRIBE_DEVICE);
	cmd.describe_device.device_descriptor_addr =
						cpu_to_be64(descriptor_bus);
	cmd.describe_device.device_descriptor_version =
			cpu_to_be32(GVE_ADMINQ_DEVICE_DESCRIPTOR_VERSION);
	cmd.describe_device.available_length = cpu_to_be32(PAGE_SIZE);

	err = gve_adminq_execute_cmd(priv, &cmd);
	if (err)
		goto free_device_descriptor;

	priv->tx_desc_cnt = be16_to_cpu(descriptor->tx_queue_entries);
	if (priv->tx_desc_cnt * sizeof(priv->tx->desc[0]) < PAGE_SIZE) {
		netif_err(priv, drv, priv->dev, "Tx desc count %d too low\n",
			  priv->tx_desc_cnt);
		err = -EINVAL;
		goto free_device_descriptor;
	}
	priv->rx_desc_cnt = be16_to_cpu(descriptor->rx_queue_entries);
	if (priv->rx_desc_cnt * sizeof(priv->rx->desc.desc_ring[0])
	    < PAGE_SIZE ||
	    priv->rx_desc_cnt * sizeof(priv->rx->data.data_ring[0])
	    < PAGE_SIZE) {
		netif_err(priv, drv, priv->dev, "Rx desc count %d too low\n",
			  priv->rx_desc_cnt);
		err = -EINVAL;
		goto free_device_descriptor;
	}
	priv->max_registered_pages =
				be64_to_cpu(descriptor->max_registered_pages);
	mtu = be16_to_cpu(descriptor->mtu);
	if (mtu < ETH_MIN_MTU) {
		netif_err(priv, drv, priv->dev, "MTU %d below minimum MTU\n",
			  mtu);
		err = -EINVAL;
		goto free_device_descriptor;
	}
	priv->dev->max_mtu = mtu;
	priv->num_event_counters = be16_to_cpu(descriptor->counters);
	ether_addr_copy(priv->dev->dev_addr, descriptor->mac);
	mac = descriptor->mac;
	netif_info(priv, drv, priv->dev, "MAC addr: %pM\n", mac);
	priv->tx_pages_per_qpl = be16_to_cpu(descriptor->tx_pages_per_qpl);
	priv->rx_pages_per_qpl = be16_to_cpu(descriptor->rx_pages_per_qpl);
	if (priv->rx_pages_per_qpl < priv->rx_desc_cnt) {
		netif_err(priv, drv, priv->dev, "rx_pages_per_qpl cannot be smaller than rx_desc_cnt, setting rx_desc_cnt down to %d.\n",
			  priv->rx_pages_per_qpl);
		priv->rx_desc_cnt = priv->rx_pages_per_qpl;
	}
	priv->default_num_queues = be16_to_cpu(descriptor->default_num_queues);

free_device_descriptor:
	dma_free_coherent(&priv->pdev->dev, sizeof(*descriptor), descriptor,
			  descriptor_bus);
	return err;
}

int gve_adminq_register_page_list(struct gve_priv *priv,
				  struct gve_queue_page_list *qpl)
{
	struct device *hdev = &priv->pdev->dev;
	u32 num_entries = qpl->num_entries;
	u32 size = num_entries * sizeof(qpl->page_buses[0]);
	union gve_adminq_command cmd;
	dma_addr_t page_list_bus;
	__be64 *page_list;
	int err;
	int i;

	memset(&cmd, 0, sizeof(cmd));
	page_list = dma_alloc_coherent(hdev, size, &page_list_bus, GFP_KERNEL);
	if (!page_list)
		return -ENOMEM;

	for (i = 0; i < num_entries; i++)
		page_list[i] = cpu_to_be64(qpl->page_buses[i]);

	cmd.opcode = cpu_to_be32(GVE_ADMINQ_REGISTER_PAGE_LIST);
	cmd.reg_page_list = (struct gve_adminq_register_page_list) {
		.page_list_id = cpu_to_be32(qpl->id),
		.num_pages = cpu_to_be32(num_entries),
		.page_address_list_addr = cpu_to_be64(page_list_bus),
	};

	err = gve_adminq_execute_cmd(priv, &cmd);
	dma_free_coherent(hdev, size, page_list, page_list_bus);
	return err;
}

int gve_adminq_unregister_page_list(struct gve_priv *priv, u32 page_list_id)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_UNREGISTER_PAGE_LIST);
	cmd.unreg_page_list = (struct gve_adminq_unregister_page_list) {
		.page_list_id = cpu_to_be32(page_list_id),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_set_mtu(struct gve_priv *priv, u64 mtu)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_SET_DRIVER_PARAMETER);
	cmd.set_driver_param = (struct gve_adminq_set_driver_parameter) {
		.parameter_type = cpu_to_be32(GVE_SET_PARAM_MTU),
		.parameter_value = cpu_to_be64(mtu),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}
