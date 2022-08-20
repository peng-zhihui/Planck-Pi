/*
 * Copyright 2015 Amazon.com, Inc. or its affiliates.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#ifdef CONFIG_RFS_ACCEL
#include <linux/cpu_rmap.h>
#endif /* CONFIG_RFS_ACCEL */
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/numa.h>
#include <linux/pci.h>
#include <linux/utsname.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <net/ip.h>

#include "ena_netdev.h"
#include "ena_pci_id_tbl.h"

static char version[] = DEVICE_NAME " v" DRV_MODULE_VERSION "\n";

MODULE_AUTHOR("Amazon.com, Inc. or its affiliates");
MODULE_DESCRIPTION(DEVICE_NAME);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_MODULE_VERSION);

/* Time in jiffies before concluding the transmitter is hung. */
#define TX_TIMEOUT  (5 * HZ)

#define ENA_NAPI_BUDGET 64

#define DEFAULT_MSG_ENABLE (NETIF_MSG_DRV | NETIF_MSG_PROBE | NETIF_MSG_IFUP | \
		NETIF_MSG_TX_DONE | NETIF_MSG_TX_ERR | NETIF_MSG_RX_ERR)
static int debug = -1;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none,...,16=all)");

static struct ena_aenq_handlers aenq_handlers;

static struct workqueue_struct *ena_wq;

MODULE_DEVICE_TABLE(pci, ena_pci_tbl);

static int ena_rss_init_default(struct ena_adapter *adapter);
static void check_for_admin_com_state(struct ena_adapter *adapter);
static void ena_destroy_device(struct ena_adapter *adapter, bool graceful);
static int ena_restore_device(struct ena_adapter *adapter);

static void ena_tx_timeout(struct net_device *dev)
{
	struct ena_adapter *adapter = netdev_priv(dev);

	/* Change the state of the device to trigger reset
	 * Check that we are not in the middle or a trigger already
	 */

	if (test_and_set_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags))
		return;

	adapter->reset_reason = ENA_REGS_RESET_OS_NETDEV_WD;
	u64_stats_update_begin(&adapter->syncp);
	adapter->dev_stats.tx_timeout++;
	u64_stats_update_end(&adapter->syncp);

	netif_err(adapter, tx_err, dev, "Transmit time out\n");
}

static void update_rx_ring_mtu(struct ena_adapter *adapter, int mtu)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++)
		adapter->rx_ring[i].mtu = mtu;
}

static int ena_change_mtu(struct net_device *dev, int new_mtu)
{
	struct ena_adapter *adapter = netdev_priv(dev);
	int ret;

	ret = ena_com_set_dev_mtu(adapter->ena_dev, new_mtu);
	if (!ret) {
		netif_dbg(adapter, drv, dev, "set MTU to %d\n", new_mtu);
		update_rx_ring_mtu(adapter, new_mtu);
		dev->mtu = new_mtu;
	} else {
		netif_err(adapter, drv, dev, "Failed to set MTU to %d\n",
			  new_mtu);
	}

	return ret;
}

static int ena_init_rx_cpu_rmap(struct ena_adapter *adapter)
{
#ifdef CONFIG_RFS_ACCEL
	u32 i;
	int rc;

	adapter->netdev->rx_cpu_rmap = alloc_irq_cpu_rmap(adapter->num_queues);
	if (!adapter->netdev->rx_cpu_rmap)
		return -ENOMEM;
	for (i = 0; i < adapter->num_queues; i++) {
		int irq_idx = ENA_IO_IRQ_IDX(i);

		rc = irq_cpu_rmap_add(adapter->netdev->rx_cpu_rmap,
				      pci_irq_vector(adapter->pdev, irq_idx));
		if (rc) {
			free_irq_cpu_rmap(adapter->netdev->rx_cpu_rmap);
			adapter->netdev->rx_cpu_rmap = NULL;
			return rc;
		}
	}
#endif /* CONFIG_RFS_ACCEL */
	return 0;
}

static void ena_init_io_rings_common(struct ena_adapter *adapter,
				     struct ena_ring *ring, u16 qid)
{
	ring->qid = qid;
	ring->pdev = adapter->pdev;
	ring->dev = &adapter->pdev->dev;
	ring->netdev = adapter->netdev;
	ring->napi = &adapter->ena_napi[qid].napi;
	ring->adapter = adapter;
	ring->ena_dev = adapter->ena_dev;
	ring->per_napi_packets = 0;
	ring->cpu = 0;
	ring->first_interrupt = false;
	ring->no_interrupt_event_cnt = 0;
	u64_stats_init(&ring->syncp);
}

static void ena_init_io_rings(struct ena_adapter *adapter)
{
	struct ena_com_dev *ena_dev;
	struct ena_ring *txr, *rxr;
	int i;

	ena_dev = adapter->ena_dev;

	for (i = 0; i < adapter->num_queues; i++) {
		txr = &adapter->tx_ring[i];
		rxr = &adapter->rx_ring[i];

		/* TX/RX common ring state */
		ena_init_io_rings_common(adapter, txr, i);
		ena_init_io_rings_common(adapter, rxr, i);

		/* TX specific ring state */
		txr->ring_size = adapter->requested_tx_ring_size;
		txr->tx_max_header_size = ena_dev->tx_max_header_size;
		txr->tx_mem_queue_type = ena_dev->tx_mem_queue_type;
		txr->sgl_size = adapter->max_tx_sgl_size;
		txr->smoothed_interval =
			ena_com_get_nonadaptive_moderation_interval_tx(ena_dev);

		/* RX specific ring state */
		rxr->ring_size = adapter->requested_rx_ring_size;
		rxr->rx_copybreak = adapter->rx_copybreak;
		rxr->sgl_size = adapter->max_rx_sgl_size;
		rxr->smoothed_interval =
			ena_com_get_nonadaptive_moderation_interval_rx(ena_dev);
		rxr->empty_rx_queue = 0;
		adapter->ena_napi[i].dim.mode = DIM_CQ_PERIOD_MODE_START_FROM_EQE;
	}
}

/* ena_setup_tx_resources - allocate I/O Tx resources (Descriptors)
 * @adapter: network interface device structure
 * @qid: queue index
 *
 * Return 0 on success, negative on failure
 */
static int ena_setup_tx_resources(struct ena_adapter *adapter, int qid)
{
	struct ena_ring *tx_ring = &adapter->tx_ring[qid];
	struct ena_irq *ena_irq = &adapter->irq_tbl[ENA_IO_IRQ_IDX(qid)];
	int size, i, node;

	if (tx_ring->tx_buffer_info) {
		netif_err(adapter, ifup,
			  adapter->netdev, "tx_buffer_info info is not NULL");
		return -EEXIST;
	}

	size = sizeof(struct ena_tx_buffer) * tx_ring->ring_size;
	node = cpu_to_node(ena_irq->cpu);

	tx_ring->tx_buffer_info = vzalloc_node(size, node);
	if (!tx_ring->tx_buffer_info) {
		tx_ring->tx_buffer_info = vzalloc(size);
		if (!tx_ring->tx_buffer_info)
			goto err_tx_buffer_info;
	}

	size = sizeof(u16) * tx_ring->ring_size;
	tx_ring->free_ids = vzalloc_node(size, node);
	if (!tx_ring->free_ids) {
		tx_ring->free_ids = vzalloc(size);
		if (!tx_ring->free_ids)
			goto err_tx_free_ids;
	}

	size = tx_ring->tx_max_header_size;
	tx_ring->push_buf_intermediate_buf = vzalloc_node(size, node);
	if (!tx_ring->push_buf_intermediate_buf) {
		tx_ring->push_buf_intermediate_buf = vzalloc(size);
		if (!tx_ring->push_buf_intermediate_buf)
			goto err_push_buf_intermediate_buf;
	}

	/* Req id ring for TX out of order completions */
	for (i = 0; i < tx_ring->ring_size; i++)
		tx_ring->free_ids[i] = i;

	/* Reset tx statistics */
	memset(&tx_ring->tx_stats, 0x0, sizeof(tx_ring->tx_stats));

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;
	tx_ring->cpu = ena_irq->cpu;
	return 0;

err_push_buf_intermediate_buf:
	vfree(tx_ring->free_ids);
	tx_ring->free_ids = NULL;
err_tx_free_ids:
	vfree(tx_ring->tx_buffer_info);
	tx_ring->tx_buffer_info = NULL;
err_tx_buffer_info:
	return -ENOMEM;
}

/* ena_free_tx_resources - Free I/O Tx Resources per Queue
 * @adapter: network interface device structure
 * @qid: queue index
 *
 * Free all transmit software resources
 */
static void ena_free_tx_resources(struct ena_adapter *adapter, int qid)
{
	struct ena_ring *tx_ring = &adapter->tx_ring[qid];

	vfree(tx_ring->tx_buffer_info);
	tx_ring->tx_buffer_info = NULL;

	vfree(tx_ring->free_ids);
	tx_ring->free_ids = NULL;

	vfree(tx_ring->push_buf_intermediate_buf);
	tx_ring->push_buf_intermediate_buf = NULL;
}

/* ena_setup_all_tx_resources - allocate I/O Tx queues resources for All queues
 * @adapter: private structure
 *
 * Return 0 on success, negative on failure
 */
static int ena_setup_all_tx_resources(struct ena_adapter *adapter)
{
	int i, rc = 0;

	for (i = 0; i < adapter->num_queues; i++) {
		rc = ena_setup_tx_resources(adapter, i);
		if (rc)
			goto err_setup_tx;
	}

	return 0;

err_setup_tx:

	netif_err(adapter, ifup, adapter->netdev,
		  "Tx queue %d: allocation failed\n", i);

	/* rewind the index freeing the rings as we go */
	while (i--)
		ena_free_tx_resources(adapter, i);
	return rc;
}

/* ena_free_all_io_tx_resources - Free I/O Tx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all transmit software resources
 */
static void ena_free_all_io_tx_resources(struct ena_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++)
		ena_free_tx_resources(adapter, i);
}

static int validate_rx_req_id(struct ena_ring *rx_ring, u16 req_id)
{
	if (likely(req_id < rx_ring->ring_size))
		return 0;

	netif_err(rx_ring->adapter, rx_err, rx_ring->netdev,
		  "Invalid rx req_id: %hu\n", req_id);

	u64_stats_update_begin(&rx_ring->syncp);
	rx_ring->rx_stats.bad_req_id++;
	u64_stats_update_end(&rx_ring->syncp);

	/* Trigger device reset */
	rx_ring->adapter->reset_reason = ENA_REGS_RESET_INV_RX_REQ_ID;
	set_bit(ENA_FLAG_TRIGGER_RESET, &rx_ring->adapter->flags);
	return -EFAULT;
}

/* ena_setup_rx_resources - allocate I/O Rx resources (Descriptors)
 * @adapter: network interface device structure
 * @qid: queue index
 *
 * Returns 0 on success, negative on failure
 */
static int ena_setup_rx_resources(struct ena_adapter *adapter,
				  u32 qid)
{
	struct ena_ring *rx_ring = &adapter->rx_ring[qid];
	struct ena_irq *ena_irq = &adapter->irq_tbl[ENA_IO_IRQ_IDX(qid)];
	int size, node, i;

	if (rx_ring->rx_buffer_info) {
		netif_err(adapter, ifup, adapter->netdev,
			  "rx_buffer_info is not NULL");
		return -EEXIST;
	}

	/* alloc extra element so in rx path
	 * we can always prefetch rx_info + 1
	 */
	size = sizeof(struct ena_rx_buffer) * (rx_ring->ring_size + 1);
	node = cpu_to_node(ena_irq->cpu);

	rx_ring->rx_buffer_info = vzalloc_node(size, node);
	if (!rx_ring->rx_buffer_info) {
		rx_ring->rx_buffer_info = vzalloc(size);
		if (!rx_ring->rx_buffer_info)
			return -ENOMEM;
	}

	size = sizeof(u16) * rx_ring->ring_size;
	rx_ring->free_ids = vzalloc_node(size, node);
	if (!rx_ring->free_ids) {
		rx_ring->free_ids = vzalloc(size);
		if (!rx_ring->free_ids) {
			vfree(rx_ring->rx_buffer_info);
			rx_ring->rx_buffer_info = NULL;
			return -ENOMEM;
		}
	}

	/* Req id ring for receiving RX pkts out of order */
	for (i = 0; i < rx_ring->ring_size; i++)
		rx_ring->free_ids[i] = i;

	/* Reset rx statistics */
	memset(&rx_ring->rx_stats, 0x0, sizeof(rx_ring->rx_stats));

	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;
	rx_ring->cpu = ena_irq->cpu;

	return 0;
}

/* ena_free_rx_resources - Free I/O Rx Resources
 * @adapter: network interface device structure
 * @qid: queue index
 *
 * Free all receive software resources
 */
static void ena_free_rx_resources(struct ena_adapter *adapter,
				  u32 qid)
{
	struct ena_ring *rx_ring = &adapter->rx_ring[qid];

	vfree(rx_ring->rx_buffer_info);
	rx_ring->rx_buffer_info = NULL;

	vfree(rx_ring->free_ids);
	rx_ring->free_ids = NULL;
}

/* ena_setup_all_rx_resources - allocate I/O Rx queues resources for all queues
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 */
static int ena_setup_all_rx_resources(struct ena_adapter *adapter)
{
	int i, rc = 0;

	for (i = 0; i < adapter->num_queues; i++) {
		rc = ena_setup_rx_resources(adapter, i);
		if (rc)
			goto err_setup_rx;
	}

	return 0;

err_setup_rx:

	netif_err(adapter, ifup, adapter->netdev,
		  "Rx queue %d: allocation failed\n", i);

	/* rewind the index freeing the rings as we go */
	while (i--)
		ena_free_rx_resources(adapter, i);
	return rc;
}

/* ena_free_all_io_rx_resources - Free I/O Rx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all receive software resources
 */
static void ena_free_all_io_rx_resources(struct ena_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++)
		ena_free_rx_resources(adapter, i);
}

static int ena_alloc_rx_page(struct ena_ring *rx_ring,
				    struct ena_rx_buffer *rx_info, gfp_t gfp)
{
	struct ena_com_buf *ena_buf;
	struct page *page;
	dma_addr_t dma;

	/* if previous allocated page is not used */
	if (unlikely(rx_info->page))
		return 0;

	page = alloc_page(gfp);
	if (unlikely(!page)) {
		u64_stats_update_begin(&rx_ring->syncp);
		rx_ring->rx_stats.page_alloc_fail++;
		u64_stats_update_end(&rx_ring->syncp);
		return -ENOMEM;
	}

	dma = dma_map_page(rx_ring->dev, page, 0, ENA_PAGE_SIZE,
			   DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(rx_ring->dev, dma))) {
		u64_stats_update_begin(&rx_ring->syncp);
		rx_ring->rx_stats.dma_mapping_err++;
		u64_stats_update_end(&rx_ring->syncp);

		__free_page(page);
		return -EIO;
	}
	netif_dbg(rx_ring->adapter, rx_status, rx_ring->netdev,
		  "alloc page %p, rx_info %p\n", page, rx_info);

	rx_info->page = page;
	rx_info->page_offset = 0;
	ena_buf = &rx_info->ena_buf;
	ena_buf->paddr = dma;
	ena_buf->len = ENA_PAGE_SIZE;

	return 0;
}

static void ena_free_rx_page(struct ena_ring *rx_ring,
			     struct ena_rx_buffer *rx_info)
{
	struct page *page = rx_info->page;
	struct ena_com_buf *ena_buf = &rx_info->ena_buf;

	if (unlikely(!page)) {
		netif_warn(rx_ring->adapter, rx_err, rx_ring->netdev,
			   "Trying to free unallocated buffer\n");
		return;
	}

	dma_unmap_page(rx_ring->dev, ena_buf->paddr, ENA_PAGE_SIZE,
		       DMA_FROM_DEVICE);

	__free_page(page);
	rx_info->page = NULL;
}

static int ena_refill_rx_bufs(struct ena_ring *rx_ring, u32 num)
{
	u16 next_to_use, req_id;
	u32 i;
	int rc;

	next_to_use = rx_ring->next_to_use;

	for (i = 0; i < num; i++) {
		struct ena_rx_buffer *rx_info;

		req_id = rx_ring->free_ids[next_to_use];
		rc = validate_rx_req_id(rx_ring, req_id);
		if (unlikely(rc < 0))
			break;

		rx_info = &rx_ring->rx_buffer_info[req_id];


		rc = ena_alloc_rx_page(rx_ring, rx_info,
				       GFP_ATOMIC | __GFP_COMP);
		if (unlikely(rc < 0)) {
			netif_warn(rx_ring->adapter, rx_err, rx_ring->netdev,
				   "failed to alloc buffer for rx queue %d\n",
				   rx_ring->qid);
			break;
		}
		rc = ena_com_add_single_rx_desc(rx_ring->ena_com_io_sq,
						&rx_info->ena_buf,
						req_id);
		if (unlikely(rc)) {
			netif_warn(rx_ring->adapter, rx_status, rx_ring->netdev,
				   "failed to add buffer for rx queue %d\n",
				   rx_ring->qid);
			break;
		}
		next_to_use = ENA_RX_RING_IDX_NEXT(next_to_use,
						   rx_ring->ring_size);
	}

	if (unlikely(i < num)) {
		u64_stats_update_begin(&rx_ring->syncp);
		rx_ring->rx_stats.refil_partial++;
		u64_stats_update_end(&rx_ring->syncp);
		netdev_warn(rx_ring->netdev,
			    "refilled rx qid %d with only %d buffers (from %d)\n",
			    rx_ring->qid, i, num);
	}

	/* ena_com_write_sq_doorbell issues a wmb() */
	if (likely(i))
		ena_com_write_sq_doorbell(rx_ring->ena_com_io_sq);

	rx_ring->next_to_use = next_to_use;

	return i;
}

static void ena_free_rx_bufs(struct ena_adapter *adapter,
			     u32 qid)
{
	struct ena_ring *rx_ring = &adapter->rx_ring[qid];
	u32 i;

	for (i = 0; i < rx_ring->ring_size; i++) {
		struct ena_rx_buffer *rx_info = &rx_ring->rx_buffer_info[i];

		if (rx_info->page)
			ena_free_rx_page(rx_ring, rx_info);
	}
}

/* ena_refill_all_rx_bufs - allocate all queues Rx buffers
 * @adapter: board private structure
 */
static void ena_refill_all_rx_bufs(struct ena_adapter *adapter)
{
	struct ena_ring *rx_ring;
	int i, rc, bufs_num;

	for (i = 0; i < adapter->num_queues; i++) {
		rx_ring = &adapter->rx_ring[i];
		bufs_num = rx_ring->ring_size - 1;
		rc = ena_refill_rx_bufs(rx_ring, bufs_num);

		if (unlikely(rc != bufs_num))
			netif_warn(rx_ring->adapter, rx_status, rx_ring->netdev,
				   "refilling Queue %d failed. allocated %d buffers from: %d\n",
				   i, rc, bufs_num);
	}
}

static void ena_free_all_rx_bufs(struct ena_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++)
		ena_free_rx_bufs(adapter, i);
}

static void ena_unmap_tx_skb(struct ena_ring *tx_ring,
				    struct ena_tx_buffer *tx_info)
{
	struct ena_com_buf *ena_buf;
	u32 cnt;
	int i;

	ena_buf = tx_info->bufs;
	cnt = tx_info->num_of_bufs;

	if (unlikely(!cnt))
		return;

	if (tx_info->map_linear_data) {
		dma_unmap_single(tx_ring->dev,
				 dma_unmap_addr(ena_buf, paddr),
				 dma_unmap_len(ena_buf, len),
				 DMA_TO_DEVICE);
		ena_buf++;
		cnt--;
	}

	/* unmap remaining mapped pages */
	for (i = 0; i < cnt; i++) {
		dma_unmap_page(tx_ring->dev, dma_unmap_addr(ena_buf, paddr),
			       dma_unmap_len(ena_buf, len), DMA_TO_DEVICE);
		ena_buf++;
	}
}

/* ena_free_tx_bufs - Free Tx Buffers per Queue
 * @tx_ring: TX ring for which buffers be freed
 */
static void ena_free_tx_bufs(struct ena_ring *tx_ring)
{
	bool print_once = true;
	u32 i;

	for (i = 0; i < tx_ring->ring_size; i++) {
		struct ena_tx_buffer *tx_info = &tx_ring->tx_buffer_info[i];

		if (!tx_info->skb)
			continue;

		if (print_once) {
			netdev_notice(tx_ring->netdev,
				      "free uncompleted tx skb qid %d idx 0x%x\n",
				      tx_ring->qid, i);
			print_once = false;
		} else {
			netdev_dbg(tx_ring->netdev,
				   "free uncompleted tx skb qid %d idx 0x%x\n",
				   tx_ring->qid, i);
		}

		ena_unmap_tx_skb(tx_ring, tx_info);

		dev_kfree_skb_any(tx_info->skb);
	}
	netdev_tx_reset_queue(netdev_get_tx_queue(tx_ring->netdev,
						  tx_ring->qid));
}

static void ena_free_all_tx_bufs(struct ena_adapter *adapter)
{
	struct ena_ring *tx_ring;
	int i;

	for (i = 0; i < adapter->num_queues; i++) {
		tx_ring = &adapter->tx_ring[i];
		ena_free_tx_bufs(tx_ring);
	}
}

static void ena_destroy_all_tx_queues(struct ena_adapter *adapter)
{
	u16 ena_qid;
	int i;

	for (i = 0; i < adapter->num_queues; i++) {
		ena_qid = ENA_IO_TXQ_IDX(i);
		ena_com_destroy_io_queue(adapter->ena_dev, ena_qid);
	}
}

static void ena_destroy_all_rx_queues(struct ena_adapter *adapter)
{
	u16 ena_qid;
	int i;

	for (i = 0; i < adapter->num_queues; i++) {
		ena_qid = ENA_IO_RXQ_IDX(i);
		cancel_work_sync(&adapter->ena_napi[i].dim.work);
		ena_com_destroy_io_queue(adapter->ena_dev, ena_qid);
	}
}

static void ena_destroy_all_io_queues(struct ena_adapter *adapter)
{
	ena_destroy_all_tx_queues(adapter);
	ena_destroy_all_rx_queues(adapter);
}

static int validate_tx_req_id(struct ena_ring *tx_ring, u16 req_id)
{
	struct ena_tx_buffer *tx_info = NULL;

	if (likely(req_id < tx_ring->ring_size)) {
		tx_info = &tx_ring->tx_buffer_info[req_id];
		if (likely(tx_info->skb))
			return 0;
	}

	if (tx_info)
		netif_err(tx_ring->adapter, tx_done, tx_ring->netdev,
			  "tx_info doesn't have valid skb\n");
	else
		netif_err(tx_ring->adapter, tx_done, tx_ring->netdev,
			  "Invalid req_id: %hu\n", req_id);

	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->tx_stats.bad_req_id++;
	u64_stats_update_end(&tx_ring->syncp);

	/* Trigger device reset */
	tx_ring->adapter->reset_reason = ENA_REGS_RESET_INV_TX_REQ_ID;
	set_bit(ENA_FLAG_TRIGGER_RESET, &tx_ring->adapter->flags);
	return -EFAULT;
}

static int ena_clean_tx_irq(struct ena_ring *tx_ring, u32 budget)
{
	struct netdev_queue *txq;
	bool above_thresh;
	u32 tx_bytes = 0;
	u32 total_done = 0;
	u16 next_to_clean;
	u16 req_id;
	int tx_pkts = 0;
	int rc;

	next_to_clean = tx_ring->next_to_clean;
	txq = netdev_get_tx_queue(tx_ring->netdev, tx_ring->qid);

	while (tx_pkts < budget) {
		struct ena_tx_buffer *tx_info;
		struct sk_buff *skb;

		rc = ena_com_tx_comp_req_id_get(tx_ring->ena_com_io_cq,
						&req_id);
		if (rc)
			break;

		rc = validate_tx_req_id(tx_ring, req_id);
		if (rc)
			break;

		tx_info = &tx_ring->tx_buffer_info[req_id];
		skb = tx_info->skb;

		/* prefetch skb_end_pointer() to speedup skb_shinfo(skb) */
		prefetch(&skb->end);

		tx_info->skb = NULL;
		tx_info->last_jiffies = 0;

		ena_unmap_tx_skb(tx_ring, tx_info);

		netif_dbg(tx_ring->adapter, tx_done, tx_ring->netdev,
			  "tx_poll: q %d skb %p completed\n", tx_ring->qid,
			  skb);

		tx_bytes += skb->len;
		dev_kfree_skb(skb);
		tx_pkts++;
		total_done += tx_info->tx_descs;

		tx_ring->free_ids[next_to_clean] = req_id;
		next_to_clean = ENA_TX_RING_IDX_NEXT(next_to_clean,
						     tx_ring->ring_size);
	}

	tx_ring->next_to_clean = next_to_clean;
	ena_com_comp_ack(tx_ring->ena_com_io_sq, total_done);
	ena_com_update_dev_comp_head(tx_ring->ena_com_io_cq);

	netdev_tx_completed_queue(txq, tx_pkts, tx_bytes);

	netif_dbg(tx_ring->adapter, tx_done, tx_ring->netdev,
		  "tx_poll: q %d done. total pkts: %d\n",
		  tx_ring->qid, tx_pkts);

	/* need to make the rings circular update visible to
	 * ena_start_xmit() before checking for netif_queue_stopped().
	 */
	smp_mb();

	above_thresh = ena_com_sq_have_enough_space(tx_ring->ena_com_io_sq,
						    ENA_TX_WAKEUP_THRESH);
	if (unlikely(netif_tx_queue_stopped(txq) && above_thresh)) {
		__netif_tx_lock(txq, smp_processor_id());
		above_thresh =
			ena_com_sq_have_enough_space(tx_ring->ena_com_io_sq,
						     ENA_TX_WAKEUP_THRESH);
		if (netif_tx_queue_stopped(txq) && above_thresh &&
		    test_bit(ENA_FLAG_DEV_UP, &tx_ring->adapter->flags)) {
			netif_tx_wake_queue(txq);
			u64_stats_update_begin(&tx_ring->syncp);
			tx_ring->tx_stats.queue_wakeup++;
			u64_stats_update_end(&tx_ring->syncp);
		}
		__netif_tx_unlock(txq);
	}

	return tx_pkts;
}

static struct sk_buff *ena_alloc_skb(struct ena_ring *rx_ring, bool frags)
{
	struct sk_buff *skb;

	if (frags)
		skb = napi_get_frags(rx_ring->napi);
	else
		skb = netdev_alloc_skb_ip_align(rx_ring->netdev,
						rx_ring->rx_copybreak);

	if (unlikely(!skb)) {
		u64_stats_update_begin(&rx_ring->syncp);
		rx_ring->rx_stats.skb_alloc_fail++;
		u64_stats_update_end(&rx_ring->syncp);
		netif_dbg(rx_ring->adapter, rx_err, rx_ring->netdev,
			  "Failed to allocate skb. frags: %d\n", frags);
		return NULL;
	}

	return skb;
}

static struct sk_buff *ena_rx_skb(struct ena_ring *rx_ring,
				  struct ena_com_rx_buf_info *ena_bufs,
				  u32 descs,
				  u16 *next_to_clean)
{
	struct sk_buff *skb;
	struct ena_rx_buffer *rx_info;
	u16 len, req_id, buf = 0;
	void *va;

	len = ena_bufs[buf].len;
	req_id = ena_bufs[buf].req_id;
	rx_info = &rx_ring->rx_buffer_info[req_id];

	if (unlikely(!rx_info->page)) {
		netif_err(rx_ring->adapter, rx_err, rx_ring->netdev,
			  "Page is NULL\n");
		return NULL;
	}

	netif_dbg(rx_ring->adapter, rx_status, rx_ring->netdev,
		  "rx_info %p page %p\n",
		  rx_info, rx_info->page);

	/* save virt address of first buffer */
	va = page_address(rx_info->page) + rx_info->page_offset;
	prefetch(va + NET_IP_ALIGN);

	if (len <= rx_ring->rx_copybreak) {
		skb = ena_alloc_skb(rx_ring, false);
		if (unlikely(!skb))
			return NULL;

		netif_dbg(rx_ring->adapter, rx_status, rx_ring->netdev,
			  "rx allocated small packet. len %d. data_len %d\n",
			  skb->len, skb->data_len);

		/* sync this buffer for CPU use */
		dma_sync_single_for_cpu(rx_ring->dev,
					dma_unmap_addr(&rx_info->ena_buf, paddr),
					len,
					DMA_FROM_DEVICE);
		skb_copy_to_linear_data(skb, va, len);
		dma_sync_single_for_device(rx_ring->dev,
					   dma_unmap_addr(&rx_info->ena_buf, paddr),
					   len,
					   DMA_FROM_DEVICE);

		skb_put(skb, len);
		skb->protocol = eth_type_trans(skb, rx_ring->netdev);
		rx_ring->free_ids[*next_to_clean] = req_id;
		*next_to_clean = ENA_RX_RING_IDX_ADD(*next_to_clean, descs,
						     rx_ring->ring_size);
		return skb;
	}

	skb = ena_alloc_skb(rx_ring, true);
	if (unlikely(!skb))
		return NULL;

	do {
		dma_unmap_page(rx_ring->dev,
			       dma_unmap_addr(&rx_info->ena_buf, paddr),
			       ENA_PAGE_SIZE, DMA_FROM_DEVICE);

		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, rx_info->page,
				rx_info->page_offset, len, ENA_PAGE_SIZE);

		netif_dbg(rx_ring->adapter, rx_status, rx_ring->netdev,
			  "rx skb updated. len %d. data_len %d\n",
			  skb->len, skb->data_len);

		rx_info->page = NULL;

		rx_ring->free_ids[*next_to_clean] = req_id;
		*next_to_clean =
			ENA_RX_RING_IDX_NEXT(*next_to_clean,
					     rx_ring->ring_size);
		if (likely(--descs == 0))
			break;

		buf++;
		len = ena_bufs[buf].len;
		req_id = ena_bufs[buf].req_id;
		rx_info = &rx_ring->rx_buffer_info[req_id];
	} while (1);

	return skb;
}

/* ena_rx_checksum - indicate in skb if hw indicated a good cksum
 * @adapter: structure containing adapter specific data
 * @ena_rx_ctx: received packet context/metadata
 * @skb: skb currently being received and modified
 */
static void ena_rx_checksum(struct ena_ring *rx_ring,
				   struct ena_com_rx_ctx *ena_rx_ctx,
				   struct sk_buff *skb)
{
	/* Rx csum disabled */
	if (unlikely(!(rx_ring->netdev->features & NETIF_F_RXCSUM))) {
		skb->ip_summed = CHECKSUM_NONE;
		return;
	}

	/* For fragmented packets the checksum isn't valid */
	if (ena_rx_ctx->frag) {
		skb->ip_summed = CHECKSUM_NONE;
		return;
	}

	/* if IP and error */
	if (unlikely((ena_rx_ctx->l3_proto == ENA_ETH_IO_L3_PROTO_IPV4) &&
		     (ena_rx_ctx->l3_csum_err))) {
		/* ipv4 checksum error */
		skb->ip_summed = CHECKSUM_NONE;
		u64_stats_update_begin(&rx_ring->syncp);
		rx_ring->rx_stats.bad_csum++;
		u64_stats_update_end(&rx_ring->syncp);
		netif_dbg(rx_ring->adapter, rx_err, rx_ring->netdev,
			  "RX IPv4 header checksum error\n");
		return;
	}

	/* if TCP/UDP */
	if (likely((ena_rx_ctx->l4_proto == ENA_ETH_IO_L4_PROTO_TCP) ||
		   (ena_rx_ctx->l4_proto == ENA_ETH_IO_L4_PROTO_UDP))) {
		if (unlikely(ena_rx_ctx->l4_csum_err)) {
			/* TCP/UDP checksum error */
			u64_stats_update_begin(&rx_ring->syncp);
			rx_ring->rx_stats.bad_csum++;
			u64_stats_update_end(&rx_ring->syncp);
			netif_dbg(rx_ring->adapter, rx_err, rx_ring->netdev,
				  "RX L4 checksum error\n");
			skb->ip_summed = CHECKSUM_NONE;
			return;
		}

		if (likely(ena_rx_ctx->l4_csum_checked)) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			u64_stats_update_begin(&rx_ring->syncp);
			rx_ring->rx_stats.csum_good++;
			u64_stats_update_end(&rx_ring->syncp);
		} else {
			u64_stats_update_begin(&rx_ring->syncp);
			rx_ring->rx_stats.csum_unchecked++;
			u64_stats_update_end(&rx_ring->syncp);
			skb->ip_summed = CHECKSUM_NONE;
		}
	} else {
		skb->ip_summed = CHECKSUM_NONE;
		return;
	}

}

static void ena_set_rx_hash(struct ena_ring *rx_ring,
			    struct ena_com_rx_ctx *ena_rx_ctx,
			    struct sk_buff *skb)
{
	enum pkt_hash_types hash_type;

	if (likely(rx_ring->netdev->features & NETIF_F_RXHASH)) {
		if (likely((ena_rx_ctx->l4_proto == ENA_ETH_IO_L4_PROTO_TCP) ||
			   (ena_rx_ctx->l4_proto == ENA_ETH_IO_L4_PROTO_UDP)))

			hash_type = PKT_HASH_TYPE_L4;
		else
			hash_type = PKT_HASH_TYPE_NONE;

		/* Override hash type if the packet is fragmented */
		if (ena_rx_ctx->frag)
			hash_type = PKT_HASH_TYPE_NONE;

		skb_set_hash(skb, ena_rx_ctx->hash, hash_type);
	}
}

/* ena_clean_rx_irq - Cleanup RX irq
 * @rx_ring: RX ring to clean
 * @napi: napi handler
 * @budget: how many packets driver is allowed to clean
 *
 * Returns the number of cleaned buffers.
 */
static int ena_clean_rx_irq(struct ena_ring *rx_ring, struct napi_struct *napi,
			    u32 budget)
{
	u16 next_to_clean = rx_ring->next_to_clean;
	u32 res_budget, work_done;

	struct ena_com_rx_ctx ena_rx_ctx;
	struct ena_adapter *adapter;
	struct sk_buff *skb;
	int refill_required;
	int refill_threshold;
	int rc = 0;
	int total_len = 0;
	int rx_copybreak_pkt = 0;
	int i;

	netif_dbg(rx_ring->adapter, rx_status, rx_ring->netdev,
		  "%s qid %d\n", __func__, rx_ring->qid);
	res_budget = budget;

	do {
		ena_rx_ctx.ena_bufs = rx_ring->ena_bufs;
		ena_rx_ctx.max_bufs = rx_ring->sgl_size;
		ena_rx_ctx.descs = 0;
		rc = ena_com_rx_pkt(rx_ring->ena_com_io_cq,
				    rx_ring->ena_com_io_sq,
				    &ena_rx_ctx);
		if (unlikely(rc))
			goto error;

		if (unlikely(ena_rx_ctx.descs == 0))
			break;

		netif_dbg(rx_ring->adapter, rx_status, rx_ring->netdev,
			  "rx_poll: q %d got packet from ena. descs #: %d l3 proto %d l4 proto %d hash: %x\n",
			  rx_ring->qid, ena_rx_ctx.descs, ena_rx_ctx.l3_proto,
			  ena_rx_ctx.l4_proto, ena_rx_ctx.hash);

		/* allocate skb and fill it */
		skb = ena_rx_skb(rx_ring, rx_ring->ena_bufs, ena_rx_ctx.descs,
				 &next_to_clean);

		/* exit if we failed to retrieve a buffer */
		if (unlikely(!skb)) {
			for (i = 0; i < ena_rx_ctx.descs; i++) {
				rx_ring->free_ids[next_to_clean] =
					rx_ring->ena_bufs[i].req_id;
				next_to_clean =
					ENA_RX_RING_IDX_NEXT(next_to_clean,
							     rx_ring->ring_size);
			}
			break;
		}

		ena_rx_checksum(rx_ring, &ena_rx_ctx, skb);

		ena_set_rx_hash(rx_ring, &ena_rx_ctx, skb);

		skb_record_rx_queue(skb, rx_ring->qid);

		if (rx_ring->ena_bufs[0].len <= rx_ring->rx_copybreak) {
			total_len += rx_ring->ena_bufs[0].len;
			rx_copybreak_pkt++;
			napi_gro_receive(napi, skb);
		} else {
			total_len += skb->len;
			napi_gro_frags(napi);
		}

		res_budget--;
	} while (likely(res_budget));

	work_done = budget - res_budget;
	rx_ring->per_napi_packets += work_done;
	u64_stats_update_begin(&rx_ring->syncp);
	rx_ring->rx_stats.bytes += total_len;
	rx_ring->rx_stats.cnt += work_done;
	rx_ring->rx_stats.rx_copybreak_pkt += rx_copybreak_pkt;
	u64_stats_update_end(&rx_ring->syncp);

	rx_ring->next_to_clean = next_to_clean;

	refill_required = ena_com_free_desc(rx_ring->ena_com_io_sq);
	refill_threshold =
		min_t(int, rx_ring->ring_size / ENA_RX_REFILL_THRESH_DIVIDER,
		      ENA_RX_REFILL_THRESH_PACKET);

	/* Optimization, try to batch new rx buffers */
	if (refill_required > refill_threshold) {
		ena_com_update_dev_comp_head(rx_ring->ena_com_io_cq);
		ena_refill_rx_bufs(rx_ring, refill_required);
	}

	return work_done;

error:
	adapter = netdev_priv(rx_ring->netdev);

	u64_stats_update_begin(&rx_ring->syncp);
	rx_ring->rx_stats.bad_desc_num++;
	u64_stats_update_end(&rx_ring->syncp);

	/* Too many desc from the device. Trigger reset */
	adapter->reset_reason = ENA_REGS_RESET_TOO_MANY_RX_DESCS;
	set_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);

	return 0;
}

static void ena_dim_work(struct work_struct *w)
{
	struct dim *dim = container_of(w, struct dim, work);
	struct dim_cq_moder cur_moder =
		net_dim_get_rx_moderation(dim->mode, dim->profile_ix);
	struct ena_napi *ena_napi = container_of(dim, struct ena_napi, dim);

	ena_napi->rx_ring->smoothed_interval = cur_moder.usec;
	dim->state = DIM_START_MEASURE;
}

static void ena_adjust_adaptive_rx_intr_moderation(struct ena_napi *ena_napi)
{
	struct dim_sample dim_sample;
	struct ena_ring *rx_ring = ena_napi->rx_ring;

	if (!rx_ring->per_napi_packets)
		return;

	rx_ring->non_empty_napi_events++;

	dim_update_sample(rx_ring->non_empty_napi_events,
			  rx_ring->rx_stats.cnt,
			  rx_ring->rx_stats.bytes,
			  &dim_sample);

	net_dim(&ena_napi->dim, dim_sample);

	rx_ring->per_napi_packets = 0;
}

static void ena_unmask_interrupt(struct ena_ring *tx_ring,
					struct ena_ring *rx_ring)
{
	struct ena_eth_io_intr_reg intr_reg;
	u32 rx_interval = ena_com_get_adaptive_moderation_enabled(rx_ring->ena_dev) ?
		rx_ring->smoothed_interval :
		ena_com_get_nonadaptive_moderation_interval_rx(rx_ring->ena_dev);

	/* Update intr register: rx intr delay,
	 * tx intr delay and interrupt unmask
	 */
	ena_com_update_intr_reg(&intr_reg,
				rx_interval,
				tx_ring->smoothed_interval,
				true);

	/* It is a shared MSI-X.
	 * Tx and Rx CQ have pointer to it.
	 * So we use one of them to reach the intr reg
	 */
	ena_com_unmask_intr(rx_ring->ena_com_io_cq, &intr_reg);
}

static void ena_update_ring_numa_node(struct ena_ring *tx_ring,
					     struct ena_ring *rx_ring)
{
	int cpu = get_cpu();
	int numa_node;

	/* Check only one ring since the 2 rings are running on the same cpu */
	if (likely(tx_ring->cpu == cpu))
		goto out;

	numa_node = cpu_to_node(cpu);
	put_cpu();

	if (numa_node != NUMA_NO_NODE) {
		ena_com_update_numa_node(tx_ring->ena_com_io_cq, numa_node);
		ena_com_update_numa_node(rx_ring->ena_com_io_cq, numa_node);
	}

	tx_ring->cpu = cpu;
	rx_ring->cpu = cpu;

	return;
out:
	put_cpu();
}

static int ena_io_poll(struct napi_struct *napi, int budget)
{
	struct ena_napi *ena_napi = container_of(napi, struct ena_napi, napi);
	struct ena_ring *tx_ring, *rx_ring;

	int tx_work_done;
	int rx_work_done = 0;
	int tx_budget;
	int napi_comp_call = 0;
	int ret;

	tx_ring = ena_napi->tx_ring;
	rx_ring = ena_napi->rx_ring;

	tx_budget = tx_ring->ring_size / ENA_TX_POLL_BUDGET_DIVIDER;

	if (!test_bit(ENA_FLAG_DEV_UP, &tx_ring->adapter->flags) ||
	    test_bit(ENA_FLAG_TRIGGER_RESET, &tx_ring->adapter->flags)) {
		napi_complete_done(napi, 0);
		return 0;
	}

	tx_work_done = ena_clean_tx_irq(tx_ring, tx_budget);
	/* On netpoll the budget is zero and the handler should only clean the
	 * tx completions.
	 */
	if (likely(budget))
		rx_work_done = ena_clean_rx_irq(rx_ring, napi, budget);

	/* If the device is about to reset or down, avoid unmask
	 * the interrupt and return 0 so NAPI won't reschedule
	 */
	if (unlikely(!test_bit(ENA_FLAG_DEV_UP, &tx_ring->adapter->flags) ||
		     test_bit(ENA_FLAG_TRIGGER_RESET, &tx_ring->adapter->flags))) {
		napi_complete_done(napi, 0);
		ret = 0;

	} else if ((budget > rx_work_done) && (tx_budget > tx_work_done)) {
		napi_comp_call = 1;

		/* Update numa and unmask the interrupt only when schedule
		 * from the interrupt context (vs from sk_busy_loop)
		 */
		if (napi_complete_done(napi, rx_work_done)) {
			/* We apply adaptive moderation on Rx path only.
			 * Tx uses static interrupt moderation.
			 */
			if (ena_com_get_adaptive_moderation_enabled(rx_ring->ena_dev))
				ena_adjust_adaptive_rx_intr_moderation(ena_napi);

			ena_unmask_interrupt(tx_ring, rx_ring);
		}

		ena_update_ring_numa_node(tx_ring, rx_ring);

		ret = rx_work_done;
	} else {
		ret = budget;
	}

	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->tx_stats.napi_comp += napi_comp_call;
	tx_ring->tx_stats.tx_poll++;
	u64_stats_update_end(&tx_ring->syncp);

	return ret;
}

static irqreturn_t ena_intr_msix_mgmnt(int irq, void *data)
{
	struct ena_adapter *adapter = (struct ena_adapter *)data;

	ena_com_admin_q_comp_intr_handler(adapter->ena_dev);

	/* Don't call the aenq handler before probe is done */
	if (likely(test_bit(ENA_FLAG_DEVICE_RUNNING, &adapter->flags)))
		ena_com_aenq_intr_handler(adapter->ena_dev, data);

	return IRQ_HANDLED;
}

/* ena_intr_msix_io - MSI-X Interrupt Handler for Tx/Rx
 * @irq: interrupt number
 * @data: pointer to a network interface private napi device structure
 */
static irqreturn_t ena_intr_msix_io(int irq, void *data)
{
	struct ena_napi *ena_napi = data;

	ena_napi->tx_ring->first_interrupt = true;
	ena_napi->rx_ring->first_interrupt = true;

	napi_schedule_irqoff(&ena_napi->napi);

	return IRQ_HANDLED;
}

/* Reserve a single MSI-X vector for management (admin + aenq).
 * plus reserve one vector for each potential io queue.
 * the number of potential io queues is the minimum of what the device
 * supports and the number of vCPUs.
 */
static int ena_enable_msix(struct ena_adapter *adapter, int num_queues)
{
	int msix_vecs, irq_cnt;

	if (test_bit(ENA_FLAG_MSIX_ENABLED, &adapter->flags)) {
		netif_err(adapter, probe, adapter->netdev,
			  "Error, MSI-X is already enabled\n");
		return -EPERM;
	}

	/* Reserved the max msix vectors we might need */
	msix_vecs = ENA_MAX_MSIX_VEC(num_queues);
	netif_dbg(adapter, probe, adapter->netdev,
		  "trying to enable MSI-X, vectors %d\n", msix_vecs);

	irq_cnt = pci_alloc_irq_vectors(adapter->pdev, ENA_MIN_MSIX_VEC,
					msix_vecs, PCI_IRQ_MSIX);

	if (irq_cnt < 0) {
		netif_err(adapter, probe, adapter->netdev,
			  "Failed to enable MSI-X. irq_cnt %d\n", irq_cnt);
		return -ENOSPC;
	}

	if (irq_cnt != msix_vecs) {
		netif_notice(adapter, probe, adapter->netdev,
			     "enable only %d MSI-X (out of %d), reduce the number of queues\n",
			     irq_cnt, msix_vecs);
		adapter->num_queues = irq_cnt - ENA_ADMIN_MSIX_VEC;
	}

	if (ena_init_rx_cpu_rmap(adapter))
		netif_warn(adapter, probe, adapter->netdev,
			   "Failed to map IRQs to CPUs\n");

	adapter->msix_vecs = irq_cnt;
	set_bit(ENA_FLAG_MSIX_ENABLED, &adapter->flags);

	return 0;
}

static void ena_setup_mgmnt_intr(struct ena_adapter *adapter)
{
	u32 cpu;

	snprintf(adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].name,
		 ENA_IRQNAME_SIZE, "ena-mgmnt@pci:%s",
		 pci_name(adapter->pdev));
	adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].handler =
		ena_intr_msix_mgmnt;
	adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].data = adapter;
	adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].vector =
		pci_irq_vector(adapter->pdev, ENA_MGMNT_IRQ_IDX);
	cpu = cpumask_first(cpu_online_mask);
	adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].cpu = cpu;
	cpumask_set_cpu(cpu,
			&adapter->irq_tbl[ENA_MGMNT_IRQ_IDX].affinity_hint_mask);
}

static void ena_setup_io_intr(struct ena_adapter *adapter)
{
	struct net_device *netdev;
	int irq_idx, i, cpu;

	netdev = adapter->netdev;

	for (i = 0; i < adapter->num_queues; i++) {
		irq_idx = ENA_IO_IRQ_IDX(i);
		cpu = i % num_online_cpus();

		snprintf(adapter->irq_tbl[irq_idx].name, ENA_IRQNAME_SIZE,
			 "%s-Tx-Rx-%d", netdev->name, i);
		adapter->irq_tbl[irq_idx].handler = ena_intr_msix_io;
		adapter->irq_tbl[irq_idx].data = &adapter->ena_napi[i];
		adapter->irq_tbl[irq_idx].vector =
			pci_irq_vector(adapter->pdev, irq_idx);
		adapter->irq_tbl[irq_idx].cpu = cpu;

		cpumask_set_cpu(cpu,
				&adapter->irq_tbl[irq_idx].affinity_hint_mask);
	}
}

static int ena_request_mgmnt_irq(struct ena_adapter *adapter)
{
	unsigned long flags = 0;
	struct ena_irq *irq;
	int rc;

	irq = &adapter->irq_tbl[ENA_MGMNT_IRQ_IDX];
	rc = request_irq(irq->vector, irq->handler, flags, irq->name,
			 irq->data);
	if (rc) {
		netif_err(adapter, probe, adapter->netdev,
			  "failed to request admin irq\n");
		return rc;
	}

	netif_dbg(adapter, probe, adapter->netdev,
		  "set affinity hint of mgmnt irq.to 0x%lx (irq vector: %d)\n",
		  irq->affinity_hint_mask.bits[0], irq->vector);

	irq_set_affinity_hint(irq->vector, &irq->affinity_hint_mask);

	return rc;
}

static int ena_request_io_irq(struct ena_adapter *adapter)
{
	unsigned long flags = 0;
	struct ena_irq *irq;
	int rc = 0, i, k;

	if (!test_bit(ENA_FLAG_MSIX_ENABLED, &adapter->flags)) {
		netif_err(adapter, ifup, adapter->netdev,
			  "Failed to request I/O IRQ: MSI-X is not enabled\n");
		return -EINVAL;
	}

	for (i = ENA_IO_IRQ_FIRST_IDX; i < adapter->msix_vecs; i++) {
		irq = &adapter->irq_tbl[i];
		rc = request_irq(irq->vector, irq->handler, flags, irq->name,
				 irq->data);
		if (rc) {
			netif_err(adapter, ifup, adapter->netdev,
				  "Failed to request I/O IRQ. index %d rc %d\n",
				   i, rc);
			goto err;
		}

		netif_dbg(adapter, ifup, adapter->netdev,
			  "set affinity hint of irq. index %d to 0x%lx (irq vector: %d)\n",
			  i, irq->affinity_hint_mask.bits[0], irq->vector);

		irq_set_affinity_hint(irq->vector, &irq->affinity_hint_mask);
	}

	return rc;

err:
	for (k = ENA_IO_IRQ_FIRST_IDX; k < i; k++) {
		irq = &adapter->irq_tbl[k];
		free_irq(irq->vector, irq->data);
	}

	return rc;
}

static void ena_free_mgmnt_irq(struct ena_adapter *adapter)
{
	struct ena_irq *irq;

	irq = &adapter->irq_tbl[ENA_MGMNT_IRQ_IDX];
	synchronize_irq(irq->vector);
	irq_set_affinity_hint(irq->vector, NULL);
	free_irq(irq->vector, irq->data);
}

static void ena_free_io_irq(struct ena_adapter *adapter)
{
	struct ena_irq *irq;
	int i;

#ifdef CONFIG_RFS_ACCEL
	if (adapter->msix_vecs >= 1) {
		free_irq_cpu_rmap(adapter->netdev->rx_cpu_rmap);
		adapter->netdev->rx_cpu_rmap = NULL;
	}
#endif /* CONFIG_RFS_ACCEL */

	for (i = ENA_IO_IRQ_FIRST_IDX; i < adapter->msix_vecs; i++) {
		irq = &adapter->irq_tbl[i];
		irq_set_affinity_hint(irq->vector, NULL);
		free_irq(irq->vector, irq->data);
	}
}

static void ena_disable_msix(struct ena_adapter *adapter)
{
	if (test_and_clear_bit(ENA_FLAG_MSIX_ENABLED, &adapter->flags))
		pci_free_irq_vectors(adapter->pdev);
}

static void ena_disable_io_intr_sync(struct ena_adapter *adapter)
{
	int i;

	if (!netif_running(adapter->netdev))
		return;

	for (i = ENA_IO_IRQ_FIRST_IDX; i < adapter->msix_vecs; i++)
		synchronize_irq(adapter->irq_tbl[i].vector);
}

static void ena_del_napi(struct ena_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++)
		netif_napi_del(&adapter->ena_napi[i].napi);
}

static void ena_init_napi(struct ena_adapter *adapter)
{
	struct ena_napi *napi;
	int i;

	for (i = 0; i < adapter->num_queues; i++) {
		napi = &adapter->ena_napi[i];

		netif_napi_add(adapter->netdev,
			       &adapter->ena_napi[i].napi,
			       ena_io_poll,
			       ENA_NAPI_BUDGET);
		napi->rx_ring = &adapter->rx_ring[i];
		napi->tx_ring = &adapter->tx_ring[i];
		napi->qid = i;
	}
}

static void ena_napi_disable_all(struct ena_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++)
		napi_disable(&adapter->ena_napi[i].napi);
}

static void ena_napi_enable_all(struct ena_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++)
		napi_enable(&adapter->ena_napi[i].napi);
}

/* Configure the Rx forwarding */
static int ena_rss_configure(struct ena_adapter *adapter)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	int rc;

	/* In case the RSS table wasn't initialized by probe */
	if (!ena_dev->rss.tbl_log_size) {
		rc = ena_rss_init_default(adapter);
		if (rc && (rc != -EOPNOTSUPP)) {
			netif_err(adapter, ifup, adapter->netdev,
				  "Failed to init RSS rc: %d\n", rc);
			return rc;
		}
	}

	/* Set indirect table */
	rc = ena_com_indirect_table_set(ena_dev);
	if (unlikely(rc && rc != -EOPNOTSUPP))
		return rc;

	/* Configure hash function (if supported) */
	rc = ena_com_set_hash_function(ena_dev);
	if (unlikely(rc && (rc != -EOPNOTSUPP)))
		return rc;

	/* Configure hash inputs (if supported) */
	rc = ena_com_set_hash_ctrl(ena_dev);
	if (unlikely(rc && (rc != -EOPNOTSUPP)))
		return rc;

	return 0;
}

static int ena_up_complete(struct ena_adapter *adapter)
{
	int rc;

	rc = ena_rss_configure(adapter);
	if (rc)
		return rc;

	ena_change_mtu(adapter->netdev, adapter->netdev->mtu);

	ena_refill_all_rx_bufs(adapter);

	/* enable transmits */
	netif_tx_start_all_queues(adapter->netdev);

	ena_napi_enable_all(adapter);

	return 0;
}

static int ena_create_io_tx_queue(struct ena_adapter *adapter, int qid)
{
	struct ena_com_create_io_ctx ctx;
	struct ena_com_dev *ena_dev;
	struct ena_ring *tx_ring;
	u32 msix_vector;
	u16 ena_qid;
	int rc;

	ena_dev = adapter->ena_dev;

	tx_ring = &adapter->tx_ring[qid];
	msix_vector = ENA_IO_IRQ_IDX(qid);
	ena_qid = ENA_IO_TXQ_IDX(qid);

	memset(&ctx, 0x0, sizeof(ctx));

	ctx.direction = ENA_COM_IO_QUEUE_DIRECTION_TX;
	ctx.qid = ena_qid;
	ctx.mem_queue_type = ena_dev->tx_mem_queue_type;
	ctx.msix_vector = msix_vector;
	ctx.queue_size = tx_ring->ring_size;
	ctx.numa_node = cpu_to_node(tx_ring->cpu);

	rc = ena_com_create_io_queue(ena_dev, &ctx);
	if (rc) {
		netif_err(adapter, ifup, adapter->netdev,
			  "Failed to create I/O TX queue num %d rc: %d\n",
			  qid, rc);
		return rc;
	}

	rc = ena_com_get_io_handlers(ena_dev, ena_qid,
				     &tx_ring->ena_com_io_sq,
				     &tx_ring->ena_com_io_cq);
	if (rc) {
		netif_err(adapter, ifup, adapter->netdev,
			  "Failed to get TX queue handlers. TX queue num %d rc: %d\n",
			  qid, rc);
		ena_com_destroy_io_queue(ena_dev, ena_qid);
		return rc;
	}

	ena_com_update_numa_node(tx_ring->ena_com_io_cq, ctx.numa_node);
	return rc;
}

static int ena_create_all_io_tx_queues(struct ena_adapter *adapter)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	int rc, i;

	for (i = 0; i < adapter->num_queues; i++) {
		rc = ena_create_io_tx_queue(adapter, i);
		if (rc)
			goto create_err;
	}

	return 0;

create_err:
	while (i--)
		ena_com_destroy_io_queue(ena_dev, ENA_IO_TXQ_IDX(i));

	return rc;
}

static int ena_create_io_rx_queue(struct ena_adapter *adapter, int qid)
{
	struct ena_com_dev *ena_dev;
	struct ena_com_create_io_ctx ctx;
	struct ena_ring *rx_ring;
	u32 msix_vector;
	u16 ena_qid;
	int rc;

	ena_dev = adapter->ena_dev;

	rx_ring = &adapter->rx_ring[qid];
	msix_vector = ENA_IO_IRQ_IDX(qid);
	ena_qid = ENA_IO_RXQ_IDX(qid);

	memset(&ctx, 0x0, sizeof(ctx));

	ctx.qid = ena_qid;
	ctx.direction = ENA_COM_IO_QUEUE_DIRECTION_RX;
	ctx.mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
	ctx.msix_vector = msix_vector;
	ctx.queue_size = rx_ring->ring_size;
	ctx.numa_node = cpu_to_node(rx_ring->cpu);

	rc = ena_com_create_io_queue(ena_dev, &ctx);
	if (rc) {
		netif_err(adapter, ifup, adapter->netdev,
			  "Failed to create I/O RX queue num %d rc: %d\n",
			  qid, rc);
		return rc;
	}

	rc = ena_com_get_io_handlers(ena_dev, ena_qid,
				     &rx_ring->ena_com_io_sq,
				     &rx_ring->ena_com_io_cq);
	if (rc) {
		netif_err(adapter, ifup, adapter->netdev,
			  "Failed to get RX queue handlers. RX queue num %d rc: %d\n",
			  qid, rc);
		ena_com_destroy_io_queue(ena_dev, ena_qid);
		return rc;
	}

	ena_com_update_numa_node(rx_ring->ena_com_io_cq, ctx.numa_node);

	return rc;
}

static int ena_create_all_io_rx_queues(struct ena_adapter *adapter)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	int rc, i;

	for (i = 0; i < adapter->num_queues; i++) {
		rc = ena_create_io_rx_queue(adapter, i);
		if (rc)
			goto create_err;
		INIT_WORK(&adapter->ena_napi[i].dim.work, ena_dim_work);
	}

	return 0;

create_err:
	while (i--) {
		cancel_work_sync(&adapter->ena_napi[i].dim.work);
		ena_com_destroy_io_queue(ena_dev, ENA_IO_RXQ_IDX(i));
	}

	return rc;
}

static void set_io_rings_size(struct ena_adapter *adapter,
				     int new_tx_size, int new_rx_size)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++) {
		adapter->tx_ring[i].ring_size = new_tx_size;
		adapter->rx_ring[i].ring_size = new_rx_size;
	}
}

/* This function allows queue allocation to backoff when the system is
 * low on memory. If there is not enough memory to allocate io queues
 * the driver will try to allocate smaller queues.
 *
 * The backoff algorithm is as follows:
 *  1. Try to allocate TX and RX and if successful.
 *  1.1. return success
 *
 *  2. Divide by 2 the size of the larger of RX and TX queues (or both if their size is the same).
 *
 *  3. If TX or RX is smaller than 256
 *  3.1. return failure.
 *  4. else
 *  4.1. go back to 1.
 */
static int create_queues_with_size_backoff(struct ena_adapter *adapter)
{
	int rc, cur_rx_ring_size, cur_tx_ring_size;
	int new_rx_ring_size, new_tx_ring_size;

	/* current queue sizes might be set to smaller than the requested
	 * ones due to past queue allocation failures.
	 */
	set_io_rings_size(adapter, adapter->requested_tx_ring_size,
			  adapter->requested_rx_ring_size);

	while (1) {
		rc = ena_setup_all_tx_resources(adapter);
		if (rc)
			goto err_setup_tx;

		rc = ena_create_all_io_tx_queues(adapter);
		if (rc)
			goto err_create_tx_queues;

		rc = ena_setup_all_rx_resources(adapter);
		if (rc)
			goto err_setup_rx;

		rc = ena_create_all_io_rx_queues(adapter);
		if (rc)
			goto err_create_rx_queues;

		return 0;

err_create_rx_queues:
		ena_free_all_io_rx_resources(adapter);
err_setup_rx:
		ena_destroy_all_tx_queues(adapter);
err_create_tx_queues:
		ena_free_all_io_tx_resources(adapter);
err_setup_tx:
		if (rc != -ENOMEM) {
			netif_err(adapter, ifup, adapter->netdev,
				  "Queue creation failed with error code %d\n",
				  rc);
			return rc;
		}

		cur_tx_ring_size = adapter->tx_ring[0].ring_size;
		cur_rx_ring_size = adapter->rx_ring[0].ring_size;

		netif_err(adapter, ifup, adapter->netdev,
			  "Not enough memory to create queues with sizes TX=%d, RX=%d\n",
			  cur_tx_ring_size, cur_rx_ring_size);

		new_tx_ring_size = cur_tx_ring_size;
		new_rx_ring_size = cur_rx_ring_size;

		/* Decrease the size of the larger queue, or
		 * decrease both if they are the same size.
		 */
		if (cur_rx_ring_size <= cur_tx_ring_size)
			new_tx_ring_size = cur_tx_ring_size / 2;
		if (cur_rx_ring_size >= cur_tx_ring_size)
			new_rx_ring_size = cur_rx_ring_size / 2;

		if (new_tx_ring_size < ENA_MIN_RING_SIZE ||
		    new_rx_ring_size < ENA_MIN_RING_SIZE) {
			netif_err(adapter, ifup, adapter->netdev,
				  "Queue creation failed with the smallest possible queue size of %d for both queues. Not retrying with smaller queues\n",
				  ENA_MIN_RING_SIZE);
			return rc;
		}

		netif_err(adapter, ifup, adapter->netdev,
			  "Retrying queue creation with sizes TX=%d, RX=%d\n",
			  new_tx_ring_size,
			  new_rx_ring_size);

		set_io_rings_size(adapter, new_tx_ring_size,
				  new_rx_ring_size);
	}
}

static int ena_up(struct ena_adapter *adapter)
{
	int rc, i;

	netdev_dbg(adapter->netdev, "%s\n", __func__);

	ena_setup_io_intr(adapter);

	/* napi poll functions should be initialized before running
	 * request_irq(), to handle a rare condition where there is a pending
	 * interrupt, causing the ISR to fire immediately while the poll
	 * function wasn't set yet, causing a null dereference
	 */
	ena_init_napi(adapter);

	rc = ena_request_io_irq(adapter);
	if (rc)
		goto err_req_irq;

	rc = create_queues_with_size_backoff(adapter);
	if (rc)
		goto err_create_queues_with_backoff;

	rc = ena_up_complete(adapter);
	if (rc)
		goto err_up;

	if (test_bit(ENA_FLAG_LINK_UP, &adapter->flags))
		netif_carrier_on(adapter->netdev);

	u64_stats_update_begin(&adapter->syncp);
	adapter->dev_stats.interface_up++;
	u64_stats_update_end(&adapter->syncp);

	set_bit(ENA_FLAG_DEV_UP, &adapter->flags);

	/* Enable completion queues interrupt */
	for (i = 0; i < adapter->num_queues; i++)
		ena_unmask_interrupt(&adapter->tx_ring[i],
				     &adapter->rx_ring[i]);

	/* schedule napi in case we had pending packets
	 * from the last time we disable napi
	 */
	for (i = 0; i < adapter->num_queues; i++)
		napi_schedule(&adapter->ena_napi[i].napi);

	return rc;

err_up:
	ena_destroy_all_tx_queues(adapter);
	ena_free_all_io_tx_resources(adapter);
	ena_destroy_all_rx_queues(adapter);
	ena_free_all_io_rx_resources(adapter);
err_create_queues_with_backoff:
	ena_free_io_irq(adapter);
err_req_irq:
	ena_del_napi(adapter);

	return rc;
}

static void ena_down(struct ena_adapter *adapter)
{
	netif_info(adapter, ifdown, adapter->netdev, "%s\n", __func__);

	clear_bit(ENA_FLAG_DEV_UP, &adapter->flags);

	u64_stats_update_begin(&adapter->syncp);
	adapter->dev_stats.interface_down++;
	u64_stats_update_end(&adapter->syncp);

	netif_carrier_off(adapter->netdev);
	netif_tx_disable(adapter->netdev);

	/* After this point the napi handler won't enable the tx queue */
	ena_napi_disable_all(adapter);

	/* After destroy the queue there won't be any new interrupts */

	if (test_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags)) {
		int rc;

		rc = ena_com_dev_reset(adapter->ena_dev, adapter->reset_reason);
		if (rc)
			dev_err(&adapter->pdev->dev, "Device reset failed\n");
		/* stop submitting admin commands on a device that was reset */
		ena_com_set_admin_running_state(adapter->ena_dev, false);
	}

	ena_destroy_all_io_queues(adapter);

	ena_disable_io_intr_sync(adapter);
	ena_free_io_irq(adapter);
	ena_del_napi(adapter);

	ena_free_all_tx_bufs(adapter);
	ena_free_all_rx_bufs(adapter);
	ena_free_all_io_tx_resources(adapter);
	ena_free_all_io_rx_resources(adapter);
}

/* ena_open - Called when a network interface is made active
 * @netdev: network interface device structure
 *
 * Returns 0 on success, negative value on failure
 *
 * The open entry point is called when a network interface is made
 * active by the system (IFF_UP).  At this point all resources needed
 * for transmit and receive operations are allocated, the interrupt
 * handler is registered with the OS, the watchdog timer is started,
 * and the stack is notified that the interface is ready.
 */
static int ena_open(struct net_device *netdev)
{
	struct ena_adapter *adapter = netdev_priv(netdev);
	int rc;

	/* Notify the stack of the actual queue counts. */
	rc = netif_set_real_num_tx_queues(netdev, adapter->num_queues);
	if (rc) {
		netif_err(adapter, ifup, netdev, "Can't set num tx queues\n");
		return rc;
	}

	rc = netif_set_real_num_rx_queues(netdev, adapter->num_queues);
	if (rc) {
		netif_err(adapter, ifup, netdev, "Can't set num rx queues\n");
		return rc;
	}

	rc = ena_up(adapter);
	if (rc)
		return rc;

	return rc;
}

/* ena_close - Disables a network interface
 * @netdev: network interface device structure
 *
 * Returns 0, this is not allowed to fail
 *
 * The close entry point is called when an interface is de-activated
 * by the OS.  The hardware is still under the drivers control, but
 * needs to be disabled.  A global MAC reset is issued to stop the
 * hardware, and all transmit and receive resources are freed.
 */
static int ena_close(struct net_device *netdev)
{
	struct ena_adapter *adapter = netdev_priv(netdev);

	netif_dbg(adapter, ifdown, netdev, "%s\n", __func__);

	if (!test_bit(ENA_FLAG_DEVICE_RUNNING, &adapter->flags))
		return 0;

	if (test_bit(ENA_FLAG_DEV_UP, &adapter->flags))
		ena_down(adapter);

	/* Check for device status and issue reset if needed*/
	check_for_admin_com_state(adapter);
	if (unlikely(test_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags))) {
		netif_err(adapter, ifdown, adapter->netdev,
			  "Destroy failure, restarting device\n");
		ena_dump_stats_to_dmesg(adapter);
		/* rtnl lock already obtained in dev_ioctl() layer */
		ena_destroy_device(adapter, false);
		ena_restore_device(adapter);
	}

	return 0;
}

int ena_update_queue_sizes(struct ena_adapter *adapter,
			   u32 new_tx_size,
			   u32 new_rx_size)
{
	bool dev_up;

	dev_up = test_bit(ENA_FLAG_DEV_UP, &adapter->flags);
	ena_close(adapter->netdev);
	adapter->requested_tx_ring_size = new_tx_size;
	adapter->requested_rx_ring_size = new_rx_size;
	ena_init_io_rings(adapter);
	return dev_up ? ena_up(adapter) : 0;
}

static void ena_tx_csum(struct ena_com_tx_ctx *ena_tx_ctx, struct sk_buff *skb)
{
	u32 mss = skb_shinfo(skb)->gso_size;
	struct ena_com_tx_meta *ena_meta = &ena_tx_ctx->ena_meta;
	u8 l4_protocol = 0;

	if ((skb->ip_summed == CHECKSUM_PARTIAL) || mss) {
		ena_tx_ctx->l4_csum_enable = 1;
		if (mss) {
			ena_tx_ctx->tso_enable = 1;
			ena_meta->l4_hdr_len = tcp_hdr(skb)->doff;
			ena_tx_ctx->l4_csum_partial = 0;
		} else {
			ena_tx_ctx->tso_enable = 0;
			ena_meta->l4_hdr_len = 0;
			ena_tx_ctx->l4_csum_partial = 1;
		}

		switch (ip_hdr(skb)->version) {
		case IPVERSION:
			ena_tx_ctx->l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
			if (ip_hdr(skb)->frag_off & htons(IP_DF))
				ena_tx_ctx->df = 1;
			if (mss)
				ena_tx_ctx->l3_csum_enable = 1;
			l4_protocol = ip_hdr(skb)->protocol;
			break;
		case 6:
			ena_tx_ctx->l3_proto = ENA_ETH_IO_L3_PROTO_IPV6;
			l4_protocol = ipv6_hdr(skb)->nexthdr;
			break;
		default:
			break;
		}

		if (l4_protocol == IPPROTO_TCP)
			ena_tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
		else
			ena_tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_UDP;

		ena_meta->mss = mss;
		ena_meta->l3_hdr_len = skb_network_header_len(skb);
		ena_meta->l3_hdr_offset = skb_network_offset(skb);
		ena_tx_ctx->meta_valid = 1;

	} else {
		ena_tx_ctx->meta_valid = 0;
	}
}

static int ena_check_and_linearize_skb(struct ena_ring *tx_ring,
				       struct sk_buff *skb)
{
	int num_frags, header_len, rc;

	num_frags = skb_shinfo(skb)->nr_frags;
	header_len = skb_headlen(skb);

	if (num_frags < tx_ring->sgl_size)
		return 0;

	if ((num_frags == tx_ring->sgl_size) &&
	    (header_len < tx_ring->tx_max_header_size))
		return 0;

	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->tx_stats.linearize++;
	u64_stats_update_end(&tx_ring->syncp);

	rc = skb_linearize(skb);
	if (unlikely(rc)) {
		u64_stats_update_begin(&tx_ring->syncp);
		tx_ring->tx_stats.linearize_failed++;
		u64_stats_update_end(&tx_ring->syncp);
	}

	return rc;
}

static int ena_tx_map_skb(struct ena_ring *tx_ring,
			  struct ena_tx_buffer *tx_info,
			  struct sk_buff *skb,
			  void **push_hdr,
			  u16 *header_len)
{
	struct ena_adapter *adapter = tx_ring->adapter;
	struct ena_com_buf *ena_buf;
	dma_addr_t dma;
	u32 skb_head_len, frag_len, last_frag;
	u16 push_len = 0;
	u16 delta = 0;
	int i = 0;

	skb_head_len = skb_headlen(skb);
	tx_info->skb = skb;
	ena_buf = tx_info->bufs;

	if (tx_ring->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
		/* When the device is LLQ mode, the driver will copy
		 * the header into the device memory space.
		 * the ena_com layer assume the header is in a linear
		 * memory space.
		 * This assumption might be wrong since part of the header
		 * can be in the fragmented buffers.
		 * Use skb_header_pointer to make sure the header is in a
		 * linear memory space.
		 */

		push_len = min_t(u32, skb->len, tx_ring->tx_max_header_size);
		*push_hdr = skb_header_pointer(skb, 0, push_len,
					       tx_ring->push_buf_intermediate_buf);
		*header_len = push_len;
		if (unlikely(skb->data != *push_hdr)) {
			u64_stats_update_begin(&tx_ring->syncp);
			tx_ring->tx_stats.llq_buffer_copy++;
			u64_stats_update_end(&tx_ring->syncp);

			delta = push_len - skb_head_len;
		}
	} else {
		*push_hdr = NULL;
		*header_len = min_t(u32, skb_head_len,
				    tx_ring->tx_max_header_size);
	}

	netif_dbg(adapter, tx_queued, adapter->netdev,
		  "skb: %p header_buf->vaddr: %p push_len: %d\n", skb,
		  *push_hdr, push_len);

	if (skb_head_len > push_len) {
		dma = dma_map_single(tx_ring->dev, skb->data + push_len,
				     skb_head_len - push_len, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(tx_ring->dev, dma)))
			goto error_report_dma_error;

		ena_buf->paddr = dma;
		ena_buf->len = skb_head_len - push_len;

		ena_buf++;
		tx_info->num_of_bufs++;
		tx_info->map_linear_data = 1;
	} else {
		tx_info->map_linear_data = 0;
	}

	last_frag = skb_shinfo(skb)->nr_frags;

	for (i = 0; i < last_frag; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		frag_len = skb_frag_size(frag);

		if (unlikely(delta >= frag_len)) {
			delta -= frag_len;
			continue;
		}

		dma = skb_frag_dma_map(tx_ring->dev, frag, delta,
				       frag_len - delta, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(tx_ring->dev, dma)))
			goto error_report_dma_error;

		ena_buf->paddr = dma;
		ena_buf->len = frag_len - delta;
		ena_buf++;
		tx_info->num_of_bufs++;
		delta = 0;
	}

	return 0;

error_report_dma_error:
	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->tx_stats.dma_mapping_err++;
	u64_stats_update_end(&tx_ring->syncp);
	netdev_warn(adapter->netdev, "failed to map skb\n");

	tx_info->skb = NULL;

	tx_info->num_of_bufs += i;
	ena_unmap_tx_skb(tx_ring, tx_info);

	return -EINVAL;
}

/* Called with netif_tx_lock. */
static netdev_tx_t ena_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ena_adapter *adapter = netdev_priv(dev);
	struct ena_tx_buffer *tx_info;
	struct ena_com_tx_ctx ena_tx_ctx;
	struct ena_ring *tx_ring;
	struct netdev_queue *txq;
	void *push_hdr;
	u16 next_to_use, req_id, header_len;
	int qid, rc, nb_hw_desc;

	netif_dbg(adapter, tx_queued, dev, "%s skb %p\n", __func__, skb);
	/*  Determine which tx ring we will be placed on */
	qid = skb_get_queue_mapping(skb);
	tx_ring = &adapter->tx_ring[qid];
	txq = netdev_get_tx_queue(dev, qid);

	rc = ena_check_and_linearize_skb(tx_ring, skb);
	if (unlikely(rc))
		goto error_drop_packet;

	skb_tx_timestamp(skb);

	next_to_use = tx_ring->next_to_use;
	req_id = tx_ring->free_ids[next_to_use];
	tx_info = &tx_ring->tx_buffer_info[req_id];
	tx_info->num_of_bufs = 0;

	WARN(tx_info->skb, "SKB isn't NULL req_id %d\n", req_id);

	rc = ena_tx_map_skb(tx_ring, tx_info, skb, &push_hdr, &header_len);
	if (unlikely(rc))
		goto error_drop_packet;

	memset(&ena_tx_ctx, 0x0, sizeof(struct ena_com_tx_ctx));
	ena_tx_ctx.ena_bufs = tx_info->bufs;
	ena_tx_ctx.push_header = push_hdr;
	ena_tx_ctx.num_bufs = tx_info->num_of_bufs;
	ena_tx_ctx.req_id = req_id;
	ena_tx_ctx.header_len = header_len;

	/* set flags and meta data */
	ena_tx_csum(&ena_tx_ctx, skb);

	if (unlikely(ena_com_is_doorbell_needed(tx_ring->ena_com_io_sq, &ena_tx_ctx))) {
		netif_dbg(adapter, tx_queued, dev,
			  "llq tx max burst size of queue %d achieved, writing doorbell to send burst\n",
			  qid);
		ena_com_write_sq_doorbell(tx_ring->ena_com_io_sq);
	}

	/* prepare the packet's descriptors to dma engine */
	rc = ena_com_prepare_tx(tx_ring->ena_com_io_sq, &ena_tx_ctx,
				&nb_hw_desc);

	/* ena_com_prepare_tx() can't fail due to overflow of tx queue,
	 * since the number of free descriptors in the queue is checked
	 * after sending the previous packet. In case there isn't enough
	 * space in the queue for the next packet, it is stopped
	 * until there is again enough available space in the queue.
	 * All other failure reasons of ena_com_prepare_tx() are fatal
	 * and therefore require a device reset.
	 */
	if (unlikely(rc)) {
		netif_err(adapter, tx_queued, dev,
			  "failed to prepare tx bufs\n");
		u64_stats_update_begin(&tx_ring->syncp);
		tx_ring->tx_stats.prepare_ctx_err++;
		u64_stats_update_end(&tx_ring->syncp);
		adapter->reset_reason = ENA_REGS_RESET_DRIVER_INVALID_STATE;
		set_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);
		goto error_unmap_dma;
	}

	netdev_tx_sent_queue(txq, skb->len);

	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->tx_stats.cnt++;
	tx_ring->tx_stats.bytes += skb->len;
	u64_stats_update_end(&tx_ring->syncp);

	tx_info->tx_descs = nb_hw_desc;
	tx_info->last_jiffies = jiffies;
	tx_info->print_once = 0;

	tx_ring->next_to_use = ENA_TX_RING_IDX_NEXT(next_to_use,
		tx_ring->ring_size);

	/* stop the queue when no more space available, the packet can have up
	 * to sgl_size + 2. one for the meta descriptor and one for header
	 * (if the header is larger than tx_max_header_size).
	 */
	if (unlikely(!ena_com_sq_have_enough_space(tx_ring->ena_com_io_sq,
						   tx_ring->sgl_size + 2))) {
		netif_dbg(adapter, tx_queued, dev, "%s stop queue %d\n",
			  __func__, qid);

		netif_tx_stop_queue(txq);
		u64_stats_update_begin(&tx_ring->syncp);
		tx_ring->tx_stats.queue_stop++;
		u64_stats_update_end(&tx_ring->syncp);

		/* There is a rare condition where this function decide to
		 * stop the queue but meanwhile clean_tx_irq updates
		 * next_to_completion and terminates.
		 * The queue will remain stopped forever.
		 * To solve this issue add a mb() to make sure that
		 * netif_tx_stop_queue() write is vissible before checking if
		 * there is additional space in the queue.
		 */
		smp_mb();

		if (ena_com_sq_have_enough_space(tx_ring->ena_com_io_sq,
						 ENA_TX_WAKEUP_THRESH)) {
			netif_tx_wake_queue(txq);
			u64_stats_update_begin(&tx_ring->syncp);
			tx_ring->tx_stats.queue_wakeup++;
			u64_stats_update_end(&tx_ring->syncp);
		}
	}

	if (netif_xmit_stopped(txq) || !netdev_xmit_more()) {
		/* trigger the dma engine. ena_com_write_sq_doorbell()
		 * has a mb
		 */
		ena_com_write_sq_doorbell(tx_ring->ena_com_io_sq);
		u64_stats_update_begin(&tx_ring->syncp);
		tx_ring->tx_stats.doorbells++;
		u64_stats_update_end(&tx_ring->syncp);
	}

	return NETDEV_TX_OK;

error_unmap_dma:
	ena_unmap_tx_skb(tx_ring, tx_info);
	tx_info->skb = NULL;

error_drop_packet:
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static u16 ena_select_queue(struct net_device *dev, struct sk_buff *skb,
			    struct net_device *sb_dev)
{
	u16 qid;
	/* we suspect that this is good for in--kernel network services that
	 * want to loop incoming skb rx to tx in normal user generated traffic,
	 * most probably we will not get to this
	 */
	if (skb_rx_queue_recorded(skb))
		qid = skb_get_rx_queue(skb);
	else
		qid = netdev_pick_tx(dev, skb, NULL);

	return qid;
}

static void ena_config_host_info(struct ena_com_dev *ena_dev,
				 struct pci_dev *pdev)
{
	struct ena_admin_host_info *host_info;
	int rc;

	/* Allocate only the host info */
	rc = ena_com_allocate_host_info(ena_dev);
	if (rc) {
		pr_err("Cannot allocate host info\n");
		return;
	}

	host_info = ena_dev->host_attr.host_info;

	host_info->bdf = (pdev->bus->number << 8) | pdev->devfn;
	host_info->os_type = ENA_ADMIN_OS_LINUX;
	host_info->kernel_ver = LINUX_VERSION_CODE;
	strlcpy(host_info->kernel_ver_str, utsname()->version,
		sizeof(host_info->kernel_ver_str) - 1);
	host_info->os_dist = 0;
	strncpy(host_info->os_dist_str, utsname()->release,
		sizeof(host_info->os_dist_str) - 1);
	host_info->driver_version =
		(DRV_MODULE_VER_MAJOR) |
		(DRV_MODULE_VER_MINOR << ENA_ADMIN_HOST_INFO_MINOR_SHIFT) |
		(DRV_MODULE_VER_SUBMINOR << ENA_ADMIN_HOST_INFO_SUB_MINOR_SHIFT) |
		("K"[0] << ENA_ADMIN_HOST_INFO_MODULE_TYPE_SHIFT);
	host_info->num_cpus = num_online_cpus();

	host_info->driver_supported_features =
		ENA_ADMIN_HOST_INFO_INTERRUPT_MODERATION_MASK;

	rc = ena_com_set_host_attributes(ena_dev);
	if (rc) {
		if (rc == -EOPNOTSUPP)
			pr_warn("Cannot set host attributes\n");
		else
			pr_err("Cannot set host attributes\n");

		goto err;
	}

	return;

err:
	ena_com_delete_host_info(ena_dev);
}

static void ena_config_debug_area(struct ena_adapter *adapter)
{
	u32 debug_area_size;
	int rc, ss_count;

	ss_count = ena_get_sset_count(adapter->netdev, ETH_SS_STATS);
	if (ss_count <= 0) {
		netif_err(adapter, drv, adapter->netdev,
			  "SS count is negative\n");
		return;
	}

	/* allocate 32 bytes for each string and 64bit for the value */
	debug_area_size = ss_count * ETH_GSTRING_LEN + sizeof(u64) * ss_count;

	rc = ena_com_allocate_debug_area(adapter->ena_dev, debug_area_size);
	if (rc) {
		pr_err("Cannot allocate debug area\n");
		return;
	}

	rc = ena_com_set_host_attributes(adapter->ena_dev);
	if (rc) {
		if (rc == -EOPNOTSUPP)
			netif_warn(adapter, drv, adapter->netdev,
				   "Cannot set host attributes\n");
		else
			netif_err(adapter, drv, adapter->netdev,
				  "Cannot set host attributes\n");
		goto err;
	}

	return;
err:
	ena_com_delete_debug_area(adapter->ena_dev);
}

static void ena_get_stats64(struct net_device *netdev,
			    struct rtnl_link_stats64 *stats)
{
	struct ena_adapter *adapter = netdev_priv(netdev);
	struct ena_ring *rx_ring, *tx_ring;
	unsigned int start;
	u64 rx_drops;
	int i;

	if (!test_bit(ENA_FLAG_DEV_UP, &adapter->flags))
		return;

	for (i = 0; i < adapter->num_queues; i++) {
		u64 bytes, packets;

		tx_ring = &adapter->tx_ring[i];

		do {
			start = u64_stats_fetch_begin_irq(&tx_ring->syncp);
			packets = tx_ring->tx_stats.cnt;
			bytes = tx_ring->tx_stats.bytes;
		} while (u64_stats_fetch_retry_irq(&tx_ring->syncp, start));

		stats->tx_packets += packets;
		stats->tx_bytes += bytes;

		rx_ring = &adapter->rx_ring[i];

		do {
			start = u64_stats_fetch_begin_irq(&rx_ring->syncp);
			packets = rx_ring->rx_stats.cnt;
			bytes = rx_ring->rx_stats.bytes;
		} while (u64_stats_fetch_retry_irq(&rx_ring->syncp, start));

		stats->rx_packets += packets;
		stats->rx_bytes += bytes;
	}

	do {
		start = u64_stats_fetch_begin_irq(&adapter->syncp);
		rx_drops = adapter->dev_stats.rx_drops;
	} while (u64_stats_fetch_retry_irq(&adapter->syncp, start));

	stats->rx_dropped = rx_drops;

	stats->multicast = 0;
	stats->collisions = 0;

	stats->rx_length_errors = 0;
	stats->rx_crc_errors = 0;
	stats->rx_frame_errors = 0;
	stats->rx_fifo_errors = 0;
	stats->rx_missed_errors = 0;
	stats->tx_window_errors = 0;

	stats->rx_errors = 0;
	stats->tx_errors = 0;
}

static const struct net_device_ops ena_netdev_ops = {
	.ndo_open		= ena_open,
	.ndo_stop		= ena_close,
	.ndo_start_xmit		= ena_start_xmit,
	.ndo_select_queue	= ena_select_queue,
	.ndo_get_stats64	= ena_get_stats64,
	.ndo_tx_timeout		= ena_tx_timeout,
	.ndo_change_mtu		= ena_change_mtu,
	.ndo_set_mac_address	= NULL,
	.ndo_validate_addr	= eth_validate_addr,
};

static int ena_device_validate_params(struct ena_adapter *adapter,
				      struct ena_com_dev_get_features_ctx *get_feat_ctx)
{
	struct net_device *netdev = adapter->netdev;
	int rc;

	rc = ether_addr_equal(get_feat_ctx->dev_attr.mac_addr,
			      adapter->mac_addr);
	if (!rc) {
		netif_err(adapter, drv, netdev,
			  "Error, mac address are different\n");
		return -EINVAL;
	}

	if (get_feat_ctx->dev_attr.max_mtu < netdev->mtu) {
		netif_err(adapter, drv, netdev,
			  "Error, device max mtu is smaller than netdev MTU\n");
		return -EINVAL;
	}

	return 0;
}

static int ena_device_init(struct ena_com_dev *ena_dev, struct pci_dev *pdev,
			   struct ena_com_dev_get_features_ctx *get_feat_ctx,
			   bool *wd_state)
{
	struct device *dev = &pdev->dev;
	bool readless_supported;
	u32 aenq_groups;
	int dma_width;
	int rc;

	rc = ena_com_mmio_reg_read_request_init(ena_dev);
	if (rc) {
		dev_err(dev, "failed to init mmio read less\n");
		return rc;
	}

	/* The PCIe configuration space revision id indicate if mmio reg
	 * read is disabled
	 */
	readless_supported = !(pdev->revision & ENA_MMIO_DISABLE_REG_READ);
	ena_com_set_mmio_read_mode(ena_dev, readless_supported);

	rc = ena_com_dev_reset(ena_dev, ENA_REGS_RESET_NORMAL);
	if (rc) {
		dev_err(dev, "Can not reset device\n");
		goto err_mmio_read_less;
	}

	rc = ena_com_validate_version(ena_dev);
	if (rc) {
		dev_err(dev, "device version is too low\n");
		goto err_mmio_read_less;
	}

	dma_width = ena_com_get_dma_width(ena_dev);
	if (dma_width < 0) {
		dev_err(dev, "Invalid dma width value %d", dma_width);
		rc = dma_width;
		goto err_mmio_read_less;
	}

	rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(dma_width));
	if (rc) {
		dev_err(dev, "pci_set_dma_mask failed 0x%x\n", rc);
		goto err_mmio_read_less;
	}

	rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(dma_width));
	if (rc) {
		dev_err(dev, "err_pci_set_consistent_dma_mask failed 0x%x\n",
			rc);
		goto err_mmio_read_less;
	}

	/* ENA admin level init */
	rc = ena_com_admin_init(ena_dev, &aenq_handlers);
	if (rc) {
		dev_err(dev,
			"Can not initialize ena admin queue with device\n");
		goto err_mmio_read_less;
	}

	/* To enable the msix interrupts the driver needs to know the number
	 * of queues. So the driver uses polling mode to retrieve this
	 * information
	 */
	ena_com_set_admin_polling_mode(ena_dev, true);

	ena_config_host_info(ena_dev, pdev);

	/* Get Device Attributes*/
	rc = ena_com_get_dev_attr_feat(ena_dev, get_feat_ctx);
	if (rc) {
		dev_err(dev, "Cannot get attribute for ena device rc=%d\n", rc);
		goto err_admin_init;
	}

	/* Try to turn all the available aenq groups */
	aenq_groups = BIT(ENA_ADMIN_LINK_CHANGE) |
		BIT(ENA_ADMIN_FATAL_ERROR) |
		BIT(ENA_ADMIN_WARNING) |
		BIT(ENA_ADMIN_NOTIFICATION) |
		BIT(ENA_ADMIN_KEEP_ALIVE);

	aenq_groups &= get_feat_ctx->aenq.supported_groups;

	rc = ena_com_set_aenq_config(ena_dev, aenq_groups);
	if (rc) {
		dev_err(dev, "Cannot configure aenq groups rc= %d\n", rc);
		goto err_admin_init;
	}

	*wd_state = !!(aenq_groups & BIT(ENA_ADMIN_KEEP_ALIVE));

	return 0;

err_admin_init:
	ena_com_delete_host_info(ena_dev);
	ena_com_admin_destroy(ena_dev);
err_mmio_read_less:
	ena_com_mmio_reg_read_request_destroy(ena_dev);

	return rc;
}

static int ena_enable_msix_and_set_admin_interrupts(struct ena_adapter *adapter,
						    int io_vectors)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	struct device *dev = &adapter->pdev->dev;
	int rc;

	rc = ena_enable_msix(adapter, io_vectors);
	if (rc) {
		dev_err(dev, "Can not reserve msix vectors\n");
		return rc;
	}

	ena_setup_mgmnt_intr(adapter);

	rc = ena_request_mgmnt_irq(adapter);
	if (rc) {
		dev_err(dev, "Can not setup management interrupts\n");
		goto err_disable_msix;
	}

	ena_com_set_admin_polling_mode(ena_dev, false);

	ena_com_admin_aenq_enable(ena_dev);

	return 0;

err_disable_msix:
	ena_disable_msix(adapter);

	return rc;
}

static void ena_destroy_device(struct ena_adapter *adapter, bool graceful)
{
	struct net_device *netdev = adapter->netdev;
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	bool dev_up;

	if (!test_bit(ENA_FLAG_DEVICE_RUNNING, &adapter->flags))
		return;

	netif_carrier_off(netdev);

	del_timer_sync(&adapter->timer_service);

	dev_up = test_bit(ENA_FLAG_DEV_UP, &adapter->flags);
	adapter->dev_up_before_reset = dev_up;
	if (!graceful)
		ena_com_set_admin_running_state(ena_dev, false);

	if (test_bit(ENA_FLAG_DEV_UP, &adapter->flags))
		ena_down(adapter);

	/* Stop the device from sending AENQ events (in case reset flag is set
	 *  and device is up, ena_down() already reset the device.
	 */
	if (!(test_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags) && dev_up))
		ena_com_dev_reset(adapter->ena_dev, adapter->reset_reason);

	ena_free_mgmnt_irq(adapter);

	ena_disable_msix(adapter);

	ena_com_abort_admin_commands(ena_dev);

	ena_com_wait_for_abort_completion(ena_dev);

	ena_com_admin_destroy(ena_dev);

	ena_com_mmio_reg_read_request_destroy(ena_dev);

	adapter->reset_reason = ENA_REGS_RESET_NORMAL;

	clear_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);
	clear_bit(ENA_FLAG_DEVICE_RUNNING, &adapter->flags);
}

static int ena_restore_device(struct ena_adapter *adapter)
{
	struct ena_com_dev_get_features_ctx get_feat_ctx;
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	struct pci_dev *pdev = adapter->pdev;
	bool wd_state;
	int rc;

	set_bit(ENA_FLAG_ONGOING_RESET, &adapter->flags);
	rc = ena_device_init(ena_dev, adapter->pdev, &get_feat_ctx, &wd_state);
	if (rc) {
		dev_err(&pdev->dev, "Can not initialize device\n");
		goto err;
	}
	adapter->wd_state = wd_state;

	rc = ena_device_validate_params(adapter, &get_feat_ctx);
	if (rc) {
		dev_err(&pdev->dev, "Validation of device parameters failed\n");
		goto err_device_destroy;
	}

	rc = ena_enable_msix_and_set_admin_interrupts(adapter,
						      adapter->num_queues);
	if (rc) {
		dev_err(&pdev->dev, "Enable MSI-X failed\n");
		goto err_device_destroy;
	}
	/* If the interface was up before the reset bring it up */
	if (adapter->dev_up_before_reset) {
		rc = ena_up(adapter);
		if (rc) {
			dev_err(&pdev->dev, "Failed to create I/O queues\n");
			goto err_disable_msix;
		}
	}

	set_bit(ENA_FLAG_DEVICE_RUNNING, &adapter->flags);

	clear_bit(ENA_FLAG_ONGOING_RESET, &adapter->flags);
	if (test_bit(ENA_FLAG_LINK_UP, &adapter->flags))
		netif_carrier_on(adapter->netdev);

	mod_timer(&adapter->timer_service, round_jiffies(jiffies + HZ));
	dev_err(&pdev->dev,
		"Device reset completed successfully, Driver info: %s\n",
		version);

	return rc;
err_disable_msix:
	ena_free_mgmnt_irq(adapter);
	ena_disable_msix(adapter);
err_device_destroy:
	ena_com_abort_admin_commands(ena_dev);
	ena_com_wait_for_abort_completion(ena_dev);
	ena_com_admin_destroy(ena_dev);
	ena_com_dev_reset(ena_dev, ENA_REGS_RESET_DRIVER_INVALID_STATE);
	ena_com_mmio_reg_read_request_destroy(ena_dev);
err:
	clear_bit(ENA_FLAG_DEVICE_RUNNING, &adapter->flags);
	clear_bit(ENA_FLAG_ONGOING_RESET, &adapter->flags);
	dev_err(&pdev->dev,
		"Reset attempt failed. Can not reset the device\n");

	return rc;
}

static void ena_fw_reset_device(struct work_struct *work)
{
	struct ena_adapter *adapter =
		container_of(work, struct ena_adapter, reset_task);

	rtnl_lock();

	if (likely(test_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags))) {
		ena_destroy_device(adapter, false);
		ena_restore_device(adapter);
	}

	rtnl_unlock();
}

static int check_for_rx_interrupt_queue(struct ena_adapter *adapter,
					struct ena_ring *rx_ring)
{
	if (likely(rx_ring->first_interrupt))
		return 0;

	if (ena_com_cq_empty(rx_ring->ena_com_io_cq))
		return 0;

	rx_ring->no_interrupt_event_cnt++;

	if (rx_ring->no_interrupt_event_cnt == ENA_MAX_NO_INTERRUPT_ITERATIONS) {
		netif_err(adapter, rx_err, adapter->netdev,
			  "Potential MSIX issue on Rx side Queue = %d. Reset the device\n",
			  rx_ring->qid);
		adapter->reset_reason = ENA_REGS_RESET_MISS_INTERRUPT;
		smp_mb__before_atomic();
		set_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);
		return -EIO;
	}

	return 0;
}

static int check_missing_comp_in_tx_queue(struct ena_adapter *adapter,
					  struct ena_ring *tx_ring)
{
	struct ena_tx_buffer *tx_buf;
	unsigned long last_jiffies;
	u32 missed_tx = 0;
	int i, rc = 0;

	for (i = 0; i < tx_ring->ring_size; i++) {
		tx_buf = &tx_ring->tx_buffer_info[i];
		last_jiffies = tx_buf->last_jiffies;

		if (last_jiffies == 0)
			/* no pending Tx at this location */
			continue;

		if (unlikely(!tx_ring->first_interrupt && time_is_before_jiffies(last_jiffies +
			     2 * adapter->missing_tx_completion_to))) {
			/* If after graceful period interrupt is still not
			 * received, we schedule a reset
			 */
			netif_err(adapter, tx_err, adapter->netdev,
				  "Potential MSIX issue on Tx side Queue = %d. Reset the device\n",
				  tx_ring->qid);
			adapter->reset_reason = ENA_REGS_RESET_MISS_INTERRUPT;
			smp_mb__before_atomic();
			set_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);
			return -EIO;
		}

		if (unlikely(time_is_before_jiffies(last_jiffies +
				adapter->missing_tx_completion_to))) {
			if (!tx_buf->print_once)
				netif_notice(adapter, tx_err, adapter->netdev,
					     "Found a Tx that wasn't completed on time, qid %d, index %d.\n",
					     tx_ring->qid, i);

			tx_buf->print_once = 1;
			missed_tx++;
		}
	}

	if (unlikely(missed_tx > adapter->missing_tx_completion_threshold)) {
		netif_err(adapter, tx_err, adapter->netdev,
			  "The number of lost tx completions is above the threshold (%d > %d). Reset the device\n",
			  missed_tx,
			  adapter->missing_tx_completion_threshold);
		adapter->reset_reason =
			ENA_REGS_RESET_MISS_TX_CMPL;
		set_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);
		rc = -EIO;
	}

	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->tx_stats.missed_tx += missed_tx;
	u64_stats_update_end(&tx_ring->syncp);

	return rc;
}

static void check_for_missing_completions(struct ena_adapter *adapter)
{
	struct ena_ring *tx_ring;
	struct ena_ring *rx_ring;
	int i, budget, rc;

	/* Make sure the driver doesn't turn the device in other process */
	smp_rmb();

	if (!test_bit(ENA_FLAG_DEV_UP, &adapter->flags))
		return;

	if (test_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags))
		return;

	if (adapter->missing_tx_completion_to == ENA_HW_HINTS_NO_TIMEOUT)
		return;

	budget = ENA_MONITORED_TX_QUEUES;

	for (i = adapter->last_monitored_tx_qid; i < adapter->num_queues; i++) {
		tx_ring = &adapter->tx_ring[i];
		rx_ring = &adapter->rx_ring[i];

		rc = check_missing_comp_in_tx_queue(adapter, tx_ring);
		if (unlikely(rc))
			return;

		rc = check_for_rx_interrupt_queue(adapter, rx_ring);
		if (unlikely(rc))
			return;

		budget--;
		if (!budget)
			break;
	}

	adapter->last_monitored_tx_qid = i % adapter->num_queues;
}

/* trigger napi schedule after 2 consecutive detections */
#define EMPTY_RX_REFILL 2
/* For the rare case where the device runs out of Rx descriptors and the
 * napi handler failed to refill new Rx descriptors (due to a lack of memory
 * for example).
 * This case will lead to a deadlock:
 * The device won't send interrupts since all the new Rx packets will be dropped
 * The napi handler won't allocate new Rx descriptors so the device will be
 * able to send new packets.
 *
 * This scenario can happen when the kernel's vm.min_free_kbytes is too small.
 * It is recommended to have at least 512MB, with a minimum of 128MB for
 * constrained environment).
 *
 * When such a situation is detected - Reschedule napi
 */
static void check_for_empty_rx_ring(struct ena_adapter *adapter)
{
	struct ena_ring *rx_ring;
	int i, refill_required;

	if (!test_bit(ENA_FLAG_DEV_UP, &adapter->flags))
		return;

	if (test_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags))
		return;

	for (i = 0; i < adapter->num_queues; i++) {
		rx_ring = &adapter->rx_ring[i];

		refill_required =
			ena_com_free_desc(rx_ring->ena_com_io_sq);
		if (unlikely(refill_required == (rx_ring->ring_size - 1))) {
			rx_ring->empty_rx_queue++;

			if (rx_ring->empty_rx_queue >= EMPTY_RX_REFILL) {
				u64_stats_update_begin(&rx_ring->syncp);
				rx_ring->rx_stats.empty_rx_ring++;
				u64_stats_update_end(&rx_ring->syncp);

				netif_err(adapter, drv, adapter->netdev,
					  "trigger refill for ring %d\n", i);

				napi_schedule(rx_ring->napi);
				rx_ring->empty_rx_queue = 0;
			}
		} else {
			rx_ring->empty_rx_queue = 0;
		}
	}
}

/* Check for keep alive expiration */
static void check_for_missing_keep_alive(struct ena_adapter *adapter)
{
	unsigned long keep_alive_expired;

	if (!adapter->wd_state)
		return;

	if (adapter->keep_alive_timeout == ENA_HW_HINTS_NO_TIMEOUT)
		return;

	keep_alive_expired = adapter->last_keep_alive_jiffies +
			     adapter->keep_alive_timeout;
	if (unlikely(time_is_before_jiffies(keep_alive_expired))) {
		netif_err(adapter, drv, adapter->netdev,
			  "Keep alive watchdog timeout.\n");
		u64_stats_update_begin(&adapter->syncp);
		adapter->dev_stats.wd_expired++;
		u64_stats_update_end(&adapter->syncp);
		adapter->reset_reason = ENA_REGS_RESET_KEEP_ALIVE_TO;
		set_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);
	}
}

static void check_for_admin_com_state(struct ena_adapter *adapter)
{
	if (unlikely(!ena_com_get_admin_running_state(adapter->ena_dev))) {
		netif_err(adapter, drv, adapter->netdev,
			  "ENA admin queue is not in running state!\n");
		u64_stats_update_begin(&adapter->syncp);
		adapter->dev_stats.admin_q_pause++;
		u64_stats_update_end(&adapter->syncp);
		adapter->reset_reason = ENA_REGS_RESET_ADMIN_TO;
		set_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);
	}
}

static void ena_update_hints(struct ena_adapter *adapter,
			     struct ena_admin_ena_hw_hints *hints)
{
	struct net_device *netdev = adapter->netdev;

	if (hints->admin_completion_tx_timeout)
		adapter->ena_dev->admin_queue.completion_timeout =
			hints->admin_completion_tx_timeout * 1000;

	if (hints->mmio_read_timeout)
		/* convert to usec */
		adapter->ena_dev->mmio_read.reg_read_to =
			hints->mmio_read_timeout * 1000;

	if (hints->missed_tx_completion_count_threshold_to_reset)
		adapter->missing_tx_completion_threshold =
			hints->missed_tx_completion_count_threshold_to_reset;

	if (hints->missing_tx_completion_timeout) {
		if (hints->missing_tx_completion_timeout == ENA_HW_HINTS_NO_TIMEOUT)
			adapter->missing_tx_completion_to = ENA_HW_HINTS_NO_TIMEOUT;
		else
			adapter->missing_tx_completion_to =
				msecs_to_jiffies(hints->missing_tx_completion_timeout);
	}

	if (hints->netdev_wd_timeout)
		netdev->watchdog_timeo = msecs_to_jiffies(hints->netdev_wd_timeout);

	if (hints->driver_watchdog_timeout) {
		if (hints->driver_watchdog_timeout == ENA_HW_HINTS_NO_TIMEOUT)
			adapter->keep_alive_timeout = ENA_HW_HINTS_NO_TIMEOUT;
		else
			adapter->keep_alive_timeout =
				msecs_to_jiffies(hints->driver_watchdog_timeout);
	}
}

static void ena_update_host_info(struct ena_admin_host_info *host_info,
				 struct net_device *netdev)
{
	host_info->supported_network_features[0] =
		netdev->features & GENMASK_ULL(31, 0);
	host_info->supported_network_features[1] =
		(netdev->features & GENMASK_ULL(63, 32)) >> 32;
}

static void ena_timer_service(struct timer_list *t)
{
	struct ena_adapter *adapter = from_timer(adapter, t, timer_service);
	u8 *debug_area = adapter->ena_dev->host_attr.debug_area_virt_addr;
	struct ena_admin_host_info *host_info =
		adapter->ena_dev->host_attr.host_info;

	check_for_missing_keep_alive(adapter);

	check_for_admin_com_state(adapter);

	check_for_missing_completions(adapter);

	check_for_empty_rx_ring(adapter);

	if (debug_area)
		ena_dump_stats_to_buf(adapter, debug_area);

	if (host_info)
		ena_update_host_info(host_info, adapter->netdev);

	if (unlikely(test_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags))) {
		netif_err(adapter, drv, adapter->netdev,
			  "Trigger reset is on\n");
		ena_dump_stats_to_dmesg(adapter);
		queue_work(ena_wq, &adapter->reset_task);
		return;
	}

	/* Reset the timer */
	mod_timer(&adapter->timer_service, round_jiffies(jiffies + HZ));
}

static int ena_calc_io_queue_num(struct pci_dev *pdev,
				 struct ena_com_dev *ena_dev,
				 struct ena_com_dev_get_features_ctx *get_feat_ctx)
{
	int io_tx_sq_num, io_tx_cq_num, io_rx_num, io_queue_num;

	if (ena_dev->supported_features & BIT(ENA_ADMIN_MAX_QUEUES_EXT)) {
		struct ena_admin_queue_ext_feature_fields *max_queue_ext =
			&get_feat_ctx->max_queue_ext.max_queue_ext;
		io_rx_num = min_t(int, max_queue_ext->max_rx_sq_num,
				  max_queue_ext->max_rx_cq_num);

		io_tx_sq_num = max_queue_ext->max_tx_sq_num;
		io_tx_cq_num = max_queue_ext->max_tx_cq_num;
	} else {
		struct ena_admin_queue_feature_desc *max_queues =
			&get_feat_ctx->max_queues;
		io_tx_sq_num = max_queues->max_sq_num;
		io_tx_cq_num = max_queues->max_cq_num;
		io_rx_num = min_t(int, io_tx_sq_num, io_tx_cq_num);
	}

	/* In case of LLQ use the llq fields for the tx SQ/CQ */
	if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV)
		io_tx_sq_num = get_feat_ctx->llq.max_llq_num;

	io_queue_num = min_t(int, num_online_cpus(), ENA_MAX_NUM_IO_QUEUES);
	io_queue_num = min_t(int, io_queue_num, io_rx_num);
	io_queue_num = min_t(int, io_queue_num, io_tx_sq_num);
	io_queue_num = min_t(int, io_queue_num, io_tx_cq_num);
	/* 1 IRQ for for mgmnt and 1 IRQs for each IO direction */
	io_queue_num = min_t(int, io_queue_num, pci_msix_vec_count(pdev) - 1);
	if (unlikely(!io_queue_num)) {
		dev_err(&pdev->dev, "The device doesn't have io queues\n");
		return -EFAULT;
	}

	return io_queue_num;
}

static int ena_set_queues_placement_policy(struct pci_dev *pdev,
					   struct ena_com_dev *ena_dev,
					   struct ena_admin_feature_llq_desc *llq,
					   struct ena_llq_configurations *llq_default_configurations)
{
	bool has_mem_bar;
	int rc;
	u32 llq_feature_mask;

	llq_feature_mask = 1 << ENA_ADMIN_LLQ;
	if (!(ena_dev->supported_features & llq_feature_mask)) {
		dev_err(&pdev->dev,
			"LLQ is not supported Fallback to host mode policy.\n");
		ena_dev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
		return 0;
	}

	has_mem_bar = pci_select_bars(pdev, IORESOURCE_MEM) & BIT(ENA_MEM_BAR);

	rc = ena_com_config_dev_mode(ena_dev, llq, llq_default_configurations);
	if (unlikely(rc)) {
		dev_err(&pdev->dev,
			"Failed to configure the device mode.  Fallback to host mode policy.\n");
		ena_dev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
		return 0;
	}

	/* Nothing to config, exit */
	if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_HOST)
		return 0;

	if (!has_mem_bar) {
		dev_err(&pdev->dev,
			"ENA device does not expose LLQ bar. Fallback to host mode policy.\n");
		ena_dev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
		return 0;
	}

	ena_dev->mem_bar = devm_ioremap_wc(&pdev->dev,
					   pci_resource_start(pdev, ENA_MEM_BAR),
					   pci_resource_len(pdev, ENA_MEM_BAR));

	if (!ena_dev->mem_bar)
		return -EFAULT;

	return 0;
}

static void ena_set_dev_offloads(struct ena_com_dev_get_features_ctx *feat,
				 struct net_device *netdev)
{
	netdev_features_t dev_features = 0;

	/* Set offload features */
	if (feat->offload.tx &
		ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_PART_MASK)
		dev_features |= NETIF_F_IP_CSUM;

	if (feat->offload.tx &
		ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV6_CSUM_PART_MASK)
		dev_features |= NETIF_F_IPV6_CSUM;

	if (feat->offload.tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV4_MASK)
		dev_features |= NETIF_F_TSO;

	if (feat->offload.tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV6_MASK)
		dev_features |= NETIF_F_TSO6;

	if (feat->offload.tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_ECN_MASK)
		dev_features |= NETIF_F_TSO_ECN;

	if (feat->offload.rx_supported &
		ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV4_CSUM_MASK)
		dev_features |= NETIF_F_RXCSUM;

	if (feat->offload.rx_supported &
		ENA_ADMIN_FEATURE_OFFLOAD_DESC_RX_L4_IPV6_CSUM_MASK)
		dev_features |= NETIF_F_RXCSUM;

	netdev->features =
		dev_features |
		NETIF_F_SG |
		NETIF_F_RXHASH |
		NETIF_F_HIGHDMA;

	netdev->hw_features |= netdev->features;
	netdev->vlan_features |= netdev->features;
}

static void ena_set_conf_feat_params(struct ena_adapter *adapter,
				     struct ena_com_dev_get_features_ctx *feat)
{
	struct net_device *netdev = adapter->netdev;

	/* Copy mac address */
	if (!is_valid_ether_addr(feat->dev_attr.mac_addr)) {
		eth_hw_addr_random(netdev);
		ether_addr_copy(adapter->mac_addr, netdev->dev_addr);
	} else {
		ether_addr_copy(adapter->mac_addr, feat->dev_attr.mac_addr);
		ether_addr_copy(netdev->dev_addr, adapter->mac_addr);
	}

	/* Set offload features */
	ena_set_dev_offloads(feat, netdev);

	adapter->max_mtu = feat->dev_attr.max_mtu;
	netdev->max_mtu = adapter->max_mtu;
	netdev->min_mtu = ENA_MIN_MTU;
}

static int ena_rss_init_default(struct ena_adapter *adapter)
{
	struct ena_com_dev *ena_dev = adapter->ena_dev;
	struct device *dev = &adapter->pdev->dev;
	int rc, i;
	u32 val;

	rc = ena_com_rss_init(ena_dev, ENA_RX_RSS_TABLE_LOG_SIZE);
	if (unlikely(rc)) {
		dev_err(dev, "Cannot init indirect table\n");
		goto err_rss_init;
	}

	for (i = 0; i < ENA_RX_RSS_TABLE_SIZE; i++) {
		val = ethtool_rxfh_indir_default(i, adapter->num_queues);
		rc = ena_com_indirect_table_fill_entry(ena_dev, i,
						       ENA_IO_RXQ_IDX(val));
		if (unlikely(rc && (rc != -EOPNOTSUPP))) {
			dev_err(dev, "Cannot fill indirect table\n");
			goto err_fill_indir;
		}
	}

	rc = ena_com_fill_hash_function(ena_dev, ENA_ADMIN_CRC32, NULL,
					ENA_HASH_KEY_SIZE, 0xFFFFFFFF);
	if (unlikely(rc && (rc != -EOPNOTSUPP))) {
		dev_err(dev, "Cannot fill hash function\n");
		goto err_fill_indir;
	}

	rc = ena_com_set_default_hash_ctrl(ena_dev);
	if (unlikely(rc && (rc != -EOPNOTSUPP))) {
		dev_err(dev, "Cannot fill hash control\n");
		goto err_fill_indir;
	}

	return 0;

err_fill_indir:
	ena_com_rss_destroy(ena_dev);
err_rss_init:

	return rc;
}

static void ena_release_bars(struct ena_com_dev *ena_dev, struct pci_dev *pdev)
{
	int release_bars = pci_select_bars(pdev, IORESOURCE_MEM) & ENA_BAR_MASK;

	pci_release_selected_regions(pdev, release_bars);
}

static void set_default_llq_configurations(struct ena_llq_configurations *llq_config)
{
	llq_config->llq_header_location = ENA_ADMIN_INLINE_HEADER;
	llq_config->llq_ring_entry_size = ENA_ADMIN_LIST_ENTRY_SIZE_128B;
	llq_config->llq_stride_ctrl = ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY;
	llq_config->llq_num_decs_before_header = ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2;
	llq_config->llq_ring_entry_size_value = 128;
}

static int ena_calc_queue_size(struct ena_calc_queue_size_ctx *ctx)
{
	struct ena_admin_feature_llq_desc *llq = &ctx->get_feat_ctx->llq;
	struct ena_com_dev *ena_dev = ctx->ena_dev;
	u32 tx_queue_size = ENA_DEFAULT_RING_SIZE;
	u32 rx_queue_size = ENA_DEFAULT_RING_SIZE;
	u32 max_tx_queue_size;
	u32 max_rx_queue_size;

	if (ctx->ena_dev->supported_features & BIT(ENA_ADMIN_MAX_QUEUES_EXT)) {
		struct ena_admin_queue_ext_feature_fields *max_queue_ext =
			&ctx->get_feat_ctx->max_queue_ext.max_queue_ext;
		max_rx_queue_size = min_t(u32, max_queue_ext->max_rx_cq_depth,
					  max_queue_ext->max_rx_sq_depth);
		max_tx_queue_size = max_queue_ext->max_tx_cq_depth;

		if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV)
			max_tx_queue_size = min_t(u32, max_tx_queue_size,
						  llq->max_llq_depth);
		else
			max_tx_queue_size = min_t(u32, max_tx_queue_size,
						  max_queue_ext->max_tx_sq_depth);

		ctx->max_tx_sgl_size = min_t(u16, ENA_PKT_MAX_BUFS,
					     max_queue_ext->max_per_packet_tx_descs);
		ctx->max_rx_sgl_size = min_t(u16, ENA_PKT_MAX_BUFS,
					     max_queue_ext->max_per_packet_rx_descs);
	} else {
		struct ena_admin_queue_feature_desc *max_queues =
			&ctx->get_feat_ctx->max_queues;
		max_rx_queue_size = min_t(u32, max_queues->max_cq_depth,
					  max_queues->max_sq_depth);
		max_tx_queue_size = max_queues->max_cq_depth;

		if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV)
			max_tx_queue_size = min_t(u32, max_tx_queue_size,
						  llq->max_llq_depth);
		else
			max_tx_queue_size = min_t(u32, max_tx_queue_size,
						  max_queues->max_sq_depth);

		ctx->max_tx_sgl_size = min_t(u16, ENA_PKT_MAX_BUFS,
					     max_queues->max_packet_tx_descs);
		ctx->max_rx_sgl_size = min_t(u16, ENA_PKT_MAX_BUFS,
					     max_queues->max_packet_rx_descs);
	}

	max_tx_queue_size = rounddown_pow_of_two(max_tx_queue_size);
	max_rx_queue_size = rounddown_pow_of_two(max_rx_queue_size);

	tx_queue_size = clamp_val(tx_queue_size, ENA_MIN_RING_SIZE,
				  max_tx_queue_size);
	rx_queue_size = clamp_val(rx_queue_size, ENA_MIN_RING_SIZE,
				  max_rx_queue_size);

	tx_queue_size = rounddown_pow_of_two(tx_queue_size);
	rx_queue_size = rounddown_pow_of_two(rx_queue_size);

	ctx->max_tx_queue_size = max_tx_queue_size;
	ctx->max_rx_queue_size = max_rx_queue_size;
	ctx->tx_queue_size = tx_queue_size;
	ctx->rx_queue_size = rx_queue_size;

	return 0;
}

/* ena_probe - Device Initialization Routine
 * @pdev: PCI device information struct
 * @ent: entry in ena_pci_tbl
 *
 * Returns 0 on success, negative on failure
 *
 * ena_probe initializes an adapter identified by a pci_dev structure.
 * The OS initialization, configuring of the adapter private structure,
 * and a hardware reset occur.
 */
static int ena_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct ena_com_dev_get_features_ctx get_feat_ctx;
	struct ena_calc_queue_size_ctx calc_queue_ctx = { 0 };
	struct ena_llq_configurations llq_config;
	struct ena_com_dev *ena_dev = NULL;
	struct ena_adapter *adapter;
	int io_queue_num, bars, rc;
	struct net_device *netdev;
	static int adapters_found;
	char *queue_type_str;
	bool wd_state;

	dev_dbg(&pdev->dev, "%s\n", __func__);

	dev_info_once(&pdev->dev, "%s", version);

	rc = pci_enable_device_mem(pdev);
	if (rc) {
		dev_err(&pdev->dev, "pci_enable_device_mem() failed!\n");
		return rc;
	}

	pci_set_master(pdev);

	ena_dev = vzalloc(sizeof(*ena_dev));
	if (!ena_dev) {
		rc = -ENOMEM;
		goto err_disable_device;
	}

	bars = pci_select_bars(pdev, IORESOURCE_MEM) & ENA_BAR_MASK;
	rc = pci_request_selected_regions(pdev, bars, DRV_MODULE_NAME);
	if (rc) {
		dev_err(&pdev->dev, "pci_request_selected_regions failed %d\n",
			rc);
		goto err_free_ena_dev;
	}

	ena_dev->reg_bar = devm_ioremap(&pdev->dev,
					pci_resource_start(pdev, ENA_REG_BAR),
					pci_resource_len(pdev, ENA_REG_BAR));
	if (!ena_dev->reg_bar) {
		dev_err(&pdev->dev, "failed to remap regs bar\n");
		rc = -EFAULT;
		goto err_free_region;
	}

	ena_dev->dmadev = &pdev->dev;

	rc = ena_device_init(ena_dev, pdev, &get_feat_ctx, &wd_state);
	if (rc) {
		dev_err(&pdev->dev, "ena device init failed\n");
		if (rc == -ETIME)
			rc = -EPROBE_DEFER;
		goto err_free_region;
	}

	set_default_llq_configurations(&llq_config);

	rc = ena_set_queues_placement_policy(pdev, ena_dev, &get_feat_ctx.llq,
					     &llq_config);
	if (rc) {
		dev_err(&pdev->dev, "ena device init failed\n");
		goto err_device_destroy;
	}

	calc_queue_ctx.ena_dev = ena_dev;
	calc_queue_ctx.get_feat_ctx = &get_feat_ctx;
	calc_queue_ctx.pdev = pdev;

	/* Initial Tx and RX interrupt delay. Assumes 1 usec granularity.
	* Updated during device initialization with the real granularity
	*/
	ena_dev->intr_moder_tx_interval = ENA_INTR_INITIAL_TX_INTERVAL_USECS;
	ena_dev->intr_moder_rx_interval = ENA_INTR_INITIAL_RX_INTERVAL_USECS;
	ena_dev->intr_delay_resolution = ENA_DEFAULT_INTR_DELAY_RESOLUTION;
	io_queue_num = ena_calc_io_queue_num(pdev, ena_dev, &get_feat_ctx);
	rc = ena_calc_queue_size(&calc_queue_ctx);
	if (rc || io_queue_num <= 0) {
		rc = -EFAULT;
		goto err_device_destroy;
	}

	dev_info(&pdev->dev, "creating %d io queues. rx queue size: %d tx queue size. %d LLQ is %s\n",
		 io_queue_num,
		 calc_queue_ctx.rx_queue_size,
		 calc_queue_ctx.tx_queue_size,
		 (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) ?
		 "ENABLED" : "DISABLED");

	/* dev zeroed in init_etherdev */
	netdev = alloc_etherdev_mq(sizeof(struct ena_adapter), io_queue_num);
	if (!netdev) {
		dev_err(&pdev->dev, "alloc_etherdev_mq failed\n");
		rc = -ENOMEM;
		goto err_device_destroy;
	}

	SET_NETDEV_DEV(netdev, &pdev->dev);

	adapter = netdev_priv(netdev);
	pci_set_drvdata(pdev, adapter);

	adapter->ena_dev = ena_dev;
	adapter->netdev = netdev;
	adapter->pdev = pdev;

	ena_set_conf_feat_params(adapter, &get_feat_ctx);

	adapter->msg_enable = netif_msg_init(debug, DEFAULT_MSG_ENABLE);
	adapter->reset_reason = ENA_REGS_RESET_NORMAL;

	adapter->requested_tx_ring_size = calc_queue_ctx.tx_queue_size;
	adapter->requested_rx_ring_size = calc_queue_ctx.rx_queue_size;
	adapter->max_tx_ring_size = calc_queue_ctx.max_tx_queue_size;
	adapter->max_rx_ring_size = calc_queue_ctx.max_rx_queue_size;
	adapter->max_tx_sgl_size = calc_queue_ctx.max_tx_sgl_size;
	adapter->max_rx_sgl_size = calc_queue_ctx.max_rx_sgl_size;

	adapter->num_queues = io_queue_num;
	adapter->last_monitored_tx_qid = 0;

	adapter->rx_copybreak = ENA_DEFAULT_RX_COPYBREAK;
	adapter->wd_state = wd_state;

	snprintf(adapter->name, ENA_NAME_MAX_LEN, "ena_%d", adapters_found);

	rc = ena_com_init_interrupt_moderation(adapter->ena_dev);
	if (rc) {
		dev_err(&pdev->dev,
			"Failed to query interrupt moderation feature\n");
		goto err_netdev_destroy;
	}
	ena_init_io_rings(adapter);

	netdev->netdev_ops = &ena_netdev_ops;
	netdev->watchdog_timeo = TX_TIMEOUT;
	ena_set_ethtool_ops(netdev);

	netdev->priv_flags |= IFF_UNICAST_FLT;

	u64_stats_init(&adapter->syncp);

	rc = ena_enable_msix_and_set_admin_interrupts(adapter, io_queue_num);
	if (rc) {
		dev_err(&pdev->dev,
			"Failed to enable and set the admin interrupts\n");
		goto err_worker_destroy;
	}
	rc = ena_rss_init_default(adapter);
	if (rc && (rc != -EOPNOTSUPP)) {
		dev_err(&pdev->dev, "Cannot init RSS rc: %d\n", rc);
		goto err_free_msix;
	}

	ena_config_debug_area(adapter);

	memcpy(adapter->netdev->perm_addr, adapter->mac_addr, netdev->addr_len);

	netif_carrier_off(netdev);

	rc = register_netdev(netdev);
	if (rc) {
		dev_err(&pdev->dev, "Cannot register net device\n");
		goto err_rss;
	}

	INIT_WORK(&adapter->reset_task, ena_fw_reset_device);

	adapter->last_keep_alive_jiffies = jiffies;
	adapter->keep_alive_timeout = ENA_DEVICE_KALIVE_TIMEOUT;
	adapter->missing_tx_completion_to = TX_TIMEOUT;
	adapter->missing_tx_completion_threshold = MAX_NUM_OF_TIMEOUTED_PACKETS;

	ena_update_hints(adapter, &get_feat_ctx.hw_hints);

	timer_setup(&adapter->timer_service, ena_timer_service, 0);
	mod_timer(&adapter->timer_service, round_jiffies(jiffies + HZ));

	if (ena_dev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_HOST)
		queue_type_str = "Regular";
	else
		queue_type_str = "Low Latency";

	dev_info(&pdev->dev,
		 "%s found at mem %lx, mac addr %pM Queues %d, Placement policy: %s\n",
		 DEVICE_NAME, (long)pci_resource_start(pdev, 0),
		 netdev->dev_addr, io_queue_num, queue_type_str);

	set_bit(ENA_FLAG_DEVICE_RUNNING, &adapter->flags);

	adapters_found++;

	return 0;

err_rss:
	ena_com_delete_debug_area(ena_dev);
	ena_com_rss_destroy(ena_dev);
err_free_msix:
	ena_com_dev_reset(ena_dev, ENA_REGS_RESET_INIT_ERR);
	/* stop submitting admin commands on a device that was reset */
	ena_com_set_admin_running_state(ena_dev, false);
	ena_free_mgmnt_irq(adapter);
	ena_disable_msix(adapter);
err_worker_destroy:
	del_timer(&adapter->timer_service);
err_netdev_destroy:
	free_netdev(netdev);
err_device_destroy:
	ena_com_delete_host_info(ena_dev);
	ena_com_admin_destroy(ena_dev);
err_free_region:
	ena_release_bars(ena_dev, pdev);
err_free_ena_dev:
	vfree(ena_dev);
err_disable_device:
	pci_disable_device(pdev);
	return rc;
}

/*****************************************************************************/

/* __ena_shutoff - Helper used in both PCI remove/shutdown routines
 * @pdev: PCI device information struct
 * @shutdown: Is it a shutdown operation? If false, means it is a removal
 *
 * __ena_shutoff is a helper routine that does the real work on shutdown and
 * removal paths; the difference between those paths is with regards to whether
 * dettach or unregister the netdevice.
 */
static void __ena_shutoff(struct pci_dev *pdev, bool shutdown)
{
	struct ena_adapter *adapter = pci_get_drvdata(pdev);
	struct ena_com_dev *ena_dev;
	struct net_device *netdev;

	ena_dev = adapter->ena_dev;
	netdev = adapter->netdev;

#ifdef CONFIG_RFS_ACCEL
	if ((adapter->msix_vecs >= 1) && (netdev->rx_cpu_rmap)) {
		free_irq_cpu_rmap(netdev->rx_cpu_rmap);
		netdev->rx_cpu_rmap = NULL;
	}
#endif /* CONFIG_RFS_ACCEL */

	/* Make sure timer and reset routine won't be called after
	 * freeing device resources.
	 */
	del_timer_sync(&adapter->timer_service);
	cancel_work_sync(&adapter->reset_task);

	rtnl_lock(); /* lock released inside the below if-else block */
	ena_destroy_device(adapter, true);
	if (shutdown) {
		netif_device_detach(netdev);
		dev_close(netdev);
		rtnl_unlock();
	} else {
		rtnl_unlock();
		unregister_netdev(netdev);
		free_netdev(netdev);
	}

	ena_com_rss_destroy(ena_dev);

	ena_com_delete_debug_area(ena_dev);

	ena_com_delete_host_info(ena_dev);

	ena_release_bars(ena_dev, pdev);

	pci_disable_device(pdev);

	vfree(ena_dev);
}

/* ena_remove - Device Removal Routine
 * @pdev: PCI device information struct
 *
 * ena_remove is called by the PCI subsystem to alert the driver
 * that it should release a PCI device.
 */

static void ena_remove(struct pci_dev *pdev)
{
	__ena_shutoff(pdev, false);
}

/* ena_shutdown - Device Shutdown Routine
 * @pdev: PCI device information struct
 *
 * ena_shutdown is called by the PCI subsystem to alert the driver that
 * a shutdown/reboot (or kexec) is happening and device must be disabled.
 */

static void ena_shutdown(struct pci_dev *pdev)
{
	__ena_shutoff(pdev, true);
}

#ifdef CONFIG_PM
/* ena_suspend - PM suspend callback
 * @pdev: PCI device information struct
 * @state:power state
 */
static int ena_suspend(struct pci_dev *pdev,  pm_message_t state)
{
	struct ena_adapter *adapter = pci_get_drvdata(pdev);

	u64_stats_update_begin(&adapter->syncp);
	adapter->dev_stats.suspend++;
	u64_stats_update_end(&adapter->syncp);

	rtnl_lock();
	if (unlikely(test_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags))) {
		dev_err(&pdev->dev,
			"ignoring device reset request as the device is being suspended\n");
		clear_bit(ENA_FLAG_TRIGGER_RESET, &adapter->flags);
	}
	ena_destroy_device(adapter, true);
	rtnl_unlock();
	return 0;
}

/* ena_resume - PM resume callback
 * @pdev: PCI device information struct
 *
 */
static int ena_resume(struct pci_dev *pdev)
{
	struct ena_adapter *adapter = pci_get_drvdata(pdev);
	int rc;

	u64_stats_update_begin(&adapter->syncp);
	adapter->dev_stats.resume++;
	u64_stats_update_end(&adapter->syncp);

	rtnl_lock();
	rc = ena_restore_device(adapter);
	rtnl_unlock();
	return rc;
}
#endif

static struct pci_driver ena_pci_driver = {
	.name		= DRV_MODULE_NAME,
	.id_table	= ena_pci_tbl,
	.probe		= ena_probe,
	.remove		= ena_remove,
	.shutdown	= ena_shutdown,
#ifdef CONFIG_PM
	.suspend    = ena_suspend,
	.resume     = ena_resume,
#endif
	.sriov_configure = pci_sriov_configure_simple,
};

static int __init ena_init(void)
{
	pr_info("%s", version);

	ena_wq = create_singlethread_workqueue(DRV_MODULE_NAME);
	if (!ena_wq) {
		pr_err("Failed to create workqueue\n");
		return -ENOMEM;
	}

	return pci_register_driver(&ena_pci_driver);
}

static void __exit ena_cleanup(void)
{
	pci_unregister_driver(&ena_pci_driver);

	if (ena_wq) {
		destroy_workqueue(ena_wq);
		ena_wq = NULL;
	}
}

/******************************************************************************
 ******************************** AENQ Handlers *******************************
 *****************************************************************************/
/* ena_update_on_link_change:
 * Notify the network interface about the change in link status
 */
static void ena_update_on_link_change(void *adapter_data,
				      struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_adapter *adapter = (struct ena_adapter *)adapter_data;
	struct ena_admin_aenq_link_change_desc *aenq_desc =
		(struct ena_admin_aenq_link_change_desc *)aenq_e;
	int status = aenq_desc->flags &
		ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK;

	if (status) {
		netdev_dbg(adapter->netdev, "%s\n", __func__);
		set_bit(ENA_FLAG_LINK_UP, &adapter->flags);
		if (!test_bit(ENA_FLAG_ONGOING_RESET, &adapter->flags))
			netif_carrier_on(adapter->netdev);
	} else {
		clear_bit(ENA_FLAG_LINK_UP, &adapter->flags);
		netif_carrier_off(adapter->netdev);
	}
}

static void ena_keep_alive_wd(void *adapter_data,
			      struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_adapter *adapter = (struct ena_adapter *)adapter_data;
	struct ena_admin_aenq_keep_alive_desc *desc;
	u64 rx_drops;

	desc = (struct ena_admin_aenq_keep_alive_desc *)aenq_e;
	adapter->last_keep_alive_jiffies = jiffies;

	rx_drops = ((u64)desc->rx_drops_high << 32) | desc->rx_drops_low;

	u64_stats_update_begin(&adapter->syncp);
	/* These stats are accumulated by the device, so the counters indicate
	 * all drops since last reset.
	 */
	adapter->dev_stats.rx_drops = rx_drops;
	u64_stats_update_end(&adapter->syncp);
}

static void ena_notification(void *adapter_data,
			     struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_adapter *adapter = (struct ena_adapter *)adapter_data;
	struct ena_admin_ena_hw_hints *hints;

	WARN(aenq_e->aenq_common_desc.group != ENA_ADMIN_NOTIFICATION,
	     "Invalid group(%x) expected %x\n",
	     aenq_e->aenq_common_desc.group,
	     ENA_ADMIN_NOTIFICATION);

	switch (aenq_e->aenq_common_desc.syndrom) {
	case ENA_ADMIN_UPDATE_HINTS:
		hints = (struct ena_admin_ena_hw_hints *)
			(&aenq_e->inline_data_w4);
		ena_update_hints(adapter, hints);
		break;
	default:
		netif_err(adapter, drv, adapter->netdev,
			  "Invalid aenq notification link state %d\n",
			  aenq_e->aenq_common_desc.syndrom);
	}
}

/* This handler will called for unknown event group or unimplemented handlers*/
static void unimplemented_aenq_handler(void *data,
				       struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_adapter *adapter = (struct ena_adapter *)data;

	netif_err(adapter, drv, adapter->netdev,
		  "Unknown event was received or event with unimplemented handler\n");
}

static struct ena_aenq_handlers aenq_handlers = {
	.handlers = {
		[ENA_ADMIN_LINK_CHANGE] = ena_update_on_link_change,
		[ENA_ADMIN_NOTIFICATION] = ena_notification,
		[ENA_ADMIN_KEEP_ALIVE] = ena_keep_alive_wd,
	},
	.unimplemented_handler = unimplemented_aenq_handler
};

module_init(ena_init);
module_exit(ena_cleanup);
