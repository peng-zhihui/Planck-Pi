// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support for the camera device found on Marvell MMP processors; known
 * to work with the Armada 610 as used in the OLPC 1.75 system.
 *
 * Copyright 2011 Jonathan Corbet <corbet@lwn.net>
 * Copyright 2018 Lubomir Rintel <lkundrak@v3.sk>
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <linux/platform_data/media/mmp-camera.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/pm.h>
#include <linux/clk.h>

#include "mcam-core.h"

MODULE_ALIAS("platform:mmp-camera");
MODULE_AUTHOR("Jonathan Corbet <corbet@lwn.net>");
MODULE_LICENSE("GPL");

static char *mcam_clks[] = {"axi", "func", "phy"};

struct mmp_camera {
	struct platform_device *pdev;
	struct mcam_camera mcam;
	struct list_head devlist;
	struct clk *mipi_clk;
	int irq;
};

static inline struct mmp_camera *mcam_to_cam(struct mcam_camera *mcam)
{
	return container_of(mcam, struct mmp_camera, mcam);
}

/*
 * A silly little infrastructure so we can keep track of our devices.
 * Chances are that we will never have more than one of them, but
 * the Armada 610 *does* have two controllers...
 */

static LIST_HEAD(mmpcam_devices);
static struct mutex mmpcam_devices_lock;

static void mmpcam_add_device(struct mmp_camera *cam)
{
	mutex_lock(&mmpcam_devices_lock);
	list_add(&cam->devlist, &mmpcam_devices);
	mutex_unlock(&mmpcam_devices_lock);
}

static void mmpcam_remove_device(struct mmp_camera *cam)
{
	mutex_lock(&mmpcam_devices_lock);
	list_del(&cam->devlist);
	mutex_unlock(&mmpcam_devices_lock);
}

/*
 * Platform dev remove passes us a platform_device, and there's
 * no handy unused drvdata to stash a backpointer in.  So just
 * dig it out of our list.
 */
static struct mmp_camera *mmpcam_find_device(struct platform_device *pdev)
{
	struct mmp_camera *cam;

	mutex_lock(&mmpcam_devices_lock);
	list_for_each_entry(cam, &mmpcam_devices, devlist) {
		if (cam->pdev == pdev) {
			mutex_unlock(&mmpcam_devices_lock);
			return cam;
		}
	}
	mutex_unlock(&mmpcam_devices_lock);
	return NULL;
}

/*
 * calc the dphy register values
 * There are three dphy registers being used.
 * dphy[0] - CSI2_DPHY3
 * dphy[1] - CSI2_DPHY5
 * dphy[2] - CSI2_DPHY6
 * CSI2_DPHY3 and CSI2_DPHY6 can be set with a default value
 * or be calculated dynamically
 */
static void mmpcam_calc_dphy(struct mcam_camera *mcam)
{
	struct mmp_camera *cam = mcam_to_cam(mcam);
	struct mmp_camera_platform_data *pdata = cam->pdev->dev.platform_data;
	struct device *dev = &cam->pdev->dev;
	unsigned long tx_clk_esc;

	/*
	 * If CSI2_DPHY3 is calculated dynamically,
	 * pdata->lane_clk should be already set
	 * either in the board driver statically
	 * or in the sensor driver dynamically.
	 */
	/*
	 * dphy[0] - CSI2_DPHY3:
	 *  bit 0 ~ bit 7: HS Term Enable.
	 *   defines the time that the DPHY
	 *   wait before enabling the data
	 *   lane termination after detecting
	 *   that the sensor has driven the data
	 *   lanes to the LP00 bridge state.
	 *   The value is calculated by:
	 *   (Max T(D_TERM_EN)/Period(DDR)) - 1
	 *  bit 8 ~ bit 15: HS_SETTLE
	 *   Time interval during which the HS
	 *   receiver shall ignore any Data Lane
	 *   HS transitions.
	 *   The value has been calibrated on
	 *   different boards. It seems to work well.
	 *
	 *  More detail please refer
	 *  MIPI Alliance Spectification for D-PHY
	 *  document for explanation of HS-SETTLE
	 *  and D-TERM-EN.
	 */
	switch (pdata->dphy3_algo) {
	case DPHY3_ALGO_PXA910:
		/*
		 * Calculate CSI2_DPHY3 algo for PXA910
		 */
		pdata->dphy[0] =
			(((1 + (pdata->lane_clk * 80) / 1000) & 0xff) << 8)
			| (1 + pdata->lane_clk * 35 / 1000);
		break;
	case DPHY3_ALGO_PXA2128:
		/*
		 * Calculate CSI2_DPHY3 algo for PXA2128
		 */
		pdata->dphy[0] =
			(((2 + (pdata->lane_clk * 110) / 1000) & 0xff) << 8)
			| (1 + pdata->lane_clk * 35 / 1000);
		break;
	default:
		/*
		 * Use default CSI2_DPHY3 value for PXA688/PXA988
		 */
		dev_dbg(dev, "camera: use the default CSI2_DPHY3 value\n");
	}

	/*
	 * mipi_clk will never be changed, it is a fixed value on MMP
	 */
	if (IS_ERR(cam->mipi_clk))
		return;

	/* get the escape clk, this is hard coded */
	clk_prepare_enable(cam->mipi_clk);
	tx_clk_esc = (clk_get_rate(cam->mipi_clk) / 1000000) / 12;
	clk_disable_unprepare(cam->mipi_clk);
	/*
	 * dphy[2] - CSI2_DPHY6:
	 * bit 0 ~ bit 7: CK Term Enable
	 *  Time for the Clock Lane receiver to enable the HS line
	 *  termination. The value is calculated similarly with
	 *  HS Term Enable
	 * bit 8 ~ bit 15: CK Settle
	 *  Time interval during which the HS receiver shall ignore
	 *  any Clock Lane HS transitions.
	 *  The value is calibrated on the boards.
	 */
	pdata->dphy[2] =
		((((534 * tx_clk_esc) / 2000 - 1) & 0xff) << 8)
		| (((38 * tx_clk_esc) / 1000 - 1) & 0xff);

	dev_dbg(dev, "camera: DPHY sets: dphy3=0x%x, dphy5=0x%x, dphy6=0x%x\n",
		pdata->dphy[0], pdata->dphy[1], pdata->dphy[2]);
}

static irqreturn_t mmpcam_irq(int irq, void *data)
{
	struct mcam_camera *mcam = data;
	unsigned int irqs, handled;

	spin_lock(&mcam->dev_lock);
	irqs = mcam_reg_read(mcam, REG_IRQSTAT);
	handled = mccic_irq(mcam, irqs);
	spin_unlock(&mcam->dev_lock);
	return IRQ_RETVAL(handled);
}

static void mcam_init_clk(struct mcam_camera *mcam)
{
	unsigned int i;

	for (i = 0; i < NR_MCAM_CLK; i++) {
		if (mcam_clks[i] != NULL) {
			/* Some clks are not necessary on some boards
			 * We still try to run even it fails getting clk
			 */
			mcam->clk[i] = devm_clk_get(mcam->dev, mcam_clks[i]);
			if (IS_ERR(mcam->clk[i]))
				dev_warn(mcam->dev, "Could not get clk: %s\n",
						mcam_clks[i]);
		}
	}
}

static int mmpcam_probe(struct platform_device *pdev)
{
	struct mmp_camera *cam;
	struct mcam_camera *mcam;
	struct resource *res;
	struct fwnode_handle *ep;
	struct mmp_camera_platform_data *pdata;
	int ret;

	cam = devm_kzalloc(&pdev->dev, sizeof(*cam), GFP_KERNEL);
	if (cam == NULL)
		return -ENOMEM;
	cam->pdev = pdev;
	INIT_LIST_HEAD(&cam->devlist);

	mcam = &cam->mcam;
	mcam->calc_dphy = mmpcam_calc_dphy;
	mcam->dev = &pdev->dev;
	pdata = pdev->dev.platform_data;
	if (pdata) {
		mcam->mclk_src = pdata->mclk_src;
		mcam->mclk_div = pdata->mclk_div;
		mcam->bus_type = pdata->bus_type;
		mcam->dphy = pdata->dphy;
		mcam->lane = pdata->lane;
	} else {
		/*
		 * These are values that used to be hardcoded in mcam-core and
		 * work well on a OLPC XO 1.75 with a parallel bus sensor.
		 * If it turns out other setups make sense, the values should
		 * be obtained from the device tree.
		 */
		mcam->mclk_src = 3;
		mcam->mclk_div = 2;
	}
	if (mcam->bus_type == V4L2_MBUS_CSI2_DPHY) {
		cam->mipi_clk = devm_clk_get(mcam->dev, "mipi");
		if ((IS_ERR(cam->mipi_clk) && mcam->dphy[2] == 0))
			return PTR_ERR(cam->mipi_clk);
	}
	mcam->mipi_enabled = false;
	mcam->chip_id = MCAM_ARMADA610;
	mcam->buffer_mode = B_DMA_sg;
	strscpy(mcam->bus_info, "platform:mmp-camera", sizeof(mcam->bus_info));
	spin_lock_init(&mcam->dev_lock);
	/*
	 * Get our I/O memory.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mcam->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mcam->regs))
		return PTR_ERR(mcam->regs);
	mcam->regs_size = resource_size(res);

	mcam_init_clk(mcam);

	/*
	 * Create a match of the sensor against its OF node.
	 */
	ep = fwnode_graph_get_next_endpoint(of_fwnode_handle(pdev->dev.of_node),
					    NULL);
	if (!ep)
		return -ENODEV;

	mcam->asd.match_type = V4L2_ASYNC_MATCH_FWNODE;
	mcam->asd.match.fwnode = fwnode_graph_get_remote_port_parent(ep);

	fwnode_handle_put(ep);

	/*
	 * Register the device with the core.
	 */
	ret = mccic_register(mcam);
	if (ret)
		return ret;

	/*
	 * Add OF clock provider.
	 */
	ret = of_clk_add_provider(pdev->dev.of_node, of_clk_src_simple_get,
								mcam->mclk);
	if (ret) {
		dev_err(&pdev->dev, "can't add DT clock provider\n");
		goto out;
	}

	/*
	 * Finally, set up our IRQ now that the core is ready to
	 * deal with it.
	 */
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		ret = -ENODEV;
		goto out;
	}
	cam->irq = res->start;
	ret = devm_request_irq(&pdev->dev, cam->irq, mmpcam_irq, IRQF_SHARED,
					"mmp-camera", mcam);
	if (ret == 0) {
		mmpcam_add_device(cam);
		return 0;
	}

out:
	fwnode_handle_put(mcam->asd.match.fwnode);
	mccic_shutdown(mcam);

	return ret;
}


static int mmpcam_remove(struct mmp_camera *cam)
{
	struct mcam_camera *mcam = &cam->mcam;

	mmpcam_remove_device(cam);
	mccic_shutdown(mcam);
	return 0;
}

static int mmpcam_platform_remove(struct platform_device *pdev)
{
	struct mmp_camera *cam = mmpcam_find_device(pdev);

	if (cam == NULL)
		return -ENODEV;
	return mmpcam_remove(cam);
}

/*
 * Suspend/resume support.
 */
#ifdef CONFIG_PM

static int mmpcam_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct mmp_camera *cam = mmpcam_find_device(pdev);

	if (state.event != PM_EVENT_SUSPEND)
		return 0;
	mccic_suspend(&cam->mcam);
	return 0;
}

static int mmpcam_resume(struct platform_device *pdev)
{
	struct mmp_camera *cam = mmpcam_find_device(pdev);

	return mccic_resume(&cam->mcam);
}

#endif

static const struct of_device_id mmpcam_of_match[] = {
	{ .compatible = "marvell,mmp2-ccic", },
	{},
};
MODULE_DEVICE_TABLE(of, mmpcam_of_match);

static struct platform_driver mmpcam_driver = {
	.probe		= mmpcam_probe,
	.remove		= mmpcam_platform_remove,
#ifdef CONFIG_PM
	.suspend	= mmpcam_suspend,
	.resume		= mmpcam_resume,
#endif
	.driver = {
		.name	= "mmp-camera",
		.of_match_table = of_match_ptr(mmpcam_of_match),
	}
};


static int __init mmpcam_init_module(void)
{
	mutex_init(&mmpcam_devices_lock);
	return platform_driver_register(&mmpcam_driver);
}

static void __exit mmpcam_exit_module(void)
{
	platform_driver_unregister(&mmpcam_driver);
	/*
	 * platform_driver_unregister() should have emptied the list
	 */
	if (!list_empty(&mmpcam_devices))
		printk(KERN_ERR "mmp_camera leaving devices behind\n");
}

module_init(mmpcam_init_module);
module_exit(mmpcam_exit_module);
