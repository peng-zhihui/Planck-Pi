// SPDX-License-Identifier: GPL-2.0
/*
 * Interrupt Timer Subsystem
 *
 * Copyright (C) 2017 Intel Corporation.
 * Copyright (C) 2017 Siemens AG
 * Copyright 2019 Google LLC
 *
 * Taken from coreboot itss.c
 */

#include <common.h>
#include <dm.h>
#include <dt-structs.h>
#include <irq.h>
#include <log.h>
#include <malloc.h>
#include <p2sb.h>
#include <spl.h>
#include <asm/itss.h>

struct itss_platdata {
#if CONFIG_IS_ENABLED(OF_PLATDATA)
	/* Put this first since driver model will copy the data here */
	struct dtd_intel_itss dtplat;
#endif
};

/* struct pmc_route - Routing for PMC to GPIO */
struct pmc_route {
	u32 pmc;
	u32 gpio;
};

struct itss_priv {
	struct pmc_route *route;
	uint route_count;
	u32 irq_snapshot[NUM_IPC_REGS];
};

static int set_polarity(struct udevice *dev, uint irq, bool active_low)
{
	u32 mask;
	uint reg;

	if (irq > ITSS_MAX_IRQ)
		return -EINVAL;

	reg = PCR_ITSS_IPC0_CONF + sizeof(u32) * (irq / IRQS_PER_IPC);
	mask = 1 << (irq % IRQS_PER_IPC);

	pcr_clrsetbits32(dev, reg, mask, active_low ? mask : 0);

	return 0;
}

#ifndef CONFIG_TPL_BUILD
static int snapshot_polarities(struct udevice *dev)
{
	struct itss_priv *priv = dev_get_priv(dev);
	const int start = GPIO_IRQ_START;
	const int end = GPIO_IRQ_END;
	int reg_start;
	int reg_end;
	int i;

	reg_start = start / IRQS_PER_IPC;
	reg_end = (end + IRQS_PER_IPC - 1) / IRQS_PER_IPC;

	for (i = reg_start; i < reg_end; i++) {
		uint reg = PCR_ITSS_IPC0_CONF + sizeof(u32) * i;

		priv->irq_snapshot[i] = pcr_read32(dev, reg);
	}

	return 0;
}

static void show_polarities(struct udevice *dev, const char *msg)
{
	int i;

	log_info("ITSS IRQ Polarities %s:\n", msg);
	for (i = 0; i < NUM_IPC_REGS; i++) {
		uint reg = PCR_ITSS_IPC0_CONF + sizeof(u32) * i;

		log_info("IPC%d: 0x%08x\n", i, pcr_read32(dev, reg));
	}
}

static int restore_polarities(struct udevice *dev)
{
	struct itss_priv *priv = dev_get_priv(dev);
	const int start = GPIO_IRQ_START;
	const int end = GPIO_IRQ_END;
	int reg_start;
	int reg_end;
	int i;

	show_polarities(dev, "Before");

	reg_start = start / IRQS_PER_IPC;
	reg_end = (end + IRQS_PER_IPC - 1) / IRQS_PER_IPC;

	for (i = reg_start; i < reg_end; i++) {
		u32 mask;
		u16 reg;
		int irq_start;
		int irq_end;

		irq_start = i * IRQS_PER_IPC;
		irq_end = min(irq_start + IRQS_PER_IPC - 1, ITSS_MAX_IRQ);

		if (start > irq_end)
			continue;
		if (end < irq_start)
			break;

		/* Track bits within the bounds of of the register */
		irq_start = max(start, irq_start) % IRQS_PER_IPC;
		irq_end = min(end, irq_end) % IRQS_PER_IPC;

		/* Create bitmask of the inclusive range of start and end */
		mask = (((1U << irq_end) - 1) | (1U << irq_end));
		mask &= ~((1U << irq_start) - 1);

		reg = PCR_ITSS_IPC0_CONF + sizeof(u32) * i;
		pcr_clrsetbits32(dev, reg, mask, mask & priv->irq_snapshot[i]);
	}

	show_polarities(dev, "After");

	return 0;
}
#endif

static int route_pmc_gpio_gpe(struct udevice *dev, uint pmc_gpe_num)
{
	struct itss_priv *priv = dev_get_priv(dev);
	struct pmc_route *route;
	int i;

	for (i = 0, route = priv->route; i < priv->route_count; i++, route++) {
		if (pmc_gpe_num == route->pmc)
			return route->gpio;
	}

	return -ENOENT;
}

static int itss_bind(struct udevice *dev)
{
	/* This is not set with of-platdata, so set it manually */
	if (CONFIG_IS_ENABLED(OF_PLATDATA))
		dev->driver_data = X86_IRQT_ITSS;

	return 0;
}

static int itss_ofdata_to_platdata(struct udevice *dev)
{
	struct itss_priv *priv = dev_get_priv(dev);
	int ret;

#if CONFIG_IS_ENABLED(OF_PLATDATA)
	struct itss_platdata *plat = dev_get_platdata(dev);
	struct dtd_intel_itss *dtplat = &plat->dtplat;

	/*
	 * It would be nice to do this in the bind() method, but with
	 * of-platdata binding happens in the order that DM finds things in the
	 * linker list (i.e. alphabetical order by driver name). So the GPIO
	 * device may well be bound before its parent (p2sb), and this call
	 * will fail if p2sb is not bound yet.
	 *
	 * TODO(sjg@chromium.org): Add a parent pointer to child devices in dtoc
	 */
	ret = p2sb_set_port_id(dev, dtplat->intel_p2sb_port_id);
	if (ret)
		return log_msg_ret("Could not set port id", ret);
	priv->route = (struct pmc_route *)dtplat->intel_pmc_routes;
	priv->route_count = ARRAY_SIZE(dtplat->intel_pmc_routes) /
		 sizeof(struct pmc_route);
#else
	int size;

	size = dev_read_size(dev, "intel,pmc-routes");
	if (size < 0)
		return size;
	priv->route = malloc(size);
	if (!priv->route)
		return -ENOMEM;
	ret = dev_read_u32_array(dev, "intel,pmc-routes", (u32 *)priv->route,
				 size / sizeof(fdt32_t));
	if (ret)
		return log_msg_ret("Cannot read pmc-routes", ret);
	priv->route_count = size / sizeof(struct pmc_route);
#endif

	return 0;
}

static const struct irq_ops itss_ops = {
	.route_pmc_gpio_gpe	= route_pmc_gpio_gpe,
	.set_polarity	= set_polarity,
#ifndef CONFIG_TPL_BUILD
	.snapshot_polarities = snapshot_polarities,
	.restore_polarities = restore_polarities,
#endif
};

static const struct udevice_id itss_ids[] = {
	{ .compatible = "intel,itss", .data = X86_IRQT_ITSS },
	{ }
};

U_BOOT_DRIVER(itss_drv) = {
	.name		= "intel_itss",
	.id		= UCLASS_IRQ,
	.of_match	= itss_ids,
	.ops		= &itss_ops,
	.bind		= itss_bind,
	.ofdata_to_platdata = itss_ofdata_to_platdata,
	.platdata_auto_alloc_size = sizeof(struct itss_platdata),
	.priv_auto_alloc_size = sizeof(struct itss_priv),
};
