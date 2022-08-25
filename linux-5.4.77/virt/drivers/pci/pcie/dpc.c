// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Downstream Port Containment services driver
 * Author: Keith Busch <keith.busch@intel.com>
 *
 * Copyright (C) 2016 Intel Corp.
 */

#define dev_fmt(fmt) "DPC: " fmt

#include <linux/aer.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/pci.h>

#include "portdrv.h"
#include "../pci.h"

struct dpc_dev {
	struct pcie_device	*dev;
	u16			cap_pos;
	bool			rp_extensions;
	u8			rp_log_size;
};

static const char * const rp_pio_error_string[] = {
	"Configuration Request received UR Completion",	 /* Bit Position 0  */
	"Configuration Request received CA Completion",	 /* Bit Position 1  */
	"Configuration Request Completion Timeout",	 /* Bit Position 2  */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"I/O Request received UR Completion",		 /* Bit Position 8  */
	"I/O Request received CA Completion",		 /* Bit Position 9  */
	"I/O Request Completion Timeout",		 /* Bit Position 10 */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"Memory Request received UR Completion",	 /* Bit Position 16 */
	"Memory Request received CA Completion",	 /* Bit Position 17 */
	"Memory Request Completion Timeout",		 /* Bit Position 18 */
};

static struct dpc_dev *to_dpc_dev(struct pci_dev *dev)
{
	struct device *device;

	device = pcie_port_find_device(dev, PCIE_PORT_SERVICE_DPC);
	if (!device)
		return NULL;
	return get_service_data(to_pcie_device(device));
}

void pci_save_dpc_state(struct pci_dev *dev)
{
	struct dpc_dev *dpc;
	struct pci_cap_saved_state *save_state;
	u16 *cap;

	if (!pci_is_pcie(dev))
		return;

	dpc = to_dpc_dev(dev);
	if (!dpc)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_DPC);
	if (!save_state)
		return;

	cap = (u16 *)&save_state->cap.data[0];
	pci_read_config_word(dev, dpc->cap_pos + PCI_EXP_DPC_CTL, cap);
}

void pci_restore_dpc_state(struct pci_dev *dev)
{
	struct dpc_dev *dpc;
	struct pci_cap_saved_state *save_state;
	u16 *cap;

	if (!pci_is_pcie(dev))
		return;

	dpc = to_dpc_dev(dev);
	if (!dpc)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_DPC);
	if (!save_state)
		return;

	cap = (u16 *)&save_state->cap.data[0];
	pci_write_config_word(dev, dpc->cap_pos + PCI_EXP_DPC_CTL, *cap);
}

static int dpc_wait_rp_inactive(struct dpc_dev *dpc)
{
	unsigned long timeout = jiffies + HZ;
	struct pci_dev *pdev = dpc->dev->port;
	u16 cap = dpc->cap_pos, status;

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);
	while (status & PCI_EXP_DPC_RP_BUSY &&
					!time_after(jiffies, timeout)) {
		msleep(10);
		pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);
	}
	if (status & PCI_EXP_DPC_RP_BUSY) {
		pci_warn(pdev, "root port still busy\n");
		return -EBUSY;
	}
	return 0;
}

static pci_ers_result_t dpc_reset_link(struct pci_dev *pdev)
{
	struct dpc_dev *dpc;
	u16 cap;

	/*
	 * DPC disables the Link automatically in hardware, so it has
	 * already been reset by the time we get here.
	 */
	dpc = to_dpc_dev(pdev);
	cap = dpc->cap_pos;

	/*
	 * Wait until the Link is inactive, then clear DPC Trigger Status
	 * to allow the Port to leave DPC.
	 */
	pcie_wait_for_link(pdev, false);

	if (dpc->rp_extensions && dpc_wait_rp_inactive(dpc))
		return PCI_ERS_RESULT_DISCONNECT;

	pci_write_config_word(pdev, cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_TRIGGER);

	if (!pcie_wait_for_link(pdev, true))
		return PCI_ERS_RESULT_DISCONNECT;

	return PCI_ERS_RESULT_RECOVERED;
}

static void dpc_process_rp_pio_error(struct dpc_dev *dpc)
{
	struct pci_dev *pdev = dpc->dev->port;
	u16 cap = dpc->cap_pos, dpc_status, first_error;
	u32 status, mask, sev, syserr, exc, dw0, dw1, dw2, dw3, log, prefix;
	int i;

	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_STATUS, &status);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_MASK, &mask);
	pci_err(pdev, "rp_pio_status: %#010x, rp_pio_mask: %#010x\n",
		status, mask);

	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_SEVERITY, &sev);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_SYSERROR, &syserr);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_EXCEPTION, &exc);
	pci_err(pdev, "RP PIO severity=%#010x, syserror=%#010x, exception=%#010x\n",
		sev, syserr, exc);

	/* Get First Error Pointer */
	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &dpc_status);
	first_error = (dpc_status & 0x1f00) >> 8;

	for (i = 0; i < ARRAY_SIZE(rp_pio_error_string); i++) {
		if ((status & ~mask) & (1 << i))
			pci_err(pdev, "[%2d] %s%s\n", i, rp_pio_error_string[i],
				first_error == i ? " (First)" : "");
	}

	if (dpc->rp_log_size < 4)
		goto clear_status;
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_HEADER_LOG,
			      &dw0);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_HEADER_LOG + 4,
			      &dw1);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_HEADER_LOG + 8,
			      &dw2);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_HEADER_LOG + 12,
			      &dw3);
	pci_err(pdev, "TLP Header: %#010x %#010x %#010x %#010x\n",
		dw0, dw1, dw2, dw3);

	if (dpc->rp_log_size < 5)
		goto clear_status;
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_IMPSPEC_LOG, &log);
	pci_err(pdev, "RP PIO ImpSpec Log %#010x\n", log);

	for (i = 0; i < dpc->rp_log_size - 5; i++) {
		pci_read_config_dword(pdev,
			cap + PCI_EXP_DPC_RP_PIO_TLPPREFIX_LOG, &prefix);
		pci_err(pdev, "TLP Prefix Header: dw%d, %#010x\n", i, prefix);
	}
 clear_status:
	pci_write_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_STATUS, status);
}

static int dpc_get_aer_uncorrect_severity(struct pci_dev *dev,
					  struct aer_err_info *info)
{
	int pos = dev->aer_cap;
	u32 status, mask, sev;

	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_STATUS, &status);
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, &mask);
	status &= ~mask;
	if (!status)
		return 0;

	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, &sev);
	status &= sev;
	if (status)
		info->severity = AER_FATAL;
	else
		info->severity = AER_NONFATAL;

	return 1;
}

static irqreturn_t dpc_handler(int irq, void *context)
{
	struct aer_err_info info;
	struct dpc_dev *dpc = context;
	struct pci_dev *pdev = dpc->dev->port;
	u16 cap = dpc->cap_pos, status, source, reason, ext_reason;

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);
	pci_read_config_word(pdev, cap + PCI_EXP_DPC_SOURCE_ID, &source);

	pci_info(pdev, "containment event, status:%#06x source:%#06x\n",
		 status, source);

	reason = (status & PCI_EXP_DPC_STATUS_TRIGGER_RSN) >> 1;
	ext_reason = (status & PCI_EXP_DPC_STATUS_TRIGGER_RSN_EXT) >> 5;
	pci_warn(pdev, "%s detected\n",
		 (reason == 0) ? "unmasked uncorrectable error" :
		 (reason == 1) ? "ERR_NONFATAL" :
		 (reason == 2) ? "ERR_FATAL" :
		 (ext_reason == 0) ? "RP PIO error" :
		 (ext_reason == 1) ? "software trigger" :
				     "reserved error");

	/* show RP PIO error detail information */
	if (dpc->rp_extensions && reason == 3 && ext_reason == 0)
		dpc_process_rp_pio_error(dpc);
	else if (reason == 0 &&
		 dpc_get_aer_uncorrect_severity(pdev, &info) &&
		 aer_get_device_error_info(pdev, &info)) {
		aer_print_error(pdev, &info);
		pci_cleanup_aer_uncorrect_error_status(pdev);
		pci_aer_clear_fatal_status(pdev);
	}

	/* We configure DPC so it only triggers on ERR_FATAL */
	pcie_do_recovery(pdev, pci_channel_io_frozen, PCIE_PORT_SERVICE_DPC);

	return IRQ_HANDLED;
}

static irqreturn_t dpc_irq(int irq, void *context)
{
	struct dpc_dev *dpc = (struct dpc_dev *)context;
	struct pci_dev *pdev = dpc->dev->port;
	u16 cap = dpc->cap_pos, status;

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);

	if (!(status & PCI_EXP_DPC_STATUS_INTERRUPT) || status == (u16)(~0))
		return IRQ_NONE;

	pci_write_config_word(pdev, cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_INTERRUPT);
	if (status & PCI_EXP_DPC_STATUS_TRIGGER)
		return IRQ_WAKE_THREAD;
	return IRQ_HANDLED;
}

#define FLAG(x, y) (((x) & (y)) ? '+' : '-')
static int dpc_probe(struct pcie_device *dev)
{
	struct dpc_dev *dpc;
	struct pci_dev *pdev = dev->port;
	struct device *device = &dev->device;
	int status;
	u16 ctl, cap;

	if (pcie_aer_get_firmware_first(pdev))
		return -ENOTSUPP;

	dpc = devm_kzalloc(device, sizeof(*dpc), GFP_KERNEL);
	if (!dpc)
		return -ENOMEM;

	dpc->cap_pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DPC);
	dpc->dev = dev;
	set_service_data(dev, dpc);

	status = devm_request_threaded_irq(device, dev->irq, dpc_irq,
					   dpc_handler, IRQF_SHARED,
					   "pcie-dpc", dpc);
	if (status) {
		pci_warn(pdev, "request IRQ%d failed: %d\n", dev->irq,
			 status);
		return status;
	}

	pci_read_config_word(pdev, dpc->cap_pos + PCI_EXP_DPC_CAP, &cap);
	pci_read_config_word(pdev, dpc->cap_pos + PCI_EXP_DPC_CTL, &ctl);

	dpc->rp_extensions = (cap & PCI_EXP_DPC_CAP_RP_EXT);
	if (dpc->rp_extensions) {
		dpc->rp_log_size = (cap & PCI_EXP_DPC_RP_PIO_LOG_SIZE) >> 8;
		if (dpc->rp_log_size < 4 || dpc->rp_log_size > 9) {
			pci_err(pdev, "RP PIO log size %u is invalid\n",
				dpc->rp_log_size);
			dpc->rp_log_size = 0;
		}
	}

	ctl = (ctl & 0xfff4) | PCI_EXP_DPC_CTL_EN_FATAL | PCI_EXP_DPC_CTL_INT_EN;
	pci_write_config_word(pdev, dpc->cap_pos + PCI_EXP_DPC_CTL, ctl);

	pci_info(pdev, "error containment capabilities: Int Msg #%d, RPExt%c PoisonedTLP%c SwTrigger%c RP PIO Log %d, DL_ActiveErr%c\n",
		 cap & PCI_EXP_DPC_IRQ, FLAG(cap, PCI_EXP_DPC_CAP_RP_EXT),
		 FLAG(cap, PCI_EXP_DPC_CAP_POISONED_TLP),
		 FLAG(cap, PCI_EXP_DPC_CAP_SW_TRIGGER), dpc->rp_log_size,
		 FLAG(cap, PCI_EXP_DPC_CAP_DL_ACTIVE));

	pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_DPC, sizeof(u16));
	return status;
}

static void dpc_remove(struct pcie_device *dev)
{
	struct dpc_dev *dpc = get_service_data(dev);
	struct pci_dev *pdev = dev->port;
	u16 ctl;

	pci_read_config_word(pdev, dpc->cap_pos + PCI_EXP_DPC_CTL, &ctl);
	ctl &= ~(PCI_EXP_DPC_CTL_EN_FATAL | PCI_EXP_DPC_CTL_INT_EN);
	pci_write_config_word(pdev, dpc->cap_pos + PCI_EXP_DPC_CTL, ctl);
}

static struct pcie_port_service_driver dpcdriver = {
	.name		= "dpc",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_DPC,
	.probe		= dpc_probe,
	.remove		= dpc_remove,
	.reset_link	= dpc_reset_link,
};

int __init pcie_dpc_init(void)
{
	return pcie_port_service_register(&dpcdriver);
}
