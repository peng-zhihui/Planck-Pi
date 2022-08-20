// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2019 MediaTek, Inc.
 * Authors: Chunfeng Yun <chunfeng.yun@mediatek.com>
 */

#include <clk.h>
#include <common.h>
#include <dm.h>
#include <dm/devres.h>
#include <generic-phy.h>
#include <malloc.h>
#include <usb.h>
#include <linux/errno.h>
#include <linux/compat.h>
#include <power/regulator.h>
#include <linux/iopoll.h>
#include <usb/xhci.h>

/* IPPC (IP Port Control) registers */
#define IPPC_IP_PW_CTRL0		0x00
#define CTRL0_IP_SW_RST			BIT(0)

#define IPPC_IP_PW_CTRL1		0x04
#define CTRL1_IP_HOST_PDN		BIT(0)

#define IPPC_IP_PW_STS1			0x10
#define STS1_IP_SLEEP_STS		BIT(30)
#define STS1_U3_MAC_RST			BIT(16)
#define STS1_XHCI_RST			BIT(11)
#define STS1_SYS125_RST			BIT(10)
#define STS1_REF_RST			BIT(8)
#define STS1_SYSPLL_STABLE		BIT(0)

#define IPPC_IP_XHCI_CAP		0x24
#define CAP_U3_PORT_NUM(p)		((p) & 0xff)
#define CAP_U2_PORT_NUM(p)		(((p) >> 8) & 0xff)

#define IPPC_U3_CTRL_0P			0x30
#define CTRL_U3_PORT_HOST_SEL		BIT(2)
#define CTRL_U3_PORT_PDN		BIT(1)
#define CTRL_U3_PORT_DIS		BIT(0)

#define IPPC_U2_CTRL_0P			0x50
#define CTRL_U2_PORT_HOST_SEL		BIT(2)
#define CTRL_U2_PORT_PDN		BIT(1)
#define CTRL_U2_PORT_DIS		BIT(0)

#define IPPC_U3_CTRL(p)	(IPPC_U3_CTRL_0P + ((p) * 0x08))
#define IPPC_U2_CTRL(p)	(IPPC_U2_CTRL_0P + ((p) * 0x08))

struct mtk_xhci {
	struct xhci_ctrl ctrl;	/* Needs to come first in this struct! */
	struct xhci_hccr *hcd;
	void __iomem *ippc;
	struct udevice *dev;
	struct udevice *vusb33_supply;
	struct udevice *vbus_supply;
	struct clk_bulk clks;
	struct phy_bulk phys;
	int num_u2ports;
	int num_u3ports;
};

static int xhci_mtk_host_enable(struct mtk_xhci *mtk)
{
	u32 value;
	u32 check_val;
	int ret;
	int i;

	/* power on host ip */
	clrbits_le32(mtk->ippc + IPPC_IP_PW_CTRL1, CTRL1_IP_HOST_PDN);

	/* power on and enable all u3 ports */
	for (i = 0; i < mtk->num_u3ports; i++) {
		clrsetbits_le32(mtk->ippc + IPPC_U3_CTRL(i),
				CTRL_U3_PORT_PDN | CTRL_U3_PORT_DIS,
				CTRL_U3_PORT_HOST_SEL);
	}

	/* power on and enable all u2 ports */
	for (i = 0; i < mtk->num_u2ports; i++) {
		clrsetbits_le32(mtk->ippc + IPPC_U2_CTRL(i),
				CTRL_U2_PORT_PDN | CTRL_U2_PORT_DIS,
				CTRL_U2_PORT_HOST_SEL);
	}

	/*
	 * wait for clocks to be stable, and clock domains reset to
	 * be inactive after power on and enable ports
	 */
	check_val = STS1_SYSPLL_STABLE | STS1_REF_RST |
			STS1_SYS125_RST | STS1_XHCI_RST;

	if (mtk->num_u3ports)
		check_val |= STS1_U3_MAC_RST;

	ret = readl_poll_timeout(mtk->ippc + IPPC_IP_PW_STS1, value,
				 (check_val == (value & check_val)), 20000);
	if (ret)
		dev_err(mtk->dev, "clocks are not stable 0x%x!\n", value);

	return ret;
}

static int xhci_mtk_host_disable(struct mtk_xhci *mtk)
{
	int i;

	/* power down all u3 ports */
	for (i = 0; i < mtk->num_u3ports; i++)
		setbits_le32(mtk->ippc + IPPC_U3_CTRL(i), CTRL_U3_PORT_PDN);

	/* power down all u2 ports */
	for (i = 0; i < mtk->num_u2ports; i++)
		setbits_le32(mtk->ippc + IPPC_U2_CTRL(i), CTRL_U2_PORT_PDN);

	/* power down host ip */
	setbits_le32(mtk->ippc + IPPC_IP_PW_CTRL1, CTRL1_IP_HOST_PDN);

	return 0;
}

static int xhci_mtk_ssusb_init(struct mtk_xhci *mtk)
{
	u32 value;

	/* reset whole ip */
	setbits_le32(mtk->ippc + IPPC_IP_PW_CTRL0, CTRL0_IP_SW_RST);
	udelay(1);
	clrbits_le32(mtk->ippc + IPPC_IP_PW_CTRL0, CTRL0_IP_SW_RST);

	value = readl(mtk->ippc + IPPC_IP_XHCI_CAP);
	mtk->num_u3ports = CAP_U3_PORT_NUM(value);
	mtk->num_u2ports = CAP_U2_PORT_NUM(value);
	dev_info(mtk->dev, "u2p:%d, u3p:%d\n",
		 mtk->num_u2ports, mtk->num_u3ports);

	return xhci_mtk_host_enable(mtk);
}

static int xhci_mtk_ofdata_get(struct mtk_xhci *mtk)
{
	struct udevice *dev = mtk->dev;
	int ret = 0;

	mtk->hcd = devfdt_remap_addr_name(dev, "mac");
	if (!mtk->hcd) {
		dev_err(dev, "failed to get xHCI base address\n");
		return -ENXIO;
	}

	mtk->ippc = devfdt_remap_addr_name(dev, "ippc");
	if (!mtk->ippc) {
		dev_err(dev, "failed to get IPPC base address\n");
		return -ENXIO;
	}

	dev_info(dev, "hcd: 0x%p, ippc: 0x%p\n", mtk->hcd, mtk->ippc);

	ret = clk_get_bulk(dev, &mtk->clks);
	if (ret) {
		dev_err(dev, "failed to get clocks %d!\n", ret);
		return ret;
	}

	ret = device_get_supply_regulator(dev, "vusb33-supply",
					  &mtk->vusb33_supply);
	if (ret)
		debug("can't get vusb33 regulator %d!\n", ret);

	ret = device_get_supply_regulator(dev, "vbus-supply",
					  &mtk->vbus_supply);
	if (ret)
		debug("can't get vbus regulator %d!\n", ret);

	return 0;
}

static int xhci_mtk_ldos_enable(struct mtk_xhci *mtk)
{
	int ret;

	ret = regulator_set_enable(mtk->vusb33_supply, true);
	if (ret < 0 && ret != -ENOSYS) {
		dev_err(mtk->dev, "failed to enable vusb33 %d!\n", ret);
		return ret;
	}

	ret = regulator_set_enable(mtk->vbus_supply, true);
	if (ret < 0 && ret != -ENOSYS) {
		dev_err(mtk->dev, "failed to enable vbus %d!\n", ret);
		regulator_set_enable(mtk->vusb33_supply, false);
		return ret;
	}

	return 0;
}

static void xhci_mtk_ldos_disable(struct mtk_xhci *mtk)
{
	regulator_set_enable(mtk->vbus_supply, false);
	regulator_set_enable(mtk->vusb33_supply, false);
}

static int xhci_mtk_phy_setup(struct mtk_xhci *mtk)
{
	struct udevice *dev = mtk->dev;
	struct phy_bulk *phys = &mtk->phys;
	int ret;

	ret = generic_phy_get_bulk(dev, phys);
	if (ret)
		return ret;

	ret = generic_phy_init_bulk(phys);
	if (ret)
		return ret;

	ret = generic_phy_power_on_bulk(phys);
	if (ret)
		generic_phy_exit_bulk(phys);

	return ret;
}

static void xhci_mtk_phy_shutdown(struct mtk_xhci *mtk)
{
	generic_phy_power_off_bulk(&mtk->phys);
	generic_phy_exit_bulk(&mtk->phys);
}

static int xhci_mtk_probe(struct udevice *dev)
{
	struct mtk_xhci *mtk = dev_get_priv(dev);
	struct xhci_hcor *hcor;
	int ret;

	mtk->dev = dev;
	ret = xhci_mtk_ofdata_get(mtk);
	if (ret)
		return ret;

	ret = xhci_mtk_ldos_enable(mtk);
	if (ret)
		goto ldos_err;

	ret = clk_enable_bulk(&mtk->clks);
	if (ret)
		goto clks_err;

	ret = xhci_mtk_phy_setup(mtk);
	if (ret)
		goto phys_err;

	ret = xhci_mtk_ssusb_init(mtk);
	if (ret)
		goto ssusb_init_err;

	hcor = (struct xhci_hcor *)((uintptr_t)mtk->hcd +
			HC_LENGTH(xhci_readl(&mtk->hcd->cr_capbase)));

	return xhci_register(dev, mtk->hcd, hcor);

ssusb_init_err:
	xhci_mtk_phy_shutdown(mtk);
phys_err:
	clk_disable_bulk(&mtk->clks);
clks_err:
	xhci_mtk_ldos_disable(mtk);
ldos_err:
	return ret;
}

static int xhci_mtk_remove(struct udevice *dev)
{
	struct mtk_xhci *mtk = dev_get_priv(dev);

	xhci_deregister(dev);
	xhci_mtk_host_disable(mtk);
	xhci_mtk_ldos_disable(mtk);
	clk_disable_bulk(&mtk->clks);

	return 0;
}

static const struct udevice_id xhci_mtk_ids[] = {
	{ .compatible = "mediatek,mtk-xhci" },
	{ }
};

U_BOOT_DRIVER(usb_xhci) = {
	.name = "xhci-mtk",
	.id = UCLASS_USB,
	.of_match = xhci_mtk_ids,
	.probe = xhci_mtk_probe,
	.remove = xhci_mtk_remove,
	.ops = &xhci_usb_ops,
	.bind = dm_scan_fdt_dev,
	.priv_auto_alloc_size = sizeof(struct mtk_xhci),
	.flags = DM_FLAG_ALLOC_PRIV_DMA,
};
