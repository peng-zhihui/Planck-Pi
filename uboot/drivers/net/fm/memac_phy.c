// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2012 Freescale Semiconductor, Inc.
 *	Andy Fleming <afleming@gmail.com>
 *	Roy Zang <tie-fei.zang@freescale.com>
 * Some part is taken from tsec.c
 */
#include <common.h>
#include <miiphy.h>
#include <phy.h>
#include <asm/io.h>
#include <fsl_memac.h>
#include <fm_eth.h>

#ifdef CONFIG_SYS_MEMAC_LITTLE_ENDIAN
#define memac_out_32(a, v)	out_le32(a, v)
#define memac_clrbits_32(a, v)	clrbits_le32(a, v)
#define memac_setbits_32(a, v)	setbits_le32(a, v)
#else
#define memac_out_32(a, v)	out_be32(a, v)
#define memac_clrbits_32(a, v)	clrbits_be32(a, v)
#define memac_setbits_32(a, v)	setbits_be32(a, v)
#endif

#ifdef CONFIG_DM_ETH
struct fm_mdio_priv {
	struct memac_mdio_controller *regs;
};
#endif

static u32 memac_in_32(u32 *reg)
{
#ifdef CONFIG_SYS_MEMAC_LITTLE_ENDIAN
	return in_le32(reg);
#else
	return in_be32(reg);
#endif
}

/*
 * Write value to the PHY for this device to the register at regnum, waiting
 * until the write is done before it returns.  All PHY configuration has to be
 * done through the TSEC1 MIIM regs
 */
int memac_mdio_write(struct mii_dev *bus, int port_addr, int dev_addr,
			int regnum, u16 value)
{
	struct memac_mdio_controller *regs;
	u32 mdio_ctl;
	u32 c45 = 1; /* Default to 10G interface */

#ifndef CONFIG_DM_ETH
	regs = bus->priv;
#else
	struct fm_mdio_priv *priv;

	if (!bus->priv)
		return -EINVAL;
	priv = dev_get_priv(bus->priv);
	regs = priv->regs;
	debug("memac_mdio_write(regs %p, port %d, dev %d, reg %d, val %#x)\n",
	      regs, port_addr, dev_addr, regnum, value);
#endif

	if (dev_addr == MDIO_DEVAD_NONE) {
		c45 = 0; /* clause 22 */
		dev_addr = regnum & 0x1f;
		memac_clrbits_32(&regs->mdio_stat, MDIO_STAT_ENC);
	} else
		memac_setbits_32(&regs->mdio_stat, MDIO_STAT_ENC);

	/* Wait till the bus is free */
	while ((memac_in_32(&regs->mdio_stat)) & MDIO_STAT_BSY)
		;

	/* Set the port and dev addr */
	mdio_ctl = MDIO_CTL_PORT_ADDR(port_addr) | MDIO_CTL_DEV_ADDR(dev_addr);
	memac_out_32(&regs->mdio_ctl, mdio_ctl);

	/* Set the register address */
	if (c45)
		memac_out_32(&regs->mdio_addr, regnum & 0xffff);

	/* Wait till the bus is free */
	while ((memac_in_32(&regs->mdio_stat)) & MDIO_STAT_BSY)
		;

	/* Write the value to the register */
	memac_out_32(&regs->mdio_data, MDIO_DATA(value));

	/* Wait till the MDIO write is complete */
	while ((memac_in_32(&regs->mdio_data)) & MDIO_DATA_BSY)
		;

	return 0;
}

/*
 * Reads from register regnum in the PHY for device dev, returning the value.
 * Clears miimcom first.  All PHY configuration has to be done through the
 * TSEC1 MIIM regs
 */
int memac_mdio_read(struct mii_dev *bus, int port_addr, int dev_addr,
			int regnum)
{
	struct memac_mdio_controller *regs;
	u32 mdio_ctl;
	u32 c45 = 1;

#ifndef CONFIG_DM_ETH
	regs = bus->priv;
#else
	struct fm_mdio_priv *priv;

	if (!bus->priv)
		return -EINVAL;
	priv = dev_get_priv(bus->priv);
	regs = priv->regs;
#endif

	if (dev_addr == MDIO_DEVAD_NONE) {
#ifndef CONFIG_DM_ETH
		if (!strcmp(bus->name, DEFAULT_FM_TGEC_MDIO_NAME))
			return 0xffff;
#endif
		c45 = 0; /* clause 22 */
		dev_addr = regnum & 0x1f;
		memac_clrbits_32(&regs->mdio_stat, MDIO_STAT_ENC);
	} else
		memac_setbits_32(&regs->mdio_stat, MDIO_STAT_ENC);

	/* Wait till the bus is free */
	while ((memac_in_32(&regs->mdio_stat)) & MDIO_STAT_BSY)
		;

	/* Set the Port and Device Addrs */
	mdio_ctl = MDIO_CTL_PORT_ADDR(port_addr) | MDIO_CTL_DEV_ADDR(dev_addr);
	memac_out_32(&regs->mdio_ctl, mdio_ctl);

	/* Set the register address */
	if (c45)
		memac_out_32(&regs->mdio_addr, regnum & 0xffff);

	/* Wait till the bus is free */
	while ((memac_in_32(&regs->mdio_stat)) & MDIO_STAT_BSY)
		;

	/* Initiate the read */
	mdio_ctl |= MDIO_CTL_READ;
	memac_out_32(&regs->mdio_ctl, mdio_ctl);

	/* Wait till the MDIO write is complete */
	while ((memac_in_32(&regs->mdio_data)) & MDIO_DATA_BSY)
		;

	/* Return all Fs if nothing was there */
	if (memac_in_32(&regs->mdio_stat) & MDIO_STAT_RD_ER)
		return 0xffff;

	return memac_in_32(&regs->mdio_data) & 0xffff;
}

int memac_mdio_reset(struct mii_dev *bus)
{
	return 0;
}

#ifndef CONFIG_DM_ETH
int fm_memac_mdio_init(bd_t *bis, struct memac_mdio_info *info)
{
	struct mii_dev *bus = mdio_alloc();

	if (!bus) {
		printf("Failed to allocate FM TGEC MDIO bus\n");
		return -1;
	}

	bus->read = memac_mdio_read;
	bus->write = memac_mdio_write;
	bus->reset = memac_mdio_reset;
	strcpy(bus->name, info->name);

	bus->priv = info->regs;

	/*
	 * On some platforms like B4860, default value of MDIO_CLK_DIV bits
	 * in mdio_stat(mdio_cfg) register generates MDIO clock too high
	 * (much higher than 2.5MHz), violating the IEEE specs.
	 * On other platforms like T1040, default value of MDIO_CLK_DIV bits
	 * is zero, so MDIO clock is disabled.
	 * So, for proper functioning of MDIO, MDIO_CLK_DIV bits needs to
	 * be properly initialized.
	 * NEG bit default should be '1' as per FMAN-v3 RM, but on platform
	 * like T2080QDS, this bit default is '0', which leads to MDIO failure
	 * on XAUI PHY, so set this bit definitely.
	 */
	memac_setbits_32(
		&((struct memac_mdio_controller *)info->regs)->mdio_stat,
		MDIO_STAT_CLKDIV(258) | MDIO_STAT_NEG);

	return mdio_register(bus);
}

#else /* CONFIG_DM_ETH */
#if defined(CONFIG_PHYLIB) && defined(CONFIG_DM_MDIO)
static int fm_mdio_read(struct udevice *dev, int addr, int devad, int reg)
{
	struct mdio_perdev_priv *pdata = (dev) ? dev_get_uclass_priv(dev) :
						 NULL;

	if (pdata && pdata->mii_bus)
		return memac_mdio_read(pdata->mii_bus, addr, devad, reg);

	return -1;
}

static int fm_mdio_write(struct udevice *dev, int addr, int devad, int reg,
			 u16 val)
{
	struct mdio_perdev_priv *pdata = (dev) ? dev_get_uclass_priv(dev) :
						 NULL;

	if (pdata && pdata->mii_bus)
		return memac_mdio_write(pdata->mii_bus, addr, devad, reg, val);

	return -1;
}

static int fm_mdio_reset(struct udevice *dev)
{
	struct mdio_perdev_priv *pdata = (dev) ? dev_get_uclass_priv(dev) :
						 NULL;

	if (pdata && pdata->mii_bus)
		return memac_mdio_reset(pdata->mii_bus);

	return -1;
}

static const struct mdio_ops fm_mdio_ops = {
	.read = fm_mdio_read,
	.write = fm_mdio_write,
	.reset = fm_mdio_reset,
};

static const struct udevice_id fm_mdio_ids[] = {
	{ .compatible = "fsl,fman-memac-mdio" },
	{}
};

static int fm_mdio_probe(struct udevice *dev)
{
	struct fm_mdio_priv *priv = (dev) ? dev_get_priv(dev) : NULL;
	struct mdio_perdev_priv *pdata = (dev) ? dev_get_uclass_priv(dev) :
						 NULL;

	if (!dev) {
		printf("%s dev = NULL\n", __func__);
		return -1;
	}
	if (!priv) {
		printf("dev_get_priv(dev %p) = NULL\n", dev);
		return -1;
	}
	priv->regs = (void *)(uintptr_t)dev_read_addr(dev);
	debug("%s priv %p @ regs %p, pdata %p\n", __func__,
	      priv, priv->regs, pdata);

	/*
	 * On some platforms like B4860, default value of MDIO_CLK_DIV bits
	 * in mdio_stat(mdio_cfg) register generates MDIO clock too high
	 * (much higher than 2.5MHz), violating the IEEE specs.
	 * On other platforms like T1040, default value of MDIO_CLK_DIV bits
	 * is zero, so MDIO clock is disabled.
	 * So, for proper functioning of MDIO, MDIO_CLK_DIV bits needs to
	 * be properly initialized.
	 * The default NEG bit should be '1' as per FMANv3 RM, but on platforms
	 * like T2080QDS, this bit default is '0', which leads to MDIO failure
	 * on XAUI PHY, so set this bit definitely.
	 */
	if (priv && priv->regs && priv->regs->mdio_stat)
		memac_setbits_32(&priv->regs->mdio_stat,
				 MDIO_STAT_CLKDIV(258) | MDIO_STAT_NEG);

	return 0;
}

static int fm_mdio_remove(struct udevice *dev)
{
	return 0;
}

U_BOOT_DRIVER(fman_mdio) = {
	.name = "fman_mdio",
	.id = UCLASS_MDIO,
	.of_match = fm_mdio_ids,
	.probe = fm_mdio_probe,
	.remove = fm_mdio_remove,
	.ops = &fm_mdio_ops,
	.priv_auto_alloc_size = sizeof(struct fm_mdio_priv),
	.platdata_auto_alloc_size = sizeof(struct mdio_perdev_priv),
};
#endif /* CONFIG_PHYLIB && CONFIG_DM_MDIO */
#endif /* CONFIG_DM_ETH */
