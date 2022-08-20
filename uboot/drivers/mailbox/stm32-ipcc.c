// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics 2019 - All Rights Reserved
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <log.h>
#include <mailbox-uclass.h>
#include <malloc.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>

/*
 * IPCC has one set of registers per CPU
 * IPCC_PROC_OFFST allows to define cpu registers set base address
 * according to the assigned proc_id.
 */

#define IPCC_PROC_OFFST		0x010

#define IPCC_XSCR		0x008
#define IPCC_XTOYSR		0x00c

#define IPCC_HWCFGR		0x3f0
#define IPCFGR_CHAN_MASK	GENMASK(7, 0)

#define RX_BIT_CHAN(chan)	BIT(chan)
#define TX_BIT_SHIFT		16
#define TX_BIT_CHAN(chan)	BIT(TX_BIT_SHIFT + (chan))

#define STM32_MAX_PROCS		2

struct stm32_ipcc {
	void __iomem *reg_base;
	void __iomem *reg_proc;
	u32 proc_id;
	u32 n_chans;
};

static int stm32_ipcc_request(struct mbox_chan *chan)
{
	struct stm32_ipcc *ipcc = dev_get_priv(chan->dev);

	debug("%s(chan=%p)\n", __func__, chan);

	if (chan->id >= ipcc->n_chans) {
		debug("%s failed to request channel: %ld\n",
		      __func__, chan->id);
		return -EINVAL;
	}

	return 0;
}

static int stm32_ipcc_free(struct mbox_chan *chan)
{
	debug("%s(chan=%p)\n", __func__, chan);

	return 0;
}

static int stm32_ipcc_send(struct mbox_chan *chan, const void *data)
{
	struct stm32_ipcc *ipcc = dev_get_priv(chan->dev);

	debug("%s(chan=%p, data=%p)\n", __func__, chan, data);

	if (readl(ipcc->reg_proc + IPCC_XTOYSR) & BIT(chan->id))
		return -EBUSY;

	/* set channel n occupied */
	setbits_le32(ipcc->reg_proc + IPCC_XSCR, TX_BIT_CHAN(chan->id));

	return 0;
}

static int stm32_ipcc_recv(struct mbox_chan *chan, void *data)
{
	struct stm32_ipcc *ipcc = dev_get_priv(chan->dev);
	u32 val;
	int proc_offset;

	debug("%s(chan=%p, data=%p)\n", __func__, chan, data);

	/* read 'channel occupied' status from other proc */
	proc_offset = ipcc->proc_id ? -IPCC_PROC_OFFST : IPCC_PROC_OFFST;
	val = readl(ipcc->reg_proc + proc_offset + IPCC_XTOYSR);

	if (!(val & BIT(chan->id)))
		return -ENODATA;

	setbits_le32(ipcc->reg_proc + IPCC_XSCR, RX_BIT_CHAN(chan->id));

	return 0;
}

static int stm32_ipcc_probe(struct udevice *dev)
{
	struct stm32_ipcc *ipcc = dev_get_priv(dev);
	fdt_addr_t addr;
	const fdt32_t *cell;
	struct clk clk;
	int len, ret;

	debug("%s(dev=%p)\n", __func__, dev);

	addr = dev_read_addr(dev);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;

	ipcc->reg_base = (void __iomem *)addr;

	/* proc_id */
	cell = dev_read_prop(dev, "st,proc_id", &len);
	if (len < sizeof(fdt32_t)) {
		dev_dbg(dev, "Missing st,proc_id\n");
		return -EINVAL;
	}

	ipcc->proc_id = fdtdec_get_number(cell, 1);

	if (ipcc->proc_id >= STM32_MAX_PROCS) {
		dev_err(dev, "Invalid proc_id (%d)\n", ipcc->proc_id);
		return -EINVAL;
	}

	ipcc->reg_proc = ipcc->reg_base + ipcc->proc_id * IPCC_PROC_OFFST;

	ret = clk_get_by_index(dev, 0, &clk);
	if (ret)
		return ret;

	ret = clk_enable(&clk);
	if (ret)
		goto clk_free;

	/* get channel number */
	ipcc->n_chans = readl(ipcc->reg_base + IPCC_HWCFGR);
	ipcc->n_chans &= IPCFGR_CHAN_MASK;

	return 0;

clk_free:
	clk_free(&clk);

	return ret;
}

static const struct udevice_id stm32_ipcc_ids[] = {
	{ .compatible = "st,stm32mp1-ipcc" },
	{ }
};

struct mbox_ops stm32_ipcc_mbox_ops = {
	.request = stm32_ipcc_request,
	.rfree = stm32_ipcc_free,
	.send = stm32_ipcc_send,
	.recv = stm32_ipcc_recv,
};

U_BOOT_DRIVER(stm32_ipcc) = {
	.name = "stm32_ipcc",
	.id = UCLASS_MAILBOX,
	.of_match = stm32_ipcc_ids,
	.probe = stm32_ipcc_probe,
	.priv_auto_alloc_size = sizeof(struct stm32_ipcc),
	.ops = &stm32_ipcc_mbox_ops,
};
