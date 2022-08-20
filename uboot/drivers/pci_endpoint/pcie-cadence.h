/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Cadence PCIe controlloer definitions
 * Adapted from linux kernel driver.
 * Copyright (c) 2017 Cadence
 *
 * Copyright (c) 2019
 * Written by Ramon Fried <ramon.fried@gmail.com>
 */

#ifndef PCIE_CADENCE_H
#define PCIE_CADENCE_H

#include <common.h>
#include <pci_ep.h>
#include <asm/io.h>
#include <linux/bitops.h>

/*
 * Local Management Registers
 */
#define CDNS_PCIE_LM_BASE	0x00100000

/* Vendor ID Register */
#define CDNS_PCIE_LM_ID		(CDNS_PCIE_LM_BASE + 0x0044)
#define  CDNS_PCIE_LM_ID_VENDOR_MASK	GENMASK(15, 0)
#define  CDNS_PCIE_LM_ID_VENDOR_SHIFT	0
#define  CDNS_PCIE_LM_ID_VENDOR(vid) \
	(((vid) << CDNS_PCIE_LM_ID_VENDOR_SHIFT) & CDNS_PCIE_LM_ID_VENDOR_MASK)
#define  CDNS_PCIE_LM_ID_SUBSYS_MASK	GENMASK(31, 16)
#define  CDNS_PCIE_LM_ID_SUBSYS_SHIFT	16
#define  CDNS_PCIE_LM_ID_SUBSYS(sub) \
	(((sub) << CDNS_PCIE_LM_ID_SUBSYS_SHIFT) & CDNS_PCIE_LM_ID_SUBSYS_MASK)

/* Root Port Requestor ID Register */
#define CDNS_PCIE_LM_RP_RID	(CDNS_PCIE_LM_BASE + 0x0228)
#define  CDNS_PCIE_LM_RP_RID_MASK	GENMASK(15, 0)
#define  CDNS_PCIE_LM_RP_RID_SHIFT	0
#define  CDNS_PCIE_LM_RP_RID_(rid) \
	(((rid) << CDNS_PCIE_LM_RP_RID_SHIFT) & CDNS_PCIE_LM_RP_RID_MASK)

/* Endpoint Bus and Device Number Register */
#define CDNS_PCIE_LM_EP_ID	(CDNS_PCIE_LM_BASE + 0x022c)
#define  CDNS_PCIE_LM_EP_ID_DEV_MASK	GENMASK(4, 0)
#define  CDNS_PCIE_LM_EP_ID_DEV_SHIFT	0
#define  CDNS_PCIE_LM_EP_ID_BUS_MASK	GENMASK(15, 8)
#define  CDNS_PCIE_LM_EP_ID_BUS_SHIFT	8

/* Endpoint Function f BAR b Configuration Registers */
#define CDNS_PCIE_LM_EP_FUNC_BAR_CFG0(fn) \
	(CDNS_PCIE_LM_BASE + 0x0240 + (fn) * 0x0008)
#define CDNS_PCIE_LM_EP_FUNC_BAR_CFG1(fn) \
	(CDNS_PCIE_LM_BASE + 0x0244 + (fn) * 0x0008)
#define  CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b) \
	(GENMASK(4, 0) << ((b) * 8))
#define  CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE(b, a) \
	(((a) << ((b) * 8)) & CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_APERTURE_MASK(b))
#define  CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b) \
	(GENMASK(7, 5) << ((b) * 8))
#define  CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL(b, c) \
	(((c) << ((b) * 8 + 5)) & CDNS_PCIE_LM_EP_FUNC_BAR_CFG_BAR_CTRL_MASK(b))

/* Endpoint Function Configuration Register */
#define CDNS_PCIE_LM_EP_FUNC_CFG	(CDNS_PCIE_LM_BASE + 0x02c0)

/* Root Complex BAR Configuration Register */
#define CDNS_PCIE_LM_RC_BAR_CFG	(CDNS_PCIE_LM_BASE + 0x0300)
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR0_APERTURE_MASK	GENMASK(5, 0)
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR0_APERTURE(a) \
	(((a) << 0) & CDNS_PCIE_LM_RC_BAR_CFG_BAR0_APERTURE_MASK)
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL_MASK		GENMASK(8, 6)
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL(c) \
	(((c) << 6) & CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL_MASK)
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR1_APERTURE_MASK	GENMASK(13, 9)
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR1_APERTURE(a) \
	(((a) << 9) & CDNS_PCIE_LM_RC_BAR_CFG_BAR1_APERTURE_MASK)
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL_MASK		GENMASK(16, 14)
#define  CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL(c) \
	(((c) << 14) & CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL_MASK)
#define  CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE	BIT(17)
#define  CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_32BITS	0
#define  CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS	BIT(18)
#define  CDNS_PCIE_LM_RC_BAR_CFG_IO_ENABLE		BIT(19)
#define  CDNS_PCIE_LM_RC_BAR_CFG_IO_16BITS		0
#define  CDNS_PCIE_LM_RC_BAR_CFG_IO_32BITS		BIT(20)
#define  CDNS_PCIE_LM_RC_BAR_CFG_CHECK_ENABLE		BIT(31)

/* BAR control values applicable to both Endpoint Function and Root Complex */
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED		0x0
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_IO_32BITS		0x1
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_32BITS		0x4
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_32BITS	0x5
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_MEM_64BITS		0x6
#define  CDNS_PCIE_LM_BAR_CFG_CTRL_PREFETCH_MEM_64BITS	0x7

/*
 * Endpoint Function Registers (PCI configuration space for endpoint functions)
 */
#define CDNS_PCIE_EP_FUNC_BASE(fn)	(((fn) << 12) & GENMASK(19, 12))

#define CDNS_PCIE_EP_FUNC_MSI_CAP_OFFSET	0x90

/*
 * Root Port Registers (PCI configuration space for the root port function)
 */
#define CDNS_PCIE_RP_BASE	0x00200000

/*
 * Address Translation Registers
 */
#define CDNS_PCIE_AT_BASE	0x00400000

/* Region r Outbound AXI to PCIe Address Translation Register 0 */
#define CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(r) \
	(CDNS_PCIE_AT_BASE + 0x0000 + ((r) & 0x1f) * 0x0020)
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS_MASK	GENMASK(5, 0)
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS_MASK)
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN_MASK	GENMASK(19, 12)
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) \
	(((devfn) << 12) & CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN_MASK)
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS_MASK	GENMASK(27, 20)
#define  CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS(bus) \
	(((bus) << 20) & CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS_MASK)

/* Region r Outbound AXI to PCIe Address Translation Register 1 */
#define CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(r) \
	(CDNS_PCIE_AT_BASE + 0x0004 + ((r) & 0x1f) * 0x0020)

/* Region r Outbound PCIe Descriptor Register 0 */
#define CDNS_PCIE_AT_OB_REGION_DESC0(r) \
	(CDNS_PCIE_AT_BASE + 0x0008 + ((r) & 0x1f) * 0x0020)
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_MASK		GENMASK(3, 0)
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_MEM		0x2
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_IO		0x6
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0	0xa
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1	0xb
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_NORMAL_MSG	0xc
#define  CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_VENDOR_MSG	0xd
/* Bit 23 MUST be set in RC mode. */
#define  CDNS_PCIE_AT_OB_REGION_DESC0_HARDCODED_RID	BIT(23)
#define  CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN_MASK	GENMASK(31, 24)
#define  CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN(devfn) \
	(((devfn) << 24) & CDNS_PCIE_AT_OB_REGION_DESC0_DEVFN_MASK)

/* Region r Outbound PCIe Descriptor Register 1 */
#define CDNS_PCIE_AT_OB_REGION_DESC1(r)	\
	(CDNS_PCIE_AT_BASE + 0x000c + ((r) & 0x1f) * 0x0020)
#define  CDNS_PCIE_AT_OB_REGION_DESC1_BUS_MASK	GENMASK(7, 0)
#define  CDNS_PCIE_AT_OB_REGION_DESC1_BUS(bus) \
	((bus) & CDNS_PCIE_AT_OB_REGION_DESC1_BUS_MASK)

/* Region r AXI Region Base Address Register 0 */
#define CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(r) \
	(CDNS_PCIE_AT_BASE + 0x0018 + ((r) & 0x1f) * 0x0020)
#define  CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS_MASK	GENMASK(5, 0)
#define  CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS_MASK)

/* Region r AXI Region Base Address Register 1 */
#define CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(r) \
	(CDNS_PCIE_AT_BASE + 0x001c + ((r) & 0x1f) * 0x0020)

/* Root Port BAR Inbound PCIe to AXI Address Translation Register */
#define CDNS_PCIE_AT_IB_RP_BAR_ADDR0(bar) \
	(CDNS_PCIE_AT_BASE + 0x0800 + (bar) * 0x0008)
#define  CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS_MASK	GENMASK(5, 0)
#define  CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS(nbits) \
	(((nbits) - 1) & CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS_MASK)
#define CDNS_PCIE_AT_IB_RP_BAR_ADDR1(bar) \
	(CDNS_PCIE_AT_BASE + 0x0804 + (bar) * 0x0008)

/* AXI link down register */
#define CDNS_PCIE_AT_LINKDOWN (CDNS_PCIE_AT_BASE + 0x0824)

enum cdns_pcie_rp_bar {
	RP_BAR0,
	RP_BAR1,
	RP_NO_BAR
};

/* Endpoint Function BAR Inbound PCIe to AXI Address Translation Register */
#define CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR0(fn, bar) \
	(CDNS_PCIE_AT_BASE + 0x0840 + (fn) * 0x0040 + (bar) * 0x0008)
#define CDNS_PCIE_AT_IB_EP_FUNC_BAR_ADDR1(fn, bar) \
	(CDNS_PCIE_AT_BASE + 0x0844 + (fn) * 0x0040 + (bar) * 0x0008)

/* Normal/Vendor specific message access: offset inside some outbound region */
#define CDNS_PCIE_NORMAL_MSG_ROUTING_MASK	GENMASK(7, 5)
#define CDNS_PCIE_NORMAL_MSG_ROUTING(route) \
	(((route) << 5) & CDNS_PCIE_NORMAL_MSG_ROUTING_MASK)
#define CDNS_PCIE_NORMAL_MSG_CODE_MASK		GENMASK(15, 8)
#define CDNS_PCIE_NORMAL_MSG_CODE(code) \
	(((code) << 8) & CDNS_PCIE_NORMAL_MSG_CODE_MASK)
#define CDNS_PCIE_MSG_NO_DATA			BIT(16)

#define CDNS_PCIE_EP_MIN_APERTURE		128	/* 128 bytes */

enum cdns_pcie_msg_code {
	MSG_CODE_ASSERT_INTA	= 0x20,
	MSG_CODE_ASSERT_INTB	= 0x21,
	MSG_CODE_ASSERT_INTC	= 0x22,
	MSG_CODE_ASSERT_INTD	= 0x23,
	MSG_CODE_DEASSERT_INTA	= 0x24,
	MSG_CODE_DEASSERT_INTB	= 0x25,
	MSG_CODE_DEASSERT_INTC	= 0x26,
	MSG_CODE_DEASSERT_INTD	= 0x27,
};

enum cdns_pcie_msg_routing {
	/* Route to Root Complex */
	MSG_ROUTING_TO_RC,

	/* Use Address Routing */
	MSG_ROUTING_BY_ADDR,

	/* Use ID Routing */
	MSG_ROUTING_BY_ID,

	/* Route as Broadcast Message from Root Complex */
	MSG_ROUTING_BCAST,

	/* Local message; terminate at receiver (INTx messages) */
	MSG_ROUTING_LOCAL,

	/* Gather & route to Root Complex (PME_TO_Ack message) */
	MSG_ROUTING_GATHER,
};

struct cdns_pcie {
	void __iomem *reg_base;
	u32	max_functions;
	u32	max_regions;
};

/* Register access */
static inline void cdns_pcie_writeb(struct cdns_pcie *pcie, u32 reg, u8 value)
{
	writeb(value, pcie->reg_base + reg);
}

static inline void cdns_pcie_writew(struct cdns_pcie *pcie, u32 reg, u16 value)
{
	writew(value, pcie->reg_base + reg);
}

static inline void cdns_pcie_writel(struct cdns_pcie *pcie, u32 reg, u32 value)
{
	writel(value, pcie->reg_base + reg);
}

static inline u32 cdns_pcie_readl(struct cdns_pcie *pcie, u32 reg)
{
	return readl(pcie->reg_base + reg);
}

/* Root Port register access */
static inline void cdns_pcie_rp_writeb(struct cdns_pcie *pcie,
				       u32 reg, u8 value)
{
	writeb(value, pcie->reg_base + CDNS_PCIE_RP_BASE + reg);
}

static inline void cdns_pcie_rp_writew(struct cdns_pcie *pcie,
				       u32 reg, u16 value)
{
	writew(value, pcie->reg_base + CDNS_PCIE_RP_BASE + reg);
}

static inline void cdns_pcie_rp_writel(struct cdns_pcie *pcie,
				       u32 reg, u32 value)
{
	writel(value, pcie->reg_base + CDNS_PCIE_RP_BASE + reg);
}

/* Endpoint Function register access */
static inline void cdns_pcie_ep_fn_writeb(struct cdns_pcie *pcie, u8 fn,
					  u32 reg, u8 value)
{
	writeb(value, pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg);
}

static inline void cdns_pcie_ep_fn_writew(struct cdns_pcie *pcie, u8 fn,
					  u32 reg, u16 value)
{
	writew(value, pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg);
}

static inline void cdns_pcie_ep_fn_writel(struct cdns_pcie *pcie, u8 fn,
					  u32 reg, u32 value)
{
	writel(value, pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg);
}

static inline u8 cdns_pcie_ep_fn_readb(struct cdns_pcie *pcie, u8 fn, u32 reg)
{
	return readb(pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg);
}

static inline u16 cdns_pcie_ep_fn_readw(struct cdns_pcie *pcie, u8 fn, u32 reg)
{
	return readw(pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg);
}

static inline u32 cdns_pcie_ep_fn_readl(struct cdns_pcie *pcie, u8 fn, u32 reg)
{
	return readl(pcie->reg_base + CDNS_PCIE_EP_FUNC_BASE(fn) + reg);
}

#endif /* end of include guard: PCIE_CADENCE_H */
