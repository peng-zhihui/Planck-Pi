/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IRQ is a type of interrupt controller used on recent Intel SoC.
 *
 * Copyright 2019 Google LLC
 */

#ifndef __irq_H
#define __irq_H

/*
 * Interrupt controller types available. You can find a particular one with
 * irq_first_device_type()
 */
enum irq_dev_t {
	X86_IRQT_BASE,		/* Base controller */
	X86_IRQT_ITSS,		/* ITSS controller, e.g. on APL */
	X86_IRQT_ACPI_GPE,	/* ACPI General-Purpose Events controller */
	SANDBOX_IRQT_BASE,	/* Sandbox testing */
};

/**
 * struct irq - A single irq line handled by an interrupt controller
 *
 * @dev: IRQ device that handles this irq
 * @id: ID to identify this irq with the device
 */
struct irq {
	struct udevice *dev;
	ulong id;
};

/**
 * struct irq_ops - Operations for the IRQ
 *
 * Each IRQ device can handle mulitple IRQ lines
 */
struct irq_ops {
	/**
	 * route_pmc_gpio_gpe() - Get the GPIO for an event
	 *
	 * @dev: IRQ device
	 * @pmc_gpe_num: Event number to check
	 * @returns GPIO for the event, or -ENOENT if none
	 */
	int (*route_pmc_gpio_gpe)(struct udevice *dev, uint pmc_gpe_num);

	/**
	 * set_polarity() - Set the IRQ polarity
	 *
	 * @dev: IRQ device
	 * @irq: Interrupt number to set
	 * @active_low: true if active low, false for active high
	 * @return 0 if OK, -EINVAL if @irq is invalid
	 */
	int (*set_polarity)(struct udevice *dev, uint irq, bool active_low);

	/**
	 * snapshot_polarities() - record IRQ polarities for later restore
	 *
	 * @dev: IRQ device
	 * @return 0
	 */
	int (*snapshot_polarities)(struct udevice *dev);

	/**
	 * restore_polarities() - restore IRQ polarities
	 *
	 * @dev: IRQ device
	 * @return 0
	 */
	int (*restore_polarities)(struct udevice *dev);

	/**
	 * read_and_clear() - get the value of an interrupt and clear it
	 *
	 * Clears the interrupt if pending
	 *
	 * @irq: IRQ line
	 * @return 0 if interrupt is not pending, 1 if it was (and so has been
	 *	cleared), -ve on error
	 */
	int (*read_and_clear)(struct irq *irq);
	/**
	 * of_xlate - Translate a client's device-tree (OF) irq specifier.
	 *
	 * The irq core calls this function as the first step in implementing
	 * a client's irq_get_by_*() call.
	 *
	 * If this function pointer is set to NULL, the irq core will use a
	 * default implementation, which assumes #interrupt-cells = <1>, and
	 * that the DT cell contains a simple integer irq ID.
	 *
	 * @irq:	The irq struct to hold the translation result.
	 * @args:	The irq specifier values from device tree.
	 * @return 0 if OK, or a negative error code.
	 */
	int (*of_xlate)(struct irq *irq, struct ofnode_phandle_args *args);
	/**
	 * request - Request a translated irq.
	 *
	 * The irq core calls this function as the second step in
	 * implementing a client's irq_get_by_*() call, following a successful
	 * xxx_xlate() call, or as the only step in implementing a client's
	 * irq_request() call.
	 *
	 * @irq:	The irq struct to request; this has been fille in by
	 *		a previoux xxx_xlate() function call, or by the caller
	 *		of irq_request().
	 * @return 0 if OK, or a negative error code.
	 */
	int (*request)(struct irq *irq);
	/**
	 * free - Free a previously requested irq.
	 *
	 * This is the implementation of the client irq_free() API.
	 *
	 * @irq:	The irq to free.
	 * @return 0 if OK, or a negative error code.
	 */
	int (*free)(struct irq *irq);
};

#define irq_get_ops(dev)	((struct irq_ops *)(dev)->driver->ops)

/**
 * irq_route_pmc_gpio_gpe() - Get the GPIO for an event
 *
 * @dev: IRQ device
 * @pmc_gpe_num: Event number to check
 * @returns GPIO for the event, or -ENOENT if none
 */
int irq_route_pmc_gpio_gpe(struct udevice *dev, uint pmc_gpe_num);

/**
 * irq_set_polarity() - Set the IRQ polarity
 *
 * @dev: IRQ device
 * @irq: Interrupt number to set
 * @active_low: true if active low, false for active high
 * @return 0 if OK, -EINVAL if @irq is invalid
 */
int irq_set_polarity(struct udevice *dev, uint irq, bool active_low);

/**
 * irq_snapshot_polarities() - record IRQ polarities for later restore
 *
 * @dev: IRQ device
 * @return 0
 */
int irq_snapshot_polarities(struct udevice *dev);

/**
 * irq_restore_polarities() - restore IRQ polarities
 *
 * @dev: IRQ device
 * @return 0
 */
int irq_restore_polarities(struct udevice *dev);

/**
 * read_and_clear() - get the value of an interrupt and clear it
 *
 * Clears the interrupt if pending
 *
 * @dev: IRQ device
 * @return 0 if interrupt is not pending, 1 if it was (and so has been
 *	cleared), -ve on error
 */
int irq_read_and_clear(struct irq *irq);

/**
 * irq_get_by_index - Get/request an irq by integer index.
 *
 * This looks up and requests an irq. The index is relative to the client
 * device; each device is assumed to have n irqs associated with it somehow,
 * and this function finds and requests one of them. The mapping of client
 * device irq indices to provider irqs may be via device-tree
 * properties, board-provided mapping tables, or some other mechanism.
 *
 * @dev:	The client device.
 * @index:	The index of the irq to request, within the client's list of
 *		irqs.
 * @irq:	A pointer to a irq struct to initialise.
 * @return 0 if OK, or a negative error code.
 */
int irq_get_by_index(struct udevice *dev, int index, struct irq *irq);

/**
 * irq_request - Request a irq by provider-specific ID.
 *
 * This requests a irq using a provider-specific ID. Generally, this function
 * should not be used, since irq_get_by_index/name() provide an interface that
 * better separates clients from intimate knowledge of irq providers.
 * However, this function may be useful in core SoC-specific code.
 *
 * @dev:	The irq provider device.
 * @irq:	A pointer to a irq struct to initialise. The caller must
 *		have already initialised any field in this struct which the
 *		irq provider uses to identify the irq.
 * @return 0 if OK, or a negative error code.
 */
int irq_request(struct udevice *dev, struct irq *irq);

/**
 * irq_free - Free a previously requested irq.
 *
 * @irq:	A irq struct that was previously successfully requested by
 *		irq_request/get_by_*().
 * @return 0 if OK, or a negative error code.
 */
int irq_free(struct irq *irq);

/**
 * irq_first_device_type() - Get a particular interrupt controller
 *
 * On success this returns an activated interrupt device.
 *
 * @type: Type to find
 * @devp: Returns the device, if found
 * @return 0 if OK, -ENODEV if not found, other -ve error if uclass failed to
 *	probe
 */
int irq_first_device_type(enum irq_dev_t type, struct udevice **devp);

#endif
