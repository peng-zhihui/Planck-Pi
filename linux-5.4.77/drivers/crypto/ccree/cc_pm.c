// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2012-2019 ARM Limited (or its affiliates). */

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include "cc_driver.h"
#include "cc_buffer_mgr.h"
#include "cc_request_mgr.h"
#include "cc_sram_mgr.h"
#include "cc_hash.h"
#include "cc_pm.h"
#include "cc_fips.h"

#define POWER_DOWN_ENABLE 0x01
#define POWER_DOWN_DISABLE 0x00

const struct dev_pm_ops ccree_pm = {
	SET_RUNTIME_PM_OPS(cc_pm_suspend, cc_pm_resume, NULL)
};

int cc_pm_suspend(struct device *dev)
{
	struct cc_drvdata *drvdata = dev_get_drvdata(dev);

	dev_dbg(dev, "set HOST_POWER_DOWN_EN\n");
	fini_cc_regs(drvdata);
	cc_iowrite(drvdata, CC_REG(HOST_POWER_DOWN_EN), POWER_DOWN_ENABLE);
	cc_clk_off(drvdata);
	return 0;
}

int cc_pm_resume(struct device *dev)
{
	int rc;
	struct cc_drvdata *drvdata = dev_get_drvdata(dev);

	dev_dbg(dev, "unset HOST_POWER_DOWN_EN\n");
	/* Enables the device source clk */
	rc = cc_clk_on(drvdata);
	if (rc) {
		dev_err(dev, "failed getting clock back on. We're toast.\n");
		return rc;
	}
	/* wait for Crytpcell reset completion */
	if (!cc_wait_for_reset_completion(drvdata)) {
		dev_err(dev, "Cryptocell reset not completed");
		return -EBUSY;
	}

	cc_iowrite(drvdata, CC_REG(HOST_POWER_DOWN_EN), POWER_DOWN_DISABLE);
	rc = init_cc_regs(drvdata, false);
	if (rc) {
		dev_err(dev, "init_cc_regs (%x)\n", rc);
		return rc;
	}
	/* check if tee fips error occurred during power down */
	cc_tee_handle_fips_error(drvdata);

	cc_init_hash_sram(drvdata);

	return 0;
}

int cc_pm_get(struct device *dev)
{
	int rc = 0;
	struct cc_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata->pm_on)
		rc = pm_runtime_get_sync(dev);

	return (rc == 1 ? 0 : rc);
}

int cc_pm_put_suspend(struct device *dev)
{
	int rc = 0;
	struct cc_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata->pm_on) {
		pm_runtime_mark_last_busy(dev);
		rc = pm_runtime_put_autosuspend(dev);
	}

	return rc;
}

bool cc_pm_is_dev_suspended(struct device *dev)
{
	/* check device state using runtime api */
	return pm_runtime_suspended(dev);
}

int cc_pm_init(struct cc_drvdata *drvdata)
{
	struct device *dev = drvdata_to_dev(drvdata);

	/* must be before the enabling to avoid resdundent suspending */
	pm_runtime_set_autosuspend_delay(dev, CC_SUSPEND_TIMEOUT);
	pm_runtime_use_autosuspend(dev);
	/* set us as active - note we won't do PM ops until cc_pm_go()! */
	return pm_runtime_set_active(dev);
}

/* enable the PM module*/
void cc_pm_go(struct cc_drvdata *drvdata)
{
	pm_runtime_enable(drvdata_to_dev(drvdata));
	drvdata->pm_on = true;
}

void cc_pm_fini(struct cc_drvdata *drvdata)
{
	pm_runtime_disable(drvdata_to_dev(drvdata));
	drvdata->pm_on = false;
}
