// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-direct.h>
#include <linux/init.h>
#include <linux/swiotlb.h>

dma_addr_t __phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	/* We extract 2bit node id (bit 44~47, only bit 44~45 used now) from
	 * Loongson-3's 48bit address space and embed it into 40bit */
	long nid = (paddr >> 44) & 0x3;
	return ((nid << 44) ^ paddr) | (nid << 37);
}

phys_addr_t __dma_to_phys(struct device *dev, dma_addr_t daddr)
{
	/* We extract 2bit node id (bit 44~47, only bit 44~45 used now) from
	 * Loongson-3's 48bit address space and embed it into 40bit */
	long nid = (daddr >> 37) & 0x3;
	return ((nid << 37) ^ daddr) | (nid << 44);
}

void __init plat_swiotlb_setup(void)
{
	swiotlb_init(1);
}
