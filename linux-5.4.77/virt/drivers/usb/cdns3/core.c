// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver.
 *
 * Copyright (C) 2018-2019 Cadence.
 * Copyright (C) 2017-2018 NXP
 * Copyright (C) 2019 Texas Instruments
 *
 * Author: Peter Chen <peter.chen@nxp.com>
 *         Pawel Laszczak <pawell@cadence.com>
 *         Roger Quadros <rogerq@ti.com>
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>

#include "gadget.h"
#include "core.h"
#include "host-export.h"
#include "gadget-export.h"
#include "drd.h"

static int cdns3_idle_init(struct cdns3 *cdns);

static inline
struct cdns3_role_driver *cdns3_get_current_role_driver(struct cdns3 *cdns)
{
	WARN_ON(!cdns->roles[cdns->role]);
	return cdns->roles[cdns->role];
}

static int cdns3_role_start(struct cdns3 *cdns, enum usb_role role)
{
	int ret;

	if (WARN_ON(role > USB_ROLE_DEVICE))
		return 0;

	mutex_lock(&cdns->mutex);
	cdns->role = role;
	mutex_unlock(&cdns->mutex);

	if (!cdns->roles[role])
		return -ENXIO;

	if (cdns->roles[role]->state == CDNS3_ROLE_STATE_ACTIVE)
		return 0;

	mutex_lock(&cdns->mutex);
	ret = cdns->roles[role]->start(cdns);
	if (!ret)
		cdns->roles[role]->state = CDNS3_ROLE_STATE_ACTIVE;
	mutex_unlock(&cdns->mutex);

	return ret;
}

static void cdns3_role_stop(struct cdns3 *cdns)
{
	enum usb_role role = cdns->role;

	if (WARN_ON(role > USB_ROLE_DEVICE))
		return;

	if (cdns->roles[role]->state == CDNS3_ROLE_STATE_INACTIVE)
		return;

	mutex_lock(&cdns->mutex);
	cdns->roles[role]->stop(cdns);
	cdns->roles[role]->state = CDNS3_ROLE_STATE_INACTIVE;
	mutex_unlock(&cdns->mutex);
}

static void cdns3_exit_roles(struct cdns3 *cdns)
{
	cdns3_role_stop(cdns);
	cdns3_drd_exit(cdns);
}

static enum usb_role cdsn3_hw_role_state_machine(struct cdns3 *cdns);

/**
 * cdns3_core_init_role - initialize role of operation
 * @cdns: Pointer to cdns3 structure
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_core_init_role(struct cdns3 *cdns)
{
	struct device *dev = cdns->dev;
	enum usb_dr_mode best_dr_mode;
	enum usb_dr_mode dr_mode;
	int ret = 0;

	dr_mode = usb_get_dr_mode(dev);
	cdns->role = USB_ROLE_NONE;

	/*
	 * If driver can't read mode by means of usb_get_dr_mode function then
	 * chooses mode according with Kernel configuration. This setting
	 * can be restricted later depending on strap pin configuration.
	 */
	if (dr_mode == USB_DR_MODE_UNKNOWN) {
		if (IS_ENABLED(CONFIG_USB_CDNS3_HOST) &&
		    IS_ENABLED(CONFIG_USB_CDNS3_GADGET))
			dr_mode = USB_DR_MODE_OTG;
		else if (IS_ENABLED(CONFIG_USB_CDNS3_HOST))
			dr_mode = USB_DR_MODE_HOST;
		else if (IS_ENABLED(CONFIG_USB_CDNS3_GADGET))
			dr_mode = USB_DR_MODE_PERIPHERAL;
	}

	/*
	 * At this point cdns->dr_mode contains strap configuration.
	 * Driver try update this setting considering kernel configuration
	 */
	best_dr_mode = cdns->dr_mode;

	ret = cdns3_idle_init(cdns);
	if (ret)
		return ret;

	if (dr_mode == USB_DR_MODE_OTG) {
		best_dr_mode = cdns->dr_mode;
	} else if (cdns->dr_mode == USB_DR_MODE_OTG) {
		best_dr_mode = dr_mode;
	} else if (cdns->dr_mode != dr_mode) {
		dev_err(dev, "Incorrect DRD configuration\n");
		return -EINVAL;
	}

	dr_mode = best_dr_mode;

	if (dr_mode == USB_DR_MODE_OTG || dr_mode == USB_DR_MODE_HOST) {
		ret = cdns3_host_init(cdns);
		if (ret) {
			dev_err(dev, "Host initialization failed with %d\n",
				ret);
			goto err;
		}
	}

	if (dr_mode == USB_DR_MODE_OTG || dr_mode == USB_DR_MODE_PERIPHERAL) {
		ret = cdns3_gadget_init(cdns);
		if (ret) {
			dev_err(dev, "Device initialization failed with %d\n",
				ret);
			goto err;
		}
	}

	cdns->dr_mode = dr_mode;

	ret = cdns3_drd_update_mode(cdns);
	if (ret)
		goto err;

	/* Initialize idle role to start with */
	ret = cdns3_role_start(cdns, USB_ROLE_NONE);
	if (ret)
		goto err;

	switch (cdns->dr_mode) {
	case USB_DR_MODE_OTG:
		ret = cdns3_hw_role_switch(cdns);
		if (ret)
			goto err;
		break;
	case USB_DR_MODE_PERIPHERAL:
		ret = cdns3_role_start(cdns, USB_ROLE_DEVICE);
		if (ret)
			goto err;
		break;
	case USB_DR_MODE_HOST:
		ret = cdns3_role_start(cdns, USB_ROLE_HOST);
		if (ret)
			goto err;
		break;
	default:
		ret = -EINVAL;
		goto err;
	}

	return ret;
err:
	cdns3_exit_roles(cdns);
	return ret;
}

/**
 * cdsn3_hw_role_state_machine  - role switch state machine based on hw events.
 * @cdns: Pointer to controller structure.
 *
 * Returns next role to be entered based on hw events.
 */
static enum usb_role cdsn3_hw_role_state_machine(struct cdns3 *cdns)
{
	enum usb_role role;
	int id, vbus;

	if (cdns->dr_mode != USB_DR_MODE_OTG)
		goto not_otg;

	id = cdns3_get_id(cdns);
	vbus = cdns3_get_vbus(cdns);

	/*
	 * Role change state machine
	 * Inputs: ID, VBUS
	 * Previous state: cdns->role
	 * Next state: role
	 */
	role = cdns->role;

	switch (role) {
	case USB_ROLE_NONE:
		/*
		 * Driver treats USB_ROLE_NONE synonymous to IDLE state from
		 * controller specification.
		 */
		if (!id)
			role = USB_ROLE_HOST;
		else if (vbus)
			role = USB_ROLE_DEVICE;
		break;
	case USB_ROLE_HOST: /* from HOST, we can only change to NONE */
		if (id)
			role = USB_ROLE_NONE;
		break;
	case USB_ROLE_DEVICE: /* from GADGET, we can only change to NONE*/
		if (!vbus)
			role = USB_ROLE_NONE;
		break;
	}

	dev_dbg(cdns->dev, "role %d -> %d\n", cdns->role, role);

	return role;

not_otg:
	if (cdns3_is_host(cdns))
		role = USB_ROLE_HOST;
	if (cdns3_is_device(cdns))
		role = USB_ROLE_DEVICE;

	return role;
}

static int cdns3_idle_role_start(struct cdns3 *cdns)
{
	return 0;
}

static void cdns3_idle_role_stop(struct cdns3 *cdns)
{
	/* Program Lane swap and bring PHY out of RESET */
	phy_reset(cdns->usb3_phy);
}

static int cdns3_idle_init(struct cdns3 *cdns)
{
	struct cdns3_role_driver *rdrv;

	rdrv = devm_kzalloc(cdns->dev, sizeof(*rdrv), GFP_KERNEL);
	if (!rdrv)
		return -ENOMEM;

	rdrv->start = cdns3_idle_role_start;
	rdrv->stop = cdns3_idle_role_stop;
	rdrv->state = CDNS3_ROLE_STATE_INACTIVE;
	rdrv->suspend = NULL;
	rdrv->resume = NULL;
	rdrv->name = "idle";

	cdns->roles[USB_ROLE_NONE] = rdrv;

	return 0;
}

/**
 * cdns3_hw_role_switch - switch roles based on HW state
 * @cdns3: controller
 */
int cdns3_hw_role_switch(struct cdns3 *cdns)
{
	enum usb_role real_role, current_role;
	int ret = 0;

	/* Do nothing if role based on syfs. */
	if (cdns->role_override)
		return 0;

	pm_runtime_get_sync(cdns->dev);

	current_role = cdns->role;
	real_role = cdsn3_hw_role_state_machine(cdns);

	/* Do nothing if nothing changed */
	if (current_role == real_role)
		goto exit;

	cdns3_role_stop(cdns);

	dev_dbg(cdns->dev, "Switching role %d -> %d", current_role, real_role);

	ret = cdns3_role_start(cdns, real_role);
	if (ret) {
		/* Back to current role */
		dev_err(cdns->dev, "set %d has failed, back to %d\n",
			real_role, current_role);
		ret = cdns3_role_start(cdns, current_role);
		if (ret)
			dev_err(cdns->dev, "back to %d failed too\n",
				current_role);
	}
exit:
	pm_runtime_put_sync(cdns->dev);
	return ret;
}

/**
 * cdsn3_role_get - get current role of controller.
 *
 * @dev: Pointer to device structure
 *
 * Returns role
 */
static enum usb_role cdns3_role_get(struct device *dev)
{
	struct cdns3 *cdns = dev_get_drvdata(dev);

	return cdns->role;
}

/**
 * cdns3_role_set - set current role of controller.
 *
 * @dev: pointer to device object
 * @role - the previous role
 * Handles below events:
 * - Role switch for dual-role devices
 * - USB_ROLE_GADGET <--> USB_ROLE_NONE for peripheral-only devices
 */
static int cdns3_role_set(struct device *dev, enum usb_role role)
{
	struct cdns3 *cdns = dev_get_drvdata(dev);
	int ret = 0;

	pm_runtime_get_sync(cdns->dev);

	/*
	 * FIXME: switch role framework should be extended to meet
	 * requirements. Driver assumes that role can be controlled
	 * by SW or HW. Temporary workaround is to use USB_ROLE_NONE to
	 * switch from SW to HW control.
	 *
	 * For dr_mode == USB_DR_MODE_OTG:
	 *	if user sets USB_ROLE_HOST or USB_ROLE_DEVICE then driver
	 *	sets role_override flag and forces that role.
	 *	if user sets USB_ROLE_NONE, driver clears role_override and lets
	 *	HW state machine take over.
	 *
	 * For dr_mode != USB_DR_MODE_OTG:
	 *	Assumptions:
	 *	1. Restricted user control between NONE and dr_mode.
	 *	2. Driver doesn't need to rely on role_override flag.
	 *	3. Driver needs to ensure that HW state machine is never called
	 *	   if dr_mode != USB_DR_MODE_OTG.
	 */
	if (role == USB_ROLE_NONE)
		cdns->role_override = 0;
	else
		cdns->role_override = 1;

	/*
	 * HW state might have changed so driver need to trigger
	 * HW state machine if dr_mode == USB_DR_MODE_OTG.
	 */
	if (!cdns->role_override && cdns->dr_mode == USB_DR_MODE_OTG) {
		cdns3_hw_role_switch(cdns);
		goto pm_put;
	}

	if (cdns->role == role)
		goto pm_put;

	if (cdns->dr_mode == USB_DR_MODE_HOST) {
		switch (role) {
		case USB_ROLE_NONE:
		case USB_ROLE_HOST:
			break;
		default:
			ret = -EPERM;
			goto pm_put;
		}
	}

	if (cdns->dr_mode == USB_DR_MODE_PERIPHERAL) {
		switch (role) {
		case USB_ROLE_NONE:
		case USB_ROLE_DEVICE:
			break;
		default:
			ret = -EPERM;
			goto pm_put;
		}
	}

	cdns3_role_stop(cdns);
	ret = cdns3_role_start(cdns, role);
	if (ret) {
		dev_err(cdns->dev, "set role %d has failed\n", role);
		ret = -EPERM;
	}

pm_put:
	pm_runtime_put_sync(cdns->dev);
	return ret;
}

static const struct usb_role_switch_desc cdns3_switch_desc = {
	.set = cdns3_role_set,
	.get = cdns3_role_get,
	.allow_userspace_control = true,
};

/**
 * cdns3_probe - probe for cdns3 core device
 * @pdev: Pointer to cdns3 core platform device
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource	*res;
	struct cdns3 *cdns;
	void __iomem *regs;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "error setting dma mask: %d\n", ret);
		return -ENODEV;
	}

	cdns = devm_kzalloc(dev, sizeof(*cdns), GFP_KERNEL);
	if (!cdns)
		return -ENOMEM;

	cdns->dev = dev;

	platform_set_drvdata(pdev, cdns);

	res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "host");
	if (!res) {
		dev_err(dev, "missing host IRQ\n");
		return -ENODEV;
	}

	cdns->xhci_res[0] = *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "xhci");
	if (!res) {
		dev_err(dev, "couldn't get xhci resource\n");
		return -ENXIO;
	}

	cdns->xhci_res[1] = *res;

	cdns->dev_irq = platform_get_irq_byname(pdev, "peripheral");
	if (cdns->dev_irq == -EPROBE_DEFER)
		return cdns->dev_irq;

	if (cdns->dev_irq < 0)
		dev_err(dev, "couldn't get peripheral irq\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dev");
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	cdns->dev_regs	= regs;

	cdns->otg_irq = platform_get_irq_byname(pdev, "otg");
	if (cdns->otg_irq == -EPROBE_DEFER)
		return cdns->otg_irq;

	if (cdns->otg_irq < 0) {
		dev_err(dev, "couldn't get otg irq\n");
		return cdns->otg_irq;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "otg");
	if (!res) {
		dev_err(dev, "couldn't get otg resource\n");
		return -ENXIO;
	}

	cdns->otg_res = *res;

	mutex_init(&cdns->mutex);

	cdns->usb2_phy = devm_phy_optional_get(dev, "cdns3,usb2-phy");
	if (IS_ERR(cdns->usb2_phy))
		return PTR_ERR(cdns->usb2_phy);

	ret = phy_init(cdns->usb2_phy);
	if (ret)
		return ret;

	cdns->usb3_phy = devm_phy_optional_get(dev, "cdns3,usb3-phy");
	if (IS_ERR(cdns->usb3_phy))
		return PTR_ERR(cdns->usb3_phy);

	ret = phy_init(cdns->usb3_phy);
	if (ret)
		goto err1;

	ret = phy_power_on(cdns->usb2_phy);
	if (ret)
		goto err2;

	ret = phy_power_on(cdns->usb3_phy);
	if (ret)
		goto err3;

	cdns->role_sw = usb_role_switch_register(dev, &cdns3_switch_desc);
	if (IS_ERR(cdns->role_sw)) {
		ret = PTR_ERR(cdns->role_sw);
		dev_warn(dev, "Unable to register Role Switch\n");
		goto err4;
	}

	ret = cdns3_drd_init(cdns);
	if (ret)
		goto err5;

	ret = cdns3_core_init_role(cdns);
	if (ret)
		goto err5;

	device_set_wakeup_capable(dev, true);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/*
	 * The controller needs less time between bus and controller suspend,
	 * and we also needs a small delay to avoid frequently entering low
	 * power mode.
	 */
	pm_runtime_set_autosuspend_delay(dev, 20);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_use_autosuspend(dev);
	dev_dbg(dev, "Cadence USB3 core: probe succeed\n");

	return 0;
err5:
	cdns3_drd_exit(cdns);
	usb_role_switch_unregister(cdns->role_sw);
err4:
	phy_power_off(cdns->usb3_phy);

err3:
	phy_power_off(cdns->usb2_phy);
err2:
	phy_exit(cdns->usb3_phy);
err1:
	phy_exit(cdns->usb2_phy);

	return ret;
}

/**
 * cdns3_remove - unbind drd driver and clean up
 * @pdev: Pointer to Linux platform device
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_remove(struct platform_device *pdev)
{
	struct cdns3 *cdns = platform_get_drvdata(pdev);

	pm_runtime_get_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
	cdns3_exit_roles(cdns);
	usb_role_switch_unregister(cdns->role_sw);
	phy_power_off(cdns->usb2_phy);
	phy_power_off(cdns->usb3_phy);
	phy_exit(cdns->usb2_phy);
	phy_exit(cdns->usb3_phy);
	return 0;
}

#ifdef CONFIG_PM_SLEEP

static int cdns3_suspend(struct device *dev)
{
	struct cdns3 *cdns = dev_get_drvdata(dev);
	unsigned long flags;

	if (cdns->role == USB_ROLE_HOST)
		return 0;

	if (pm_runtime_status_suspended(dev))
		pm_runtime_resume(dev);

	if (cdns->roles[cdns->role]->suspend) {
		spin_lock_irqsave(&cdns->gadget_dev->lock, flags);
		cdns->roles[cdns->role]->suspend(cdns, false);
		spin_unlock_irqrestore(&cdns->gadget_dev->lock, flags);
	}

	return 0;
}

static int cdns3_resume(struct device *dev)
{
	struct cdns3 *cdns = dev_get_drvdata(dev);
	unsigned long flags;

	if (cdns->role == USB_ROLE_HOST)
		return 0;

	if (cdns->roles[cdns->role]->resume) {
		spin_lock_irqsave(&cdns->gadget_dev->lock, flags);
		cdns->roles[cdns->role]->resume(cdns, false);
		spin_unlock_irqrestore(&cdns->gadget_dev->lock, flags);
	}

	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	return 0;
}
#endif

static const struct dev_pm_ops cdns3_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cdns3_suspend, cdns3_resume)
};

#ifdef CONFIG_OF
static const struct of_device_id of_cdns3_match[] = {
	{ .compatible = "cdns,usb3" },
	{ },
};
MODULE_DEVICE_TABLE(of, of_cdns3_match);
#endif

static struct platform_driver cdns3_driver = {
	.probe		= cdns3_probe,
	.remove		= cdns3_remove,
	.driver		= {
		.name	= "cdns-usb3",
		.of_match_table	= of_match_ptr(of_cdns3_match),
		.pm	= &cdns3_pm_ops,
	},
};

module_platform_driver(cdns3_driver);

MODULE_ALIAS("platform:cdns3");
MODULE_AUTHOR("Pawel Laszczak <pawell@cadence.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cadence USB3 DRD Controller Driver");
