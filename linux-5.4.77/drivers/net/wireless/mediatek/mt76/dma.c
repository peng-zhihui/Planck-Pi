// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 */

#include <linux/dma-mapping.h>
#include "mt76.h"
#include "dma.h"

static int
mt76_dma_alloc_queue(struct mt76_dev *dev, struct mt76_queue *q,
		     int idx, int n_desc, int bufsize,
		     u32 ring_base)
{
	int size;
	int i;

	spin_lock_init(&q->lock);

	q->regs = dev->mmio.regs + ring_base + idx * MT_RING_SIZE;
	q->ndesc = n_desc;
	q->buf_size = bufsize;
	q->hw_idx = idx;

	size = q->ndesc * sizeof(struct mt76_desc);
	q->desc = dmam_alloc_coherent(dev->dev, size, &q->desc_dma, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	size = q->ndesc * sizeof(*q->entry);
	q->entry = devm_kzalloc(dev->dev, size, GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	/* clear descriptors */
	for (i = 0; i < q->ndesc; i++)
		q->desc[i].ctrl = cpu_to_le32(MT_DMA_CTL_DMA_DONE);

	writel(q->desc_dma, &q->regs->desc_base);
	writel(0, &q->regs->cpu_idx);
	writel(0, &q->regs->dma_idx);
	writel(q->ndesc, &q->regs->ring_size);

	return 0;
}

static int
mt76_dma_add_buf(struct mt76_dev *dev, struct mt76_queue *q,
		 struct mt76_queue_buf *buf, int nbufs, u32 info,
		 struct sk_buff *skb, void *txwi)
{
	struct mt76_desc *desc;
	u32 ctrl;
	int i, idx = -1;

	if (txwi) {
		q->entry[q->head].txwi = DMA_DUMMY_DATA;
		q->entry[q->head].skip_buf0 = true;
	}

	for (i = 0; i < nbufs; i += 2, buf += 2) {
		u32 buf0 = buf[0].addr, buf1 = 0;

		ctrl = FIELD_PREP(MT_DMA_CTL_SD_LEN0, buf[0].len);
		if (i < nbufs - 1) {
			buf1 = buf[1].addr;
			ctrl |= FIELD_PREP(MT_DMA_CTL_SD_LEN1, buf[1].len);
		}

		if (i == nbufs - 1)
			ctrl |= MT_DMA_CTL_LAST_SEC0;
		else if (i == nbufs - 2)
			ctrl |= MT_DMA_CTL_LAST_SEC1;

		idx = q->head;
		q->head = (q->head + 1) % q->ndesc;

		desc = &q->desc[idx];

		WRITE_ONCE(desc->buf0, cpu_to_le32(buf0));
		WRITE_ONCE(desc->buf1, cpu_to_le32(buf1));
		WRITE_ONCE(desc->info, cpu_to_le32(info));
		WRITE_ONCE(desc->ctrl, cpu_to_le32(ctrl));

		q->queued++;
	}

	q->entry[idx].txwi = txwi;
	q->entry[idx].skb = skb;

	return idx;
}

static void
mt76_dma_tx_cleanup_idx(struct mt76_dev *dev, struct mt76_queue *q, int idx,
			struct mt76_queue_entry *prev_e)
{
	struct mt76_queue_entry *e = &q->entry[idx];
	__le32 __ctrl = READ_ONCE(q->desc[idx].ctrl);
	u32 ctrl = le32_to_cpu(__ctrl);

	if (!e->skip_buf0) {
		__le32 addr = READ_ONCE(q->desc[idx].buf0);
		u32 len = FIELD_GET(MT_DMA_CTL_SD_LEN0, ctrl);

		dma_unmap_single(dev->dev, le32_to_cpu(addr), len,
				 DMA_TO_DEVICE);
	}

	if (!(ctrl & MT_DMA_CTL_LAST_SEC0)) {
		__le32 addr = READ_ONCE(q->desc[idx].buf1);
		u32 len = FIELD_GET(MT_DMA_CTL_SD_LEN1, ctrl);

		dma_unmap_single(dev->dev, le32_to_cpu(addr), len,
				 DMA_TO_DEVICE);
	}

	if (e->txwi == DMA_DUMMY_DATA)
		e->txwi = NULL;

	if (e->skb == DMA_DUMMY_DATA)
		e->skb = NULL;

	*prev_e = *e;
	memset(e, 0, sizeof(*e));
}

static void
mt76_dma_sync_idx(struct mt76_dev *dev, struct mt76_queue *q)
{
	writel(q->desc_dma, &q->regs->desc_base);
	writel(q->ndesc, &q->regs->ring_size);
	q->head = readl(&q->regs->dma_idx);
	q->tail = q->head;
	writel(q->head, &q->regs->cpu_idx);
}

static void
mt76_dma_tx_cleanup(struct mt76_dev *dev, enum mt76_txq_id qid, bool flush)
{
	struct mt76_sw_queue *sq = &dev->q_tx[qid];
	struct mt76_queue *q = sq->q;
	struct mt76_queue_entry entry;
	unsigned int n_swq_queued[4] = {};
	unsigned int n_queued = 0;
	bool wake = false;
	int i, last;

	if (!q)
		return;

	if (flush)
		last = -1;
	else
		last = readl(&q->regs->dma_idx);

	while ((q->queued > n_queued) && q->tail != last) {
		mt76_dma_tx_cleanup_idx(dev, q, q->tail, &entry);
		if (entry.schedule)
			n_swq_queued[entry.qid]++;

		q->tail = (q->tail + 1) % q->ndesc;
		n_queued++;

		if (entry.skb)
			dev->drv->tx_complete_skb(dev, qid, &entry);

		if (entry.txwi) {
			if (!(dev->drv->txwi_flags & MT_TXWI_NO_FREE))
				mt76_put_txwi(dev, entry.txwi);
			wake = !flush;
		}

		if (!flush && q->tail == last)
			last = readl(&q->regs->dma_idx);
	}

	spin_lock_bh(&q->lock);

	q->queued -= n_queued;
	for (i = 0; i < ARRAY_SIZE(n_swq_queued); i++) {
		if (!n_swq_queued[i])
			continue;

		dev->q_tx[i].swq_queued -= n_swq_queued[i];
	}

	if (flush)
		mt76_dma_sync_idx(dev, q);

	wake = wake && q->stopped &&
	       qid < IEEE80211_NUM_ACS && q->queued < q->ndesc - 8;
	if (wake)
		q->stopped = false;

	if (!q->queued)
		wake_up(&dev->tx_wait);

	spin_unlock_bh(&q->lock);

	if (wake)
		ieee80211_wake_queue(dev->hw, qid);
}

static void *
mt76_dma_get_buf(struct mt76_dev *dev, struct mt76_queue *q, int idx,
		 int *len, u32 *info, bool *more)
{
	struct mt76_queue_entry *e = &q->entry[idx];
	struct mt76_desc *desc = &q->desc[idx];
	dma_addr_t buf_addr;
	void *buf = e->buf;
	int buf_len = SKB_WITH_OVERHEAD(q->buf_size);

	buf_addr = le32_to_cpu(READ_ONCE(desc->buf0));
	if (len) {
		u32 ctl = le32_to_cpu(READ_ONCE(desc->ctrl));
		*len = FIELD_GET(MT_DMA_CTL_SD_LEN0, ctl);
		*more = !(ctl & MT_DMA_CTL_LAST_SEC0);
	}

	if (info)
		*info = le32_to_cpu(desc->info);

	dma_unmap_single(dev->dev, buf_addr, buf_len, DMA_FROM_DEVICE);
	e->buf = NULL;

	return buf;
}

static void *
mt76_dma_dequeue(struct mt76_dev *dev, struct mt76_queue *q, bool flush,
		 int *len, u32 *info, bool *more)
{
	int idx = q->tail;

	*more = false;
	if (!q->queued)
		return NULL;

	if (!flush && !(q->desc[idx].ctrl & cpu_to_le32(MT_DMA_CTL_DMA_DONE)))
		return NULL;

	q->tail = (q->tail + 1) % q->ndesc;
	q->queued--;

	return mt76_dma_get_buf(dev, q, idx, len, info, more);
}

static void
mt76_dma_kick_queue(struct mt76_dev *dev, struct mt76_queue *q)
{
	writel(q->head, &q->regs->cpu_idx);
}

static int
mt76_dma_tx_queue_skb_raw(struct mt76_dev *dev, enum mt76_txq_id qid,
			  struct sk_buff *skb, u32 tx_info)
{
	struct mt76_queue *q = dev->q_tx[qid].q;
	struct mt76_queue_buf buf;
	dma_addr_t addr;

	if (q->queued + 1 >= q->ndesc - 1)
		goto error;

	addr = dma_map_single(dev->dev, skb->data, skb->len,
			      DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(dev->dev, addr)))
		goto error;

	buf.addr = addr;
	buf.len = skb->len;

	spin_lock_bh(&q->lock);
	mt76_dma_add_buf(dev, q, &buf, 1, tx_info, skb, NULL);
	mt76_dma_kick_queue(dev, q);
	spin_unlock_bh(&q->lock);

	return 0;

error:
	dev_kfree_skb(skb);
	return -ENOMEM;
}

static int
mt76_dma_tx_queue_skb(struct mt76_dev *dev, enum mt76_txq_id qid,
		      struct sk_buff *skb, struct mt76_wcid *wcid,
		      struct ieee80211_sta *sta)
{
	struct mt76_queue *q = dev->q_tx[qid].q;
	struct mt76_tx_info tx_info = {
		.skb = skb,
	};
	int len, n = 0, ret = -ENOMEM;
	struct mt76_queue_entry e;
	struct mt76_txwi_cache *t;
	struct sk_buff *iter;
	dma_addr_t addr;
	u8 *txwi;

	t = mt76_get_txwi(dev);
	if (!t) {
		ieee80211_free_txskb(dev->hw, skb);
		return -ENOMEM;
	}
	txwi = mt76_get_txwi_ptr(dev, t);

	skb->prev = skb->next = NULL;
	if (dev->drv->tx_aligned4_skbs)
		mt76_insert_hdr_pad(skb);

	len = skb_headlen(skb);
	addr = dma_map_single(dev->dev, skb->data, len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(dev->dev, addr)))
		goto free;

	tx_info.buf[n].addr = t->dma_addr;
	tx_info.buf[n++].len = dev->drv->txwi_size;
	tx_info.buf[n].addr = addr;
	tx_info.buf[n++].len = len;

	skb_walk_frags(skb, iter) {
		if (n == ARRAY_SIZE(tx_info.buf))
			goto unmap;

		addr = dma_map_single(dev->dev, iter->data, iter->len,
				      DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(dev->dev, addr)))
			goto unmap;

		tx_info.buf[n].addr = addr;
		tx_info.buf[n++].len = iter->len;
	}
	tx_info.nbuf = n;

	dma_sync_single_for_cpu(dev->dev, t->dma_addr, dev->drv->txwi_size,
				DMA_TO_DEVICE);
	ret = dev->drv->tx_prepare_skb(dev, txwi, qid, wcid, sta, &tx_info);
	dma_sync_single_for_device(dev->dev, t->dma_addr, dev->drv->txwi_size,
				   DMA_TO_DEVICE);
	if (ret < 0)
		goto unmap;

	if (q->queued + (tx_info.nbuf + 1) / 2 >= q->ndesc - 1) {
		ret = -ENOMEM;
		goto unmap;
	}

	return mt76_dma_add_buf(dev, q, tx_info.buf, tx_info.nbuf,
				tx_info.info, tx_info.skb, t);

unmap:
	for (n--; n > 0; n--)
		dma_unmap_single(dev->dev, tx_info.buf[n].addr,
				 tx_info.buf[n].len, DMA_TO_DEVICE);

free:
	e.skb = tx_info.skb;
	e.txwi = t;
	dev->drv->tx_complete_skb(dev, qid, &e);
	mt76_put_txwi(dev, t);
	return ret;
}

static int
mt76_dma_rx_fill(struct mt76_dev *dev, struct mt76_queue *q)
{
	dma_addr_t addr;
	void *buf;
	int frames = 0;
	int len = SKB_WITH_OVERHEAD(q->buf_size);
	int offset = q->buf_offset;
	int idx;

	spin_lock_bh(&q->lock);

	while (q->queued < q->ndesc - 1) {
		struct mt76_queue_buf qbuf;

		buf = page_frag_alloc(&q->rx_page, q->buf_size, GFP_ATOMIC);
		if (!buf)
			break;

		addr = dma_map_single(dev->dev, buf, len, DMA_FROM_DEVICE);
		if (unlikely(dma_mapping_error(dev->dev, addr))) {
			skb_free_frag(buf);
			break;
		}

		qbuf.addr = addr + offset;
		qbuf.len = len - offset;
		idx = mt76_dma_add_buf(dev, q, &qbuf, 1, 0, buf, NULL);
		frames++;
	}

	if (frames)
		mt76_dma_kick_queue(dev, q);

	spin_unlock_bh(&q->lock);

	return frames;
}

static void
mt76_dma_rx_cleanup(struct mt76_dev *dev, struct mt76_queue *q)
{
	struct page *page;
	void *buf;
	bool more;

	spin_lock_bh(&q->lock);
	do {
		buf = mt76_dma_dequeue(dev, q, true, NULL, NULL, &more);
		if (!buf)
			break;

		skb_free_frag(buf);
	} while (1);
	spin_unlock_bh(&q->lock);

	if (!q->rx_page.va)
		return;

	page = virt_to_page(q->rx_page.va);
	__page_frag_cache_drain(page, q->rx_page.pagecnt_bias);
	memset(&q->rx_page, 0, sizeof(q->rx_page));
}

static void
mt76_dma_rx_reset(struct mt76_dev *dev, enum mt76_rxq_id qid)
{
	struct mt76_queue *q = &dev->q_rx[qid];
	int i;

	for (i = 0; i < q->ndesc; i++)
		q->desc[i].ctrl &= ~cpu_to_le32(MT_DMA_CTL_DMA_DONE);

	mt76_dma_rx_cleanup(dev, q);
	mt76_dma_sync_idx(dev, q);
	mt76_dma_rx_fill(dev, q);

	if (!q->rx_head)
		return;

	dev_kfree_skb(q->rx_head);
	q->rx_head = NULL;
}

static void
mt76_add_fragment(struct mt76_dev *dev, struct mt76_queue *q, void *data,
		  int len, bool more)
{
	struct page *page = virt_to_head_page(data);
	int offset = data - page_address(page);
	struct sk_buff *skb = q->rx_head;
	struct skb_shared_info *shinfo = skb_shinfo(skb);

	if (shinfo->nr_frags < ARRAY_SIZE(shinfo->frags)) {
		offset += q->buf_offset;
		skb_add_rx_frag(skb, shinfo->nr_frags, page, offset, len,
				q->buf_size);
	}

	if (more)
		return;

	q->rx_head = NULL;
	dev->drv->rx_skb(dev, q - dev->q_rx, skb);
}

static int
mt76_dma_rx_process(struct mt76_dev *dev, struct mt76_queue *q, int budget)
{
	int len, data_len, done = 0;
	struct sk_buff *skb;
	unsigned char *data;
	bool more;

	while (done < budget) {
		u32 info;

		data = mt76_dma_dequeue(dev, q, false, &len, &info, &more);
		if (!data)
			break;

		if (q->rx_head)
			data_len = q->buf_size;
		else
			data_len = SKB_WITH_OVERHEAD(q->buf_size);

		if (data_len < len + q->buf_offset) {
			dev_kfree_skb(q->rx_head);
			q->rx_head = NULL;

			skb_free_frag(data);
			continue;
		}

		if (q->rx_head) {
			mt76_add_fragment(dev, q, data, len, more);
			continue;
		}

		skb = build_skb(data, q->buf_size);
		if (!skb) {
			skb_free_frag(data);
			continue;
		}
		skb_reserve(skb, q->buf_offset);

		if (q == &dev->q_rx[MT_RXQ_MCU]) {
			u32 *rxfce = (u32 *)skb->cb;
			*rxfce = info;
		}

		__skb_put(skb, len);
		done++;

		if (more) {
			q->rx_head = skb;
			continue;
		}

		dev->drv->rx_skb(dev, q - dev->q_rx, skb);
	}

	mt76_dma_rx_fill(dev, q);
	return done;
}

static int
mt76_dma_rx_poll(struct napi_struct *napi, int budget)
{
	struct mt76_dev *dev;
	int qid, done = 0, cur;

	dev = container_of(napi->dev, struct mt76_dev, napi_dev);
	qid = napi - dev->napi;

	rcu_read_lock();

	do {
		cur = mt76_dma_rx_process(dev, &dev->q_rx[qid], budget - done);
		mt76_rx_poll_complete(dev, qid, napi);
		done += cur;
	} while (cur && done < budget);

	rcu_read_unlock();

	if (done < budget) {
		napi_complete(napi);
		dev->drv->rx_poll_complete(dev, qid);
	}

	return done;
}

static int
mt76_dma_init(struct mt76_dev *dev)
{
	int i;

	init_dummy_netdev(&dev->napi_dev);

	for (i = 0; i < ARRAY_SIZE(dev->q_rx); i++) {
		netif_napi_add(&dev->napi_dev, &dev->napi[i], mt76_dma_rx_poll,
			       64);
		mt76_dma_rx_fill(dev, &dev->q_rx[i]);
		skb_queue_head_init(&dev->rx_skb[i]);
		napi_enable(&dev->napi[i]);
	}

	return 0;
}

static const struct mt76_queue_ops mt76_dma_ops = {
	.init = mt76_dma_init,
	.alloc = mt76_dma_alloc_queue,
	.tx_queue_skb_raw = mt76_dma_tx_queue_skb_raw,
	.tx_queue_skb = mt76_dma_tx_queue_skb,
	.tx_cleanup = mt76_dma_tx_cleanup,
	.rx_reset = mt76_dma_rx_reset,
	.kick = mt76_dma_kick_queue,
};

void mt76_dma_attach(struct mt76_dev *dev)
{
	dev->queue_ops = &mt76_dma_ops;
}
EXPORT_SYMBOL_GPL(mt76_dma_attach);

void mt76_dma_cleanup(struct mt76_dev *dev)
{
	int i;

	netif_napi_del(&dev->tx_napi);
	for (i = 0; i < ARRAY_SIZE(dev->q_tx); i++)
		mt76_dma_tx_cleanup(dev, i, true);

	for (i = 0; i < ARRAY_SIZE(dev->q_rx); i++) {
		netif_napi_del(&dev->napi[i]);
		mt76_dma_rx_cleanup(dev, &dev->q_rx[i]);
	}
}
EXPORT_SYMBOL_GPL(mt76_dma_cleanup);
