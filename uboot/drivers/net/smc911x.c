// SPDX-License-Identifier: GPL-2.0+
/*
 * SMSC LAN9[12]1[567] Network driver
 *
 * (c) 2007 Pengutronix, Sascha Hauer <s.hauer@pengutronix.de>
 */

#include <common.h>
#include <command.h>
#include <malloc.h>
#include <net.h>
#include <miiphy.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/types.h>

#include "smc911x.h"

struct chip_id {
	u16 id;
	char *name;
};

struct smc911x_priv {
#ifndef CONFIG_DM_ETH
	struct eth_device	dev;
#endif
	phys_addr_t		iobase;
	const struct chip_id	*chipid;
	unsigned char		enetaddr[6];
};

static const struct chip_id chip_ids[] =  {
	{ CHIP_89218, "LAN89218" },
	{ CHIP_9115, "LAN9115" },
	{ CHIP_9116, "LAN9116" },
	{ CHIP_9117, "LAN9117" },
	{ CHIP_9118, "LAN9118" },
	{ CHIP_9211, "LAN9211" },
	{ CHIP_9215, "LAN9215" },
	{ CHIP_9216, "LAN9216" },
	{ CHIP_9217, "LAN9217" },
	{ CHIP_9218, "LAN9218" },
	{ CHIP_9220, "LAN9220" },
	{ CHIP_9221, "LAN9221" },
	{ 0, NULL },
};

#define DRIVERNAME "smc911x"

#if defined (CONFIG_SMC911X_32_BIT) && \
	defined (CONFIG_SMC911X_16_BIT)
#error "SMC911X: Only one of CONFIG_SMC911X_32_BIT and \
	CONFIG_SMC911X_16_BIT shall be set"
#endif

#if defined (CONFIG_SMC911X_32_BIT)
static u32 smc911x_reg_read(struct smc911x_priv *priv, u32 offset)
{
	return readl(priv->iobase + offset);
}

static void smc911x_reg_write(struct smc911x_priv *priv, u32 offset, u32 val)
{
	writel(val, priv->iobase + offset);
}
#elif defined (CONFIG_SMC911X_16_BIT)
static u32 smc911x_reg_read(struct smc911x_priv *priv, u32 offset)
{
	return (readw(priv->iobase + offset) & 0xffff) |
	       (readw(priv->iobase + offset + 2) << 16);
}
static void smc911x_reg_write(struct smc911x_priv *priv, u32 offset, u32 val)
{
	writew(val & 0xffff, priv->iobase + offset);
	writew(val >> 16, priv->iobase + offset + 2);
}
#else
#error "SMC911X: undefined bus width"
#endif /* CONFIG_SMC911X_16_BIT */

static u32 smc911x_get_mac_csr(struct smc911x_priv *priv, u8 reg)
{
	while (smc911x_reg_read(priv, MAC_CSR_CMD) & MAC_CSR_CMD_CSR_BUSY)
		;
	smc911x_reg_write(priv, MAC_CSR_CMD,
			MAC_CSR_CMD_CSR_BUSY | MAC_CSR_CMD_R_NOT_W | reg);
	while (smc911x_reg_read(priv, MAC_CSR_CMD) & MAC_CSR_CMD_CSR_BUSY)
		;

	return smc911x_reg_read(priv, MAC_CSR_DATA);
}

static void smc911x_set_mac_csr(struct smc911x_priv *priv, u8 reg, u32 data)
{
	while (smc911x_reg_read(priv, MAC_CSR_CMD) & MAC_CSR_CMD_CSR_BUSY)
		;
	smc911x_reg_write(priv, MAC_CSR_DATA, data);
	smc911x_reg_write(priv, MAC_CSR_CMD, MAC_CSR_CMD_CSR_BUSY | reg);
	while (smc911x_reg_read(priv, MAC_CSR_CMD) & MAC_CSR_CMD_CSR_BUSY)
		;
}

static int smc911x_detect_chip(struct smc911x_priv *priv)
{
	unsigned long val, i;

	val = smc911x_reg_read(priv, BYTE_TEST);
	if (val == 0xffffffff) {
		/* Special case -- no chip present */
		return -1;
	} else if (val != 0x87654321) {
		printf(DRIVERNAME ": Invalid chip endian 0x%08lx\n", val);
		return -1;
	}

	val = smc911x_reg_read(priv, ID_REV) >> 16;
	for (i = 0; chip_ids[i].id != 0; i++) {
		if (chip_ids[i].id == val) break;
	}
	if (!chip_ids[i].id) {
		printf(DRIVERNAME ": Unknown chip ID %04lx\n", val);
		return -1;
	}

	priv->chipid = &chip_ids[i];

	return 0;
}

static void smc911x_reset(struct smc911x_priv *priv)
{
	int timeout;

	/*
	 *  Take out of PM setting first
	 *  Device is already wake up if PMT_CTRL_READY bit is set
	 */
	if ((smc911x_reg_read(priv, PMT_CTRL) & PMT_CTRL_READY) == 0) {
		/* Write to the bytetest will take out of powerdown */
		smc911x_reg_write(priv, BYTE_TEST, 0x0);

		timeout = 10;

		while (timeout-- &&
			!(smc911x_reg_read(priv, PMT_CTRL) & PMT_CTRL_READY))
			udelay(10);
		if (timeout < 0) {
			printf(DRIVERNAME
				": timeout waiting for PM restore\n");
			return;
		}
	}

	/* Disable interrupts */
	smc911x_reg_write(priv, INT_EN, 0);

	smc911x_reg_write(priv, HW_CFG, HW_CFG_SRST);

	timeout = 1000;
	while (timeout-- && smc911x_reg_read(priv, E2P_CMD) & E2P_CMD_EPC_BUSY)
		udelay(10);

	if (timeout < 0) {
		printf(DRIVERNAME ": reset timeout\n");
		return;
	}

	/* Reset the FIFO level and flow control settings */
	smc911x_set_mac_csr(priv, FLOW, FLOW_FCPT | FLOW_FCEN);
	smc911x_reg_write(priv, AFC_CFG, 0x0050287F);

	/* Set to LED outputs */
	smc911x_reg_write(priv, GPIO_CFG, 0x70070000);
}

static void smc911x_handle_mac_address(struct smc911x_priv *priv)
{
	unsigned long addrh, addrl;
	unsigned char *m = priv->enetaddr;

	addrl = m[0] | (m[1] << 8) | (m[2] << 16) | (m[3] << 24);
	addrh = m[4] | (m[5] << 8);
	smc911x_set_mac_csr(priv, ADDRL, addrl);
	smc911x_set_mac_csr(priv, ADDRH, addrh);

	printf(DRIVERNAME ": MAC %pM\n", m);
}

static int smc911x_eth_phy_read(struct smc911x_priv *priv,
				u8 phy, u8 reg, u16 *val)
{
	while (smc911x_get_mac_csr(priv, MII_ACC) & MII_ACC_MII_BUSY)
		;

	smc911x_set_mac_csr(priv, MII_ACC, phy << 11 | reg << 6 |
				MII_ACC_MII_BUSY);

	while (smc911x_get_mac_csr(priv, MII_ACC) & MII_ACC_MII_BUSY)
		;

	*val = smc911x_get_mac_csr(priv, MII_DATA);

	return 0;
}

static int smc911x_eth_phy_write(struct smc911x_priv *priv,
				u8 phy, u8 reg, u16  val)
{
	while (smc911x_get_mac_csr(priv, MII_ACC) & MII_ACC_MII_BUSY)
		;

	smc911x_set_mac_csr(priv, MII_DATA, val);
	smc911x_set_mac_csr(priv, MII_ACC,
		phy << 11 | reg << 6 | MII_ACC_MII_BUSY | MII_ACC_MII_WRITE);

	while (smc911x_get_mac_csr(priv, MII_ACC) & MII_ACC_MII_BUSY)
		;
	return 0;
}

static int smc911x_phy_reset(struct smc911x_priv *priv)
{
	u32 reg;

	reg = smc911x_reg_read(priv, PMT_CTRL);
	reg &= ~0xfffff030;
	reg |= PMT_CTRL_PHY_RST;
	smc911x_reg_write(priv, PMT_CTRL, reg);

	mdelay(100);

	return 0;
}

static void smc911x_phy_configure(struct smc911x_priv *priv)
{
	int timeout;
	u16 status;

	smc911x_phy_reset(priv);

	smc911x_eth_phy_write(priv, 1, MII_BMCR, BMCR_RESET);
	mdelay(1);
	smc911x_eth_phy_write(priv, 1, MII_ADVERTISE, 0x01e1);
	smc911x_eth_phy_write(priv, 1, MII_BMCR, BMCR_ANENABLE |
				BMCR_ANRESTART);

	timeout = 5000;
	do {
		mdelay(1);
		if ((timeout--) == 0)
			goto err_out;

		if (smc911x_eth_phy_read(priv, 1, MII_BMSR, &status) != 0)
			goto err_out;
	} while (!(status & BMSR_LSTATUS));

	printf(DRIVERNAME ": phy initialized\n");

	return;

err_out:
	printf(DRIVERNAME ": autonegotiation timed out\n");
}

static void smc911x_enable(struct smc911x_priv *priv)
{
	/* Enable TX */
	smc911x_reg_write(priv, HW_CFG, 8 << 16 | HW_CFG_SF);

	smc911x_reg_write(priv, GPT_CFG, GPT_CFG_TIMER_EN | 10000);

	smc911x_reg_write(priv, TX_CFG, TX_CFG_TX_ON);

	/* no padding to start of packets */
	smc911x_reg_write(priv, RX_CFG, 0);

	smc911x_set_mac_csr(priv, MAC_CR, MAC_CR_TXEN | MAC_CR_RXEN |
				MAC_CR_HBDIS);
}

static int smc911x_init_common(struct smc911x_priv *priv)
{
	const struct chip_id *id = priv->chipid;

	printf(DRIVERNAME ": detected %s controller\n", id->name);

	smc911x_reset(priv);

	/* Configure the PHY, initialize the link state */
	smc911x_phy_configure(priv);

	smc911x_handle_mac_address(priv);

	/* Turn on Tx + Rx */
	smc911x_enable(priv);

	return 0;
}

static int smc911x_send_common(struct smc911x_priv *priv,
			       void *packet, int length)
{
	u32 *data = (u32*)packet;
	u32 tmplen;
	u32 status;

	smc911x_reg_write(priv, TX_DATA_FIFO, TX_CMD_A_INT_FIRST_SEG |
				TX_CMD_A_INT_LAST_SEG | length);
	smc911x_reg_write(priv, TX_DATA_FIFO, length);

	tmplen = (length + 3) / 4;

	while (tmplen--)
		smc911x_reg_write(priv, TX_DATA_FIFO, *data++);

	/* wait for transmission */
	while (!((smc911x_reg_read(priv, TX_FIFO_INF) &
					TX_FIFO_INF_TSUSED) >> 16));

	/* get status. Ignore 'no carrier' error, it has no meaning for
	 * full duplex operation
	 */
	status = smc911x_reg_read(priv, TX_STATUS_FIFO) &
			(TX_STS_LOC | TX_STS_LATE_COLL | TX_STS_MANY_COLL |
			TX_STS_MANY_DEFER | TX_STS_UNDERRUN);

	if (!status)
		return 0;

	printf(DRIVERNAME ": failed to send packet: %s%s%s%s%s\n",
		status & TX_STS_LOC ? "TX_STS_LOC " : "",
		status & TX_STS_LATE_COLL ? "TX_STS_LATE_COLL " : "",
		status & TX_STS_MANY_COLL ? "TX_STS_MANY_COLL " : "",
		status & TX_STS_MANY_DEFER ? "TX_STS_MANY_DEFER " : "",
		status & TX_STS_UNDERRUN ? "TX_STS_UNDERRUN" : "");

	return -1;
}

static void smc911x_halt_common(struct smc911x_priv *priv)
{
	smc911x_reset(priv);
	smc911x_handle_mac_address(priv);
}

static int smc911x_recv_common(struct smc911x_priv *priv, u32 *data)
{
	u32 pktlen, tmplen;
	u32 status;

	status = smc911x_reg_read(priv, RX_FIFO_INF);
	if (!(status & RX_FIFO_INF_RXSUSED))
		return 0;

	status = smc911x_reg_read(priv, RX_STATUS_FIFO);
	pktlen = (status & RX_STS_PKT_LEN) >> 16;

	smc911x_reg_write(priv, RX_CFG, 0);

	tmplen = (pktlen + 3) / 4;
	while (tmplen--)
		*data++ = smc911x_reg_read(priv, RX_DATA_FIFO);

	if (status & RX_STS_ES) {
		printf(DRIVERNAME
			": dropped bad packet. Status: 0x%08x\n",
			status);
		return 0;
	}

	return pktlen;
}

#ifndef CONFIG_DM_ETH

#if defined(CONFIG_MII) || defined(CONFIG_CMD_MII)
/* wrapper for smc911x_eth_phy_read */
static int smc911x_miiphy_read(struct mii_dev *bus, int phy, int devad,
			       int reg)
{
	struct eth_device *dev = eth_get_dev_by_name(bus->name);
	struct smc911x_priv *priv = container_of(dev, struct smc911x_priv, dev);
	u16 val = 0;
	int ret;

	if (!dev || !priv)
		return -ENODEV;

	ret = smc911x_eth_phy_read(priv, phy, reg, &val);
	if (ret < 0)
		return ret;

	return val;
}

/* wrapper for smc911x_eth_phy_write */
static int smc911x_miiphy_write(struct mii_dev *bus, int phy, int devad,
				int reg, u16 val)
{
	struct eth_device *dev = eth_get_dev_by_name(bus->name);
	struct smc911x_priv *priv = container_of(dev, struct smc911x_priv, dev);

	if (!dev || !priv)
		return -ENODEV;

	return smc911x_eth_phy_write(priv, phy, reg, val);
}

static int smc911x_initialize_mii(struct smc911x_priv *priv)
{
	struct mii_dev *mdiodev = mdio_alloc();
	int ret;

	if (!mdiodev)
		return -ENOMEM;

	strncpy(mdiodev->name, priv->dev.name, MDIO_NAME_LEN);
	mdiodev->read = smc911x_miiphy_read;
	mdiodev->write = smc911x_miiphy_write;

	ret = mdio_register(mdiodev);
	if (ret < 0) {
		mdio_free(mdiodev);
		return ret;
	}

	return 0;
}
#else
static int smc911x_initialize_mii(struct smc911x_priv *priv)
{
	return 0;
}
#endif

static int smc911x_init(struct eth_device *dev, bd_t *bd)
{
	struct smc911x_priv *priv = container_of(dev, struct smc911x_priv, dev);

	return smc911x_init_common(priv);
}

static void smc911x_halt(struct eth_device *dev)
{
	struct smc911x_priv *priv = container_of(dev, struct smc911x_priv, dev);

	smc911x_halt_common(priv);
}

static int smc911x_send(struct eth_device *dev, void *packet, int length)
{
	struct smc911x_priv *priv = container_of(dev, struct smc911x_priv, dev);

	return smc911x_send_common(priv, packet, length);
}

static int smc911x_recv(struct eth_device *dev)
{
	struct smc911x_priv *priv = container_of(dev, struct smc911x_priv, dev);
	u32 *data = (u32 *)net_rx_packets[0];
	int ret;

	ret = smc911x_recv_common(priv, data);
	if (ret)
		net_process_received_packet(net_rx_packets[0], ret);

	return ret;
}

int smc911x_initialize(u8 dev_num, int base_addr)
{
	unsigned long addrl, addrh;
	struct smc911x_priv *priv;
	int ret;

	priv = calloc(1, sizeof(*priv));
	if (!priv)
		return -ENOMEM;

	priv->iobase = base_addr;
	priv->dev.iobase = base_addr;

	/* Try to detect chip. Will fail if not present. */
	ret = smc911x_detect_chip(priv);
	if (ret) {
		ret = 0;	/* Card not detected is not an error */
		goto err_detect;
	}

	addrh = smc911x_get_mac_csr(priv, ADDRH);
	addrl = smc911x_get_mac_csr(priv, ADDRL);
	if (!(addrl == 0xffffffff && addrh == 0x0000ffff)) {
		/* address is obtained from optional eeprom */
		priv->enetaddr[0] = addrl;
		priv->enetaddr[1] = addrl >>  8;
		priv->enetaddr[2] = addrl >> 16;
		priv->enetaddr[3] = addrl >> 24;
		priv->enetaddr[4] = addrh;
		priv->enetaddr[5] = addrh >> 8;
		memcpy(priv->dev.enetaddr, priv->enetaddr, 6);
	}

	priv->dev.init = smc911x_init;
	priv->dev.halt = smc911x_halt;
	priv->dev.send = smc911x_send;
	priv->dev.recv = smc911x_recv;
	sprintf(priv->dev.name, "%s-%hu", DRIVERNAME, dev_num);

	eth_register(&priv->dev);

	ret = smc911x_initialize_mii(priv);
	if (ret)
		goto err_mii;

	return 1;

err_mii:
	eth_unregister(&priv->dev);
err_detect:
	free(priv);
	return ret;
}

#else	/* ifdef CONFIG_DM_ETH */

static int smc911x_start(struct udevice *dev)
{
	struct eth_pdata *plat = dev_get_platdata(dev);
	struct smc911x_priv *priv = dev_get_priv(dev);

	memcpy(priv->enetaddr, plat->enetaddr, sizeof(plat->enetaddr));

	return smc911x_init_common(priv);
}

static void smc911x_stop(struct udevice *dev)
{
	struct smc911x_priv *priv = dev_get_priv(dev);

	smc911x_halt_common(priv);
}

static int smc911x_send(struct udevice *dev, void *packet, int length)
{
	struct smc911x_priv *priv = dev_get_priv(dev);
	int ret;

	ret = smc911x_send_common(priv, packet, length);

	return ret ? 0 : -ETIMEDOUT;
}

static int smc911x_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct smc911x_priv *priv = dev_get_priv(dev);
	u32 *data = (u32 *)net_rx_packets[0];
	int ret;

	ret = smc911x_recv_common(priv, data);
	if (ret)
		*packetp = (void *)data;

	return ret ? ret : -EAGAIN;
}

static int smc911x_bind(struct udevice *dev)
{
	return device_set_name(dev, dev->name);
}

static int smc911x_probe(struct udevice *dev)
{
	struct smc911x_priv *priv = dev_get_priv(dev);
	unsigned long addrh, addrl;
	int ret;

	/* Try to detect chip. Will fail if not present. */
	ret = smc911x_detect_chip(priv);
	if (ret)
		return ret;

	addrh = smc911x_get_mac_csr(priv, ADDRH);
	addrl = smc911x_get_mac_csr(priv, ADDRL);
	if (!(addrl == 0xffffffff && addrh == 0x0000ffff)) {
		/* address is obtained from optional eeprom */
		priv->enetaddr[0] = addrl;
		priv->enetaddr[1] = addrl >>  8;
		priv->enetaddr[2] = addrl >> 16;
		priv->enetaddr[3] = addrl >> 24;
		priv->enetaddr[4] = addrh;
		priv->enetaddr[5] = addrh >> 8;
	}

	return 0;
}

static int smc911x_ofdata_to_platdata(struct udevice *dev)
{
	struct smc911x_priv *priv = dev_get_priv(dev);
	struct eth_pdata *pdata = dev_get_platdata(dev);

	pdata->iobase = devfdt_get_addr(dev);
	priv->iobase = pdata->iobase;

	return 0;
}

static const struct eth_ops smc911x_ops = {
	.start	= smc911x_start,
	.send	= smc911x_send,
	.recv	= smc911x_recv,
	.stop	= smc911x_stop,
};

static const struct udevice_id smc911x_ids[] = {
	{ .compatible = "smsc,lan9115" },
	{ }
};

U_BOOT_DRIVER(smc911x) = {
	.name		= "eth_smc911x",
	.id		= UCLASS_ETH,
	.of_match	= smc911x_ids,
	.bind		= smc911x_bind,
	.ofdata_to_platdata = smc911x_ofdata_to_platdata,
	.probe		= smc911x_probe,
	.ops		= &smc911x_ops,
	.priv_auto_alloc_size = sizeof(struct smc911x_priv),
	.platdata_auto_alloc_size = sizeof(struct eth_pdata),
	.flags		= DM_FLAG_ALLOC_PRIV_DMA,
};
#endif
