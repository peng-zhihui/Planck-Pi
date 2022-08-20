// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Andes Technology Corporation
 * Rick Chen, Andes Technology Corporation <rick@andestech.com>
 */

#include <common.h>
#include <command.h>
#include <cache.h>
#include <dm.h>
#include <hang.h>
#include <asm/io.h>
#include <dm/ofnode.h>
#include <linux/bitops.h>

struct l2cache {
	volatile u64	configure;
	volatile u64	control;
	volatile u64	hpm0;
	volatile u64	hpm1;
	volatile u64	hpm2;
	volatile u64	hpm3;
	volatile u64	error_status;
	volatile u64	ecc_error;
	volatile u64	cctl_command0;
	volatile u64	cctl_access_line0;
	volatile u64	cctl_command1;
	volatile u64	cctl_access_line1;
	volatile u64	cctl_command2;
	volatile u64	cctl_access_line2;
	volatile u64	cctl_command3;
	volatile u64	cctl_access_line4;
	volatile u64	cctl_status;
};

/* Control Register */
#define L2_ENABLE	0x1
/* prefetch */
#define IPREPETCH_OFF	3
#define DPREPETCH_OFF	5
#define IPREPETCH_MSK	(3 << IPREPETCH_OFF)
#define DPREPETCH_MSK	(3 << DPREPETCH_OFF)
/* tag ram */
#define TRAMOCTL_OFF	8
#define TRAMICTL_OFF	10
#define TRAMOCTL_MSK	(3 << TRAMOCTL_OFF)
#define TRAMICTL_MSK	BIT(TRAMICTL_OFF)
/* data ram */
#define DRAMOCTL_OFF	11
#define DRAMICTL_OFF	13
#define DRAMOCTL_MSK	(3 << DRAMOCTL_OFF)
#define DRAMICTL_MSK	BIT(DRAMICTL_OFF)

/* CCTL Command Register */
#define CCTL_CMD_REG(base, hart)	((ulong)(base) + 0x40 + (hart) * 0x10)
#define L2_WBINVAL_ALL	0x12

/* CCTL Status Register */
#define CCTL_STATUS_MSK(hart)		(0xf << ((hart) * 4))
#define CCTL_STATUS_IDLE(hart)		(0 << ((hart) * 4))
#define CCTL_STATUS_PROCESS(hart)	(1 << ((hart) * 4))
#define CCTL_STATUS_ILLEGAL(hart)	(2 << ((hart) * 4))

DECLARE_GLOBAL_DATA_PTR;

struct v5l2_plat {
	struct l2cache	*regs;
	u32		iprefetch;
	u32		dprefetch;
	u32 		tram_ctl[2];
	u32 		dram_ctl[2];
};

static int v5l2_enable(struct udevice *dev)
{
	struct v5l2_plat *plat = dev_get_platdata(dev);
	volatile struct l2cache *regs = plat->regs;

	if (regs)
		setbits_le32(&regs->control, L2_ENABLE);

	return 0;
}

static int v5l2_disable(struct udevice *dev)
{
	struct v5l2_plat *plat = dev_get_platdata(dev);
	volatile struct l2cache *regs = plat->regs;
	u8 hart = gd->arch.boot_hart;
	void __iomem *cctlcmd = (void __iomem *)CCTL_CMD_REG(regs, hart);

	if ((regs) && (readl(&regs->control) & L2_ENABLE)) {
		writel(L2_WBINVAL_ALL, cctlcmd);

		while ((readl(&regs->cctl_status) & CCTL_STATUS_MSK(hart))) {
			if ((readl(&regs->cctl_status) & CCTL_STATUS_ILLEGAL(hart))) {
				printf("L2 flush illegal! hanging...");
				hang();
			}
		}
		clrbits_le32(&regs->control, L2_ENABLE);
	}

	return 0;
}

static int v5l2_ofdata_to_platdata(struct udevice *dev)
{
	struct v5l2_plat *plat = dev_get_platdata(dev);
	struct l2cache *regs;

	regs = (struct l2cache *)dev_read_addr(dev);
	plat->regs = regs;

	plat->iprefetch = -EINVAL;
	plat->dprefetch = -EINVAL;
	plat->tram_ctl[0] = -EINVAL;
	plat->dram_ctl[0] = -EINVAL;

	/* Instruction and data fetch prefetch depth */
	dev_read_u32(dev, "andes,inst-prefetch", &plat->iprefetch);
	dev_read_u32(dev, "andes,data-prefetch", &plat->dprefetch);

	/* Set tag RAM and data RAM setup and output cycle */
	dev_read_u32_array(dev, "andes,tag-ram-ctl", plat->tram_ctl, 2);
	dev_read_u32_array(dev, "andes,data-ram-ctl", plat->dram_ctl, 2);

	return 0;
}

static int v5l2_probe(struct udevice *dev)
{
	struct v5l2_plat *plat = dev_get_platdata(dev);
	struct l2cache *regs = plat->regs;
	u32 ctl_val;

	ctl_val = readl(&regs->control);

	if (!(ctl_val & L2_ENABLE))
		ctl_val |= L2_ENABLE;

	if (plat->iprefetch != -EINVAL) {
		ctl_val &= ~(IPREPETCH_MSK);
		ctl_val |= (plat->iprefetch << IPREPETCH_OFF);
	}

	if (plat->dprefetch != -EINVAL) {
		ctl_val &= ~(DPREPETCH_MSK);
		ctl_val |= (plat->dprefetch << DPREPETCH_OFF);
	}

	if (plat->tram_ctl[0] != -EINVAL) {
		ctl_val &= ~(TRAMOCTL_MSK | TRAMICTL_MSK);
		ctl_val |= plat->tram_ctl[0] << TRAMOCTL_OFF;
		ctl_val |= plat->tram_ctl[1] << TRAMICTL_OFF;
	}

	if (plat->dram_ctl[0] != -EINVAL) {
		ctl_val &= ~(DRAMOCTL_MSK | DRAMICTL_MSK);
		ctl_val |= plat->dram_ctl[0] << DRAMOCTL_OFF;
		ctl_val |= plat->dram_ctl[1] << DRAMICTL_OFF;
	}

	writel(ctl_val, &regs->control);

	return 0;
}

static const struct udevice_id v5l2_cache_ids[] = {
	{ .compatible = "v5l2cache" },
	{}
};

static const struct cache_ops v5l2_cache_ops = {
	.enable		= v5l2_enable,
	.disable	= v5l2_disable,
};

U_BOOT_DRIVER(v5l2_cache) = {
	.name   = "v5l2_cache",
	.id     = UCLASS_CACHE,
	.of_match = v5l2_cache_ids,
	.ofdata_to_platdata = v5l2_ofdata_to_platdata,
	.probe	= v5l2_probe,
	.platdata_auto_alloc_size = sizeof(struct v5l2_plat),
	.ops = &v5l2_cache_ops,
	.flags  = DM_FLAG_PRE_RELOC,
};
