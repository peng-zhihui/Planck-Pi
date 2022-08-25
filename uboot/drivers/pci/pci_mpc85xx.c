// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2019
 * Heiko Schocher, DENX Software Engineering, hs@denx.de.
 *
 */
#include <common.h>
#include <asm/bitops.h>
#include <asm/cpm_85xx.h>
#include <pci.h>
#include <dm.h>
#include <asm/fsl_law.h>

struct mpc85xx_pci_priv {
	void __iomem		*cfg_addr;
	void __iomem		*cfg_data;
};

static int mpc85xx_pci_dm_read_config(const struct udevice *dev, pci_dev_t bdf,
				      uint offset, ulong *value,
				      enum pci_size_t size)
{
	struct mpc85xx_pci_priv *priv = dev_get_priv(dev);
	u32 addr;

	addr = bdf | (offset & 0xfc) | ((offset & 0xf00) << 16) | 0x80000000;
	out_be32(priv->cfg_addr, addr);
	sync();
	*value = pci_conv_32_to_size(in_le32(priv->cfg_data), offset, size);

	return 0;
}

static int mpc85xx_pci_dm_write_config(struct udevice *dev, pci_dev_t bdf,
				       uint offset, ulong value,
				       enum pci_size_t size)
{
	struct mpc85xx_pci_priv *priv = dev_get_priv(dev);
	u32 addr;

	addr = bdf | (offset & 0xfc) | ((offset & 0xf00) << 16) | 0x80000000;
	out_be32(priv->cfg_addr, addr);
	sync();
	out_le32(priv->cfg_data, pci_conv_size_to_32(0, value, offset, size));

	return 0;
}

static int
mpc85xx_pci_dm_setup_laws(struct pci_region *io, struct pci_region *mem,
			  struct pci_region *pre)
{
	/*
	 * Unfortunately we have defines for this addresse,
	 * as we have to setup the TLB, and at this stage
	 * we have no access to DT ... may we check here
	 * if the value in the define is the same ?
	 */
	if (mem)
		set_next_law(mem->phys_start, law_size_bits(mem->size),
			     LAW_TRGT_IF_PCI);
	if (io)
		set_next_law(io->phys_start, law_size_bits(io->size),
			     LAW_TRGT_IF_PCI);
	if (pre)
		set_next_law(pre->phys_start, law_size_bits(pre->size),
			     LAW_TRGT_IF_PCI);

	return 0;
}

static int mpc85xx_pci_dm_probe(struct udevice *dev)
{
	struct mpc85xx_pci_priv *priv = dev_get_priv(dev);
	struct pci_region *io;
	struct pci_region *mem;
	struct pci_region *pre;
	int count;
	ccsr_pcix_t *pcix;

	count = pci_get_regions(dev, &io, &mem, &pre);
	if (count != 2) {
		printf("%s: wrong count of regions %d only 2 allowed\n",
		       __func__, count);
		return -EINVAL;
	}

	mpc85xx_pci_dm_setup_laws(io, mem, pre);

	pcix = priv->cfg_addr;
	/* BAR 1: memory */
	out_be32(&pcix->potar1, (mem->bus_start >> 12) & 0x000fffff);
	out_be32(&pcix->potear1, 0);
	out_be32(&pcix->powbar1, (mem->phys_start >> 12) & 0x000fffff);
	out_be32(&pcix->powbear1, 0);
	out_be32(&pcix->powar1, (POWAR_EN | POWAR_MEM_READ |
		 POWAR_MEM_WRITE | (__ilog2(mem->size) - 1)));

	/* BAR 1: IO */
	out_be32(&pcix->potar2, (io->bus_start >> 12) & 0x000fffff);
	out_be32(&pcix->potear2, 0);
	out_be32(&pcix->powbar2, (io->phys_start >> 12) & 0x000fffff);
	out_be32(&pcix->powbear2, 0);
	out_be32(&pcix->powar2, (POWAR_EN | POWAR_IO_READ |
		 POWAR_IO_WRITE | (__ilog2(io->size) - 1)));

	out_be32(&pcix->pitar1, 0);
	out_be32(&pcix->piwbar1, 0);
	out_be32(&pcix->piwar1, (PIWAR_EN | PIWAR_PF | PIWAR_LOCAL |
		 PIWAR_READ_SNOOP | PIWAR_WRITE_SNOOP | PIWAR_MEM_2G));

	out_be32(&pcix->powar3, 0);
	out_be32(&pcix->powar4, 0);
	out_be32(&pcix->piwar2, 0);
	out_be32(&pcix->piwar3, 0);

	return 0;
}

static int mpc85xx_pci_dm_remove(struct udevice *dev)
{
	return 0;
}

static int mpc85xx_pci_ofdata_to_platdata(struct udevice *dev)
{
	struct mpc85xx_pci_priv *priv = dev_get_priv(dev);
	fdt_addr_t addr;

	addr = devfdt_get_addr_index(dev, 0);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;
	priv->cfg_addr = (void __iomem *)addr;
	addr += 4;
	priv->cfg_data = (void __iomem *)addr;

	return 0;
}

static const struct dm_pci_ops mpc85xx_pci_ops = {
	.read_config	= mpc85xx_pci_dm_read_config,
	.write_config	= mpc85xx_pci_dm_write_config,
};

static const struct udevice_id mpc85xx_pci_ids[] = {
	{ .compatible = "fsl,mpc8540-pci" },
	{ }
};

U_BOOT_DRIVER(mpc85xx_pci) = {
	.name			= "mpc85xx_pci",
	.id			= UCLASS_PCI,
	.of_match		= mpc85xx_pci_ids,
	.ops			= &mpc85xx_pci_ops,
	.probe			= mpc85xx_pci_dm_probe,
	.remove			= mpc85xx_pci_dm_remove,
	.ofdata_to_platdata	= mpc85xx_pci_ofdata_to_platdata,
	.priv_auto_alloc_size	= sizeof(struct mpc85xx_pci_priv),
};
