// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * Inspired by dwc3-of-simple.c
 */

#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/extcon.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/usb/of.h>
#include <linux/reset.h>
#include <linux/iopoll.h>

#include "core.h"

/* USB QSCRATCH Hardware registers */
#define QSCRATCH_HS_PHY_CTRL			0x10
#define UTMI_OTG_VBUS_VALID			BIT(20)
#define SW_SESSVLD_SEL				BIT(28)

#define QSCRATCH_SS_PHY_CTRL			0x30
#define LANE0_PWR_PRESENT			BIT(24)

#define QSCRATCH_GENERAL_CFG			0x08
#define PIPE_UTMI_CLK_SEL			BIT(0)
#define PIPE3_PHYSTATUS_SW			BIT(3)
#define PIPE_UTMI_CLK_DIS			BIT(8)

#define PWR_EVNT_IRQ_STAT_REG			0x58
#define PWR_EVNT_LPM_IN_L2_MASK			BIT(4)
#define PWR_EVNT_LPM_OUT_L2_MASK		BIT(5)

#define SDM845_QSCRATCH_BASE_OFFSET		0xf8800
#define SDM845_QSCRATCH_SIZE			0x400
#define SDM845_DWC3_CORE_SIZE			0xcd00

struct dwc3_acpi_pdata {
	u32			qscratch_base_offset;
	u32			qscratch_base_size;
	u32			dwc3_core_base_size;
	int			hs_phy_irq_index;
	int			dp_hs_phy_irq_index;
	int			dm_hs_phy_irq_index;
	int			ss_phy_irq_index;
};

struct dwc3_qcom {
	struct device		*dev;
	void __iomem		*qscratch_base;
	struct platform_device	*dwc3;
	struct clk		**clks;
	int			num_clocks;
	struct reset_control	*resets;

	int			hs_phy_irq;
	int			dp_hs_phy_irq;
	int			dm_hs_phy_irq;
	int			ss_phy_irq;

	struct extcon_dev	*edev;
	struct extcon_dev	*host_edev;
	struct notifier_block	vbus_nb;
	struct notifier_block	host_nb;

	const struct dwc3_acpi_pdata *acpi_pdata;

	enum usb_dr_mode	mode;
	bool			is_suspended;
	bool			pm_suspended;
};

static inline void dwc3_qcom_setbits(void __iomem *base, u32 offset, u32 val)
{
	u32 reg;

	reg = readl(base + offset);
	reg |= val;
	writel(reg, base + offset);

	/* ensure that above write is through */
	readl(base + offset);
}

static inline void dwc3_qcom_clrbits(void __iomem *base, u32 offset, u32 val)
{
	u32 reg;

	reg = readl(base + offset);
	reg &= ~val;
	writel(reg, base + offset);

	/* ensure that above write is through */
	readl(base + offset);
}

static void dwc3_qcom_vbus_overrride_enable(struct dwc3_qcom *qcom, bool enable)
{
	if (enable) {
		dwc3_qcom_setbits(qcom->qscratch_base, QSCRATCH_SS_PHY_CTRL,
				  LANE0_PWR_PRESENT);
		dwc3_qcom_setbits(qcom->qscratch_base, QSCRATCH_HS_PHY_CTRL,
				  UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
	} else {
		dwc3_qcom_clrbits(qcom->qscratch_base, QSCRATCH_SS_PHY_CTRL,
				  LANE0_PWR_PRESENT);
		dwc3_qcom_clrbits(qcom->qscratch_base, QSCRATCH_HS_PHY_CTRL,
				  UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
	}
}

static int dwc3_qcom_vbus_notifier(struct notifier_block *nb,
				   unsigned long event, void *ptr)
{
	struct dwc3_qcom *qcom = container_of(nb, struct dwc3_qcom, vbus_nb);

	/* enable vbus override for device mode */
	dwc3_qcom_vbus_overrride_enable(qcom, event);
	qcom->mode = event ? USB_DR_MODE_PERIPHERAL : USB_DR_MODE_HOST;

	return NOTIFY_DONE;
}

static int dwc3_qcom_host_notifier(struct notifier_block *nb,
				   unsigned long event, void *ptr)
{
	struct dwc3_qcom *qcom = container_of(nb, struct dwc3_qcom, host_nb);

	/* disable vbus override in host mode */
	dwc3_qcom_vbus_overrride_enable(qcom, !event);
	qcom->mode = event ? USB_DR_MODE_HOST : USB_DR_MODE_PERIPHERAL;

	return NOTIFY_DONE;
}

static int dwc3_qcom_register_extcon(struct dwc3_qcom *qcom)
{
	struct device		*dev = qcom->dev;
	struct extcon_dev	*host_edev;
	int			ret;

	if (!of_property_read_bool(dev->of_node, "extcon"))
		return 0;

	qcom->edev = extcon_get_edev_by_phandle(dev, 0);
	if (IS_ERR(qcom->edev))
		return PTR_ERR(qcom->edev);

	qcom->vbus_nb.notifier_call = dwc3_qcom_vbus_notifier;

	qcom->host_edev = extcon_get_edev_by_phandle(dev, 1);
	if (IS_ERR(qcom->host_edev))
		qcom->host_edev = NULL;

	ret = devm_extcon_register_notifier(dev, qcom->edev, EXTCON_USB,
					    &qcom->vbus_nb);
	if (ret < 0) {
		dev_err(dev, "VBUS notifier register failed\n");
		return ret;
	}

	if (qcom->host_edev)
		host_edev = qcom->host_edev;
	else
		host_edev = qcom->edev;

	qcom->host_nb.notifier_call = dwc3_qcom_host_notifier;
	ret = devm_extcon_register_notifier(dev, host_edev, EXTCON_USB_HOST,
					    &qcom->host_nb);
	if (ret < 0) {
		dev_err(dev, "Host notifier register failed\n");
		return ret;
	}

	/* Update initial VBUS override based on extcon state */
	if (extcon_get_state(qcom->edev, EXTCON_USB) ||
	    !extcon_get_state(host_edev, EXTCON_USB_HOST))
		dwc3_qcom_vbus_notifier(&qcom->vbus_nb, true, qcom->edev);
	else
		dwc3_qcom_vbus_notifier(&qcom->vbus_nb, false, qcom->edev);

	return 0;
}

static void dwc3_qcom_disable_interrupts(struct dwc3_qcom *qcom)
{
	if (qcom->hs_phy_irq) {
		disable_irq_wake(qcom->hs_phy_irq);
		disable_irq_nosync(qcom->hs_phy_irq);
	}

	if (qcom->dp_hs_phy_irq) {
		disable_irq_wake(qcom->dp_hs_phy_irq);
		disable_irq_nosync(qcom->dp_hs_phy_irq);
	}

	if (qcom->dm_hs_phy_irq) {
		disable_irq_wake(qcom->dm_hs_phy_irq);
		disable_irq_nosync(qcom->dm_hs_phy_irq);
	}

	if (qcom->ss_phy_irq) {
		disable_irq_wake(qcom->ss_phy_irq);
		disable_irq_nosync(qcom->ss_phy_irq);
	}
}

static void dwc3_qcom_enable_interrupts(struct dwc3_qcom *qcom)
{
	if (qcom->hs_phy_irq) {
		enable_irq(qcom->hs_phy_irq);
		enable_irq_wake(qcom->hs_phy_irq);
	}

	if (qcom->dp_hs_phy_irq) {
		enable_irq(qcom->dp_hs_phy_irq);
		enable_irq_wake(qcom->dp_hs_phy_irq);
	}

	if (qcom->dm_hs_phy_irq) {
		enable_irq(qcom->dm_hs_phy_irq);
		enable_irq_wake(qcom->dm_hs_phy_irq);
	}

	if (qcom->ss_phy_irq) {
		enable_irq(qcom->ss_phy_irq);
		enable_irq_wake(qcom->ss_phy_irq);
	}
}

static int dwc3_qcom_suspend(struct dwc3_qcom *qcom)
{
	u32 val;
	int i;

	if (qcom->is_suspended)
		return 0;

	val = readl(qcom->qscratch_base + PWR_EVNT_IRQ_STAT_REG);
	if (!(val & PWR_EVNT_LPM_IN_L2_MASK))
		dev_err(qcom->dev, "HS-PHY not in L2\n");

	for (i = qcom->num_clocks - 1; i >= 0; i--)
		clk_disable_unprepare(qcom->clks[i]);

	qcom->is_suspended = true;
	dwc3_qcom_enable_interrupts(qcom);

	return 0;
}

static int dwc3_qcom_resume(struct dwc3_qcom *qcom)
{
	int ret;
	int i;

	if (!qcom->is_suspended)
		return 0;

	dwc3_qcom_disable_interrupts(qcom);

	for (i = 0; i < qcom->num_clocks; i++) {
		ret = clk_prepare_enable(qcom->clks[i]);
		if (ret < 0) {
			while (--i >= 0)
				clk_disable_unprepare(qcom->clks[i]);
			return ret;
		}
	}

	/* Clear existing events from PHY related to L2 in/out */
	dwc3_qcom_setbits(qcom->qscratch_base, PWR_EVNT_IRQ_STAT_REG,
			  PWR_EVNT_LPM_IN_L2_MASK | PWR_EVNT_LPM_OUT_L2_MASK);

	qcom->is_suspended = false;

	return 0;
}

static irqreturn_t qcom_dwc3_resume_irq(int irq, void *data)
{
	struct dwc3_qcom *qcom = data;
	struct dwc3	*dwc = platform_get_drvdata(qcom->dwc3);

	/* If pm_suspended then let pm_resume take care of resuming h/w */
	if (qcom->pm_suspended)
		return IRQ_HANDLED;

	if (dwc->xhci)
		pm_runtime_resume(&dwc->xhci->dev);

	return IRQ_HANDLED;
}

static void dwc3_qcom_select_utmi_clk(struct dwc3_qcom *qcom)
{
	/* Configure dwc3 to use UTMI clock as PIPE clock not present */
	dwc3_qcom_setbits(qcom->qscratch_base, QSCRATCH_GENERAL_CFG,
			  PIPE_UTMI_CLK_DIS);

	usleep_range(100, 1000);

	dwc3_qcom_setbits(qcom->qscratch_base, QSCRATCH_GENERAL_CFG,
			  PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);

	usleep_range(100, 1000);

	dwc3_qcom_clrbits(qcom->qscratch_base, QSCRATCH_GENERAL_CFG,
			  PIPE_UTMI_CLK_DIS);
}

static int dwc3_qcom_get_irq(struct platform_device *pdev,
			     const char *name, int num)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;

	if (np)
		ret = platform_get_irq_byname(pdev, name);
	else
		ret = platform_get_irq(pdev, num);

	return ret;
}

static int dwc3_qcom_setup_irq(struct platform_device *pdev)
{
	struct dwc3_qcom *qcom = platform_get_drvdata(pdev);
	const struct dwc3_acpi_pdata *pdata = qcom->acpi_pdata;
	int irq, ret;
	irq = dwc3_qcom_get_irq(pdev, "hs_phy_irq",
				pdata ? pdata->hs_phy_irq_index : -1);
	if (irq > 0) {
		/* Keep wakeup interrupts disabled until suspend */
		irq_set_status_flags(irq, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(qcom->dev, irq, NULL,
					qcom_dwc3_resume_irq,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"qcom_dwc3 HS", qcom);
		if (ret) {
			dev_err(qcom->dev, "hs_phy_irq failed: %d\n", ret);
			return ret;
		}
		qcom->hs_phy_irq = irq;
	}

	irq = dwc3_qcom_get_irq(pdev, "dp_hs_phy_irq",
				pdata ? pdata->dp_hs_phy_irq_index : -1);
	if (irq > 0) {
		irq_set_status_flags(irq, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(qcom->dev, irq, NULL,
					qcom_dwc3_resume_irq,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"qcom_dwc3 DP_HS", qcom);
		if (ret) {
			dev_err(qcom->dev, "dp_hs_phy_irq failed: %d\n", ret);
			return ret;
		}
		qcom->dp_hs_phy_irq = irq;
	}

	irq = dwc3_qcom_get_irq(pdev, "dm_hs_phy_irq",
				pdata ? pdata->dm_hs_phy_irq_index : -1);
	if (irq > 0) {
		irq_set_status_flags(irq, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(qcom->dev, irq, NULL,
					qcom_dwc3_resume_irq,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"qcom_dwc3 DM_HS", qcom);
		if (ret) {
			dev_err(qcom->dev, "dm_hs_phy_irq failed: %d\n", ret);
			return ret;
		}
		qcom->dm_hs_phy_irq = irq;
	}

	irq = dwc3_qcom_get_irq(pdev, "ss_phy_irq",
				pdata ? pdata->ss_phy_irq_index : -1);
	if (irq > 0) {
		irq_set_status_flags(irq, IRQ_NOAUTOEN);
		ret = devm_request_threaded_irq(qcom->dev, irq, NULL,
					qcom_dwc3_resume_irq,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"qcom_dwc3 SS", qcom);
		if (ret) {
			dev_err(qcom->dev, "ss_phy_irq failed: %d\n", ret);
			return ret;
		}
		qcom->ss_phy_irq = irq;
	}

	return 0;
}

static int dwc3_qcom_clk_init(struct dwc3_qcom *qcom, int count)
{
	struct device		*dev = qcom->dev;
	struct device_node	*np = dev->of_node;
	int			i;

	if (!np || !count)
		return 0;

	if (count < 0)
		return count;

	qcom->num_clocks = count;

	qcom->clks = devm_kcalloc(dev, qcom->num_clocks,
				  sizeof(struct clk *), GFP_KERNEL);
	if (!qcom->clks)
		return -ENOMEM;

	for (i = 0; i < qcom->num_clocks; i++) {
		struct clk	*clk;
		int		ret;

		clk = of_clk_get(np, i);
		if (IS_ERR(clk)) {
			while (--i >= 0)
				clk_put(qcom->clks[i]);
			return PTR_ERR(clk);
		}

		ret = clk_prepare_enable(clk);
		if (ret < 0) {
			while (--i >= 0) {
				clk_disable_unprepare(qcom->clks[i]);
				clk_put(qcom->clks[i]);
			}
			clk_put(clk);

			return ret;
		}

		qcom->clks[i] = clk;
	}

	return 0;
}

static const struct property_entry dwc3_qcom_acpi_properties[] = {
	PROPERTY_ENTRY_STRING("dr_mode", "host"),
	{}
};

static int dwc3_qcom_acpi_register_core(struct platform_device *pdev)
{
	struct dwc3_qcom 	*qcom = platform_get_drvdata(pdev);
	struct device		*dev = &pdev->dev;
	struct resource		*res, *child_res = NULL;
	int			irq;
	int			ret;

	qcom->dwc3 = platform_device_alloc("dwc3", PLATFORM_DEVID_AUTO);
	if (!qcom->dwc3)
		return -ENOMEM;

	qcom->dwc3->dev.parent = dev;
	qcom->dwc3->dev.type = dev->type;
	qcom->dwc3->dev.dma_mask = dev->dma_mask;
	qcom->dwc3->dev.dma_parms = dev->dma_parms;
	qcom->dwc3->dev.coherent_dma_mask = dev->coherent_dma_mask;

	child_res = kcalloc(2, sizeof(*child_res), GFP_KERNEL);
	if (!child_res)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get memory resource\n");
		ret = -ENODEV;
		goto out;
	}

	child_res[0].flags = res->flags;
	child_res[0].start = res->start;
	child_res[0].end = child_res[0].start +
		qcom->acpi_pdata->dwc3_core_base_size;

	irq = platform_get_irq(pdev, 0);
	child_res[1].flags = IORESOURCE_IRQ;
	child_res[1].start = child_res[1].end = irq;

	ret = platform_device_add_resources(qcom->dwc3, child_res, 2);
	if (ret) {
		dev_err(&pdev->dev, "failed to add resources\n");
		goto out;
	}

	ret = platform_device_add_properties(qcom->dwc3,
					     dwc3_qcom_acpi_properties);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add properties\n");
		goto out;
	}

	ret = platform_device_add(qcom->dwc3);
	if (ret)
		dev_err(&pdev->dev, "failed to add device\n");

out:
	kfree(child_res);
	return ret;
}

static int dwc3_qcom_of_register_core(struct platform_device *pdev)
{
	struct dwc3_qcom 	*qcom = platform_get_drvdata(pdev);
	struct device_node	*np = pdev->dev.of_node, *dwc3_np;
	struct device		*dev = &pdev->dev;
	int			ret;

	dwc3_np = of_get_child_by_name(np, "dwc3");
	if (!dwc3_np) {
		dev_err(dev, "failed to find dwc3 core child\n");
		return -ENODEV;
	}

	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret) {
		dev_err(dev, "failed to register dwc3 core - %d\n", ret);
		return ret;
	}

	qcom->dwc3 = of_find_device_by_node(dwc3_np);
	if (!qcom->dwc3) {
		dev_err(dev, "failed to get dwc3 platform device\n");
		return -ENODEV;
	}

	return 0;
}

static const struct dwc3_acpi_pdata sdm845_acpi_pdata = {
	.qscratch_base_offset = SDM845_QSCRATCH_BASE_OFFSET,
	.qscratch_base_size = SDM845_QSCRATCH_SIZE,
	.dwc3_core_base_size = SDM845_DWC3_CORE_SIZE,
	.hs_phy_irq_index = 1,
	.dp_hs_phy_irq_index = 4,
	.dm_hs_phy_irq_index = 3,
	.ss_phy_irq_index = 2
};

static int dwc3_qcom_probe(struct platform_device *pdev)
{
	struct device_node	*np = pdev->dev.of_node;
	struct device		*dev = &pdev->dev;
	struct dwc3_qcom	*qcom;
	struct resource		*res, *parent_res = NULL;
	int			ret, i;
	bool			ignore_pipe_clk;

	qcom = devm_kzalloc(&pdev->dev, sizeof(*qcom), GFP_KERNEL);
	if (!qcom)
		return -ENOMEM;

	platform_set_drvdata(pdev, qcom);
	qcom->dev = &pdev->dev;

	if (has_acpi_companion(dev)) {
		qcom->acpi_pdata = acpi_device_get_match_data(dev);
		if (!qcom->acpi_pdata) {
			dev_err(&pdev->dev, "no supporting ACPI device data\n");
			return -EINVAL;
		}
	}

	qcom->resets = devm_reset_control_array_get_optional_exclusive(dev);
	if (IS_ERR(qcom->resets)) {
		ret = PTR_ERR(qcom->resets);
		dev_err(&pdev->dev, "failed to get resets, err=%d\n", ret);
		return ret;
	}

	ret = reset_control_assert(qcom->resets);
	if (ret) {
		dev_err(&pdev->dev, "failed to assert resets, err=%d\n", ret);
		return ret;
	}

	usleep_range(10, 1000);

	ret = reset_control_deassert(qcom->resets);
	if (ret) {
		dev_err(&pdev->dev, "failed to deassert resets, err=%d\n", ret);
		goto reset_assert;
	}

	ret = dwc3_qcom_clk_init(qcom, of_clk_get_parent_count(np));
	if (ret) {
		dev_err(dev, "failed to get clocks\n");
		goto reset_assert;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (np) {
		parent_res = res;
	} else {
		parent_res = kmemdup(res, sizeof(struct resource), GFP_KERNEL);
		if (!parent_res)
			return -ENOMEM;

		parent_res->start = res->start +
			qcom->acpi_pdata->qscratch_base_offset;
		parent_res->end = parent_res->start +
			qcom->acpi_pdata->qscratch_base_size;
	}

	qcom->qscratch_base = devm_ioremap_resource(dev, parent_res);
	if (IS_ERR(qcom->qscratch_base)) {
		dev_err(dev, "failed to map qscratch, err=%d\n", ret);
		ret = PTR_ERR(qcom->qscratch_base);
		goto clk_disable;
	}

	ret = dwc3_qcom_setup_irq(pdev);
	if (ret) {
		dev_err(dev, "failed to setup IRQs, err=%d\n", ret);
		goto clk_disable;
	}

	/*
	 * Disable pipe_clk requirement if specified. Used when dwc3
	 * operates without SSPHY and only HS/FS/LS modes are supported.
	 */
	ignore_pipe_clk = device_property_read_bool(dev,
				"qcom,select-utmi-as-pipe-clk");
	if (ignore_pipe_clk)
		dwc3_qcom_select_utmi_clk(qcom);

	if (np)
		ret = dwc3_qcom_of_register_core(pdev);
	else
		ret = dwc3_qcom_acpi_register_core(pdev);

	if (ret) {
		dev_err(dev, "failed to register DWC3 Core, err=%d\n", ret);
		goto depopulate;
	}

	qcom->mode = usb_get_dr_mode(&qcom->dwc3->dev);

	/* enable vbus override for device mode */
	if (qcom->mode == USB_DR_MODE_PERIPHERAL)
		dwc3_qcom_vbus_overrride_enable(qcom, true);

	/* register extcon to override sw_vbus on Vbus change later */
	ret = dwc3_qcom_register_extcon(qcom);
	if (ret)
		goto depopulate;

	device_init_wakeup(&pdev->dev, 1);
	qcom->is_suspended = false;
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_forbid(dev);

	return 0;

depopulate:
	if (np)
		of_platform_depopulate(&pdev->dev);
	else
		platform_device_put(pdev);
clk_disable:
	for (i = qcom->num_clocks - 1; i >= 0; i--) {
		clk_disable_unprepare(qcom->clks[i]);
		clk_put(qcom->clks[i]);
	}
reset_assert:
	reset_control_assert(qcom->resets);

	return ret;
}

static int dwc3_qcom_remove(struct platform_device *pdev)
{
	struct dwc3_qcom *qcom = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int i;

	of_platform_depopulate(dev);

	for (i = qcom->num_clocks - 1; i >= 0; i--) {
		clk_disable_unprepare(qcom->clks[i]);
		clk_put(qcom->clks[i]);
	}
	qcom->num_clocks = 0;

	reset_control_assert(qcom->resets);

	pm_runtime_allow(dev);
	pm_runtime_disable(dev);

	return 0;
}

static int __maybe_unused dwc3_qcom_pm_suspend(struct device *dev)
{
	struct dwc3_qcom *qcom = dev_get_drvdata(dev);
	int ret = 0;

	ret = dwc3_qcom_suspend(qcom);
	if (!ret)
		qcom->pm_suspended = true;

	return ret;
}

static int __maybe_unused dwc3_qcom_pm_resume(struct device *dev)
{
	struct dwc3_qcom *qcom = dev_get_drvdata(dev);
	int ret;

	ret = dwc3_qcom_resume(qcom);
	if (!ret)
		qcom->pm_suspended = false;

	return ret;
}

static int __maybe_unused dwc3_qcom_runtime_suspend(struct device *dev)
{
	struct dwc3_qcom *qcom = dev_get_drvdata(dev);

	return dwc3_qcom_suspend(qcom);
}

static int __maybe_unused dwc3_qcom_runtime_resume(struct device *dev)
{
	struct dwc3_qcom *qcom = dev_get_drvdata(dev);

	return dwc3_qcom_resume(qcom);
}

static const struct dev_pm_ops dwc3_qcom_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_qcom_pm_suspend, dwc3_qcom_pm_resume)
	SET_RUNTIME_PM_OPS(dwc3_qcom_runtime_suspend, dwc3_qcom_runtime_resume,
			   NULL)
};

static const struct of_device_id dwc3_qcom_of_match[] = {
	{ .compatible = "qcom,dwc3" },
	{ .compatible = "qcom,msm8996-dwc3" },
	{ .compatible = "qcom,msm8998-dwc3" },
	{ .compatible = "qcom,sdm845-dwc3" },
	{ }
};
MODULE_DEVICE_TABLE(of, dwc3_qcom_of_match);

static const struct acpi_device_id dwc3_qcom_acpi_match[] = {
	{ "QCOM2430", (unsigned long)&sdm845_acpi_pdata },
	{ },
};
MODULE_DEVICE_TABLE(acpi, dwc3_qcom_acpi_match);

static struct platform_driver dwc3_qcom_driver = {
	.probe		= dwc3_qcom_probe,
	.remove		= dwc3_qcom_remove,
	.driver		= {
		.name	= "dwc3-qcom",
		.pm	= &dwc3_qcom_dev_pm_ops,
		.of_match_table	= dwc3_qcom_of_match,
		.acpi_match_table = ACPI_PTR(dwc3_qcom_acpi_match),
	},
};

module_platform_driver(dwc3_qcom_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare DWC3 QCOM Glue Driver");
