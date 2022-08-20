/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Core pinctrl/GPIO driver for Intel GPIO controllers
 *
 * Copyright (C) 2015, Intel Corporation
 * Authors: Mathias Nyman <mathias.nyman@linux.intel.com>
 *          Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#ifndef PINCTRL_INTEL_H
#define PINCTRL_INTEL_H

#include <linux/pm.h>

struct pinctrl_pin_desc;
struct platform_device;
struct device;

/**
 * struct intel_pingroup - Description about group of pins
 * @name: Name of the groups
 * @pins: All pins in this group
 * @npins: Number of pins in this groups
 * @mode: Native mode in which the group is muxed out @pins. Used if @modes
 *        is %NULL.
 * @modes: If not %NULL this will hold mode for each pin in @pins
 */
struct intel_pingroup {
	const char *name;
	const unsigned int *pins;
	size_t npins;
	unsigned short mode;
	const unsigned int *modes;
};

/**
 * struct intel_function - Description about a function
 * @name: Name of the function
 * @groups: An array of groups for this function
 * @ngroups: Number of groups in @groups
 */
struct intel_function {
	const char *name;
	const char * const *groups;
	size_t ngroups;
};

/**
 * struct intel_padgroup - Hardware pad group information
 * @reg_num: GPI_IS register number
 * @base: Starting pin of this group
 * @size: Size of this group (maximum is 32).
 * @gpio_base: Starting GPIO base of this group (%0 if matches with @base,
 *	       and %-1 if no GPIO mapping should be created)
 * @padown_num: PAD_OWN register number (assigned by the core driver)
 *
 * If pad groups of a community are not the same size, use this structure
 * to specify them.
 */
struct intel_padgroup {
	unsigned int reg_num;
	unsigned int base;
	unsigned int size;
	int gpio_base;
	unsigned int padown_num;
};

/**
 * struct intel_community - Intel pin community description
 * @barno: MMIO BAR number where registers for this community reside
 * @padown_offset: Register offset of PAD_OWN register from @regs. If %0
 *                 then there is no support for owner.
 * @padcfglock_offset: Register offset of PADCFGLOCK from @regs. If %0 then
 *                     locking is not supported.
 * @hostown_offset: Register offset of HOSTSW_OWN from @regs. If %0 then it
 *                  is assumed that the host owns the pin (rather than
 *                  ACPI).
 * @is_offset: Register offset of GPI_IS from @regs.
 * @ie_offset: Register offset of GPI_IE from @regs.
 * @features: Additional features supported by the hardware
 * @pin_base: Starting pin of pins in this community
 * @gpp_size: Maximum number of pads in each group, such as PADCFGLOCK,
 *            HOSTSW_OWN,  GPI_IS, GPI_IE, etc. Used when @gpps is %NULL.
 * @gpp_num_padown_regs: Number of pad registers each pad group consumes at
 *			 minimum. Use %0 if the number of registers can be
 *			 determined by the size of the group.
 * @npins: Number of pins in this community
 * @gpps: Pad groups if the controller has variable size pad groups
 * @ngpps: Number of pad groups in this community
 * @pad_map: Optional non-linear mapping of the pads
 * @regs: Community specific common registers (reserved for core driver)
 * @pad_regs: Community specific pad registers (reserved for core driver)
 *
 * Most Intel GPIO host controllers this driver supports each pad group is
 * of equal size (except the last one). In that case the driver can just
 * fill in @gpp_size field and let the core driver to handle the rest. If
 * the controller has pad groups of variable size the client driver can
 * pass custom @gpps and @ngpps instead.
 */
struct intel_community {
	unsigned int barno;
	unsigned int padown_offset;
	unsigned int padcfglock_offset;
	unsigned int hostown_offset;
	unsigned int is_offset;
	unsigned int ie_offset;
	unsigned int features;
	unsigned int pin_base;
	unsigned int gpp_size;
	unsigned int gpp_num_padown_regs;
	size_t npins;
	const struct intel_padgroup *gpps;
	size_t ngpps;
	const unsigned int *pad_map;
	/* Reserved for the core driver */
	void __iomem *regs;
	void __iomem *pad_regs;
};

/* Additional features supported by the hardware */
#define PINCTRL_FEATURE_DEBOUNCE	BIT(0)
#define PINCTRL_FEATURE_1K_PD		BIT(1)

/**
 * PIN_GROUP - Declare a pin group
 * @n: Name of the group
 * @p: An array of pins this group consists
 * @m: Mode which the pins are put when this group is active. Can be either
 *     a single integer or an array of integers in which case mode is per
 *     pin.
 */
#define PIN_GROUP(n, p, m)					\
	{							\
		.name = (n),					\
		.pins = (p),					\
		.npins = ARRAY_SIZE((p)),			\
		.mode = __builtin_choose_expr(			\
			__builtin_constant_p((m)), (m), 0),	\
		.modes = __builtin_choose_expr(			\
			__builtin_constant_p((m)), NULL, (m)),	\
	}

#define FUNCTION(n, g)				\
	{					\
		.name = (n),			\
		.groups = (g),			\
		.ngroups = ARRAY_SIZE((g)),	\
	}

/**
 * struct intel_pinctrl_soc_data - Intel pin controller per-SoC configuration
 * @uid: ACPI _UID for the probe driver use if needed
 * @pins: Array if pins this pinctrl controls
 * @npins: Number of pins in the array
 * @groups: Array of pin groups
 * @ngroups: Number of groups in the array
 * @functions: Array of functions
 * @nfunctions: Number of functions in the array
 * @communities: Array of communities this pinctrl handles
 * @ncommunities: Number of communities in the array
 *
 * The @communities is used as a template by the core driver. It will make
 * copy of all communities and fill in rest of the information.
 */
struct intel_pinctrl_soc_data {
	const char *uid;
	const struct pinctrl_pin_desc *pins;
	size_t npins;
	const struct intel_pingroup *groups;
	size_t ngroups;
	const struct intel_function *functions;
	size_t nfunctions;
	const struct intel_community *communities;
	size_t ncommunities;
};

int intel_pinctrl_probe_by_hid(struct platform_device *pdev);
int intel_pinctrl_probe_by_uid(struct platform_device *pdev);

#ifdef CONFIG_PM_SLEEP
int intel_pinctrl_suspend_noirq(struct device *dev);
int intel_pinctrl_resume_noirq(struct device *dev);
#endif

#define INTEL_PINCTRL_PM_OPS(_name)					\
const struct dev_pm_ops _name = {					\
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(intel_pinctrl_suspend_noirq,	\
				      intel_pinctrl_resume_noirq)	\
}

#endif /* PINCTRL_INTEL_H */
