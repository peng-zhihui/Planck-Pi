// SPDX-License-Identifier: GPL-2.0
/*
 * TI PHY drivers
 *
 */
#include <common.h>
#include <log.h>
#include <phy.h>
#include <dm/devres.h>
#include <linux/bitops.h>
#include <linux/compat.h>
#include <malloc.h>

#include <dm.h>
#include <dt-bindings/net/ti-dp83867.h>


/* TI DP83867 */
#define DP83867_DEVADDR		0x1f

#define MII_DP83867_PHYCTRL	0x10
#define MII_DP83867_MICR	0x12
#define MII_DP83867_CFG2	0x14
#define MII_DP83867_BISCR	0x16
#define DP83867_CTRL		0x1f

/* Extended Registers */
#define DP83867_CFG4		0x0031
#define DP83867_RGMIICTL	0x0032
#define DP83867_STRAP_STS1	0x006E
#define DP83867_STRAP_STS2	0x006f
#define DP83867_RGMIIDCTL	0x0086
#define DP83867_IO_MUX_CFG	0x0170
#define DP83867_SGMIICTL	0x00D3

#define DP83867_SW_RESET	BIT(15)
#define DP83867_SW_RESTART	BIT(14)

/* MICR Interrupt bits */
#define MII_DP83867_MICR_AN_ERR_INT_EN		BIT(15)
#define MII_DP83867_MICR_SPEED_CHNG_INT_EN	BIT(14)
#define MII_DP83867_MICR_DUP_MODE_CHNG_INT_EN	BIT(13)
#define MII_DP83867_MICR_PAGE_RXD_INT_EN	BIT(12)
#define MII_DP83867_MICR_AUTONEG_COMP_INT_EN	BIT(11)
#define MII_DP83867_MICR_LINK_STS_CHNG_INT_EN	BIT(10)
#define MII_DP83867_MICR_FALSE_CARRIER_INT_EN	BIT(8)
#define MII_DP83867_MICR_SLEEP_MODE_CHNG_INT_EN	BIT(4)
#define MII_DP83867_MICR_WOL_INT_EN		BIT(3)
#define MII_DP83867_MICR_XGMII_ERR_INT_EN	BIT(2)
#define MII_DP83867_MICR_POL_CHNG_INT_EN	BIT(1)
#define MII_DP83867_MICR_JABBER_INT_EN		BIT(0)

/* RGMIICTL bits */
#define DP83867_RGMII_TX_CLK_DELAY_EN		BIT(1)
#define DP83867_RGMII_RX_CLK_DELAY_EN		BIT(0)

/* STRAP_STS1 bits */
#define DP83867_STRAP_STS1_RESERVED		BIT(11)

/* STRAP_STS2 bits */
#define DP83867_STRAP_STS2_CLK_SKEW_TX_MASK	GENMASK(6, 4)
#define DP83867_STRAP_STS2_CLK_SKEW_TX_SHIFT	4
#define DP83867_STRAP_STS2_CLK_SKEW_RX_MASK	GENMASK(2, 0)
#define DP83867_STRAP_STS2_CLK_SKEW_RX_SHIFT	0
#define DP83867_STRAP_STS2_CLK_SKEW_NONE	BIT(2)

/* PHY CTRL bits */
#define DP83867_PHYCR_FIFO_DEPTH_SHIFT		14
#define DP83867_PHYCR_FIFO_DEPTH_MASK		GENMASK(15, 14)
#define DP83867_PHYCR_RESERVED_MASK	BIT(11)
#define DP83867_PHYCR_FORCE_LINK_GOOD	BIT(10)
#define DP83867_MDI_CROSSOVER		5
#define DP83867_MDI_CROSSOVER_MDIX	2
#define DP83867_PHYCTRL_SGMIIEN			0x0800
#define DP83867_PHYCTRL_RXFIFO_SHIFT	12
#define DP83867_PHYCTRL_TXFIFO_SHIFT	14

/* RGMIIDCTL bits */
#define DP83867_RGMII_TX_CLK_DELAY_MAX		0xf
#define DP83867_RGMII_TX_CLK_DELAY_SHIFT	4
#define DP83867_RGMII_RX_CLK_DELAY_MAX		0xf

/* CFG2 bits */
#define MII_DP83867_CFG2_SPEEDOPT_10EN		0x0040
#define MII_DP83867_CFG2_SGMII_AUTONEGEN	0x0080
#define MII_DP83867_CFG2_SPEEDOPT_ENH		0x0100
#define MII_DP83867_CFG2_SPEEDOPT_CNT		0x0800
#define MII_DP83867_CFG2_SPEEDOPT_INTLOW	0x2000
#define MII_DP83867_CFG2_MASK			0x003F

/* User setting - can be taken from DTS */
#define DEFAULT_FIFO_DEPTH	DP83867_PHYCR_FIFO_DEPTH_4_B_NIB

/* IO_MUX_CFG bits */
#define DP83867_IO_MUX_CFG_IO_IMPEDANCE_CTRL	0x1f

#define DP83867_IO_MUX_CFG_IO_IMPEDANCE_MAX	0x0
#define DP83867_IO_MUX_CFG_IO_IMPEDANCE_MIN	0x1f
#define DP83867_IO_MUX_CFG_CLK_O_DISABLE	BIT(6)
#define DP83867_IO_MUX_CFG_CLK_O_SEL_SHIFT	8
#define DP83867_IO_MUX_CFG_CLK_O_SEL_MASK	\
		GENMASK(0x1f, DP83867_IO_MUX_CFG_CLK_O_SEL_SHIFT)

/* CFG4 bits */
#define DP83867_CFG4_PORT_MIRROR_EN		BIT(0)

/* SGMIICTL bits */
#define DP83867_SGMII_TYPE			BIT(14)

enum {
	DP83867_PORT_MIRRORING_KEEP,
	DP83867_PORT_MIRRORING_EN,
	DP83867_PORT_MIRRORING_DIS,
};

struct dp83867_private {
	u32 rx_id_delay;
	u32 tx_id_delay;
	int fifo_depth;
	int io_impedance;
	bool rxctrl_strap_quirk;
	int port_mirroring;
	bool set_clk_output;
	unsigned int clk_output_sel;
	bool sgmii_ref_clk_en;
};

static int dp83867_config_port_mirroring(struct phy_device *phydev)
{
	struct dp83867_private *dp83867 =
		(struct dp83867_private *)phydev->priv;
	u16 val;

	val = phy_read_mmd(phydev, DP83867_DEVADDR, DP83867_CFG4);

	if (dp83867->port_mirroring == DP83867_PORT_MIRRORING_EN)
		val |= DP83867_CFG4_PORT_MIRROR_EN;
	else
		val &= ~DP83867_CFG4_PORT_MIRROR_EN;

	phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_CFG4, val);

	return 0;
}

#if defined(CONFIG_DM_ETH)
/**
 * dp83867_data_init - Convenience function for setting PHY specific data
 *
 * @phydev: the phy_device struct
 */
static int dp83867_of_init(struct phy_device *phydev)
{
	struct dp83867_private *dp83867 = phydev->priv;
	ofnode node;
	int ret;

	node = phy_get_ofnode(phydev);
	if (!ofnode_valid(node))
		return -EINVAL;

	/* Optional configuration */
	ret = ofnode_read_u32(node, "ti,clk-output-sel",
			      &dp83867->clk_output_sel);
	/* If not set, keep default */
	if (!ret) {
		dp83867->set_clk_output = true;
		/* Valid values are 0 to DP83867_CLK_O_SEL_REF_CLK or
		 * DP83867_CLK_O_SEL_OFF.
		 */
		if (dp83867->clk_output_sel > DP83867_CLK_O_SEL_REF_CLK &&
		    dp83867->clk_output_sel != DP83867_CLK_O_SEL_OFF) {
			pr_debug("ti,clk-output-sel value %u out of range\n",
				 dp83867->clk_output_sel);
			return -EINVAL;
		}
	}

	if (ofnode_read_bool(node, "ti,max-output-impedance"))
		dp83867->io_impedance = DP83867_IO_MUX_CFG_IO_IMPEDANCE_MAX;
	else if (ofnode_read_bool(node, "ti,min-output-impedance"))
		dp83867->io_impedance = DP83867_IO_MUX_CFG_IO_IMPEDANCE_MIN;
	else
		dp83867->io_impedance = -EINVAL;

	if (ofnode_read_bool(node, "ti,dp83867-rxctrl-strap-quirk"))
		dp83867->rxctrl_strap_quirk = true;

	/* Existing behavior was to use default pin strapping delay in rgmii
	 * mode, but rgmii should have meant no delay.  Warn existing users.
	 */
	if (phydev->interface == PHY_INTERFACE_MODE_RGMII) {
		u16 val = phy_read_mmd(phydev, DP83867_DEVADDR,
				       DP83867_STRAP_STS2);
		u16 txskew = (val & DP83867_STRAP_STS2_CLK_SKEW_TX_MASK) >>
			     DP83867_STRAP_STS2_CLK_SKEW_TX_SHIFT;
		u16 rxskew = (val & DP83867_STRAP_STS2_CLK_SKEW_RX_MASK) >>
			     DP83867_STRAP_STS2_CLK_SKEW_RX_SHIFT;

		if (txskew != DP83867_STRAP_STS2_CLK_SKEW_NONE ||
		    rxskew != DP83867_STRAP_STS2_CLK_SKEW_NONE)
			pr_warn("PHY has delays via pin strapping, but phy-mode = 'rgmii'\n"
				"Should be 'rgmii-id' to use internal delays\n");
	}

	/* RX delay *must* be specified if internal delay of RX is used. */
	if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID ||
	    phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID) {
		ret = ofnode_read_u32(node, "ti,rx-internal-delay",
				      &dp83867->rx_id_delay);
		if (ret) {
			pr_debug("ti,rx-internal-delay must be specified\n");
			return ret;
		}
		if (dp83867->rx_id_delay > DP83867_RGMII_RX_CLK_DELAY_MAX) {
			pr_debug("ti,rx-internal-delay value of %u out of range\n",
				 dp83867->rx_id_delay);
			return -EINVAL;
		}
	}

	/* TX delay *must* be specified if internal delay of RX is used. */
	if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID ||
	    phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID) {
		ret = ofnode_read_u32(node, "ti,tx-internal-delay",
				      &dp83867->tx_id_delay);
		if (ret) {
			debug("ti,tx-internal-delay must be specified\n");
			return ret;
		}
		if (dp83867->tx_id_delay > DP83867_RGMII_TX_CLK_DELAY_MAX) {
			pr_debug("ti,tx-internal-delay value of %u out of range\n",
				 dp83867->tx_id_delay);
			return -EINVAL;
		}
	}

	dp83867->fifo_depth = ofnode_read_u32_default(node, "ti,fifo-depth",
						      DEFAULT_FIFO_DEPTH);
	if (ofnode_read_bool(node, "enet-phy-lane-swap"))
		dp83867->port_mirroring = DP83867_PORT_MIRRORING_EN;

	if (ofnode_read_bool(node, "enet-phy-lane-no-swap"))
		dp83867->port_mirroring = DP83867_PORT_MIRRORING_DIS;

	if (ofnode_read_bool(node, "ti,sgmii-ref-clock-output-enable"))
		dp83867->sgmii_ref_clk_en = true;

	return 0;
}
#else
static int dp83867_of_init(struct phy_device *phydev)
{
	struct dp83867_private *dp83867 = phydev->priv;

	dp83867->rx_id_delay = DP83867_RGMIIDCTL_2_25_NS;
	dp83867->tx_id_delay = DP83867_RGMIIDCTL_2_75_NS;
	dp83867->fifo_depth = DEFAULT_FIFO_DEPTH;
	dp83867->io_impedance = -EINVAL;

	return 0;
}
#endif

static int dp83867_config(struct phy_device *phydev)
{
	struct dp83867_private *dp83867;
	unsigned int val, delay, cfg2;
	int ret, bs;

	dp83867 = (struct dp83867_private *)phydev->priv;

	ret = dp83867_of_init(phydev);
	if (ret)
		return ret;

	/* Restart the PHY.  */
	val = phy_read(phydev, MDIO_DEVAD_NONE, DP83867_CTRL);
	phy_write(phydev, MDIO_DEVAD_NONE, DP83867_CTRL,
		  val | DP83867_SW_RESTART);

	/* Mode 1 or 2 workaround */
	if (dp83867->rxctrl_strap_quirk) {
		val = phy_read_mmd(phydev, DP83867_DEVADDR,
				   DP83867_CFG4);
		val &= ~BIT(7);
		phy_write_mmd(phydev, DP83867_DEVADDR,
			      DP83867_CFG4, val);
	}

	if (phy_interface_is_rgmii(phydev)) {
		val = phy_read(phydev, MDIO_DEVAD_NONE, MII_DP83867_PHYCTRL);
		if (val < 0)
			goto err_out;
		val &= ~DP83867_PHYCR_FIFO_DEPTH_MASK;
		val |= (dp83867->fifo_depth << DP83867_PHYCR_FIFO_DEPTH_SHIFT);

		/* Do not force link good */
		val &= ~DP83867_PHYCR_FORCE_LINK_GOOD;

		/* The code below checks if "port mirroring" N/A MODE4 has been
		 * enabled during power on bootstrap.
		 *
		 * Such N/A mode enabled by mistake can put PHY IC in some
		 * internal testing mode and disable RGMII transmission.
		 *
		 * In this particular case one needs to check STRAP_STS1
		 * register's bit 11 (marked as RESERVED).
		 */

		bs = phy_read_mmd(phydev, DP83867_DEVADDR, DP83867_STRAP_STS1);
		if (bs & DP83867_STRAP_STS1_RESERVED)
			val &= ~DP83867_PHYCR_RESERVED_MASK;

		ret = phy_write(phydev, MDIO_DEVAD_NONE,
				MII_DP83867_PHYCTRL, val);

		val = phy_read_mmd(phydev, DP83867_DEVADDR,
				   DP83867_RGMIICTL);

		val &= ~(DP83867_RGMII_TX_CLK_DELAY_EN |
			 DP83867_RGMII_RX_CLK_DELAY_EN);
		if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID)
			val |= (DP83867_RGMII_TX_CLK_DELAY_EN |
				DP83867_RGMII_RX_CLK_DELAY_EN);

		if (phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID)
			val |= DP83867_RGMII_TX_CLK_DELAY_EN;

		if (phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
			val |= DP83867_RGMII_RX_CLK_DELAY_EN;

		phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_RGMIICTL, val);

		delay = (dp83867->rx_id_delay |
			(dp83867->tx_id_delay <<
			DP83867_RGMII_TX_CLK_DELAY_SHIFT));

		phy_write_mmd(phydev, DP83867_DEVADDR,
			      DP83867_RGMIIDCTL, delay);
	}

	if (phy_interface_is_sgmii(phydev)) {
		if (dp83867->sgmii_ref_clk_en)
			phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_SGMIICTL,
				      DP83867_SGMII_TYPE);

		phy_write(phydev, MDIO_DEVAD_NONE, MII_BMCR,
			  (BMCR_ANENABLE | BMCR_FULLDPLX | BMCR_SPEED1000));

		cfg2 = phy_read(phydev, phydev->addr, MII_DP83867_CFG2);
		cfg2 &= MII_DP83867_CFG2_MASK;
		cfg2 |= (MII_DP83867_CFG2_SPEEDOPT_10EN |
			 MII_DP83867_CFG2_SGMII_AUTONEGEN |
			 MII_DP83867_CFG2_SPEEDOPT_ENH |
			 MII_DP83867_CFG2_SPEEDOPT_CNT |
			 MII_DP83867_CFG2_SPEEDOPT_INTLOW);
		phy_write(phydev, MDIO_DEVAD_NONE, MII_DP83867_CFG2, cfg2);

		phy_write_mmd(phydev, DP83867_DEVADDR,
			      DP83867_RGMIICTL, 0x0);

		phy_write(phydev, MDIO_DEVAD_NONE, MII_DP83867_PHYCTRL,
			  DP83867_PHYCTRL_SGMIIEN |
			  (DP83867_MDI_CROSSOVER_MDIX <<
			  DP83867_MDI_CROSSOVER) |
			  (dp83867->fifo_depth << DP83867_PHYCTRL_RXFIFO_SHIFT) |
			  (dp83867->fifo_depth << DP83867_PHYCTRL_TXFIFO_SHIFT));
		phy_write(phydev, MDIO_DEVAD_NONE, MII_DP83867_BISCR, 0x0);
	}

	if (dp83867->io_impedance >= 0) {
		val = phy_read_mmd(phydev,
				   DP83867_DEVADDR,
				   DP83867_IO_MUX_CFG);
		val &= ~DP83867_IO_MUX_CFG_IO_IMPEDANCE_CTRL;
		val |= dp83867->io_impedance &
		       DP83867_IO_MUX_CFG_IO_IMPEDANCE_CTRL;
		phy_write_mmd(phydev, DP83867_DEVADDR,
			      DP83867_IO_MUX_CFG, val);
	}

	if (dp83867->port_mirroring != DP83867_PORT_MIRRORING_KEEP)
		dp83867_config_port_mirroring(phydev);

	/* Clock output selection if muxing property is set */
	if (dp83867->set_clk_output) {
		val = phy_read_mmd(phydev, DP83867_DEVADDR,
				   DP83867_IO_MUX_CFG);

		if (dp83867->clk_output_sel == DP83867_CLK_O_SEL_OFF) {
			val |= DP83867_IO_MUX_CFG_CLK_O_DISABLE;
		} else {
			val &= ~(DP83867_IO_MUX_CFG_CLK_O_SEL_MASK |
				 DP83867_IO_MUX_CFG_CLK_O_DISABLE);
			val |= dp83867->clk_output_sel <<
			       DP83867_IO_MUX_CFG_CLK_O_SEL_SHIFT;
		}
		phy_write_mmd(phydev, DP83867_DEVADDR,
			      DP83867_IO_MUX_CFG, val);
	}

	genphy_config_aneg(phydev);
	return 0;

err_out:
	return ret;
}

static int dp83867_probe(struct phy_device *phydev)
{
	struct dp83867_private *dp83867;

	dp83867 = kzalloc(sizeof(*dp83867), GFP_KERNEL);
	if (!dp83867)
		return -ENOMEM;

	phydev->priv = dp83867;
	return 0;
}

static struct phy_driver DP83867_driver = {
	.name = "TI DP83867",
	.uid = 0x2000a231,
	.mask = 0xfffffff0,
	.features = PHY_GBIT_FEATURES,
	.probe = dp83867_probe,
	.config = &dp83867_config,
	.startup = &genphy_startup,
	.shutdown = &genphy_shutdown,
};

int phy_ti_init(void)
{
	phy_register(&DP83867_driver);
	return 0;
}
