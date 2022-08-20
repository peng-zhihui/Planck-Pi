// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2015 Google, Inc
 */

#include <common.h>
#include <clk-uclass.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <asm/clk.h>

struct sandbox_clk_priv {
	bool probed;
	ulong rate[SANDBOX_CLK_ID_COUNT];
	bool enabled[SANDBOX_CLK_ID_COUNT];
	bool requested[SANDBOX_CLK_ID_COUNT];
};

static ulong sandbox_clk_get_rate(struct clk *clk)
{
	struct sandbox_clk_priv *priv = dev_get_priv(clk->dev);

	if (!priv->probed)
		return -ENODEV;

	if (clk->id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	return priv->rate[clk->id];
}

static ulong sandbox_clk_set_rate(struct clk *clk, ulong rate)
{
	struct sandbox_clk_priv *priv = dev_get_priv(clk->dev);
	ulong old_rate;

	if (!priv->probed)
		return -ENODEV;

	if (clk->id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	if (!rate)
		return -EINVAL;

	old_rate = priv->rate[clk->id];
	priv->rate[clk->id] = rate;

	return old_rate;
}

static int sandbox_clk_enable(struct clk *clk)
{
	struct sandbox_clk_priv *priv = dev_get_priv(clk->dev);

	if (!priv->probed)
		return -ENODEV;

	if (clk->id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	priv->enabled[clk->id] = true;

	return 0;
}

static int sandbox_clk_disable(struct clk *clk)
{
	struct sandbox_clk_priv *priv = dev_get_priv(clk->dev);

	if (!priv->probed)
		return -ENODEV;

	if (clk->id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	priv->enabled[clk->id] = false;

	return 0;
}

static int sandbox_clk_request(struct clk *clk)
{
	struct sandbox_clk_priv *priv = dev_get_priv(clk->dev);

	if (clk->id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	priv->requested[clk->id] = true;
	return 0;
}

static int sandbox_clk_free(struct clk *clk)
{
	struct sandbox_clk_priv *priv = dev_get_priv(clk->dev);

	if (clk->id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	priv->requested[clk->id] = false;
	return 0;
}

static struct clk_ops sandbox_clk_ops = {
	.get_rate	= sandbox_clk_get_rate,
	.set_rate	= sandbox_clk_set_rate,
	.enable		= sandbox_clk_enable,
	.disable	= sandbox_clk_disable,
	.request	= sandbox_clk_request,
	.rfree		= sandbox_clk_free,
};

static int sandbox_clk_probe(struct udevice *dev)
{
	struct sandbox_clk_priv *priv = dev_get_priv(dev);

	priv->probed = true;
	return 0;
}

static const struct udevice_id sandbox_clk_ids[] = {
	{ .compatible = "sandbox,clk" },
	{ }
};

U_BOOT_DRIVER(clk_sandbox) = {
	.name		= "clk_sandbox",
	.id		= UCLASS_CLK,
	.of_match	= sandbox_clk_ids,
	.ops		= &sandbox_clk_ops,
	.probe		= sandbox_clk_probe,
	.priv_auto_alloc_size = sizeof(struct sandbox_clk_priv),
};

ulong sandbox_clk_query_rate(struct udevice *dev, int id)
{
	struct sandbox_clk_priv *priv = dev_get_priv(dev);

	if (id < 0 || id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	return priv->rate[id];
}

int sandbox_clk_query_enable(struct udevice *dev, int id)
{
	struct sandbox_clk_priv *priv = dev_get_priv(dev);

	if (id < 0 || id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	return priv->enabled[id];
}

int sandbox_clk_query_requested(struct udevice *dev, int id)
{
	struct sandbox_clk_priv *priv = dev_get_priv(dev);

	if (id < 0 || id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;
	return priv->requested[id];
}
