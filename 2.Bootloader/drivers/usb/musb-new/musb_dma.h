/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MUSB OTG driver DMA controller abstraction
 *
 * Copyright 2005 Mentor Graphics Corporation
 * Copyright (C) 2005-2006 by Texas Instruments
 * Copyright (C) 2006-2007 Nokia Corporation
 */

#ifndef __MUSB_DMA_H__
#define __MUSB_DMA_H__

struct musb_hw_ep;

/*
 * DMA Controller Abstraction
 *
 * DMA Controllers are abstracted to allow use of a variety of different
 * implementations of DMA, as allowed by the Inventra USB cores.  On the
 * host side, usbcore sets up the DMA mappings and flushes caches; on the
 * peripheral side, the gadget controller driver does.  Responsibilities
 * of a DMA controller driver include:
 *
 *  - Handling the details of moving multiple USB packets
 *    in cooperation with the Inventra USB core, including especially
 *    the correct RX side treatment of short packets and buffer-full
 *    states (both of which terminate transfers).
 *
 *  - Knowing the correlation between dma channels and the
 *    Inventra core's local endpoint resources and data direction.
 *
 *  - Maintaining a list of allocated/available channels.
 *
 *  - Updating channel status on interrupts,
 *    whether shared with the Inventra core or separate.
 */

#define	DMA_ADDR_INVALID	(~(dma_addr_t)0)

#ifndef CONFIG_USB_MUSB_PIO_ONLY
#define	is_dma_capable()	(1)
#else
#define	is_dma_capable()	(0)
#endif

#ifdef CONFIG_USB_TI_CPPI_DMA
#define	is_cppi_enabled()	1
#else
#define	is_cppi_enabled()	0
#endif

#ifdef CONFIG_USB_TUSB_OMAP_DMA
#define tusb_dma_omap()			1
#else
#define tusb_dma_omap()			0
#endif

/*
 * DMA channel status ... updated by the dma controller driver whenever that
 * status changes, and protected by the overall controller spinlock.
 */
enum dma_channel_status {
	/* unallocated */
	MUSB_DMA_STATUS_UNKNOWN,
	/* allocated ... but not busy, no errors */
	MUSB_DMA_STATUS_FREE,
	/* busy ... transactions are active */
	MUSB_DMA_STATUS_BUSY,
	/* transaction(s) aborted due to ... dma or memory bus error */
	MUSB_DMA_STATUS_BUS_ABORT,
	/* transaction(s) aborted due to ... core error or USB fault */
	MUSB_DMA_STATUS_CORE_ABORT
};

struct dma_controller;

/**
 * struct dma_channel - A DMA channel.
 * @private_data: channel-private data
 * @max_len: the maximum number of bytes the channel can move in one
 *	transaction (typically representing many USB maximum-sized packets)
 * @actual_len: how many bytes have been transferred
 * @status: current channel status (updated e.g. on interrupt)
 * @desired_mode: true if mode 1 is desired; false if mode 0 is desired
 *
 * channels are associated with an endpoint for the duration of at least
 * one usb transfer.
 */
struct dma_channel {
	void			*private_data;
	/* FIXME not void* private_data, but a dma_controller * */
	size_t			max_len;
	size_t			actual_len;
	enum dma_channel_status	status;
	bool			desired_mode;
};

/*
 * dma_channel_status - return status of dma channel
 * @c: the channel
 *
 * Returns the software's view of the channel status.  If that status is BUSY
 * then it's possible that the hardware has completed (or aborted) a transfer,
 * so the driver needs to update that status.
 */
static inline enum dma_channel_status
dma_channel_status(struct dma_channel *c)
{
	return (is_dma_capable() && c) ? c->status : MUSB_DMA_STATUS_UNKNOWN;
}

/**
 * struct dma_controller - A DMA Controller.
 * @start: call this to start a DMA controller;
 *	return 0 on success, else negative errno
 * @stop: call this to stop a DMA controller
 *	return 0 on success, else negative errno
 * @channel_alloc: call this to allocate a DMA channel
 * @channel_release: call this to release a DMA channel
 * @channel_abort: call this to abort a pending DMA transaction,
 *	returning it to FREE (but allocated) state
 *
 * Controllers manage dma channels.
 */
struct dma_controller {
	int			(*start)(struct dma_controller *);
	int			(*stop)(struct dma_controller *);
	struct dma_channel	*(*channel_alloc)(struct dma_controller *,
					struct musb_hw_ep *, u8 is_tx);
	void			(*channel_release)(struct dma_channel *);
	int			(*channel_program)(struct dma_channel *channel,
							u16 maxpacket, u8 mode,
							dma_addr_t dma_addr,
							u32 length);
	int			(*channel_abort)(struct dma_channel *);
	int			(*is_compatible)(struct dma_channel *channel,
							u16 maxpacket,
							void *buf, u32 length);
};

/* called after channel_program(), may indicate a fault */
extern void musb_dma_completion(struct musb *musb, u8 epnum, u8 transmit);


extern struct dma_controller *__init
dma_controller_create(struct musb *, void __iomem *);

extern void dma_controller_destroy(struct dma_controller *);

#endif	/* __MUSB_DMA_H__ */
