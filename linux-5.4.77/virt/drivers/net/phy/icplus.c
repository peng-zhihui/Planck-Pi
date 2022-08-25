// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for ICPlus PHYs
 *
 * Copyright (c) 2007 Freescale Semiconductor, Inc.
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/property.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <linux/uaccess.h>

MODULE_DESCRIPTION("ICPlus IP175C/IP101A/IP101G/IC1001 PHY drivers");
MODULE_AUTHOR("Michael Barkowski");
MODULE_LICENSE("GPL");

/* IP101A/G - IP1001 */
#define IP10XX_SPEC_CTRL_STATUS		16	/* Spec. Control Register */
#define IP1001_RXPHASE_SEL		BIT(0)	/* Add delay on RX_CLK */
#define IP1001_TXPHASE_SEL		BIT(1)	/* Add delay on TX_CLK */
#define IP1001_SPEC_CTRL_STATUS_2	20	/* IP1001 Spec. Control Reg 2 */
#define IP1001_APS_ON			11	/* IP1001 APS Mode  bit */
#define IP101A_G_APS_ON			BIT(1)	/* IP101A/G APS Mode bit */
#define IP101A_G_IRQ_CONF_STATUS	0x11	/* Conf Info IRQ & Status Reg */
#define	IP101A_G_IRQ_PIN_USED		BIT(15) /* INTR pin used */
#define IP101A_G_IRQ_ALL_MASK		BIT(11) /* IRQ's inactive */
#define IP101A_G_IRQ_SPEED_CHANGE	BIT(2)
#define IP101A_G_IRQ_DUPLEX_CHANGE	BIT(1)
#define IP101A_G_IRQ_LINK_CHANGE	BIT(0)

#define IP101G_DIGITAL_IO_SPEC_CTRL			0x1d
#define IP101G_DIGITAL_IO_SPEC_CTRL_SEL_INTR32		BIT(2)

/* The 32-pin IP101GR package can re-configure the mode of the RXER/INTR_32 pin
 * (pin number 21). The hardware default is RXER (receive error) mode. But it
 * can be configured to interrupt mode manually.
 */
enum ip101gr_sel_intr32 {
	IP101GR_SEL_INTR32_KEEP,
	IP101GR_SEL_INTR32_INTR,
	IP101GR_SEL_INTR32_RXER,
};

struct ip101a_g_phy_priv {
	enum ip101gr_sel_intr32 sel_intr32;
};

static int ip175c_config_init(struct phy_device *phydev)
{
	int err, i;
	static int full_reset_performed;

	if (full_reset_performed == 0) {

		/* master reset */
		err = mdiobus_write(phydev->mdio.bus, 30, 0, 0x175c);
		if (err < 0)
			return err;

		/* ensure no bus delays overlap reset period */
		err = mdiobus_read(phydev->mdio.bus, 30, 0);

		/* data sheet specifies reset period is 2 msec */
		mdelay(2);

		/* enable IP175C mode */
		err = mdiobus_write(phydev->mdio.bus, 29, 31, 0x175c);
		if (err < 0)
			return err;

		/* Set MII0 speed and duplex (in PHY mode) */
		err = mdiobus_write(phydev->mdio.bus, 29, 22, 0x420);
		if (err < 0)
			return err;

		/* reset switch ports */
		for (i = 0; i < 5; i++) {
			err = mdiobus_write(phydev->mdio.bus, i,
					    MII_BMCR, BMCR_RESET);
			if (err < 0)
				return err;
		}

		for (i = 0; i < 5; i++)
			err = mdiobus_read(phydev->mdio.bus, i, MII_BMCR);

		mdelay(2);

		full_reset_performed = 1;
	}

	if (phydev->mdio.addr != 4) {
		phydev->state = PHY_RUNNING;
		phydev->speed = SPEED_100;
		phydev->duplex = DUPLEX_FULL;
		phydev->link = 1;
		netif_carrier_on(phydev->attached_dev);
	}

	return 0;
}

static int ip1xx_reset(struct phy_device *phydev)
{
	int bmcr;

	/* Software Reset PHY */
	bmcr = phy_read(phydev, MII_BMCR);
	if (bmcr < 0)
		return bmcr;
	bmcr |= BMCR_RESET;
	bmcr = phy_write(phydev, MII_BMCR, bmcr);
	if (bmcr < 0)
		return bmcr;

	do {
		bmcr = phy_read(phydev, MII_BMCR);
		if (bmcr < 0)
			return bmcr;
	} while (bmcr & BMCR_RESET);

	return 0;
}

static int ip1001_config_init(struct phy_device *phydev)
{
	int c;

	c = ip1xx_reset(phydev);
	if (c < 0)
		return c;

	/* Enable Auto Power Saving mode */
	c = phy_read(phydev, IP1001_SPEC_CTRL_STATUS_2);
	if (c < 0)
		return c;
	c |= IP1001_APS_ON;
	c = phy_write(phydev, IP1001_SPEC_CTRL_STATUS_2, c);
	if (c < 0)
		return c;

	if (phy_interface_is_rgmii(phydev)) {

		c = phy_read(phydev, IP10XX_SPEC_CTRL_STATUS);
		if (c < 0)
			return c;

		c &= ~(IP1001_RXPHASE_SEL | IP1001_TXPHASE_SEL);

		if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID)
			c |= (IP1001_RXPHASE_SEL | IP1001_TXPHASE_SEL);
		else if (phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
			c |= IP1001_RXPHASE_SEL;
		else if (phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID)
			c |= IP1001_TXPHASE_SEL;

		c = phy_write(phydev, IP10XX_SPEC_CTRL_STATUS, c);
		if (c < 0)
			return c;
	}

	return 0;
}

static int ip175c_read_status(struct phy_device *phydev)
{
	if (phydev->mdio.addr == 4) /* WAN port */
		genphy_read_status(phydev);
	else
		/* Don't need to read status for switch ports */
		phydev->irq = PHY_IGNORE_INTERRUPT;

	return 0;
}

static int ip175c_config_aneg(struct phy_device *phydev)
{
	if (phydev->mdio.addr == 4) /* WAN port */
		genphy_config_aneg(phydev);

	return 0;
}

static int ip101a_g_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct ip101a_g_phy_priv *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Both functions (RX error and interrupt status) are sharing the same
	 * pin on the 32-pin IP101GR, so this is an exclusive choice.
	 */
	if (device_property_read_bool(dev, "icplus,select-rx-error") &&
	    device_property_read_bool(dev, "icplus,select-interrupt")) {
		dev_err(dev,
			"RXER and INTR mode cannot be selected together\n");
		return -EINVAL;
	}

	if (device_property_read_bool(dev, "icplus,select-rx-error"))
		priv->sel_intr32 = IP101GR_SEL_INTR32_RXER;
	else if (device_property_read_bool(dev, "icplus,select-interrupt"))
		priv->sel_intr32 = IP101GR_SEL_INTR32_INTR;
	else
		priv->sel_intr32 = IP101GR_SEL_INTR32_KEEP;

	phydev->priv = priv;

	return 0;
}

static int ip101a_g_config_init(struct phy_device *phydev)
{
	struct ip101a_g_phy_priv *priv = phydev->priv;
	int err, c;

	c = ip1xx_reset(phydev);
	if (c < 0)
		return c;

	/* configure the RXER/INTR_32 pin of the 32-pin IP101GR if needed: */
	switch (priv->sel_intr32) {
	case IP101GR_SEL_INTR32_RXER:
		err = phy_modify(phydev, IP101G_DIGITAL_IO_SPEC_CTRL,
				 IP101G_DIGITAL_IO_SPEC_CTRL_SEL_INTR32, 0);
		if (err < 0)
			return err;
		break;

	case IP101GR_SEL_INTR32_INTR:
		err = phy_modify(phydev, IP101G_DIGITAL_IO_SPEC_CTRL,
				 IP101G_DIGITAL_IO_SPEC_CTRL_SEL_INTR32,
				 IP101G_DIGITAL_IO_SPEC_CTRL_SEL_INTR32);
		if (err < 0)
			return err;
		break;

	default:
		/* Don't touch IP101G_DIGITAL_IO_SPEC_CTRL because it's not
		 * documented on IP101A and it's not clear whether this would
		 * cause problems.
		 * For the 32-pin IP101GR we simply keep the SEL_INTR32
		 * configuration as set by the bootloader when not configured
		 * to one of the special functions.
		 */
		break;
	}

	/* Enable Auto Power Saving mode */
	c = phy_read(phydev, IP10XX_SPEC_CTRL_STATUS);
	c |= IP101A_G_APS_ON;

	return phy_write(phydev, IP10XX_SPEC_CTRL_STATUS, c);
}

static int ip101a_g_config_intr(struct phy_device *phydev)
{
	u16 val;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		/* INTR pin used: Speed/link/duplex will cause an interrupt */
		val = IP101A_G_IRQ_PIN_USED;
	else
		val = IP101A_G_IRQ_ALL_MASK;

	return phy_write(phydev, IP101A_G_IRQ_CONF_STATUS, val);
}

static int ip101a_g_did_interrupt(struct phy_device *phydev)
{
	int val = phy_read(phydev, IP101A_G_IRQ_CONF_STATUS);

	if (val < 0)
		return 0;

	return val & (IP101A_G_IRQ_SPEED_CHANGE |
		      IP101A_G_IRQ_DUPLEX_CHANGE |
		      IP101A_G_IRQ_LINK_CHANGE);
}

static int ip101a_g_ack_interrupt(struct phy_device *phydev)
{
	int err = phy_read(phydev, IP101A_G_IRQ_CONF_STATUS);
	if (err < 0)
		return err;

	return 0;
}

static struct phy_driver icplus_driver[] = {
{
	.phy_id		= 0x02430d80,
	.name		= "ICPlus IP175C",
	.phy_id_mask	= 0x0ffffff0,
	/* PHY_BASIC_FEATURES */
	.config_init	= &ip175c_config_init,
	.config_aneg	= &ip175c_config_aneg,
	.read_status	= &ip175c_read_status,
	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
}, {
	.phy_id		= 0x02430d90,
	.name		= "ICPlus IP1001",
	.phy_id_mask	= 0x0ffffff0,
	/* PHY_GBIT_FEATURES */
	.config_init	= &ip1001_config_init,
	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
}, {
	.phy_id		= 0x02430c54,
	.name		= "ICPlus IP101A/G",
	.phy_id_mask	= 0x0ffffff0,
	/* PHY_BASIC_FEATURES */
	.probe		= ip101a_g_probe,
	.config_intr	= ip101a_g_config_intr,
	.did_interrupt	= ip101a_g_did_interrupt,
	.ack_interrupt	= ip101a_g_ack_interrupt,
	.config_init	= &ip101a_g_config_init,
	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
} };

module_phy_driver(icplus_driver);

static struct mdio_device_id __maybe_unused icplus_tbl[] = {
	{ 0x02430d80, 0x0ffffff0 },
	{ 0x02430d90, 0x0ffffff0 },
	{ 0x02430c54, 0x0ffffff0 },
	{ }
};

MODULE_DEVICE_TABLE(mdio, icplus_tbl);
