// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, Intel Corporation. */

/* The driver transmit and receive code */

#include <linux/prefetch.h>
#include <linux/mm.h>
#include "ice.h"
#include "ice_dcb_lib.h"

#define ICE_RX_HDR_SIZE		256

/**
 * ice_unmap_and_free_tx_buf - Release a Tx buffer
 * @ring: the ring that owns the buffer
 * @tx_buf: the buffer to free
 */
static void
ice_unmap_and_free_tx_buf(struct ice_ring *ring, struct ice_tx_buf *tx_buf)
{
	if (tx_buf->skb) {
		dev_kfree_skb_any(tx_buf->skb);
		if (dma_unmap_len(tx_buf, len))
			dma_unmap_single(ring->dev,
					 dma_unmap_addr(tx_buf, dma),
					 dma_unmap_len(tx_buf, len),
					 DMA_TO_DEVICE);
	} else if (dma_unmap_len(tx_buf, len)) {
		dma_unmap_page(ring->dev,
			       dma_unmap_addr(tx_buf, dma),
			       dma_unmap_len(tx_buf, len),
			       DMA_TO_DEVICE);
	}

	tx_buf->next_to_watch = NULL;
	tx_buf->skb = NULL;
	dma_unmap_len_set(tx_buf, len, 0);
	/* tx_buf must be completely set up in the transmit path */
}

static struct netdev_queue *txring_txq(const struct ice_ring *ring)
{
	return netdev_get_tx_queue(ring->netdev, ring->q_index);
}

/**
 * ice_clean_tx_ring - Free any empty Tx buffers
 * @tx_ring: ring to be cleaned
 */
void ice_clean_tx_ring(struct ice_ring *tx_ring)
{
	u16 i;

	/* ring already cleared, nothing to do */
	if (!tx_ring->tx_buf)
		return;

	/* Free all the Tx ring sk_buffs */
	for (i = 0; i < tx_ring->count; i++)
		ice_unmap_and_free_tx_buf(tx_ring, &tx_ring->tx_buf[i]);

	memset(tx_ring->tx_buf, 0, sizeof(*tx_ring->tx_buf) * tx_ring->count);

	/* Zero out the descriptor ring */
	memset(tx_ring->desc, 0, tx_ring->size);

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;

	if (!tx_ring->netdev)
		return;

	/* cleanup Tx queue statistics */
	netdev_tx_reset_queue(txring_txq(tx_ring));
}

/**
 * ice_free_tx_ring - Free Tx resources per queue
 * @tx_ring: Tx descriptor ring for a specific queue
 *
 * Free all transmit software resources
 */
void ice_free_tx_ring(struct ice_ring *tx_ring)
{
	ice_clean_tx_ring(tx_ring);
	devm_kfree(tx_ring->dev, tx_ring->tx_buf);
	tx_ring->tx_buf = NULL;

	if (tx_ring->desc) {
		dmam_free_coherent(tx_ring->dev, tx_ring->size,
				   tx_ring->desc, tx_ring->dma);
		tx_ring->desc = NULL;
	}
}

/**
 * ice_clean_tx_irq - Reclaim resources after transmit completes
 * @tx_ring: Tx ring to clean
 * @napi_budget: Used to determine if we are in netpoll
 *
 * Returns true if there's any budget left (e.g. the clean is finished)
 */
static bool ice_clean_tx_irq(struct ice_ring *tx_ring, int napi_budget)
{
	unsigned int total_bytes = 0, total_pkts = 0;
	unsigned int budget = ICE_DFLT_IRQ_WORK;
	struct ice_vsi *vsi = tx_ring->vsi;
	s16 i = tx_ring->next_to_clean;
	struct ice_tx_desc *tx_desc;
	struct ice_tx_buf *tx_buf;

	tx_buf = &tx_ring->tx_buf[i];
	tx_desc = ICE_TX_DESC(tx_ring, i);
	i -= tx_ring->count;

	prefetch(&vsi->state);

	do {
		struct ice_tx_desc *eop_desc = tx_buf->next_to_watch;

		/* if next_to_watch is not set then there is no work pending */
		if (!eop_desc)
			break;

		smp_rmb();	/* prevent any other reads prior to eop_desc */

		/* if the descriptor isn't done, no work yet to do */
		if (!(eop_desc->cmd_type_offset_bsz &
		      cpu_to_le64(ICE_TX_DESC_DTYPE_DESC_DONE)))
			break;

		/* clear next_to_watch to prevent false hangs */
		tx_buf->next_to_watch = NULL;

		/* update the statistics for this packet */
		total_bytes += tx_buf->bytecount;
		total_pkts += tx_buf->gso_segs;

		/* free the skb */
		napi_consume_skb(tx_buf->skb, napi_budget);

		/* unmap skb header data */
		dma_unmap_single(tx_ring->dev,
				 dma_unmap_addr(tx_buf, dma),
				 dma_unmap_len(tx_buf, len),
				 DMA_TO_DEVICE);

		/* clear tx_buf data */
		tx_buf->skb = NULL;
		dma_unmap_len_set(tx_buf, len, 0);

		/* unmap remaining buffers */
		while (tx_desc != eop_desc) {
			tx_buf++;
			tx_desc++;
			i++;
			if (unlikely(!i)) {
				i -= tx_ring->count;
				tx_buf = tx_ring->tx_buf;
				tx_desc = ICE_TX_DESC(tx_ring, 0);
			}

			/* unmap any remaining paged data */
			if (dma_unmap_len(tx_buf, len)) {
				dma_unmap_page(tx_ring->dev,
					       dma_unmap_addr(tx_buf, dma),
					       dma_unmap_len(tx_buf, len),
					       DMA_TO_DEVICE);
				dma_unmap_len_set(tx_buf, len, 0);
			}
		}

		/* move us one more past the eop_desc for start of next pkt */
		tx_buf++;
		tx_desc++;
		i++;
		if (unlikely(!i)) {
			i -= tx_ring->count;
			tx_buf = tx_ring->tx_buf;
			tx_desc = ICE_TX_DESC(tx_ring, 0);
		}

		prefetch(tx_desc);

		/* update budget accounting */
		budget--;
	} while (likely(budget));

	i += tx_ring->count;
	tx_ring->next_to_clean = i;
	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->stats.bytes += total_bytes;
	tx_ring->stats.pkts += total_pkts;
	u64_stats_update_end(&tx_ring->syncp);
	tx_ring->q_vector->tx.total_bytes += total_bytes;
	tx_ring->q_vector->tx.total_pkts += total_pkts;

	netdev_tx_completed_queue(txring_txq(tx_ring), total_pkts,
				  total_bytes);

#define TX_WAKE_THRESHOLD ((s16)(DESC_NEEDED * 2))
	if (unlikely(total_pkts && netif_carrier_ok(tx_ring->netdev) &&
		     (ICE_DESC_UNUSED(tx_ring) >= TX_WAKE_THRESHOLD))) {
		/* Make sure that anybody stopping the queue after this
		 * sees the new next_to_clean.
		 */
		smp_mb();
		if (__netif_subqueue_stopped(tx_ring->netdev,
					     tx_ring->q_index) &&
		    !test_bit(__ICE_DOWN, vsi->state)) {
			netif_wake_subqueue(tx_ring->netdev,
					    tx_ring->q_index);
			++tx_ring->tx_stats.restart_q;
		}
	}

	return !!budget;
}

/**
 * ice_setup_tx_ring - Allocate the Tx descriptors
 * @tx_ring: the Tx ring to set up
 *
 * Return 0 on success, negative on error
 */
int ice_setup_tx_ring(struct ice_ring *tx_ring)
{
	struct device *dev = tx_ring->dev;

	if (!dev)
		return -ENOMEM;

	/* warn if we are about to overwrite the pointer */
	WARN_ON(tx_ring->tx_buf);
	tx_ring->tx_buf =
		devm_kzalloc(dev, sizeof(*tx_ring->tx_buf) * tx_ring->count,
			     GFP_KERNEL);
	if (!tx_ring->tx_buf)
		return -ENOMEM;

	/* round up to nearest page */
	tx_ring->size = ALIGN(tx_ring->count * sizeof(struct ice_tx_desc),
			      PAGE_SIZE);
	tx_ring->desc = dmam_alloc_coherent(dev, tx_ring->size, &tx_ring->dma,
					    GFP_KERNEL);
	if (!tx_ring->desc) {
		dev_err(dev, "Unable to allocate memory for the Tx descriptor ring, size=%d\n",
			tx_ring->size);
		goto err;
	}

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;
	tx_ring->tx_stats.prev_pkt = -1;
	return 0;

err:
	devm_kfree(dev, tx_ring->tx_buf);
	tx_ring->tx_buf = NULL;
	return -ENOMEM;
}

/**
 * ice_clean_rx_ring - Free Rx buffers
 * @rx_ring: ring to be cleaned
 */
void ice_clean_rx_ring(struct ice_ring *rx_ring)
{
	struct device *dev = rx_ring->dev;
	u16 i;

	/* ring already cleared, nothing to do */
	if (!rx_ring->rx_buf)
		return;

	/* Free all the Rx ring sk_buffs */
	for (i = 0; i < rx_ring->count; i++) {
		struct ice_rx_buf *rx_buf = &rx_ring->rx_buf[i];

		if (rx_buf->skb) {
			dev_kfree_skb(rx_buf->skb);
			rx_buf->skb = NULL;
		}
		if (!rx_buf->page)
			continue;

		/* Invalidate cache lines that may have been written to by
		 * device so that we avoid corrupting memory.
		 */
		dma_sync_single_range_for_cpu(dev, rx_buf->dma,
					      rx_buf->page_offset,
					      ICE_RXBUF_2048, DMA_FROM_DEVICE);

		/* free resources associated with mapping */
		dma_unmap_page_attrs(dev, rx_buf->dma, PAGE_SIZE,
				     DMA_FROM_DEVICE, ICE_RX_DMA_ATTR);
		__page_frag_cache_drain(rx_buf->page, rx_buf->pagecnt_bias);

		rx_buf->page = NULL;
		rx_buf->page_offset = 0;
	}

	memset(rx_ring->rx_buf, 0, sizeof(*rx_ring->rx_buf) * rx_ring->count);

	/* Zero out the descriptor ring */
	memset(rx_ring->desc, 0, rx_ring->size);

	rx_ring->next_to_alloc = 0;
	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;
}

/**
 * ice_free_rx_ring - Free Rx resources
 * @rx_ring: ring to clean the resources from
 *
 * Free all receive software resources
 */
void ice_free_rx_ring(struct ice_ring *rx_ring)
{
	ice_clean_rx_ring(rx_ring);
	devm_kfree(rx_ring->dev, rx_ring->rx_buf);
	rx_ring->rx_buf = NULL;

	if (rx_ring->desc) {
		dmam_free_coherent(rx_ring->dev, rx_ring->size,
				   rx_ring->desc, rx_ring->dma);
		rx_ring->desc = NULL;
	}
}

/**
 * ice_setup_rx_ring - Allocate the Rx descriptors
 * @rx_ring: the Rx ring to set up
 *
 * Return 0 on success, negative on error
 */
int ice_setup_rx_ring(struct ice_ring *rx_ring)
{
	struct device *dev = rx_ring->dev;

	if (!dev)
		return -ENOMEM;

	/* warn if we are about to overwrite the pointer */
	WARN_ON(rx_ring->rx_buf);
	rx_ring->rx_buf =
		devm_kzalloc(dev, sizeof(*rx_ring->rx_buf) * rx_ring->count,
			     GFP_KERNEL);
	if (!rx_ring->rx_buf)
		return -ENOMEM;

	/* round up to nearest page */
	rx_ring->size = ALIGN(rx_ring->count * sizeof(union ice_32byte_rx_desc),
			      PAGE_SIZE);
	rx_ring->desc = dmam_alloc_coherent(dev, rx_ring->size, &rx_ring->dma,
					    GFP_KERNEL);
	if (!rx_ring->desc) {
		dev_err(dev, "Unable to allocate memory for the Rx descriptor ring, size=%d\n",
			rx_ring->size);
		goto err;
	}

	rx_ring->next_to_use = 0;
	rx_ring->next_to_clean = 0;
	return 0;

err:
	devm_kfree(dev, rx_ring->rx_buf);
	rx_ring->rx_buf = NULL;
	return -ENOMEM;
}

/**
 * ice_release_rx_desc - Store the new tail and head values
 * @rx_ring: ring to bump
 * @val: new head index
 */
static void ice_release_rx_desc(struct ice_ring *rx_ring, u32 val)
{
	u16 prev_ntu = rx_ring->next_to_use;

	rx_ring->next_to_use = val;

	/* update next to alloc since we have filled the ring */
	rx_ring->next_to_alloc = val;

	/* QRX_TAIL will be updated with any tail value, but hardware ignores
	 * the lower 3 bits. This makes it so we only bump tail on meaningful
	 * boundaries. Also, this allows us to bump tail on intervals of 8 up to
	 * the budget depending on the current traffic load.
	 */
	val &= ~0x7;
	if (prev_ntu != val) {
		/* Force memory writes to complete before letting h/w
		 * know there are new descriptors to fetch. (Only
		 * applicable for weak-ordered memory model archs,
		 * such as IA-64).
		 */
		wmb();
		writel(val, rx_ring->tail);
	}
}

/**
 * ice_alloc_mapped_page - recycle or make a new page
 * @rx_ring: ring to use
 * @bi: rx_buf struct to modify
 *
 * Returns true if the page was successfully allocated or
 * reused.
 */
static bool
ice_alloc_mapped_page(struct ice_ring *rx_ring, struct ice_rx_buf *bi)
{
	struct page *page = bi->page;
	dma_addr_t dma;

	/* since we are recycling buffers we should seldom need to alloc */
	if (likely(page)) {
		rx_ring->rx_stats.page_reuse_count++;
		return true;
	}

	/* alloc new page for storage */
	page = alloc_page(GFP_ATOMIC | __GFP_NOWARN);
	if (unlikely(!page)) {
		rx_ring->rx_stats.alloc_page_failed++;
		return false;
	}

	/* map page for use */
	dma = dma_map_page_attrs(rx_ring->dev, page, 0, PAGE_SIZE,
				 DMA_FROM_DEVICE, ICE_RX_DMA_ATTR);

	/* if mapping failed free memory back to system since
	 * there isn't much point in holding memory we can't use
	 */
	if (dma_mapping_error(rx_ring->dev, dma)) {
		__free_pages(page, 0);
		rx_ring->rx_stats.alloc_page_failed++;
		return false;
	}

	bi->dma = dma;
	bi->page = page;
	bi->page_offset = 0;
	page_ref_add(page, USHRT_MAX - 1);
	bi->pagecnt_bias = USHRT_MAX;

	return true;
}

/**
 * ice_alloc_rx_bufs - Replace used receive buffers
 * @rx_ring: ring to place buffers on
 * @cleaned_count: number of buffers to replace
 *
 * Returns false if all allocations were successful, true if any fail. Returning
 * true signals to the caller that we didn't replace cleaned_count buffers and
 * there is more work to do.
 *
 * First, try to clean "cleaned_count" Rx buffers. Then refill the cleaned Rx
 * buffers. Then bump tail at most one time. Grouping like this lets us avoid
 * multiple tail writes per call.
 */
bool ice_alloc_rx_bufs(struct ice_ring *rx_ring, u16 cleaned_count)
{
	union ice_32b_rx_flex_desc *rx_desc;
	u16 ntu = rx_ring->next_to_use;
	struct ice_rx_buf *bi;

	/* do nothing if no valid netdev defined */
	if (!rx_ring->netdev || !cleaned_count)
		return false;

	/* get the Rx descriptor and buffer based on next_to_use */
	rx_desc = ICE_RX_DESC(rx_ring, ntu);
	bi = &rx_ring->rx_buf[ntu];

	do {
		/* if we fail here, we have work remaining */
		if (!ice_alloc_mapped_page(rx_ring, bi))
			break;

		/* sync the buffer for use by the device */
		dma_sync_single_range_for_device(rx_ring->dev, bi->dma,
						 bi->page_offset,
						 ICE_RXBUF_2048,
						 DMA_FROM_DEVICE);

		/* Refresh the desc even if buffer_addrs didn't change
		 * because each write-back erases this info.
		 */
		rx_desc->read.pkt_addr = cpu_to_le64(bi->dma + bi->page_offset);

		rx_desc++;
		bi++;
		ntu++;
		if (unlikely(ntu == rx_ring->count)) {
			rx_desc = ICE_RX_DESC(rx_ring, 0);
			bi = rx_ring->rx_buf;
			ntu = 0;
		}

		/* clear the status bits for the next_to_use descriptor */
		rx_desc->wb.status_error0 = 0;

		cleaned_count--;
	} while (cleaned_count);

	if (rx_ring->next_to_use != ntu)
		ice_release_rx_desc(rx_ring, ntu);

	return !!cleaned_count;
}

/**
 * ice_page_is_reserved - check if reuse is possible
 * @page: page struct to check
 */
static bool ice_page_is_reserved(struct page *page)
{
	return (page_to_nid(page) != numa_mem_id()) || page_is_pfmemalloc(page);
}

/**
 * ice_rx_buf_adjust_pg_offset - Prepare Rx buffer for reuse
 * @rx_buf: Rx buffer to adjust
 * @size: Size of adjustment
 *
 * Update the offset within page so that Rx buf will be ready to be reused.
 * For systems with PAGE_SIZE < 8192 this function will flip the page offset
 * so the second half of page assigned to Rx buffer will be used, otherwise
 * the offset is moved by the @size bytes
 */
static void
ice_rx_buf_adjust_pg_offset(struct ice_rx_buf *rx_buf, unsigned int size)
{
#if (PAGE_SIZE < 8192)
	/* flip page offset to other buffer */
	rx_buf->page_offset ^= size;
#else
	/* move offset up to the next cache line */
	rx_buf->page_offset += size;
#endif
}

/**
 * ice_can_reuse_rx_page - Determine if page can be reused for another Rx
 * @rx_buf: buffer containing the page
 *
 * If page is reusable, we have a green light for calling ice_reuse_rx_page,
 * which will assign the current buffer to the buffer that next_to_alloc is
 * pointing to; otherwise, the DMA mapping needs to be destroyed and
 * page freed
 */
static bool ice_can_reuse_rx_page(struct ice_rx_buf *rx_buf)
{
#if (PAGE_SIZE >= 8192)
	unsigned int last_offset = PAGE_SIZE - ICE_RXBUF_2048;
#endif
	unsigned int pagecnt_bias = rx_buf->pagecnt_bias;
	struct page *page = rx_buf->page;

	/* avoid re-using remote pages */
	if (unlikely(ice_page_is_reserved(page)))
		return false;

#if (PAGE_SIZE < 8192)
	/* if we are only owner of page we can reuse it */
	if (unlikely((page_count(page) - pagecnt_bias) > 1))
		return false;
#else
	if (rx_buf->page_offset > last_offset)
		return false;
#endif /* PAGE_SIZE < 8192) */

	/* If we have drained the page fragment pool we need to update
	 * the pagecnt_bias and page count so that we fully restock the
	 * number of references the driver holds.
	 */
	if (unlikely(pagecnt_bias == 1)) {
		page_ref_add(page, USHRT_MAX - 1);
		rx_buf->pagecnt_bias = USHRT_MAX;
	}

	return true;
}

/**
 * ice_add_rx_frag - Add contents of Rx buffer to sk_buff as a frag
 * @rx_buf: buffer containing page to add
 * @skb: sk_buff to place the data into
 * @size: packet length from rx_desc
 *
 * This function will add the data contained in rx_buf->page to the skb.
 * It will just attach the page as a frag to the skb.
 * The function will then update the page offset.
 */
static void
ice_add_rx_frag(struct ice_rx_buf *rx_buf, struct sk_buff *skb,
		unsigned int size)
{
#if (PAGE_SIZE >= 8192)
	unsigned int truesize = SKB_DATA_ALIGN(size);
#else
	unsigned int truesize = ICE_RXBUF_2048;
#endif

	if (!size)
		return;
	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, rx_buf->page,
			rx_buf->page_offset, size, truesize);

	/* page is being used so we must update the page offset */
	ice_rx_buf_adjust_pg_offset(rx_buf, truesize);
}

/**
 * ice_reuse_rx_page - page flip buffer and store it back on the ring
 * @rx_ring: Rx descriptor ring to store buffers on
 * @old_buf: donor buffer to have page reused
 *
 * Synchronizes page for reuse by the adapter
 */
static void
ice_reuse_rx_page(struct ice_ring *rx_ring, struct ice_rx_buf *old_buf)
{
	u16 nta = rx_ring->next_to_alloc;
	struct ice_rx_buf *new_buf;

	new_buf = &rx_ring->rx_buf[nta];

	/* update, and store next to alloc */
	nta++;
	rx_ring->next_to_alloc = (nta < rx_ring->count) ? nta : 0;

	/* Transfer page from old buffer to new buffer.
	 * Move each member individually to avoid possible store
	 * forwarding stalls and unnecessary copy of skb.
	 */
	new_buf->dma = old_buf->dma;
	new_buf->page = old_buf->page;
	new_buf->page_offset = old_buf->page_offset;
	new_buf->pagecnt_bias = old_buf->pagecnt_bias;
}

/**
 * ice_get_rx_buf - Fetch Rx buffer and synchronize data for use
 * @rx_ring: Rx descriptor ring to transact packets on
 * @skb: skb to be used
 * @size: size of buffer to add to skb
 *
 * This function will pull an Rx buffer from the ring and synchronize it
 * for use by the CPU.
 */
static struct ice_rx_buf *
ice_get_rx_buf(struct ice_ring *rx_ring, struct sk_buff **skb,
	       const unsigned int size)
{
	struct ice_rx_buf *rx_buf;

	rx_buf = &rx_ring->rx_buf[rx_ring->next_to_clean];
	prefetchw(rx_buf->page);
	*skb = rx_buf->skb;

	if (!size)
		return rx_buf;
	/* we are reusing so sync this buffer for CPU use */
	dma_sync_single_range_for_cpu(rx_ring->dev, rx_buf->dma,
				      rx_buf->page_offset, size,
				      DMA_FROM_DEVICE);

	/* We have pulled a buffer for use, so decrement pagecnt_bias */
	rx_buf->pagecnt_bias--;

	return rx_buf;
}

/**
 * ice_construct_skb - Allocate skb and populate it
 * @rx_ring: Rx descriptor ring to transact packets on
 * @rx_buf: Rx buffer to pull data from
 * @size: the length of the packet
 *
 * This function allocates an skb. It then populates it with the page
 * data from the current receive descriptor, taking care to set up the
 * skb correctly.
 */
static struct sk_buff *
ice_construct_skb(struct ice_ring *rx_ring, struct ice_rx_buf *rx_buf,
		  unsigned int size)
{
	void *va = page_address(rx_buf->page) + rx_buf->page_offset;
	unsigned int headlen;
	struct sk_buff *skb;

	/* prefetch first cache line of first page */
	prefetch(va);
#if L1_CACHE_BYTES < 128
	prefetch((u8 *)va + L1_CACHE_BYTES);
#endif /* L1_CACHE_BYTES */

	/* allocate a skb to store the frags */
	skb = __napi_alloc_skb(&rx_ring->q_vector->napi, ICE_RX_HDR_SIZE,
			       GFP_ATOMIC | __GFP_NOWARN);
	if (unlikely(!skb))
		return NULL;

	skb_record_rx_queue(skb, rx_ring->q_index);
	/* Determine available headroom for copy */
	headlen = size;
	if (headlen > ICE_RX_HDR_SIZE)
		headlen = eth_get_headlen(skb->dev, va, ICE_RX_HDR_SIZE);

	/* align pull length to size of long to optimize memcpy performance */
	memcpy(__skb_put(skb, headlen), va, ALIGN(headlen, sizeof(long)));

	/* if we exhaust the linear part then add what is left as a frag */
	size -= headlen;
	if (size) {
#if (PAGE_SIZE >= 8192)
		unsigned int truesize = SKB_DATA_ALIGN(size);
#else
		unsigned int truesize = ICE_RXBUF_2048;
#endif
		skb_add_rx_frag(skb, 0, rx_buf->page,
				rx_buf->page_offset + headlen, size, truesize);
		/* buffer is used by skb, update page_offset */
		ice_rx_buf_adjust_pg_offset(rx_buf, truesize);
	} else {
		/* buffer is unused, reset bias back to rx_buf; data was copied
		 * onto skb's linear part so there's no need for adjusting
		 * page offset and we can reuse this buffer as-is
		 */
		rx_buf->pagecnt_bias++;
	}

	return skb;
}

/**
 * ice_put_rx_buf - Clean up used buffer and either recycle or free
 * @rx_ring: Rx descriptor ring to transact packets on
 * @rx_buf: Rx buffer to pull data from
 *
 * This function will  clean up the contents of the rx_buf. It will
 * either recycle the buffer or unmap it and free the associated resources.
 */
static void ice_put_rx_buf(struct ice_ring *rx_ring, struct ice_rx_buf *rx_buf)
{
	if (!rx_buf)
		return;

	if (ice_can_reuse_rx_page(rx_buf)) {
		/* hand second half of page back to the ring */
		ice_reuse_rx_page(rx_ring, rx_buf);
		rx_ring->rx_stats.page_reuse_count++;
	} else {
		/* we are not reusing the buffer so unmap it */
		dma_unmap_page_attrs(rx_ring->dev, rx_buf->dma, PAGE_SIZE,
				     DMA_FROM_DEVICE, ICE_RX_DMA_ATTR);
		__page_frag_cache_drain(rx_buf->page, rx_buf->pagecnt_bias);
	}

	/* clear contents of buffer_info */
	rx_buf->page = NULL;
	rx_buf->skb = NULL;
}

/**
 * ice_cleanup_headers - Correct empty headers
 * @skb: pointer to current skb being fixed
 *
 * Also address the case where we are pulling data in on pages only
 * and as such no data is present in the skb header.
 *
 * In addition if skb is not at least 60 bytes we need to pad it so that
 * it is large enough to qualify as a valid Ethernet frame.
 *
 * Returns true if an error was encountered and skb was freed.
 */
static bool ice_cleanup_headers(struct sk_buff *skb)
{
	/* if eth_skb_pad returns an error the skb was freed */
	if (eth_skb_pad(skb))
		return true;

	return false;
}

/**
 * ice_test_staterr - tests bits in Rx descriptor status and error fields
 * @rx_desc: pointer to receive descriptor (in le64 format)
 * @stat_err_bits: value to mask
 *
 * This function does some fast chicanery in order to return the
 * value of the mask which is really only used for boolean tests.
 * The status_error_len doesn't need to be shifted because it begins
 * at offset zero.
 */
static bool
ice_test_staterr(union ice_32b_rx_flex_desc *rx_desc, const u16 stat_err_bits)
{
	return !!(rx_desc->wb.status_error0 &
		  cpu_to_le16(stat_err_bits));
}

/**
 * ice_is_non_eop - process handling of non-EOP buffers
 * @rx_ring: Rx ring being processed
 * @rx_desc: Rx descriptor for current buffer
 * @skb: Current socket buffer containing buffer in progress
 *
 * This function updates next to clean. If the buffer is an EOP buffer
 * this function exits returning false, otherwise it will place the
 * sk_buff in the next buffer to be chained and return true indicating
 * that this is in fact a non-EOP buffer.
 */
static bool
ice_is_non_eop(struct ice_ring *rx_ring, union ice_32b_rx_flex_desc *rx_desc,
	       struct sk_buff *skb)
{
	u32 ntc = rx_ring->next_to_clean + 1;

	/* fetch, update, and store next to clean */
	ntc = (ntc < rx_ring->count) ? ntc : 0;
	rx_ring->next_to_clean = ntc;

	prefetch(ICE_RX_DESC(rx_ring, ntc));

	/* if we are the last buffer then there is nothing else to do */
#define ICE_RXD_EOF BIT(ICE_RX_FLEX_DESC_STATUS0_EOF_S)
	if (likely(ice_test_staterr(rx_desc, ICE_RXD_EOF)))
		return false;

	/* place skb in next buffer to be received */
	rx_ring->rx_buf[ntc].skb = skb;
	rx_ring->rx_stats.non_eop_descs++;

	return true;
}

/**
 * ice_ptype_to_htype - get a hash type
 * @ptype: the ptype value from the descriptor
 *
 * Returns a hash type to be used by skb_set_hash
 */
static enum pkt_hash_types ice_ptype_to_htype(u8 __always_unused ptype)
{
	return PKT_HASH_TYPE_NONE;
}

/**
 * ice_rx_hash - set the hash value in the skb
 * @rx_ring: descriptor ring
 * @rx_desc: specific descriptor
 * @skb: pointer to current skb
 * @rx_ptype: the ptype value from the descriptor
 */
static void
ice_rx_hash(struct ice_ring *rx_ring, union ice_32b_rx_flex_desc *rx_desc,
	    struct sk_buff *skb, u8 rx_ptype)
{
	struct ice_32b_rx_flex_desc_nic *nic_mdid;
	u32 hash;

	if (!(rx_ring->netdev->features & NETIF_F_RXHASH))
		return;

	if (rx_desc->wb.rxdid != ICE_RXDID_FLEX_NIC)
		return;

	nic_mdid = (struct ice_32b_rx_flex_desc_nic *)rx_desc;
	hash = le32_to_cpu(nic_mdid->rss_hash);
	skb_set_hash(skb, hash, ice_ptype_to_htype(rx_ptype));
}

/**
 * ice_rx_csum - Indicate in skb if checksum is good
 * @ring: the ring we care about
 * @skb: skb currently being received and modified
 * @rx_desc: the receive descriptor
 * @ptype: the packet type decoded by hardware
 *
 * skb->protocol must be set before this function is called
 */
static void
ice_rx_csum(struct ice_ring *ring, struct sk_buff *skb,
	    union ice_32b_rx_flex_desc *rx_desc, u8 ptype)
{
	struct ice_rx_ptype_decoded decoded;
	u32 rx_error, rx_status;
	bool ipv4, ipv6;

	rx_status = le16_to_cpu(rx_desc->wb.status_error0);
	rx_error = rx_status;

	decoded = ice_decode_rx_desc_ptype(ptype);

	/* Start with CHECKSUM_NONE and by default csum_level = 0 */
	skb->ip_summed = CHECKSUM_NONE;
	skb_checksum_none_assert(skb);

	/* check if Rx checksum is enabled */
	if (!(ring->netdev->features & NETIF_F_RXCSUM))
		return;

	/* check if HW has decoded the packet and checksum */
	if (!(rx_status & BIT(ICE_RX_FLEX_DESC_STATUS0_L3L4P_S)))
		return;

	if (!(decoded.known && decoded.outer_ip))
		return;

	ipv4 = (decoded.outer_ip == ICE_RX_PTYPE_OUTER_IP) &&
	       (decoded.outer_ip_ver == ICE_RX_PTYPE_OUTER_IPV4);
	ipv6 = (decoded.outer_ip == ICE_RX_PTYPE_OUTER_IP) &&
	       (decoded.outer_ip_ver == ICE_RX_PTYPE_OUTER_IPV6);

	if (ipv4 && (rx_error & (BIT(ICE_RX_FLEX_DESC_STATUS0_XSUM_IPE_S) |
				 BIT(ICE_RX_FLEX_DESC_STATUS0_XSUM_EIPE_S))))
		goto checksum_fail;
	else if (ipv6 && (rx_status &
		 (BIT(ICE_RX_FLEX_DESC_STATUS0_IPV6EXADD_S))))
		goto checksum_fail;

	/* check for L4 errors and handle packets that were not able to be
	 * checksummed due to arrival speed
	 */
	if (rx_error & BIT(ICE_RX_FLEX_DESC_STATUS0_XSUM_L4E_S))
		goto checksum_fail;

	/* Only report checksum unnecessary for TCP, UDP, or SCTP */
	switch (decoded.inner_prot) {
	case ICE_RX_PTYPE_INNER_PROT_TCP:
	case ICE_RX_PTYPE_INNER_PROT_UDP:
	case ICE_RX_PTYPE_INNER_PROT_SCTP:
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	default:
		break;
	}
	return;

checksum_fail:
	ring->vsi->back->hw_csum_rx_error++;
}

/**
 * ice_process_skb_fields - Populate skb header fields from Rx descriptor
 * @rx_ring: Rx descriptor ring packet is being transacted on
 * @rx_desc: pointer to the EOP Rx descriptor
 * @skb: pointer to current skb being populated
 * @ptype: the packet type decoded by hardware
 *
 * This function checks the ring, descriptor, and packet information in
 * order to populate the hash, checksum, VLAN, protocol, and
 * other fields within the skb.
 */
static void
ice_process_skb_fields(struct ice_ring *rx_ring,
		       union ice_32b_rx_flex_desc *rx_desc,
		       struct sk_buff *skb, u8 ptype)
{
	ice_rx_hash(rx_ring, rx_desc, skb, ptype);

	/* modifies the skb - consumes the enet header */
	skb->protocol = eth_type_trans(skb, rx_ring->netdev);

	ice_rx_csum(rx_ring, skb, rx_desc, ptype);
}

/**
 * ice_receive_skb - Send a completed packet up the stack
 * @rx_ring: Rx ring in play
 * @skb: packet to send up
 * @vlan_tag: VLAN tag for packet
 *
 * This function sends the completed packet (via. skb) up the stack using
 * gro receive functions (with/without VLAN tag)
 */
static void
ice_receive_skb(struct ice_ring *rx_ring, struct sk_buff *skb, u16 vlan_tag)
{
	if ((rx_ring->netdev->features & NETIF_F_HW_VLAN_CTAG_RX) &&
	    (vlan_tag & VLAN_VID_MASK))
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vlan_tag);
	napi_gro_receive(&rx_ring->q_vector->napi, skb);
}

/**
 * ice_clean_rx_irq - Clean completed descriptors from Rx ring - bounce buf
 * @rx_ring: Rx descriptor ring to transact packets on
 * @budget: Total limit on number of packets to process
 *
 * This function provides a "bounce buffer" approach to Rx interrupt
 * processing. The advantage to this is that on systems that have
 * expensive overhead for IOMMU access this provides a means of avoiding
 * it by maintaining the mapping of the page to the system.
 *
 * Returns amount of work completed
 */
static int ice_clean_rx_irq(struct ice_ring *rx_ring, int budget)
{
	unsigned int total_rx_bytes = 0, total_rx_pkts = 0;
	u16 cleaned_count = ICE_DESC_UNUSED(rx_ring);
	bool failure;

	/* start the loop to process Rx packets bounded by 'budget' */
	while (likely(total_rx_pkts < (unsigned int)budget)) {
		union ice_32b_rx_flex_desc *rx_desc;
		struct ice_rx_buf *rx_buf;
		struct sk_buff *skb;
		unsigned int size;
		u16 stat_err_bits;
		u16 vlan_tag = 0;
		u8 rx_ptype;

		/* get the Rx desc from Rx ring based on 'next_to_clean' */
		rx_desc = ICE_RX_DESC(rx_ring, rx_ring->next_to_clean);

		/* status_error_len will always be zero for unused descriptors
		 * because it's cleared in cleanup, and overlaps with hdr_addr
		 * which is always zero because packet split isn't used, if the
		 * hardware wrote DD then it will be non-zero
		 */
		stat_err_bits = BIT(ICE_RX_FLEX_DESC_STATUS0_DD_S);
		if (!ice_test_staterr(rx_desc, stat_err_bits))
			break;

		/* This memory barrier is needed to keep us from reading
		 * any other fields out of the rx_desc until we know the
		 * DD bit is set.
		 */
		dma_rmb();

		size = le16_to_cpu(rx_desc->wb.pkt_len) &
			ICE_RX_FLX_DESC_PKT_LEN_M;

		/* retrieve a buffer from the ring */
		rx_buf = ice_get_rx_buf(rx_ring, &skb, size);

		if (skb)
			ice_add_rx_frag(rx_buf, skb, size);
		else
			skb = ice_construct_skb(rx_ring, rx_buf, size);

		/* exit if we failed to retrieve a buffer */
		if (!skb) {
			rx_ring->rx_stats.alloc_buf_failed++;
			if (rx_buf)
				rx_buf->pagecnt_bias++;
			break;
		}

		ice_put_rx_buf(rx_ring, rx_buf);
		cleaned_count++;

		/* skip if it is NOP desc */
		if (ice_is_non_eop(rx_ring, rx_desc, skb))
			continue;

		stat_err_bits = BIT(ICE_RX_FLEX_DESC_STATUS0_RXE_S);
		if (unlikely(ice_test_staterr(rx_desc, stat_err_bits))) {
			dev_kfree_skb_any(skb);
			continue;
		}

		stat_err_bits = BIT(ICE_RX_FLEX_DESC_STATUS0_L2TAG1P_S);
		if (ice_test_staterr(rx_desc, stat_err_bits))
			vlan_tag = le16_to_cpu(rx_desc->wb.l2tag1);

		/* correct empty headers and pad skb if needed (to make valid
		 * ethernet frame
		 */
		if (ice_cleanup_headers(skb)) {
			skb = NULL;
			continue;
		}

		/* probably a little skewed due to removing CRC */
		total_rx_bytes += skb->len;

		/* populate checksum, VLAN, and protocol */
		rx_ptype = le16_to_cpu(rx_desc->wb.ptype_flex_flags0) &
			ICE_RX_FLEX_DESC_PTYPE_M;

		ice_process_skb_fields(rx_ring, rx_desc, skb, rx_ptype);

		/* send completed skb up the stack */
		ice_receive_skb(rx_ring, skb, vlan_tag);

		/* update budget accounting */
		total_rx_pkts++;
	}

	/* return up to cleaned_count buffers to hardware */
	failure = ice_alloc_rx_bufs(rx_ring, cleaned_count);

	/* update queue and vector specific stats */
	u64_stats_update_begin(&rx_ring->syncp);
	rx_ring->stats.pkts += total_rx_pkts;
	rx_ring->stats.bytes += total_rx_bytes;
	u64_stats_update_end(&rx_ring->syncp);
	rx_ring->q_vector->rx.total_pkts += total_rx_pkts;
	rx_ring->q_vector->rx.total_bytes += total_rx_bytes;

	/* guarantee a trip back through this routine if there was a failure */
	return failure ? budget : (int)total_rx_pkts;
}

/**
 * ice_adjust_itr_by_size_and_speed - Adjust ITR based on current traffic
 * @port_info: port_info structure containing the current link speed
 * @avg_pkt_size: average size of Tx or Rx packets based on clean routine
 * @itr: ITR value to update
 *
 * Calculate how big of an increment should be applied to the ITR value passed
 * in based on wmem_default, SKB overhead, Ethernet overhead, and the current
 * link speed.
 *
 * The following is a calculation derived from:
 *  wmem_default / (size + overhead) = desired_pkts_per_int
 *  rate / bits_per_byte / (size + Ethernet overhead) = pkt_rate
 *  (desired_pkt_rate / pkt_rate) * usecs_per_sec = ITR value
 *
 * Assuming wmem_default is 212992 and overhead is 640 bytes per
 * packet, (256 skb, 64 headroom, 320 shared info), we can reduce the
 * formula down to:
 *
 *	 wmem_default * bits_per_byte * usecs_per_sec   pkt_size + 24
 * ITR = -------------------------------------------- * --------------
 *			     rate			pkt_size + 640
 */
static unsigned int
ice_adjust_itr_by_size_and_speed(struct ice_port_info *port_info,
				 unsigned int avg_pkt_size,
				 unsigned int itr)
{
	switch (port_info->phy.link_info.link_speed) {
	case ICE_AQ_LINK_SPEED_100GB:
		itr += DIV_ROUND_UP(17 * (avg_pkt_size + 24),
				    avg_pkt_size + 640);
		break;
	case ICE_AQ_LINK_SPEED_50GB:
		itr += DIV_ROUND_UP(34 * (avg_pkt_size + 24),
				    avg_pkt_size + 640);
		break;
	case ICE_AQ_LINK_SPEED_40GB:
		itr += DIV_ROUND_UP(43 * (avg_pkt_size + 24),
				    avg_pkt_size + 640);
		break;
	case ICE_AQ_LINK_SPEED_25GB:
		itr += DIV_ROUND_UP(68 * (avg_pkt_size + 24),
				    avg_pkt_size + 640);
		break;
	case ICE_AQ_LINK_SPEED_20GB:
		itr += DIV_ROUND_UP(85 * (avg_pkt_size + 24),
				    avg_pkt_size + 640);
		break;
	case ICE_AQ_LINK_SPEED_10GB:
		/* fall through */
	default:
		itr += DIV_ROUND_UP(170 * (avg_pkt_size + 24),
				    avg_pkt_size + 640);
		break;
	}

	if ((itr & ICE_ITR_MASK) > ICE_ITR_ADAPTIVE_MAX_USECS) {
		itr &= ICE_ITR_ADAPTIVE_LATENCY;
		itr += ICE_ITR_ADAPTIVE_MAX_USECS;
	}

	return itr;
}

/**
 * ice_update_itr - update the adaptive ITR value based on statistics
 * @q_vector: structure containing interrupt and ring information
 * @rc: structure containing ring performance data
 *
 * Stores a new ITR value based on packets and byte
 * counts during the last interrupt.  The advantage of per interrupt
 * computation is faster updates and more accurate ITR for the current
 * traffic pattern.  Constants in this function were computed
 * based on theoretical maximum wire speed and thresholds were set based
 * on testing data as well as attempting to minimize response time
 * while increasing bulk throughput.
 */
static void
ice_update_itr(struct ice_q_vector *q_vector, struct ice_ring_container *rc)
{
	unsigned long next_update = jiffies;
	unsigned int packets, bytes, itr;
	bool container_is_rx;

	if (!rc->ring || !ITR_IS_DYNAMIC(rc->itr_setting))
		return;

	/* If itr_countdown is set it means we programmed an ITR within
	 * the last 4 interrupt cycles. This has a side effect of us
	 * potentially firing an early interrupt. In order to work around
	 * this we need to throw out any data received for a few
	 * interrupts following the update.
	 */
	if (q_vector->itr_countdown) {
		itr = rc->target_itr;
		goto clear_counts;
	}

	container_is_rx = (&q_vector->rx == rc);
	/* For Rx we want to push the delay up and default to low latency.
	 * for Tx we want to pull the delay down and default to high latency.
	 */
	itr = container_is_rx ?
		ICE_ITR_ADAPTIVE_MIN_USECS | ICE_ITR_ADAPTIVE_LATENCY :
		ICE_ITR_ADAPTIVE_MAX_USECS | ICE_ITR_ADAPTIVE_LATENCY;

	/* If we didn't update within up to 1 - 2 jiffies we can assume
	 * that either packets are coming in so slow there hasn't been
	 * any work, or that there is so much work that NAPI is dealing
	 * with interrupt moderation and we don't need to do anything.
	 */
	if (time_after(next_update, rc->next_update))
		goto clear_counts;

	prefetch(q_vector->vsi->port_info);

	packets = rc->total_pkts;
	bytes = rc->total_bytes;

	if (container_is_rx) {
		/* If Rx there are 1 to 4 packets and bytes are less than
		 * 9000 assume insufficient data to use bulk rate limiting
		 * approach unless Tx is already in bulk rate limiting. We
		 * are likely latency driven.
		 */
		if (packets && packets < 4 && bytes < 9000 &&
		    (q_vector->tx.target_itr & ICE_ITR_ADAPTIVE_LATENCY)) {
			itr = ICE_ITR_ADAPTIVE_LATENCY;
			goto adjust_by_size_and_speed;
		}
	} else if (packets < 4) {
		/* If we have Tx and Rx ITR maxed and Tx ITR is running in
		 * bulk mode and we are receiving 4 or fewer packets just
		 * reset the ITR_ADAPTIVE_LATENCY bit for latency mode so
		 * that the Rx can relax.
		 */
		if (rc->target_itr == ICE_ITR_ADAPTIVE_MAX_USECS &&
		    (q_vector->rx.target_itr & ICE_ITR_MASK) ==
		    ICE_ITR_ADAPTIVE_MAX_USECS)
			goto clear_counts;
	} else if (packets > 32) {
		/* If we have processed over 32 packets in a single interrupt
		 * for Tx assume we need to switch over to "bulk" mode.
		 */
		rc->target_itr &= ~ICE_ITR_ADAPTIVE_LATENCY;
	}

	/* We have no packets to actually measure against. This means
	 * either one of the other queues on this vector is active or
	 * we are a Tx queue doing TSO with too high of an interrupt rate.
	 *
	 * Between 4 and 56 we can assume that our current interrupt delay
	 * is only slightly too low. As such we should increase it by a small
	 * fixed amount.
	 */
	if (packets < 56) {
		itr = rc->target_itr + ICE_ITR_ADAPTIVE_MIN_INC;
		if ((itr & ICE_ITR_MASK) > ICE_ITR_ADAPTIVE_MAX_USECS) {
			itr &= ICE_ITR_ADAPTIVE_LATENCY;
			itr += ICE_ITR_ADAPTIVE_MAX_USECS;
		}
		goto clear_counts;
	}

	if (packets <= 256) {
		itr = min(q_vector->tx.current_itr, q_vector->rx.current_itr);
		itr &= ICE_ITR_MASK;

		/* Between 56 and 112 is our "goldilocks" zone where we are
		 * working out "just right". Just report that our current
		 * ITR is good for us.
		 */
		if (packets <= 112)
			goto clear_counts;

		/* If packet count is 128 or greater we are likely looking
		 * at a slight overrun of the delay we want. Try halving
		 * our delay to see if that will cut the number of packets
		 * in half per interrupt.
		 */
		itr >>= 1;
		itr &= ICE_ITR_MASK;
		if (itr < ICE_ITR_ADAPTIVE_MIN_USECS)
			itr = ICE_ITR_ADAPTIVE_MIN_USECS;

		goto clear_counts;
	}

	/* The paths below assume we are dealing with a bulk ITR since
	 * number of packets is greater than 256. We are just going to have
	 * to compute a value and try to bring the count under control,
	 * though for smaller packet sizes there isn't much we can do as
	 * NAPI polling will likely be kicking in sooner rather than later.
	 */
	itr = ICE_ITR_ADAPTIVE_BULK;

adjust_by_size_and_speed:

	/* based on checks above packets cannot be 0 so division is safe */
	itr = ice_adjust_itr_by_size_and_speed(q_vector->vsi->port_info,
					       bytes / packets, itr);

clear_counts:
	/* write back value */
	rc->target_itr = itr;

	/* next update should occur within next jiffy */
	rc->next_update = next_update + 1;

	rc->total_bytes = 0;
	rc->total_pkts = 0;
}

/**
 * ice_buildreg_itr - build value for writing to the GLINT_DYN_CTL register
 * @itr_idx: interrupt throttling index
 * @itr: interrupt throttling value in usecs
 */
static u32 ice_buildreg_itr(u16 itr_idx, u16 itr)
{
	/* The ITR value is reported in microseconds, and the register value is
	 * recorded in 2 microsecond units. For this reason we only need to
	 * shift by the GLINT_DYN_CTL_INTERVAL_S - ICE_ITR_GRAN_S to apply this
	 * granularity as a shift instead of division. The mask makes sure the
	 * ITR value is never odd so we don't accidentally write into the field
	 * prior to the ITR field.
	 */
	itr &= ICE_ITR_MASK;

	return GLINT_DYN_CTL_INTENA_M | GLINT_DYN_CTL_CLEARPBA_M |
		(itr_idx << GLINT_DYN_CTL_ITR_INDX_S) |
		(itr << (GLINT_DYN_CTL_INTERVAL_S - ICE_ITR_GRAN_S));
}

/* The act of updating the ITR will cause it to immediately trigger. In order
 * to prevent this from throwing off adaptive update statistics we defer the
 * update so that it can only happen so often. So after either Tx or Rx are
 * updated we make the adaptive scheme wait until either the ITR completely
 * expires via the next_update expiration or we have been through at least
 * 3 interrupts.
 */
#define ITR_COUNTDOWN_START 3

/**
 * ice_update_ena_itr - Update ITR and re-enable MSIX interrupt
 * @q_vector: q_vector for which ITR is being updated and interrupt enabled
 */
static void ice_update_ena_itr(struct ice_q_vector *q_vector)
{
	struct ice_ring_container *tx = &q_vector->tx;
	struct ice_ring_container *rx = &q_vector->rx;
	struct ice_vsi *vsi = q_vector->vsi;
	u32 itr_val;

	/* when exiting WB_ON_ITR lets set a low ITR value and trigger
	 * interrupts to expire right away in case we have more work ready to go
	 * already
	 */
	if (q_vector->itr_countdown == ICE_IN_WB_ON_ITR_MODE) {
		itr_val = ice_buildreg_itr(rx->itr_idx, ICE_WB_ON_ITR_USECS);
		wr32(&vsi->back->hw, GLINT_DYN_CTL(q_vector->reg_idx), itr_val);
		/* set target back to last user set value */
		rx->target_itr = rx->itr_setting;
		/* set current to what we just wrote and dynamic if needed */
		rx->current_itr = ICE_WB_ON_ITR_USECS |
			(rx->itr_setting & ICE_ITR_DYNAMIC);
		/* allow normal interrupt flow to start */
		q_vector->itr_countdown = 0;
		return;
	}

	/* This will do nothing if dynamic updates are not enabled */
	ice_update_itr(q_vector, tx);
	ice_update_itr(q_vector, rx);

	/* This block of logic allows us to get away with only updating
	 * one ITR value with each interrupt. The idea is to perform a
	 * pseudo-lazy update with the following criteria.
	 *
	 * 1. Rx is given higher priority than Tx if both are in same state
	 * 2. If we must reduce an ITR that is given highest priority.
	 * 3. We then give priority to increasing ITR based on amount.
	 */
	if (rx->target_itr < rx->current_itr) {
		/* Rx ITR needs to be reduced, this is highest priority */
		itr_val = ice_buildreg_itr(rx->itr_idx, rx->target_itr);
		rx->current_itr = rx->target_itr;
		q_vector->itr_countdown = ITR_COUNTDOWN_START;
	} else if ((tx->target_itr < tx->current_itr) ||
		   ((rx->target_itr - rx->current_itr) <
		    (tx->target_itr - tx->current_itr))) {
		/* Tx ITR needs to be reduced, this is second priority
		 * Tx ITR needs to be increased more than Rx, fourth priority
		 */
		itr_val = ice_buildreg_itr(tx->itr_idx, tx->target_itr);
		tx->current_itr = tx->target_itr;
		q_vector->itr_countdown = ITR_COUNTDOWN_START;
	} else if (rx->current_itr != rx->target_itr) {
		/* Rx ITR needs to be increased, third priority */
		itr_val = ice_buildreg_itr(rx->itr_idx, rx->target_itr);
		rx->current_itr = rx->target_itr;
		q_vector->itr_countdown = ITR_COUNTDOWN_START;
	} else {
		/* Still have to re-enable the interrupts */
		itr_val = ice_buildreg_itr(ICE_ITR_NONE, 0);
		if (q_vector->itr_countdown)
			q_vector->itr_countdown--;
	}

	if (!test_bit(__ICE_DOWN, q_vector->vsi->state))
		wr32(&q_vector->vsi->back->hw,
		     GLINT_DYN_CTL(q_vector->reg_idx),
		     itr_val);
}

/**
 * ice_set_wb_on_itr - set WB_ON_ITR for this q_vector
 * @q_vector: q_vector to set WB_ON_ITR on
 *
 * We need to tell hardware to write-back completed descriptors even when
 * interrupts are disabled. Descriptors will be written back on cache line
 * boundaries without WB_ON_ITR enabled, but if we don't enable WB_ON_ITR
 * descriptors may not be written back if they don't fill a cache line until the
 * next interrupt.
 *
 * This sets the write-back frequency to 2 microseconds as that is the minimum
 * value that's not 0 due to ITR granularity. Also, set the INTENA_MSK bit to
 * make sure hardware knows we aren't meddling with the INTENA_M bit.
 */
static void ice_set_wb_on_itr(struct ice_q_vector *q_vector)
{
	struct ice_vsi *vsi = q_vector->vsi;

	/* already in WB_ON_ITR mode no need to change it */
	if (q_vector->itr_countdown == ICE_IN_WB_ON_ITR_MODE)
		return;

	if (q_vector->num_ring_rx)
		wr32(&vsi->back->hw, GLINT_DYN_CTL(q_vector->reg_idx),
		     ICE_GLINT_DYN_CTL_WB_ON_ITR(ICE_WB_ON_ITR_USECS,
						 ICE_RX_ITR));

	if (q_vector->num_ring_tx)
		wr32(&vsi->back->hw, GLINT_DYN_CTL(q_vector->reg_idx),
		     ICE_GLINT_DYN_CTL_WB_ON_ITR(ICE_WB_ON_ITR_USECS,
						 ICE_TX_ITR));

	q_vector->itr_countdown = ICE_IN_WB_ON_ITR_MODE;
}

/**
 * ice_napi_poll - NAPI polling Rx/Tx cleanup routine
 * @napi: napi struct with our devices info in it
 * @budget: amount of work driver is allowed to do this pass, in packets
 *
 * This function will clean all queues associated with a q_vector.
 *
 * Returns the amount of work done
 */
int ice_napi_poll(struct napi_struct *napi, int budget)
{
	struct ice_q_vector *q_vector =
				container_of(napi, struct ice_q_vector, napi);
	bool clean_complete = true;
	struct ice_ring *ring;
	int budget_per_ring;
	int work_done = 0;

	/* Since the actual Tx work is minimal, we can give the Tx a larger
	 * budget and be more aggressive about cleaning up the Tx descriptors.
	 */
	ice_for_each_ring(ring, q_vector->tx)
		if (!ice_clean_tx_irq(ring, budget))
			clean_complete = false;

	/* Handle case where we are called by netpoll with a budget of 0 */
	if (unlikely(budget <= 0))
		return budget;

	/* normally we have 1 Rx ring per q_vector */
	if (unlikely(q_vector->num_ring_rx > 1))
		/* We attempt to distribute budget to each Rx queue fairly, but
		 * don't allow the budget to go below 1 because that would exit
		 * polling early.
		 */
		budget_per_ring = max(budget / q_vector->num_ring_rx, 1);
	else
		/* Max of 1 Rx ring in this q_vector so give it the budget */
		budget_per_ring = budget;

	ice_for_each_ring(ring, q_vector->rx) {
		int cleaned;

		cleaned = ice_clean_rx_irq(ring, budget_per_ring);
		work_done += cleaned;
		/* if we clean as many as budgeted, we must not be done */
		if (cleaned >= budget_per_ring)
			clean_complete = false;
	}

	/* If work not completed, return budget and polling will return */
	if (!clean_complete)
		return budget;

	/* Exit the polling mode, but don't re-enable interrupts if stack might
	 * poll us due to busy-polling
	 */
	if (likely(napi_complete_done(napi, work_done)))
		ice_update_ena_itr(q_vector);
	else
		ice_set_wb_on_itr(q_vector);

	return min_t(int, work_done, budget - 1);
}

/* helper function for building cmd/type/offset */
static __le64
build_ctob(u64 td_cmd, u64 td_offset, unsigned int size, u64 td_tag)
{
	return cpu_to_le64(ICE_TX_DESC_DTYPE_DATA |
			   (td_cmd    << ICE_TXD_QW1_CMD_S) |
			   (td_offset << ICE_TXD_QW1_OFFSET_S) |
			   ((u64)size << ICE_TXD_QW1_TX_BUF_SZ_S) |
			   (td_tag    << ICE_TXD_QW1_L2TAG1_S));
}

/**
 * __ice_maybe_stop_tx - 2nd level check for Tx stop conditions
 * @tx_ring: the ring to be checked
 * @size: the size buffer we want to assure is available
 *
 * Returns -EBUSY if a stop is needed, else 0
 */
static int __ice_maybe_stop_tx(struct ice_ring *tx_ring, unsigned int size)
{
	netif_stop_subqueue(tx_ring->netdev, tx_ring->q_index);
	/* Memory barrier before checking head and tail */
	smp_mb();

	/* Check again in a case another CPU has just made room available. */
	if (likely(ICE_DESC_UNUSED(tx_ring) < size))
		return -EBUSY;

	/* A reprieve! - use start_subqueue because it doesn't call schedule */
	netif_start_subqueue(tx_ring->netdev, tx_ring->q_index);
	++tx_ring->tx_stats.restart_q;
	return 0;
}

/**
 * ice_maybe_stop_tx - 1st level check for Tx stop conditions
 * @tx_ring: the ring to be checked
 * @size:    the size buffer we want to assure is available
 *
 * Returns 0 if stop is not needed
 */
static int ice_maybe_stop_tx(struct ice_ring *tx_ring, unsigned int size)
{
	if (likely(ICE_DESC_UNUSED(tx_ring) >= size))
		return 0;

	return __ice_maybe_stop_tx(tx_ring, size);
}

/**
 * ice_tx_map - Build the Tx descriptor
 * @tx_ring: ring to send buffer on
 * @first: first buffer info buffer to use
 * @off: pointer to struct that holds offload parameters
 *
 * This function loops over the skb data pointed to by *first
 * and gets a physical address for each memory location and programs
 * it and the length into the transmit descriptor.
 */
static void
ice_tx_map(struct ice_ring *tx_ring, struct ice_tx_buf *first,
	   struct ice_tx_offload_params *off)
{
	u64 td_offset, td_tag, td_cmd;
	u16 i = tx_ring->next_to_use;
	skb_frag_t *frag;
	unsigned int data_len, size;
	struct ice_tx_desc *tx_desc;
	struct ice_tx_buf *tx_buf;
	struct sk_buff *skb;
	dma_addr_t dma;

	td_tag = off->td_l2tag1;
	td_cmd = off->td_cmd;
	td_offset = off->td_offset;
	skb = first->skb;

	data_len = skb->data_len;
	size = skb_headlen(skb);

	tx_desc = ICE_TX_DESC(tx_ring, i);

	if (first->tx_flags & ICE_TX_FLAGS_HW_VLAN) {
		td_cmd |= (u64)ICE_TX_DESC_CMD_IL2TAG1;
		td_tag = (first->tx_flags & ICE_TX_FLAGS_VLAN_M) >>
			  ICE_TX_FLAGS_VLAN_S;
	}

	dma = dma_map_single(tx_ring->dev, skb->data, size, DMA_TO_DEVICE);

	tx_buf = first;

	for (frag = &skb_shinfo(skb)->frags[0];; frag++) {
		unsigned int max_data = ICE_MAX_DATA_PER_TXD_ALIGNED;

		if (dma_mapping_error(tx_ring->dev, dma))
			goto dma_error;

		/* record length, and DMA address */
		dma_unmap_len_set(tx_buf, len, size);
		dma_unmap_addr_set(tx_buf, dma, dma);

		/* align size to end of page */
		max_data += -dma & (ICE_MAX_READ_REQ_SIZE - 1);
		tx_desc->buf_addr = cpu_to_le64(dma);

		/* account for data chunks larger than the hardware
		 * can handle
		 */
		while (unlikely(size > ICE_MAX_DATA_PER_TXD)) {
			tx_desc->cmd_type_offset_bsz =
				build_ctob(td_cmd, td_offset, max_data, td_tag);

			tx_desc++;
			i++;

			if (i == tx_ring->count) {
				tx_desc = ICE_TX_DESC(tx_ring, 0);
				i = 0;
			}

			dma += max_data;
			size -= max_data;

			max_data = ICE_MAX_DATA_PER_TXD_ALIGNED;
			tx_desc->buf_addr = cpu_to_le64(dma);
		}

		if (likely(!data_len))
			break;

		tx_desc->cmd_type_offset_bsz = build_ctob(td_cmd, td_offset,
							  size, td_tag);

		tx_desc++;
		i++;

		if (i == tx_ring->count) {
			tx_desc = ICE_TX_DESC(tx_ring, 0);
			i = 0;
		}

		size = skb_frag_size(frag);
		data_len -= size;

		dma = skb_frag_dma_map(tx_ring->dev, frag, 0, size,
				       DMA_TO_DEVICE);

		tx_buf = &tx_ring->tx_buf[i];
	}

	/* record bytecount for BQL */
	netdev_tx_sent_queue(txring_txq(tx_ring), first->bytecount);

	/* record SW timestamp if HW timestamp is not available */
	skb_tx_timestamp(first->skb);

	i++;
	if (i == tx_ring->count)
		i = 0;

	/* write last descriptor with RS and EOP bits */
	td_cmd |= (u64)(ICE_TX_DESC_CMD_EOP | ICE_TX_DESC_CMD_RS);
	tx_desc->cmd_type_offset_bsz =
			build_ctob(td_cmd, td_offset, size, td_tag);

	/* Force memory writes to complete before letting h/w know there
	 * are new descriptors to fetch.
	 *
	 * We also use this memory barrier to make certain all of the
	 * status bits have been updated before next_to_watch is written.
	 */
	wmb();

	/* set next_to_watch value indicating a packet is present */
	first->next_to_watch = tx_desc;

	tx_ring->next_to_use = i;

	ice_maybe_stop_tx(tx_ring, DESC_NEEDED);

	/* notify HW of packet */
	if (netif_xmit_stopped(txring_txq(tx_ring)) || !netdev_xmit_more()) {
		writel(i, tx_ring->tail);
	}

	return;

dma_error:
	/* clear DMA mappings for failed tx_buf map */
	for (;;) {
		tx_buf = &tx_ring->tx_buf[i];
		ice_unmap_and_free_tx_buf(tx_ring, tx_buf);
		if (tx_buf == first)
			break;
		if (i == 0)
			i = tx_ring->count;
		i--;
	}

	tx_ring->next_to_use = i;
}

/**
 * ice_tx_csum - Enable Tx checksum offloads
 * @first: pointer to the first descriptor
 * @off: pointer to struct that holds offload parameters
 *
 * Returns 0 or error (negative) if checksum offload can't happen, 1 otherwise.
 */
static
int ice_tx_csum(struct ice_tx_buf *first, struct ice_tx_offload_params *off)
{
	u32 l4_len = 0, l3_len = 0, l2_len = 0;
	struct sk_buff *skb = first->skb;
	union {
		struct iphdr *v4;
		struct ipv6hdr *v6;
		unsigned char *hdr;
	} ip;
	union {
		struct tcphdr *tcp;
		unsigned char *hdr;
	} l4;
	__be16 frag_off, protocol;
	unsigned char *exthdr;
	u32 offset, cmd = 0;
	u8 l4_proto = 0;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	ip.hdr = skb_network_header(skb);
	l4.hdr = skb_transport_header(skb);

	/* compute outer L2 header size */
	l2_len = ip.hdr - skb->data;
	offset = (l2_len / 2) << ICE_TX_DESC_LEN_MACLEN_S;

	if (skb->encapsulation)
		return -1;

	/* Enable IP checksum offloads */
	protocol = vlan_get_protocol(skb);
	if (protocol == htons(ETH_P_IP)) {
		l4_proto = ip.v4->protocol;
		/* the stack computes the IP header already, the only time we
		 * need the hardware to recompute it is in the case of TSO.
		 */
		if (first->tx_flags & ICE_TX_FLAGS_TSO)
			cmd |= ICE_TX_DESC_CMD_IIPT_IPV4_CSUM;
		else
			cmd |= ICE_TX_DESC_CMD_IIPT_IPV4;

	} else if (protocol == htons(ETH_P_IPV6)) {
		cmd |= ICE_TX_DESC_CMD_IIPT_IPV6;
		exthdr = ip.hdr + sizeof(*ip.v6);
		l4_proto = ip.v6->nexthdr;
		if (l4.hdr != exthdr)
			ipv6_skip_exthdr(skb, exthdr - skb->data, &l4_proto,
					 &frag_off);
	} else {
		return -1;
	}

	/* compute inner L3 header size */
	l3_len = l4.hdr - ip.hdr;
	offset |= (l3_len / 4) << ICE_TX_DESC_LEN_IPLEN_S;

	/* Enable L4 checksum offloads */
	switch (l4_proto) {
	case IPPROTO_TCP:
		/* enable checksum offloads */
		cmd |= ICE_TX_DESC_CMD_L4T_EOFT_TCP;
		l4_len = l4.tcp->doff;
		offset |= l4_len << ICE_TX_DESC_LEN_L4_LEN_S;
		break;
	case IPPROTO_UDP:
		/* enable UDP checksum offload */
		cmd |= ICE_TX_DESC_CMD_L4T_EOFT_UDP;
		l4_len = (sizeof(struct udphdr) >> 2);
		offset |= l4_len << ICE_TX_DESC_LEN_L4_LEN_S;
		break;
	case IPPROTO_SCTP:
		/* enable SCTP checksum offload */
		cmd |= ICE_TX_DESC_CMD_L4T_EOFT_SCTP;
		l4_len = sizeof(struct sctphdr) >> 2;
		offset |= l4_len << ICE_TX_DESC_LEN_L4_LEN_S;
		break;

	default:
		if (first->tx_flags & ICE_TX_FLAGS_TSO)
			return -1;
		skb_checksum_help(skb);
		return 0;
	}

	off->td_cmd |= cmd;
	off->td_offset |= offset;
	return 1;
}

/**
 * ice_tx_prepare_vlan_flags - prepare generic Tx VLAN tagging flags for HW
 * @tx_ring: ring to send buffer on
 * @first: pointer to struct ice_tx_buf
 *
 * Checks the skb and set up correspondingly several generic transmit flags
 * related to VLAN tagging for the HW, such as VLAN, DCB, etc.
 *
 * Returns error code indicate the frame should be dropped upon error and the
 * otherwise returns 0 to indicate the flags has been set properly.
 */
static int
ice_tx_prepare_vlan_flags(struct ice_ring *tx_ring, struct ice_tx_buf *first)
{
	struct sk_buff *skb = first->skb;
	__be16 protocol = skb->protocol;

	if (protocol == htons(ETH_P_8021Q) &&
	    !(tx_ring->netdev->features & NETIF_F_HW_VLAN_CTAG_TX)) {
		/* when HW VLAN acceleration is turned off by the user the
		 * stack sets the protocol to 8021q so that the driver
		 * can take any steps required to support the SW only
		 * VLAN handling. In our case the driver doesn't need
		 * to take any further steps so just set the protocol
		 * to the encapsulated ethertype.
		 */
		skb->protocol = vlan_get_protocol(skb);
		return 0;
	}

	/* if we have a HW VLAN tag being added, default to the HW one */
	if (skb_vlan_tag_present(skb)) {
		first->tx_flags |= skb_vlan_tag_get(skb) << ICE_TX_FLAGS_VLAN_S;
		first->tx_flags |= ICE_TX_FLAGS_HW_VLAN;
	} else if (protocol == htons(ETH_P_8021Q)) {
		struct vlan_hdr *vhdr, _vhdr;

		/* for SW VLAN, check the next protocol and store the tag */
		vhdr = (struct vlan_hdr *)skb_header_pointer(skb, ETH_HLEN,
							     sizeof(_vhdr),
							     &_vhdr);
		if (!vhdr)
			return -EINVAL;

		first->tx_flags |= ntohs(vhdr->h_vlan_TCI) <<
				   ICE_TX_FLAGS_VLAN_S;
		first->tx_flags |= ICE_TX_FLAGS_SW_VLAN;
	}

	return ice_tx_prepare_vlan_flags_dcb(tx_ring, first);
}

/**
 * ice_tso - computes mss and TSO length to prepare for TSO
 * @first: pointer to struct ice_tx_buf
 * @off: pointer to struct that holds offload parameters
 *
 * Returns 0 or error (negative) if TSO can't happen, 1 otherwise.
 */
static
int ice_tso(struct ice_tx_buf *first, struct ice_tx_offload_params *off)
{
	struct sk_buff *skb = first->skb;
	union {
		struct iphdr *v4;
		struct ipv6hdr *v6;
		unsigned char *hdr;
	} ip;
	union {
		struct tcphdr *tcp;
		unsigned char *hdr;
	} l4;
	u64 cd_mss, cd_tso_len;
	u32 paylen, l4_start;
	int err;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	if (!skb_is_gso(skb))
		return 0;

	err = skb_cow_head(skb, 0);
	if (err < 0)
		return err;

	/* cppcheck-suppress unreadVariable */
	ip.hdr = skb_network_header(skb);
	l4.hdr = skb_transport_header(skb);

	/* initialize outer IP header fields */
	if (ip.v4->version == 4) {
		ip.v4->tot_len = 0;
		ip.v4->check = 0;
	} else {
		ip.v6->payload_len = 0;
	}

	/* determine offset of transport header */
	l4_start = l4.hdr - skb->data;

	/* remove payload length from checksum */
	paylen = skb->len - l4_start;
	csum_replace_by_diff(&l4.tcp->check, (__force __wsum)htonl(paylen));

	/* compute length of segmentation header */
	off->header_len = (l4.tcp->doff * 4) + l4_start;

	/* update gso_segs and bytecount */
	first->gso_segs = skb_shinfo(skb)->gso_segs;
	first->bytecount += (first->gso_segs - 1) * off->header_len;

	cd_tso_len = skb->len - off->header_len;
	cd_mss = skb_shinfo(skb)->gso_size;

	/* record cdesc_qw1 with TSO parameters */
	off->cd_qw1 |= (u64)(ICE_TX_DESC_DTYPE_CTX |
			     (ICE_TX_CTX_DESC_TSO << ICE_TXD_CTX_QW1_CMD_S) |
			     (cd_tso_len << ICE_TXD_CTX_QW1_TSO_LEN_S) |
			     (cd_mss << ICE_TXD_CTX_QW1_MSS_S));
	first->tx_flags |= ICE_TX_FLAGS_TSO;
	return 1;
}

/**
 * ice_txd_use_count  - estimate the number of descriptors needed for Tx
 * @size: transmit request size in bytes
 *
 * Due to hardware alignment restrictions (4K alignment), we need to
 * assume that we can have no more than 12K of data per descriptor, even
 * though each descriptor can take up to 16K - 1 bytes of aligned memory.
 * Thus, we need to divide by 12K. But division is slow! Instead,
 * we decompose the operation into shifts and one relatively cheap
 * multiply operation.
 *
 * To divide by 12K, we first divide by 4K, then divide by 3:
 *     To divide by 4K, shift right by 12 bits
 *     To divide by 3, multiply by 85, then divide by 256
 *     (Divide by 256 is done by shifting right by 8 bits)
 * Finally, we add one to round up. Because 256 isn't an exact multiple of
 * 3, we'll underestimate near each multiple of 12K. This is actually more
 * accurate as we have 4K - 1 of wiggle room that we can fit into the last
 * segment. For our purposes this is accurate out to 1M which is orders of
 * magnitude greater than our largest possible GSO size.
 *
 * This would then be implemented as:
 *     return (((size >> 12) * 85) >> 8) + ICE_DESCS_FOR_SKB_DATA_PTR;
 *
 * Since multiplication and division are commutative, we can reorder
 * operations into:
 *     return ((size * 85) >> 20) + ICE_DESCS_FOR_SKB_DATA_PTR;
 */
static unsigned int ice_txd_use_count(unsigned int size)
{
	return ((size * 85) >> 20) + ICE_DESCS_FOR_SKB_DATA_PTR;
}

/**
 * ice_xmit_desc_count - calculate number of Tx descriptors needed
 * @skb: send buffer
 *
 * Returns number of data descriptors needed for this skb.
 */
static unsigned int ice_xmit_desc_count(struct sk_buff *skb)
{
	const skb_frag_t *frag = &skb_shinfo(skb)->frags[0];
	unsigned int nr_frags = skb_shinfo(skb)->nr_frags;
	unsigned int count = 0, size = skb_headlen(skb);

	for (;;) {
		count += ice_txd_use_count(size);

		if (!nr_frags--)
			break;

		size = skb_frag_size(frag++);
	}

	return count;
}

/**
 * __ice_chk_linearize - Check if there are more than 8 buffers per packet
 * @skb: send buffer
 *
 * Note: This HW can't DMA more than 8 buffers to build a packet on the wire
 * and so we need to figure out the cases where we need to linearize the skb.
 *
 * For TSO we need to count the TSO header and segment payload separately.
 * As such we need to check cases where we have 7 fragments or more as we
 * can potentially require 9 DMA transactions, 1 for the TSO header, 1 for
 * the segment payload in the first descriptor, and another 7 for the
 * fragments.
 */
static bool __ice_chk_linearize(struct sk_buff *skb)
{
	const skb_frag_t *frag, *stale;
	int nr_frags, sum;

	/* no need to check if number of frags is less than 7 */
	nr_frags = skb_shinfo(skb)->nr_frags;
	if (nr_frags < (ICE_MAX_BUF_TXD - 1))
		return false;

	/* We need to walk through the list and validate that each group
	 * of 6 fragments totals at least gso_size.
	 */
	nr_frags -= ICE_MAX_BUF_TXD - 2;
	frag = &skb_shinfo(skb)->frags[0];

	/* Initialize size to the negative value of gso_size minus 1. We
	 * use this as the worst case scenerio in which the frag ahead
	 * of us only provides one byte which is why we are limited to 6
	 * descriptors for a single transmit as the header and previous
	 * fragment are already consuming 2 descriptors.
	 */
	sum = 1 - skb_shinfo(skb)->gso_size;

	/* Add size of frags 0 through 4 to create our initial sum */
	sum += skb_frag_size(frag++);
	sum += skb_frag_size(frag++);
	sum += skb_frag_size(frag++);
	sum += skb_frag_size(frag++);
	sum += skb_frag_size(frag++);

	/* Walk through fragments adding latest fragment, testing it, and
	 * then removing stale fragments from the sum.
	 */
	stale = &skb_shinfo(skb)->frags[0];
	for (;;) {
		sum += skb_frag_size(frag++);

		/* if sum is negative we failed to make sufficient progress */
		if (sum < 0)
			return true;

		if (!nr_frags--)
			break;

		sum -= skb_frag_size(stale++);
	}

	return false;
}

/**
 * ice_chk_linearize - Check if there are more than 8 fragments per packet
 * @skb:      send buffer
 * @count:    number of buffers used
 *
 * Note: Our HW can't scatter-gather more than 8 fragments to build
 * a packet on the wire and so we need to figure out the cases where we
 * need to linearize the skb.
 */
static bool ice_chk_linearize(struct sk_buff *skb, unsigned int count)
{
	/* Both TSO and single send will work if count is less than 8 */
	if (likely(count < ICE_MAX_BUF_TXD))
		return false;

	if (skb_is_gso(skb))
		return __ice_chk_linearize(skb);

	/* we can support up to 8 data buffers for a single send */
	return count != ICE_MAX_BUF_TXD;
}

/**
 * ice_xmit_frame_ring - Sends buffer on Tx ring
 * @skb: send buffer
 * @tx_ring: ring to send buffer on
 *
 * Returns NETDEV_TX_OK if sent, else an error code
 */
static netdev_tx_t
ice_xmit_frame_ring(struct sk_buff *skb, struct ice_ring *tx_ring)
{
	struct ice_tx_offload_params offload = { 0 };
	struct ice_vsi *vsi = tx_ring->vsi;
	struct ice_tx_buf *first;
	unsigned int count;
	int tso, csum;

	count = ice_xmit_desc_count(skb);
	if (ice_chk_linearize(skb, count)) {
		if (__skb_linearize(skb))
			goto out_drop;
		count = ice_txd_use_count(skb->len);
		tx_ring->tx_stats.tx_linearize++;
	}

	/* need: 1 descriptor per page * PAGE_SIZE/ICE_MAX_DATA_PER_TXD,
	 *       + 1 desc for skb_head_len/ICE_MAX_DATA_PER_TXD,
	 *       + 4 desc gap to avoid the cache line where head is,
	 *       + 1 desc for context descriptor,
	 * otherwise try next time
	 */
	if (ice_maybe_stop_tx(tx_ring, count + ICE_DESCS_PER_CACHE_LINE +
			      ICE_DESCS_FOR_CTX_DESC)) {
		tx_ring->tx_stats.tx_busy++;
		return NETDEV_TX_BUSY;
	}

	offload.tx_ring = tx_ring;

	/* record the location of the first descriptor for this packet */
	first = &tx_ring->tx_buf[tx_ring->next_to_use];
	first->skb = skb;
	first->bytecount = max_t(unsigned int, skb->len, ETH_ZLEN);
	first->gso_segs = 1;
	first->tx_flags = 0;

	/* prepare the VLAN tagging flags for Tx */
	if (ice_tx_prepare_vlan_flags(tx_ring, first))
		goto out_drop;

	/* set up TSO offload */
	tso = ice_tso(first, &offload);
	if (tso < 0)
		goto out_drop;

	/* always set up Tx checksum offload */
	csum = ice_tx_csum(first, &offload);
	if (csum < 0)
		goto out_drop;

	/* allow CONTROL frames egress from main VSI if FW LLDP disabled */
	if (unlikely(skb->priority == TC_PRIO_CONTROL &&
		     vsi->type == ICE_VSI_PF &&
		     vsi->port_info->is_sw_lldp))
		offload.cd_qw1 |= (u64)(ICE_TX_DESC_DTYPE_CTX |
					ICE_TX_CTX_DESC_SWTCH_UPLINK <<
					ICE_TXD_CTX_QW1_CMD_S);

	if (offload.cd_qw1 & ICE_TX_DESC_DTYPE_CTX) {
		struct ice_tx_ctx_desc *cdesc;
		int i = tx_ring->next_to_use;

		/* grab the next descriptor */
		cdesc = ICE_TX_CTX_DESC(tx_ring, i);
		i++;
		tx_ring->next_to_use = (i < tx_ring->count) ? i : 0;

		/* setup context descriptor */
		cdesc->tunneling_params = cpu_to_le32(offload.cd_tunnel_params);
		cdesc->l2tag2 = cpu_to_le16(offload.cd_l2tag2);
		cdesc->rsvd = cpu_to_le16(0);
		cdesc->qw1 = cpu_to_le64(offload.cd_qw1);
	}

	ice_tx_map(tx_ring, first, &offload);
	return NETDEV_TX_OK;

out_drop:
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/**
 * ice_start_xmit - Selects the correct VSI and Tx queue to send buffer
 * @skb: send buffer
 * @netdev: network interface device structure
 *
 * Returns NETDEV_TX_OK if sent, else an error code
 */
netdev_tx_t ice_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_ring *tx_ring;

	tx_ring = vsi->tx_rings[skb->queue_mapping];

	/* hardware can't handle really short frames, hardware padding works
	 * beyond this point
	 */
	if (skb_put_padto(skb, ICE_MIN_TX_LEN))
		return NETDEV_TX_OK;

	return ice_xmit_frame_ring(skb, tx_ring);
}
