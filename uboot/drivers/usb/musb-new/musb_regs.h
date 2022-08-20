/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MUSB OTG driver register defines
 *
 * Copyright 2005 Mentor Graphics Corporation
 * Copyright (C) 2005-2006 by Texas Instruments
 * Copyright (C) 2006-2007 Nokia Corporation
 */

#ifndef __MUSB_REGS_H__
#define __MUSB_REGS_H__

#define MUSB_EP0_FIFOSIZE	64	/* This is non-configurable */

/*
 * MUSB Register bits
 */

/* POWER */
#define MUSB_POWER_ISOUPDATE	0x80
#define MUSB_POWER_SOFTCONN	0x40
#define MUSB_POWER_HSENAB	0x20
#define MUSB_POWER_HSMODE	0x10
#define MUSB_POWER_RESET	0x08
#define MUSB_POWER_RESUME	0x04
#define MUSB_POWER_SUSPENDM	0x02
#define MUSB_POWER_ENSUSPEND	0x01

/* INTRUSB */
#define MUSB_INTR_SUSPEND	0x01
#define MUSB_INTR_RESUME	0x02
#define MUSB_INTR_RESET		0x04
#define MUSB_INTR_BABBLE	0x04
#define MUSB_INTR_SOF		0x08
#define MUSB_INTR_CONNECT	0x10
#define MUSB_INTR_DISCONNECT	0x20
#define MUSB_INTR_SESSREQ	0x40
#define MUSB_INTR_VBUSERROR	0x80	/* For SESSION end */

/* DEVCTL */
#define MUSB_DEVCTL_BDEVICE	0x80
#define MUSB_DEVCTL_FSDEV	0x40
#define MUSB_DEVCTL_LSDEV	0x20
#define MUSB_DEVCTL_VBUS	0x18
#define MUSB_DEVCTL_VBUS_SHIFT	3
#define MUSB_DEVCTL_HM		0x04
#define MUSB_DEVCTL_HR		0x02
#define MUSB_DEVCTL_SESSION	0x01

/* MUSB ULPI VBUSCONTROL */
#define MUSB_ULPI_USE_EXTVBUS	0x01
#define MUSB_ULPI_USE_EXTVBUSIND 0x02
/* ULPI_REG_CONTROL */
#define MUSB_ULPI_REG_REQ	(1 << 0)
#define MUSB_ULPI_REG_CMPLT	(1 << 1)
#define MUSB_ULPI_RDN_WR	(1 << 2)

/* TESTMODE */
#define MUSB_TEST_FORCE_HOST	0x80
#define MUSB_TEST_FIFO_ACCESS	0x40
#define MUSB_TEST_FORCE_FS	0x20
#define MUSB_TEST_FORCE_HS	0x10
#define MUSB_TEST_PACKET	0x08
#define MUSB_TEST_K		0x04
#define MUSB_TEST_J		0x02
#define MUSB_TEST_SE0_NAK	0x01

/* Allocate for double-packet buffering (effectively doubles assigned _SIZE) */
#define MUSB_FIFOSZ_DPB	0x10
/* Allocation size (8, 16, 32, ... 4096) */
#define MUSB_FIFOSZ_SIZE	0x0f

/* CSR0 */
#define MUSB_CSR0_FLUSHFIFO	0x0100
#define MUSB_CSR0_TXPKTRDY	0x0002
#define MUSB_CSR0_RXPKTRDY	0x0001

/* CSR0 in Peripheral mode */
#define MUSB_CSR0_P_SVDSETUPEND	0x0080
#define MUSB_CSR0_P_SVDRXPKTRDY	0x0040
#define MUSB_CSR0_P_SENDSTALL	0x0020
#define MUSB_CSR0_P_SETUPEND	0x0010
#define MUSB_CSR0_P_DATAEND	0x0008
#define MUSB_CSR0_P_SENTSTALL	0x0004

/* CSR0 in Host mode */
#define MUSB_CSR0_H_DIS_PING		0x0800
#define MUSB_CSR0_H_WR_DATATOGGLE	0x0400	/* Set to allow setting: */
#define MUSB_CSR0_H_DATATOGGLE		0x0200	/* Data toggle control */
#define MUSB_CSR0_H_NAKTIMEOUT		0x0080
#define MUSB_CSR0_H_STATUSPKT		0x0040
#define MUSB_CSR0_H_REQPKT		0x0020
#define MUSB_CSR0_H_ERROR		0x0010
#define MUSB_CSR0_H_SETUPPKT		0x0008
#define MUSB_CSR0_H_RXSTALL		0x0004

/* CSR0 bits to avoid zeroing (write zero clears, write 1 ignored) */
#define MUSB_CSR0_P_WZC_BITS	\
	(MUSB_CSR0_P_SENTSTALL)
#define MUSB_CSR0_H_WZC_BITS	\
	(MUSB_CSR0_H_NAKTIMEOUT | MUSB_CSR0_H_RXSTALL \
	| MUSB_CSR0_RXPKTRDY)

/* TxType/RxType */
#define MUSB_TYPE_SPEED		0xc0
#define MUSB_TYPE_SPEED_SHIFT	6
#define MUSB_TYPE_PROTO		0x30	/* Implicitly zero for ep0 */
#define MUSB_TYPE_PROTO_SHIFT	4
#define MUSB_TYPE_REMOTE_END	0xf	/* Implicitly zero for ep0 */

/* CONFIGDATA */
#define MUSB_CONFIGDATA_MPRXE		0x80	/* Auto bulk pkt combining */
#define MUSB_CONFIGDATA_MPTXE		0x40	/* Auto bulk pkt splitting */
#define MUSB_CONFIGDATA_BIGENDIAN	0x20
#define MUSB_CONFIGDATA_HBRXE		0x10	/* HB-ISO for RX */
#define MUSB_CONFIGDATA_HBTXE		0x08	/* HB-ISO for TX */
#define MUSB_CONFIGDATA_DYNFIFO		0x04	/* Dynamic FIFO sizing */
#define MUSB_CONFIGDATA_SOFTCONE	0x02	/* SoftConnect */
#define MUSB_CONFIGDATA_UTMIDW		0x01	/* Data width 0/1 => 8/16bits */

/* TXCSR in Peripheral and Host mode */
#define MUSB_TXCSR_AUTOSET		0x8000
#define MUSB_TXCSR_DMAENAB		0x1000
#define MUSB_TXCSR_FRCDATATOG		0x0800
#define MUSB_TXCSR_DMAMODE		0x0400
#define MUSB_TXCSR_CLRDATATOG		0x0040
#define MUSB_TXCSR_FLUSHFIFO		0x0008
#define MUSB_TXCSR_FIFONOTEMPTY		0x0002
#define MUSB_TXCSR_TXPKTRDY		0x0001

/* TXCSR in Peripheral mode */
#define MUSB_TXCSR_P_ISO		0x4000
#define MUSB_TXCSR_P_INCOMPTX		0x0080
#define MUSB_TXCSR_P_SENTSTALL		0x0020
#define MUSB_TXCSR_P_SENDSTALL		0x0010
#define MUSB_TXCSR_P_UNDERRUN		0x0004

/* TXCSR in Host mode */
#define MUSB_TXCSR_H_WR_DATATOGGLE	0x0200
#define MUSB_TXCSR_H_DATATOGGLE		0x0100
#define MUSB_TXCSR_H_NAKTIMEOUT		0x0080
#define MUSB_TXCSR_H_RXSTALL		0x0020
#define MUSB_TXCSR_H_ERROR		0x0004

/* TXCSR bits to avoid zeroing (write zero clears, write 1 ignored) */
#define MUSB_TXCSR_P_WZC_BITS	\
	(MUSB_TXCSR_P_INCOMPTX | MUSB_TXCSR_P_SENTSTALL \
	| MUSB_TXCSR_P_UNDERRUN | MUSB_TXCSR_FIFONOTEMPTY)
#define MUSB_TXCSR_H_WZC_BITS	\
	(MUSB_TXCSR_H_NAKTIMEOUT | MUSB_TXCSR_H_RXSTALL \
	| MUSB_TXCSR_H_ERROR | MUSB_TXCSR_FIFONOTEMPTY)

/* RXCSR in Peripheral and Host mode */
#define MUSB_RXCSR_AUTOCLEAR		0x8000
#define MUSB_RXCSR_DMAENAB		0x2000
#define MUSB_RXCSR_DISNYET		0x1000
#define MUSB_RXCSR_PID_ERR		0x1000
#define MUSB_RXCSR_DMAMODE		0x0800
#define MUSB_RXCSR_INCOMPRX		0x0100
#define MUSB_RXCSR_CLRDATATOG		0x0080
#define MUSB_RXCSR_FLUSHFIFO		0x0010
#define MUSB_RXCSR_DATAERROR		0x0008
#define MUSB_RXCSR_FIFOFULL		0x0002
#define MUSB_RXCSR_RXPKTRDY		0x0001

/* RXCSR in Peripheral mode */
#define MUSB_RXCSR_P_ISO		0x4000
#define MUSB_RXCSR_P_SENTSTALL		0x0040
#define MUSB_RXCSR_P_SENDSTALL		0x0020
#define MUSB_RXCSR_P_OVERRUN		0x0004

/* RXCSR in Host mode */
#define MUSB_RXCSR_H_AUTOREQ		0x4000
#define MUSB_RXCSR_H_WR_DATATOGGLE	0x0400
#define MUSB_RXCSR_H_DATATOGGLE		0x0200
#define MUSB_RXCSR_H_RXSTALL		0x0040
#define MUSB_RXCSR_H_REQPKT		0x0020
#define MUSB_RXCSR_H_ERROR		0x0004

/* RXCSR bits to avoid zeroing (write zero clears, write 1 ignored) */
#define MUSB_RXCSR_P_WZC_BITS	\
	(MUSB_RXCSR_P_SENTSTALL | MUSB_RXCSR_P_OVERRUN \
	| MUSB_RXCSR_RXPKTRDY)
#define MUSB_RXCSR_H_WZC_BITS	\
	(MUSB_RXCSR_H_RXSTALL | MUSB_RXCSR_H_ERROR \
	| MUSB_RXCSR_DATAERROR | MUSB_RXCSR_RXPKTRDY)

/* HUBADDR */
#define MUSB_HUBADDR_MULTI_TT		0x80


/* SUNXI has different reg addresses, but identical r/w functions */
#ifndef CONFIG_ARCH_SUNXI 

/*
 * Common USB registers
 */

#define MUSB_FADDR		0x00	/* 8-bit */
#define MUSB_POWER		0x01	/* 8-bit */

#define MUSB_INTRTX		0x02	/* 16-bit */
#define MUSB_INTRRX		0x04
#define MUSB_INTRTXE		0x06
#define MUSB_INTRRXE		0x08
#define MUSB_INTRUSB		0x0A	/* 8 bit */
#define MUSB_INTRUSBE		0x0B	/* 8 bit */
#define MUSB_FRAME		0x0C
#define MUSB_INDEX		0x0E	/* 8 bit */
#define MUSB_TESTMODE		0x0F	/* 8 bit */

/* Get offset for a given FIFO from musb->mregs */
#if defined(CONFIG_USB_MUSB_TUSB6010) ||	\
	defined(CONFIG_USB_MUSB_TUSB6010_MODULE)
#define MUSB_FIFO_OFFSET(epnum)	(0x200 + ((epnum) * 0x20))
#else
#define MUSB_FIFO_OFFSET(epnum)	(0x20 + ((epnum) * 4))
#endif

/*
 * Additional Control Registers
 */

#define MUSB_DEVCTL		0x60	/* 8 bit */

/* These are always controlled through the INDEX register */
#define MUSB_TXFIFOSZ		0x62	/* 8-bit (see masks) */
#define MUSB_RXFIFOSZ		0x63	/* 8-bit (see masks) */
#define MUSB_TXFIFOADD		0x64	/* 16-bit offset shifted right 3 */
#define MUSB_RXFIFOADD		0x66	/* 16-bit offset shifted right 3 */

/* REVISIT: vctrl/vstatus: optional vendor utmi+phy register at 0x68 */
#define MUSB_HWVERS		0x6C	/* 8 bit */
#define MUSB_ULPI_BUSCONTROL	0x70	/* 8 bit */
#define MUSB_ULPI_INT_MASK	0x72	/* 8 bit */
#define MUSB_ULPI_INT_SRC	0x73	/* 8 bit */
#define MUSB_ULPI_REG_DATA	0x74	/* 8 bit */
#define MUSB_ULPI_REG_ADDR	0x75	/* 8 bit */
#define MUSB_ULPI_REG_CONTROL	0x76	/* 8 bit */
#define MUSB_ULPI_RAW_DATA	0x77	/* 8 bit */

#define MUSB_EPINFO		0x78	/* 8 bit */
#define MUSB_RAMINFO		0x79	/* 8 bit */
#define MUSB_LINKINFO		0x7a	/* 8 bit */
#define MUSB_VPLEN		0x7b	/* 8 bit */
#define MUSB_HS_EOF1		0x7c	/* 8 bit */
#define MUSB_FS_EOF1		0x7d	/* 8 bit */
#define MUSB_LS_EOF1		0x7e	/* 8 bit */

/* Offsets to endpoint registers */
#define MUSB_TXMAXP		0x00
#define MUSB_TXCSR		0x02
#define MUSB_CSR0		MUSB_TXCSR	/* Re-used for EP0 */
#define MUSB_RXMAXP		0x04
#define MUSB_RXCSR		0x06
#define MUSB_RXCOUNT		0x08
#define MUSB_COUNT0		MUSB_RXCOUNT	/* Re-used for EP0 */
#define MUSB_TXTYPE		0x0A
#define MUSB_TYPE0		MUSB_TXTYPE	/* Re-used for EP0 */
#define MUSB_TXINTERVAL		0x0B
#define MUSB_NAKLIMIT0		MUSB_TXINTERVAL	/* Re-used for EP0 */
#define MUSB_RXTYPE		0x0C
#define MUSB_RXINTERVAL		0x0D
#define MUSB_FIFOSIZE		0x0F
#define MUSB_CONFIGDATA		MUSB_FIFOSIZE	/* Re-used for EP0 */

/* Offsets to endpoint registers in indexed model (using INDEX register) */
#define MUSB_INDEXED_OFFSET(_epnum, _offset)	\
	(0x10 + (_offset))

/* Offsets to endpoint registers in flat models */
#define MUSB_FLAT_OFFSET(_epnum, _offset)	\
	(0x100 + (0x10*(_epnum)) + (_offset))

#if defined(CONFIG_USB_MUSB_TUSB6010) ||	\
	defined(CONFIG_USB_MUSB_TUSB6010_MODULE)
/* TUSB6010 EP0 configuration register is special */
#define MUSB_TUSB_OFFSET(_epnum, _offset)	\
	(0x10 + _offset)
#include "tusb6010.h"		/* Needed "only" for TUSB_EP0_CONF */
#endif

#define MUSB_TXCSR_MODE			0x2000

/* "bus control"/target registers, for host side multipoint (external hubs) */
#define MUSB_TXFUNCADDR		0x00
#define MUSB_TXHUBADDR		0x02
#define MUSB_TXHUBPORT		0x03

#define MUSB_RXFUNCADDR		0x04
#define MUSB_RXHUBADDR		0x06
#define MUSB_RXHUBPORT		0x07

#define MUSB_BUSCTL_OFFSET(_epnum, _offset) \
	(0x80 + (8*(_epnum)) + (_offset))

#else /* CONFIG_ARCH_SUNXI */

/*
 * Common USB registers
 */

#define MUSB_FADDR		0x0098
#define MUSB_POWER		0x0040

#define MUSB_INTRTX		0x0044
#define MUSB_INTRRX		0x0046
#define MUSB_INTRTXE		0x0048
#define MUSB_INTRRXE		0x004A
#define MUSB_INTRUSB		0x004C
#define MUSB_INTRUSBE		0x0050
#define MUSB_FRAME		0x0054
#define MUSB_INDEX		0x0042
#define MUSB_TESTMODE		0x007C

/* Get offset for a given FIFO from musb->mregs */
#define MUSB_FIFO_OFFSET(epnum)	(0x00 + ((epnum) * 4))

/*
 * Additional Control Registers
 */

#define MUSB_DEVCTL		0x0041

/* These are always controlled through the INDEX register */
#define MUSB_TXFIFOSZ		0x0090
#define MUSB_RXFIFOSZ		0x0094
#define MUSB_TXFIFOADD		0x0092
#define MUSB_RXFIFOADD		0x0096

#define MUSB_EPINFO		0x0078
#define MUSB_RAMINFO		0x0079
#define MUSB_LINKINFO		0x007A
#define MUSB_VPLEN		0x007B
#define MUSB_HS_EOF1		0x007C
#define MUSB_FS_EOF1		0x007D
#define MUSB_LS_EOF1		0x007E

/* Offsets to endpoint registers */
#define MUSB_TXMAXP		0x0080
#define MUSB_TXCSR		0x0082
#define MUSB_CSR0		0x0082
#define MUSB_RXMAXP		0x0084
#define MUSB_RXCSR		0x0086
#define MUSB_RXCOUNT		0x0088
#define MUSB_COUNT0		0x0088
#define MUSB_TXTYPE		0x008C
#define MUSB_TYPE0		0x008C
#define MUSB_TXINTERVAL		0x008D
#define MUSB_NAKLIMIT0		0x008D
#define MUSB_RXTYPE		0x008E
#define MUSB_RXINTERVAL		0x008F

#define MUSB_CONFIGDATA		0x00b0 /* musb_read_configdata adds 0x10 ! */
#define MUSB_FIFOSIZE		0x0090

/* Offsets to endpoint registers in indexed model (using INDEX register) */
#define MUSB_INDEXED_OFFSET(_epnum, _offset) (_offset)

#define MUSB_TXCSR_MODE		0x2000

/* "bus control"/target registers, for host side multipoint (external hubs) */
#define MUSB_TXFUNCADDR		0x0098
#define MUSB_TXHUBADDR		0x009A
#define MUSB_TXHUBPORT		0x009B

#define MUSB_RXFUNCADDR		0x009C
#define MUSB_RXHUBADDR		0x009E
#define MUSB_RXHUBPORT		0x009F

/* Endpoint is selected with MUSB_INDEX. */
#define MUSB_BUSCTL_OFFSET(_epnum, _offset) (_offset)

#endif /* CONFIG_ARCH_SUNXI */

static inline void musb_write_txfifosz(void __iomem *mbase, u8 c_size)
{
	musb_writeb(mbase, MUSB_TXFIFOSZ, c_size);
}

static inline void musb_write_txfifoadd(void __iomem *mbase, u16 c_off)
{
	musb_writew(mbase, MUSB_TXFIFOADD, c_off);
}

static inline void musb_write_rxfifosz(void __iomem *mbase, u8 c_size)
{
	musb_writeb(mbase, MUSB_RXFIFOSZ, c_size);
}

static inline void  musb_write_rxfifoadd(void __iomem *mbase, u16 c_off)
{
	musb_writew(mbase, MUSB_RXFIFOADD, c_off);
}

static inline void musb_write_ulpi_buscontrol(void __iomem *mbase, u8 val)
{
#ifndef CONFIG_ARCH_SUNXI /* No ulpi on sunxi */
	musb_writeb(mbase, MUSB_ULPI_BUSCONTROL, val);
#endif
}

static inline u8 musb_read_txfifosz(void __iomem *mbase)
{
	return musb_readb(mbase, MUSB_TXFIFOSZ);
}

static inline u16 musb_read_txfifoadd(void __iomem *mbase)
{
	return musb_readw(mbase, MUSB_TXFIFOADD);
}

static inline u8 musb_read_rxfifosz(void __iomem *mbase)
{
	return musb_readb(mbase, MUSB_RXFIFOSZ);
}

static inline u16  musb_read_rxfifoadd(void __iomem *mbase)
{
	return musb_readw(mbase, MUSB_RXFIFOADD);
}

static inline u8 musb_read_ulpi_buscontrol(void __iomem *mbase)
{
#ifdef CONFIG_ARCH_SUNXI /* No ulpi on sunxi */
	return 0;
#else
	return musb_readb(mbase, MUSB_ULPI_BUSCONTROL);
#endif
}

static inline u8 musb_read_configdata(void __iomem *mbase)
{
#if defined CONFIG_MACH_SUN8I_A33 || defined CONFIG_MACH_SUN8I_A83T || \
		defined CONFIG_MACH_SUNXI_H3_H5 || defined CONFIG_MACH_SUN50I
	/* <Sigh> allwinner saves a reg, and we need to hardcode this */
	return 0xde;
#else
	musb_writeb(mbase, MUSB_INDEX, 0);
	return musb_readb(mbase, 0x10 + MUSB_CONFIGDATA);
#endif
}

static inline u16 musb_read_hwvers(void __iomem *mbase)
{
#ifdef CONFIG_ARCH_SUNXI
	return 0; /* Unknown version */
#else
	return musb_readw(mbase, MUSB_HWVERS);
#endif
}

static inline void __iomem *musb_read_target_reg_base(u8 i, void __iomem *mbase)
{
	return (MUSB_BUSCTL_OFFSET(i, 0) + mbase);
}

static inline void musb_write_rxfunaddr(void __iomem *ep_target_regs,
		u8 qh_addr_reg)
{
	musb_writeb(ep_target_regs, MUSB_RXFUNCADDR, qh_addr_reg);
}

static inline void musb_write_rxhubaddr(void __iomem *ep_target_regs,
		u8 qh_h_addr_reg)
{
	musb_writeb(ep_target_regs, MUSB_RXHUBADDR, qh_h_addr_reg);
}

static inline void musb_write_rxhubport(void __iomem *ep_target_regs,
		u8 qh_h_port_reg)
{
	musb_writeb(ep_target_regs, MUSB_RXHUBPORT, qh_h_port_reg);
}

static inline void  musb_write_txfunaddr(void __iomem *mbase, u8 epnum,
		u8 qh_addr_reg)
{
	musb_writeb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_TXFUNCADDR),
			qh_addr_reg);
}

static inline void  musb_write_txhubaddr(void __iomem *mbase, u8 epnum,
		u8 qh_addr_reg)
{
	musb_writeb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_TXHUBADDR),
			qh_addr_reg);
}

static inline void  musb_write_txhubport(void __iomem *mbase, u8 epnum,
		u8 qh_h_port_reg)
{
	musb_writeb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_TXHUBPORT),
			qh_h_port_reg);
}

static inline u8 musb_read_rxfunaddr(void __iomem *mbase, u8 epnum)
{
	return musb_readb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_RXFUNCADDR));
}

static inline u8 musb_read_rxhubaddr(void __iomem *mbase, u8 epnum)
{
	return musb_readb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_RXHUBADDR));
}

static inline u8 musb_read_rxhubport(void __iomem *mbase, u8 epnum)
{
	return musb_readb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_RXHUBPORT));
}

static inline u8  musb_read_txfunaddr(void __iomem *mbase, u8 epnum)
{
	return musb_readb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_TXFUNCADDR));
}

static inline u8  musb_read_txhubaddr(void __iomem *mbase, u8 epnum)
{
	return musb_readb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_TXHUBADDR));
}

static inline u8  musb_read_txhubport(void __iomem *mbase, u8 epnum)
{
	return musb_readb(mbase, MUSB_BUSCTL_OFFSET(epnum, MUSB_TXHUBPORT));
}

#endif	/* __MUSB_REGS_H__ */
