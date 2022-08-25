// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2012-2019 ARM Limited or its affiliates. */

#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/stringify.h>
#include "cc_driver.h"
#include "cc_crypto_ctx.h"
#include "cc_debugfs.h"

struct cc_debugfs_ctx {
	struct dentry *dir;
};

#define CC_DEBUG_REG(_X) {	\
	.name = __stringify(_X),\
	.offset = CC_REG(_X)	\
	}

/*
 * This is a global var for the dentry of the
 * debugfs ccree/ dir. It is not tied down to
 * a specific instance of ccree, hence it is
 * global.
 */
static struct dentry *cc_debugfs_dir;

static struct debugfs_reg32 ver_sig_regs[] = {
	{ .name = "SIGNATURE" }, /* Must be 0th */
	{ .name = "VERSION" }, /* Must be 1st */
};

static struct debugfs_reg32 pid_cid_regs[] = {
	CC_DEBUG_REG(PERIPHERAL_ID_0),
	CC_DEBUG_REG(PERIPHERAL_ID_1),
	CC_DEBUG_REG(PERIPHERAL_ID_2),
	CC_DEBUG_REG(PERIPHERAL_ID_3),
	CC_DEBUG_REG(PERIPHERAL_ID_4),
	CC_DEBUG_REG(COMPONENT_ID_0),
	CC_DEBUG_REG(COMPONENT_ID_1),
	CC_DEBUG_REG(COMPONENT_ID_2),
	CC_DEBUG_REG(COMPONENT_ID_3),
};

static struct debugfs_reg32 debug_regs[] = {
	CC_DEBUG_REG(HOST_IRR),
	CC_DEBUG_REG(HOST_POWER_DOWN_EN),
	CC_DEBUG_REG(AXIM_MON_ERR),
	CC_DEBUG_REG(DSCRPTR_QUEUE_CONTENT),
	CC_DEBUG_REG(HOST_IMR),
	CC_DEBUG_REG(AXIM_CFG),
	CC_DEBUG_REG(AXIM_CACHE_PARAMS),
	CC_DEBUG_REG(GPR_HOST),
	CC_DEBUG_REG(AXIM_MON_COMP),
};

void __init cc_debugfs_global_init(void)
{
	cc_debugfs_dir = debugfs_create_dir("ccree", NULL);
}

void __exit cc_debugfs_global_fini(void)
{
	debugfs_remove(cc_debugfs_dir);
}

int cc_debugfs_init(struct cc_drvdata *drvdata)
{
	struct device *dev = drvdata_to_dev(drvdata);
	struct cc_debugfs_ctx *ctx;
	struct debugfs_regset32 *regset, *verset;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	regset = devm_kzalloc(dev, sizeof(*regset), GFP_KERNEL);
	if (!regset)
		return -ENOMEM;

	regset->regs = debug_regs;
	regset->nregs = ARRAY_SIZE(debug_regs);
	regset->base = drvdata->cc_base;

	ctx->dir = debugfs_create_dir(drvdata->plat_dev->name, cc_debugfs_dir);

	debugfs_create_regset32("regs", 0400, ctx->dir, regset);
	debugfs_create_bool("coherent", 0400, ctx->dir, &drvdata->coherent);

	verset = devm_kzalloc(dev, sizeof(*verset), GFP_KERNEL);
	/* Failing here is not important enough to fail the module load */
	if (!verset)
		goto out;

	if (drvdata->hw_rev <= CC_HW_REV_712) {
		ver_sig_regs[0].offset = drvdata->sig_offset;
		ver_sig_regs[1].offset = drvdata->ver_offset;
		verset->regs = ver_sig_regs;
		verset->nregs = ARRAY_SIZE(ver_sig_regs);
	} else {
		verset->regs = pid_cid_regs;
		verset->nregs = ARRAY_SIZE(pid_cid_regs);
	}
	verset->base = drvdata->cc_base;

	debugfs_create_regset32("version", 0400, ctx->dir, verset);

out:
	drvdata->debugfs = ctx;
	return 0;
}

void cc_debugfs_fini(struct cc_drvdata *drvdata)
{
	struct cc_debugfs_ctx *ctx = (struct cc_debugfs_ctx *)drvdata->debugfs;

	debugfs_remove_recursive(ctx->dir);
}
