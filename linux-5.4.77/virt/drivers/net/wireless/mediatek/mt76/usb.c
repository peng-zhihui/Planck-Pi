// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2018 Lorenzo Bianconi <lorenzo.bianconi83@gmail.com>
 */

#include <linux/module.h>
#include "mt76.h"
#include "usb_trace.h"
#include "dma.h"

#define MT_VEND_REQ_MAX_RETRY	10
#define MT_VEND_REQ_TOUT_MS	300

static bool disable_usb_sg;
module_param_named(disable_usb_sg, disable_usb_sg, bool, 0644);
MODULE_PARM_DESC(disable_usb_sg, "Disable usb scatter-gather support");

/* should be called with usb_ctrl_mtx locked */
static int __mt76u_vendor_request(struct mt76_dev *dev, u8 req,
				  u8 req_type, u16 val, u16 offset,
				  void *buf, size_t len)
{
	struct usb_interface *uintf = to_usb_interface(dev->dev);
	struct usb_device *udev = interface_to_usbdev(uintf);
	unsigned int pipe;
	int i, ret;

	pipe = (req_type & USB_DIR_IN) ? usb_rcvctrlpipe(udev, 0)
				       : usb_sndctrlpipe(udev, 0);
	for (i = 0; i < MT_VEND_REQ_MAX_RETRY; i++) {
		if (test_bit(MT76_REMOVED, &dev->state))
			return -EIO;

		ret = usb_control_msg(udev, pipe, req, req_type, val,
				      offset, buf, len, MT_VEND_REQ_TOUT_MS);
		if (ret == -ENODEV)
			set_bit(MT76_REMOVED, &dev->state);
		if (ret >= 0 || ret == -ENODEV)
			return ret;
		usleep_range(5000, 10000);
	}

	dev_err(dev->dev, "vendor request req:%02x off:%04x failed:%d\n",
		req, offset, ret);
	return ret;
}

int mt76u_vendor_request(struct mt76_dev *dev, u8 req,
			 u8 req_type, u16 val, u16 offset,
			 void *buf, size_t len)
{
	int ret;

	mutex_lock(&dev->usb.usb_ctrl_mtx);
	ret = __mt76u_vendor_request(dev, req, req_type,
				     val, offset, buf, len);
	trace_usb_reg_wr(dev, offset, val);
	mutex_unlock(&dev->usb.usb_ctrl_mtx);

	return ret;
}
EXPORT_SYMBOL_GPL(mt76u_vendor_request);

/* should be called with usb_ctrl_mtx locked */
static u32 __mt76u_rr(struct mt76_dev *dev, u32 addr)
{
	struct mt76_usb *usb = &dev->usb;
	u32 data = ~0;
	u16 offset;
	int ret;
	u8 req;

	switch (addr & MT_VEND_TYPE_MASK) {
	case MT_VEND_TYPE_EEPROM:
		req = MT_VEND_READ_EEPROM;
		break;
	case MT_VEND_TYPE_CFG:
		req = MT_VEND_READ_CFG;
		break;
	default:
		req = MT_VEND_MULTI_READ;
		break;
	}
	offset = addr & ~MT_VEND_TYPE_MASK;

	ret = __mt76u_vendor_request(dev, req,
				     USB_DIR_IN | USB_TYPE_VENDOR,
				     0, offset, &usb->reg_val, sizeof(__le32));
	if (ret == sizeof(__le32))
		data = le32_to_cpu(usb->reg_val);
	trace_usb_reg_rr(dev, addr, data);

	return data;
}

static u32 mt76u_rr(struct mt76_dev *dev, u32 addr)
{
	u32 ret;

	mutex_lock(&dev->usb.usb_ctrl_mtx);
	ret = __mt76u_rr(dev, addr);
	mutex_unlock(&dev->usb.usb_ctrl_mtx);

	return ret;
}

/* should be called with usb_ctrl_mtx locked */
static void __mt76u_wr(struct mt76_dev *dev, u32 addr, u32 val)
{
	struct mt76_usb *usb = &dev->usb;
	u16 offset;
	u8 req;

	switch (addr & MT_VEND_TYPE_MASK) {
	case MT_VEND_TYPE_CFG:
		req = MT_VEND_WRITE_CFG;
		break;
	default:
		req = MT_VEND_MULTI_WRITE;
		break;
	}
	offset = addr & ~MT_VEND_TYPE_MASK;

	usb->reg_val = cpu_to_le32(val);
	__mt76u_vendor_request(dev, req,
			       USB_DIR_OUT | USB_TYPE_VENDOR, 0,
			       offset, &usb->reg_val, sizeof(__le32));
	trace_usb_reg_wr(dev, addr, val);
}

static void mt76u_wr(struct mt76_dev *dev, u32 addr, u32 val)
{
	mutex_lock(&dev->usb.usb_ctrl_mtx);
	__mt76u_wr(dev, addr, val);
	mutex_unlock(&dev->usb.usb_ctrl_mtx);
}

static u32 mt76u_rmw(struct mt76_dev *dev, u32 addr,
		     u32 mask, u32 val)
{
	mutex_lock(&dev->usb.usb_ctrl_mtx);
	val |= __mt76u_rr(dev, addr) & ~mask;
	__mt76u_wr(dev, addr, val);
	mutex_unlock(&dev->usb.usb_ctrl_mtx);

	return val;
}

static void mt76u_copy(struct mt76_dev *dev, u32 offset,
		       const void *data, int len)
{
	struct mt76_usb *usb = &dev->usb;
	const u32 *val = data;
	int i, ret;

	mutex_lock(&usb->usb_ctrl_mtx);
	for (i = 0; i < DIV_ROUND_UP(len, 4); i++) {
		put_unaligned(val[i], (u32 *)usb->data);
		ret = __mt76u_vendor_request(dev, MT_VEND_MULTI_WRITE,
					     USB_DIR_OUT | USB_TYPE_VENDOR,
					     0, offset + i * 4, usb->data,
					     sizeof(u32));
		if (ret < 0)
			break;
	}
	mutex_unlock(&usb->usb_ctrl_mtx);
}

void mt76u_single_wr(struct mt76_dev *dev, const u8 req,
		     const u16 offset, const u32 val)
{
	mutex_lock(&dev->usb.usb_ctrl_mtx);
	__mt76u_vendor_request(dev, req,
			       USB_DIR_OUT | USB_TYPE_VENDOR,
			       val & 0xffff, offset, NULL, 0);
	__mt76u_vendor_request(dev, req,
			       USB_DIR_OUT | USB_TYPE_VENDOR,
			       val >> 16, offset + 2, NULL, 0);
	mutex_unlock(&dev->usb.usb_ctrl_mtx);
}
EXPORT_SYMBOL_GPL(mt76u_single_wr);

static int
mt76u_req_wr_rp(struct mt76_dev *dev, u32 base,
		const struct mt76_reg_pair *data, int len)
{
	struct mt76_usb *usb = &dev->usb;

	mutex_lock(&usb->usb_ctrl_mtx);
	while (len > 0) {
		__mt76u_wr(dev, base + data->reg, data->value);
		len--;
		data++;
	}
	mutex_unlock(&usb->usb_ctrl_mtx);

	return 0;
}

static int
mt76u_wr_rp(struct mt76_dev *dev, u32 base,
	    const struct mt76_reg_pair *data, int n)
{
	if (test_bit(MT76_STATE_MCU_RUNNING, &dev->state))
		return dev->mcu_ops->mcu_wr_rp(dev, base, data, n);
	else
		return mt76u_req_wr_rp(dev, base, data, n);
}

static int
mt76u_req_rd_rp(struct mt76_dev *dev, u32 base, struct mt76_reg_pair *data,
		int len)
{
	struct mt76_usb *usb = &dev->usb;

	mutex_lock(&usb->usb_ctrl_mtx);
	while (len > 0) {
		data->value = __mt76u_rr(dev, base + data->reg);
		len--;
		data++;
	}
	mutex_unlock(&usb->usb_ctrl_mtx);

	return 0;
}

static int
mt76u_rd_rp(struct mt76_dev *dev, u32 base,
	    struct mt76_reg_pair *data, int n)
{
	if (test_bit(MT76_STATE_MCU_RUNNING, &dev->state))
		return dev->mcu_ops->mcu_rd_rp(dev, base, data, n);
	else
		return mt76u_req_rd_rp(dev, base, data, n);
}

static bool mt76u_check_sg(struct mt76_dev *dev)
{
	struct usb_interface *uintf = to_usb_interface(dev->dev);
	struct usb_device *udev = interface_to_usbdev(uintf);

	return (!disable_usb_sg && udev->bus->sg_tablesize > 0 &&
		(udev->bus->no_sg_constraint ||
		 udev->speed == USB_SPEED_WIRELESS));
}

static int
mt76u_set_endpoints(struct usb_interface *intf,
		    struct mt76_usb *usb)
{
	struct usb_host_interface *intf_desc = intf->cur_altsetting;
	struct usb_endpoint_descriptor *ep_desc;
	int i, in_ep = 0, out_ep = 0;

	for (i = 0; i < intf_desc->desc.bNumEndpoints; i++) {
		ep_desc = &intf_desc->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(ep_desc) &&
		    in_ep < __MT_EP_IN_MAX) {
			usb->in_ep[in_ep] = usb_endpoint_num(ep_desc);
			in_ep++;
		} else if (usb_endpoint_is_bulk_out(ep_desc) &&
			   out_ep < __MT_EP_OUT_MAX) {
			usb->out_ep[out_ep] = usb_endpoint_num(ep_desc);
			out_ep++;
		}
	}

	if (in_ep != __MT_EP_IN_MAX || out_ep != __MT_EP_OUT_MAX)
		return -EINVAL;
	return 0;
}

static int
mt76u_fill_rx_sg(struct mt76_dev *dev, struct mt76_queue *q, struct urb *urb,
		 int nsgs, gfp_t gfp)
{
	int i;

	for (i = 0; i < nsgs; i++) {
		struct page *page;
		void *data;
		int offset;

		data = page_frag_alloc(&q->rx_page, q->buf_size, gfp);
		if (!data)
			break;

		page = virt_to_head_page(data);
		offset = data - page_address(page);
		sg_set_page(&urb->sg[i], page, q->buf_size, offset);
	}

	if (i < nsgs) {
		int j;

		for (j = nsgs; j < urb->num_sgs; j++)
			skb_free_frag(sg_virt(&urb->sg[j]));
		urb->num_sgs = i;
	}

	urb->num_sgs = max_t(int, i, urb->num_sgs);
	urb->transfer_buffer_length = urb->num_sgs * q->buf_size;
	sg_init_marker(urb->sg, urb->num_sgs);

	return i ? : -ENOMEM;
}

static int
mt76u_refill_rx(struct mt76_dev *dev, struct urb *urb, int nsgs, gfp_t gfp)
{
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];

	if (dev->usb.sg_en)
		return mt76u_fill_rx_sg(dev, q, urb, nsgs, gfp);

	urb->transfer_buffer_length = q->buf_size;
	urb->transfer_buffer = page_frag_alloc(&q->rx_page, q->buf_size, gfp);

	return urb->transfer_buffer ? 0 : -ENOMEM;
}

static int
mt76u_urb_alloc(struct mt76_dev *dev, struct mt76_queue_entry *e,
		int sg_max_size)
{
	unsigned int size = sizeof(struct urb);

	if (dev->usb.sg_en)
		size += sg_max_size * sizeof(struct scatterlist);

	e->urb = kzalloc(size, GFP_KERNEL);
	if (!e->urb)
		return -ENOMEM;

	usb_init_urb(e->urb);

	if (dev->usb.sg_en)
		e->urb->sg = (struct scatterlist *)(e->urb + 1);

	return 0;
}

static int
mt76u_rx_urb_alloc(struct mt76_dev *dev, struct mt76_queue_entry *e)
{
	int err;

	err = mt76u_urb_alloc(dev, e, MT_RX_SG_MAX_SIZE);
	if (err)
		return err;

	return mt76u_refill_rx(dev, e->urb, MT_RX_SG_MAX_SIZE,
			       GFP_KERNEL);
}

static void mt76u_urb_free(struct urb *urb)
{
	int i;

	for (i = 0; i < urb->num_sgs; i++)
		skb_free_frag(sg_virt(&urb->sg[i]));

	if (urb->transfer_buffer)
		skb_free_frag(urb->transfer_buffer);

	usb_free_urb(urb);
}

static void
mt76u_fill_bulk_urb(struct mt76_dev *dev, int dir, int index,
		    struct urb *urb, usb_complete_t complete_fn,
		    void *context)
{
	struct usb_interface *uintf = to_usb_interface(dev->dev);
	struct usb_device *udev = interface_to_usbdev(uintf);
	unsigned int pipe;

	if (dir == USB_DIR_IN)
		pipe = usb_rcvbulkpipe(udev, dev->usb.in_ep[index]);
	else
		pipe = usb_sndbulkpipe(udev, dev->usb.out_ep[index]);

	urb->dev = udev;
	urb->pipe = pipe;
	urb->complete = complete_fn;
	urb->context = context;
}

static inline struct urb *
mt76u_get_next_rx_entry(struct mt76_dev *dev)
{
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];
	struct urb *urb = NULL;
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	if (q->queued > 0) {
		urb = q->entry[q->head].urb;
		q->head = (q->head + 1) % q->ndesc;
		q->queued--;
	}
	spin_unlock_irqrestore(&q->lock, flags);

	return urb;
}

static int mt76u_get_rx_entry_len(u8 *data, u32 data_len)
{
	u16 dma_len, min_len;

	dma_len = get_unaligned_le16(data);
	min_len = MT_DMA_HDR_LEN + MT_RX_RXWI_LEN +
		  MT_FCE_INFO_LEN;

	if (data_len < min_len || !dma_len ||
	    dma_len + MT_DMA_HDR_LEN > data_len ||
	    (dma_len & 0x3))
		return -EINVAL;
	return dma_len;
}

static struct sk_buff *
mt76u_build_rx_skb(void *data, int len, int buf_size)
{
	struct sk_buff *skb;

	if (SKB_WITH_OVERHEAD(buf_size) < MT_DMA_HDR_LEN + len) {
		struct page *page;

		/* slow path, not enough space for data and
		 * skb_shared_info
		 */
		skb = alloc_skb(MT_SKB_HEAD_LEN, GFP_ATOMIC);
		if (!skb)
			return NULL;

		skb_put_data(skb, data + MT_DMA_HDR_LEN, MT_SKB_HEAD_LEN);
		data += (MT_DMA_HDR_LEN + MT_SKB_HEAD_LEN);
		page = virt_to_head_page(data);
		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
				page, data - page_address(page),
				len - MT_SKB_HEAD_LEN, buf_size);

		return skb;
	}

	/* fast path */
	skb = build_skb(data, buf_size);
	if (!skb)
		return NULL;

	skb_reserve(skb, MT_DMA_HDR_LEN);
	__skb_put(skb, len);

	return skb;
}

static int
mt76u_process_rx_entry(struct mt76_dev *dev, struct urb *urb)
{
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];
	u8 *data = urb->num_sgs ? sg_virt(&urb->sg[0]) : urb->transfer_buffer;
	int data_len = urb->num_sgs ? urb->sg[0].length : urb->actual_length;
	int len, nsgs = 1;
	struct sk_buff *skb;

	if (!test_bit(MT76_STATE_INITIALIZED, &dev->state))
		return 0;

	len = mt76u_get_rx_entry_len(data, urb->actual_length);
	if (len < 0)
		return 0;

	data_len = min_t(int, len, data_len - MT_DMA_HDR_LEN);
	skb = mt76u_build_rx_skb(data, data_len, q->buf_size);
	if (!skb)
		return 0;

	len -= data_len;
	while (len > 0 && nsgs < urb->num_sgs) {
		data_len = min_t(int, len, urb->sg[nsgs].length);
		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
				sg_page(&urb->sg[nsgs]),
				urb->sg[nsgs].offset,
				data_len, q->buf_size);
		len -= data_len;
		nsgs++;
	}
	dev->drv->rx_skb(dev, MT_RXQ_MAIN, skb);

	return nsgs;
}

static void mt76u_complete_rx(struct urb *urb)
{
	struct mt76_dev *dev = urb->context;
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];
	unsigned long flags;

	trace_rx_urb(dev, urb);

	switch (urb->status) {
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENOENT:
		return;
	default:
		dev_err_ratelimited(dev->dev, "rx urb failed: %d\n",
				    urb->status);
		/* fall through */
	case 0:
		break;
	}

	spin_lock_irqsave(&q->lock, flags);
	if (WARN_ONCE(q->entry[q->tail].urb != urb, "rx urb mismatch"))
		goto out;

	q->tail = (q->tail + 1) % q->ndesc;
	q->queued++;
	tasklet_schedule(&dev->usb.rx_tasklet);
out:
	spin_unlock_irqrestore(&q->lock, flags);
}

static int
mt76u_submit_rx_buf(struct mt76_dev *dev, struct urb *urb)
{
	mt76u_fill_bulk_urb(dev, USB_DIR_IN, MT_EP_IN_PKT_RX, urb,
			    mt76u_complete_rx, dev);
	trace_submit_urb(dev, urb);

	return usb_submit_urb(urb, GFP_ATOMIC);
}

static void mt76u_rx_tasklet(unsigned long data)
{
	struct mt76_dev *dev = (struct mt76_dev *)data;
	struct urb *urb;
	int err, count;

	rcu_read_lock();

	while (true) {
		urb = mt76u_get_next_rx_entry(dev);
		if (!urb)
			break;

		count = mt76u_process_rx_entry(dev, urb);
		if (count > 0) {
			err = mt76u_refill_rx(dev, urb, count, GFP_ATOMIC);
			if (err < 0)
				break;
		}
		mt76u_submit_rx_buf(dev, urb);
	}
	mt76_rx_poll_complete(dev, MT_RXQ_MAIN, NULL);

	rcu_read_unlock();
}

static int mt76u_submit_rx_buffers(struct mt76_dev *dev)
{
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];
	unsigned long flags;
	int i, err = 0;

	spin_lock_irqsave(&q->lock, flags);
	for (i = 0; i < q->ndesc; i++) {
		err = mt76u_submit_rx_buf(dev, q->entry[i].urb);
		if (err < 0)
			break;
	}
	q->head = q->tail = 0;
	q->queued = 0;
	spin_unlock_irqrestore(&q->lock, flags);

	return err;
}

static int mt76u_alloc_rx(struct mt76_dev *dev)
{
	struct mt76_usb *usb = &dev->usb;
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];
	int i, err;

	usb->mcu.data = devm_kmalloc(dev->dev, MCU_RESP_URB_SIZE, GFP_KERNEL);
	if (!usb->mcu.data)
		return -ENOMEM;

	spin_lock_init(&q->lock);
	q->entry = devm_kcalloc(dev->dev,
				MT_NUM_RX_ENTRIES, sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->ndesc = MT_NUM_RX_ENTRIES;
	q->buf_size = PAGE_SIZE;

	for (i = 0; i < q->ndesc; i++) {
		err = mt76u_rx_urb_alloc(dev, &q->entry[i]);
		if (err < 0)
			return err;
	}

	return mt76u_submit_rx_buffers(dev);
}

static void mt76u_free_rx(struct mt76_dev *dev)
{
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];
	struct page *page;
	int i;

	for (i = 0; i < q->ndesc; i++)
		mt76u_urb_free(q->entry[i].urb);

	if (!q->rx_page.va)
		return;

	page = virt_to_page(q->rx_page.va);
	__page_frag_cache_drain(page, q->rx_page.pagecnt_bias);
	memset(&q->rx_page, 0, sizeof(q->rx_page));
}

void mt76u_stop_rx(struct mt76_dev *dev)
{
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];
	int i;

	for (i = 0; i < q->ndesc; i++)
		usb_poison_urb(q->entry[i].urb);

	tasklet_kill(&dev->usb.rx_tasklet);
}
EXPORT_SYMBOL_GPL(mt76u_stop_rx);

int mt76u_resume_rx(struct mt76_dev *dev)
{
	struct mt76_queue *q = &dev->q_rx[MT_RXQ_MAIN];
	int i;

	for (i = 0; i < q->ndesc; i++)
		usb_unpoison_urb(q->entry[i].urb);

	return mt76u_submit_rx_buffers(dev);
}
EXPORT_SYMBOL_GPL(mt76u_resume_rx);

static void mt76u_tx_tasklet(unsigned long data)
{
	struct mt76_dev *dev = (struct mt76_dev *)data;
	struct mt76_queue_entry entry;
	struct mt76_sw_queue *sq;
	struct mt76_queue *q;
	bool wake;
	int i;

	for (i = 0; i < IEEE80211_NUM_ACS; i++) {
		u32 n_dequeued = 0, n_sw_dequeued = 0;

		sq = &dev->q_tx[i];
		q = sq->q;

		while (q->queued > n_dequeued) {
			if (!q->entry[q->head].done)
				break;

			if (q->entry[q->head].schedule) {
				q->entry[q->head].schedule = false;
				n_sw_dequeued++;
			}

			entry = q->entry[q->head];
			q->entry[q->head].done = false;
			q->head = (q->head + 1) % q->ndesc;
			n_dequeued++;

			dev->drv->tx_complete_skb(dev, i, &entry);
		}

		spin_lock_bh(&q->lock);

		sq->swq_queued -= n_sw_dequeued;
		q->queued -= n_dequeued;

		wake = q->stopped && q->queued < q->ndesc - 8;
		if (wake)
			q->stopped = false;

		if (!q->queued)
			wake_up(&dev->tx_wait);

		spin_unlock_bh(&q->lock);

		mt76_txq_schedule(dev, i);

		if (!test_and_set_bit(MT76_READING_STATS, &dev->state))
			ieee80211_queue_delayed_work(dev->hw,
						     &dev->usb.stat_work,
						     msecs_to_jiffies(10));

		if (wake)
			ieee80211_wake_queue(dev->hw, i);
	}
}

static void mt76u_tx_status_data(struct work_struct *work)
{
	struct mt76_usb *usb;
	struct mt76_dev *dev;
	u8 update = 1;
	u16 count = 0;

	usb = container_of(work, struct mt76_usb, stat_work.work);
	dev = container_of(usb, struct mt76_dev, usb);

	while (true) {
		if (test_bit(MT76_REMOVED, &dev->state))
			break;

		if (!dev->drv->tx_status_data(dev, &update))
			break;
		count++;
	}

	if (count && test_bit(MT76_STATE_RUNNING, &dev->state))
		ieee80211_queue_delayed_work(dev->hw, &usb->stat_work,
					     msecs_to_jiffies(10));
	else
		clear_bit(MT76_READING_STATS, &dev->state);
}

static void mt76u_complete_tx(struct urb *urb)
{
	struct mt76_dev *dev = dev_get_drvdata(&urb->dev->dev);
	struct mt76_queue_entry *e = urb->context;

	if (mt76u_urb_error(urb))
		dev_err(dev->dev, "tx urb failed: %d\n", urb->status);
	e->done = true;

	tasklet_schedule(&dev->tx_tasklet);
}

static int
mt76u_tx_setup_buffers(struct mt76_dev *dev, struct sk_buff *skb,
		       struct urb *urb)
{
	urb->transfer_buffer_length = skb->len;

	if (!dev->usb.sg_en) {
		urb->transfer_buffer = skb->data;
		return 0;
	}

	sg_init_table(urb->sg, MT_TX_SG_MAX_SIZE);
	urb->num_sgs = skb_to_sgvec(skb, urb->sg, 0, skb->len);
	if (!urb->num_sgs)
		return -ENOMEM;

	return urb->num_sgs;
}

static int
mt76u_tx_queue_skb(struct mt76_dev *dev, enum mt76_txq_id qid,
		   struct sk_buff *skb, struct mt76_wcid *wcid,
		   struct ieee80211_sta *sta)
{
	struct mt76_queue *q = dev->q_tx[qid].q;
	struct mt76_tx_info tx_info = {
		.skb = skb,
	};
	u16 idx = q->tail;
	int err;

	if (q->queued == q->ndesc)
		return -ENOSPC;

	skb->prev = skb->next = NULL;
	err = dev->drv->tx_prepare_skb(dev, NULL, qid, wcid, sta, &tx_info);
	if (err < 0)
		return err;

	err = mt76u_tx_setup_buffers(dev, tx_info.skb, q->entry[idx].urb);
	if (err < 0)
		return err;

	mt76u_fill_bulk_urb(dev, USB_DIR_OUT, q2ep(q->hw_idx),
			    q->entry[idx].urb, mt76u_complete_tx,
			    &q->entry[idx]);

	q->tail = (q->tail + 1) % q->ndesc;
	q->entry[idx].skb = tx_info.skb;
	q->queued++;

	return idx;
}

static void mt76u_tx_kick(struct mt76_dev *dev, struct mt76_queue *q)
{
	struct urb *urb;
	int err;

	while (q->first != q->tail) {
		urb = q->entry[q->first].urb;

		trace_submit_urb(dev, urb);
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err < 0) {
			if (err == -ENODEV)
				set_bit(MT76_REMOVED, &dev->state);
			else
				dev_err(dev->dev, "tx urb submit failed:%d\n",
					err);
			break;
		}
		q->first = (q->first + 1) % q->ndesc;
	}
}

static int mt76u_alloc_tx(struct mt76_dev *dev)
{
	struct mt76_queue *q;
	int i, j, err;

	for (i = 0; i <= MT_TXQ_PSD; i++) {
		INIT_LIST_HEAD(&dev->q_tx[i].swq);

		if (i >= IEEE80211_NUM_ACS) {
			dev->q_tx[i].q = dev->q_tx[0].q;
			continue;
		}

		q = devm_kzalloc(dev->dev, sizeof(*q), GFP_KERNEL);
		if (!q)
			return -ENOMEM;

		spin_lock_init(&q->lock);
		q->hw_idx = mt76_ac_to_hwq(i);
		dev->q_tx[i].q = q;

		q->entry = devm_kcalloc(dev->dev,
					MT_NUM_TX_ENTRIES, sizeof(*q->entry),
					GFP_KERNEL);
		if (!q->entry)
			return -ENOMEM;

		q->ndesc = MT_NUM_TX_ENTRIES;
		for (j = 0; j < q->ndesc; j++) {
			err = mt76u_urb_alloc(dev, &q->entry[j],
					      MT_TX_SG_MAX_SIZE);
			if (err < 0)
				return err;
		}
	}
	return 0;
}

static void mt76u_free_tx(struct mt76_dev *dev)
{
	struct mt76_queue *q;
	int i, j;

	for (i = 0; i < IEEE80211_NUM_ACS; i++) {
		q = dev->q_tx[i].q;
		for (j = 0; j < q->ndesc; j++)
			usb_free_urb(q->entry[j].urb);
	}
}

void mt76u_stop_tx(struct mt76_dev *dev)
{
	struct mt76_queue_entry entry;
	struct mt76_queue *q;
	int i, j, ret;

	ret = wait_event_timeout(dev->tx_wait, !mt76_has_tx_pending(dev),
				 HZ / 5);
	if (!ret) {
		dev_err(dev->dev, "timed out waiting for pending tx\n");

		for (i = 0; i < IEEE80211_NUM_ACS; i++) {
			q = dev->q_tx[i].q;
			for (j = 0; j < q->ndesc; j++)
				usb_kill_urb(q->entry[j].urb);
		}

		tasklet_kill(&dev->tx_tasklet);

		/* On device removal we maight queue skb's, but mt76u_tx_kick()
		 * will fail to submit urb, cleanup those skb's manually.
		 */
		for (i = 0; i < IEEE80211_NUM_ACS; i++) {
			q = dev->q_tx[i].q;

			/* Assure we are in sync with killed tasklet. */
			spin_lock_bh(&q->lock);
			while (q->queued) {
				entry = q->entry[q->head];
				q->head = (q->head + 1) % q->ndesc;
				q->queued--;

				dev->drv->tx_complete_skb(dev, i, &entry);
			}
			spin_unlock_bh(&q->lock);
		}
	}

	cancel_delayed_work_sync(&dev->usb.stat_work);
	clear_bit(MT76_READING_STATS, &dev->state);

	mt76_tx_status_check(dev, NULL, true);
}
EXPORT_SYMBOL_GPL(mt76u_stop_tx);

void mt76u_queues_deinit(struct mt76_dev *dev)
{
	mt76u_stop_rx(dev);
	mt76u_stop_tx(dev);

	mt76u_free_rx(dev);
	mt76u_free_tx(dev);
}
EXPORT_SYMBOL_GPL(mt76u_queues_deinit);

int mt76u_alloc_queues(struct mt76_dev *dev)
{
	int err;

	err = mt76u_alloc_rx(dev);
	if (err < 0)
		return err;

	return mt76u_alloc_tx(dev);
}
EXPORT_SYMBOL_GPL(mt76u_alloc_queues);

static const struct mt76_queue_ops usb_queue_ops = {
	.tx_queue_skb = mt76u_tx_queue_skb,
	.kick = mt76u_tx_kick,
};

int mt76u_init(struct mt76_dev *dev,
	       struct usb_interface *intf)
{
	static const struct mt76_bus_ops mt76u_ops = {
		.rr = mt76u_rr,
		.wr = mt76u_wr,
		.rmw = mt76u_rmw,
		.write_copy = mt76u_copy,
		.wr_rp = mt76u_wr_rp,
		.rd_rp = mt76u_rd_rp,
		.type = MT76_BUS_USB,
	};
	struct usb_device *udev = interface_to_usbdev(intf);
	struct mt76_usb *usb = &dev->usb;

	tasklet_init(&usb->rx_tasklet, mt76u_rx_tasklet, (unsigned long)dev);
	tasklet_init(&dev->tx_tasklet, mt76u_tx_tasklet, (unsigned long)dev);
	INIT_DELAYED_WORK(&usb->stat_work, mt76u_tx_status_data);
	skb_queue_head_init(&dev->rx_skb[MT_RXQ_MAIN]);

	mutex_init(&usb->mcu.mutex);

	mutex_init(&usb->usb_ctrl_mtx);
	dev->bus = &mt76u_ops;
	dev->queue_ops = &usb_queue_ops;

	dev_set_drvdata(&udev->dev, dev);

	usb->sg_en = mt76u_check_sg(dev);

	return mt76u_set_endpoints(intf, usb);
}
EXPORT_SYMBOL_GPL(mt76u_init);

MODULE_AUTHOR("Lorenzo Bianconi <lorenzo.bianconi83@gmail.com>");
MODULE_LICENSE("Dual BSD/GPL");
