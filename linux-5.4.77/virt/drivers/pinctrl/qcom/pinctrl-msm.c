// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013, Sony Mobile Communications AB.
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/slab.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/log2.h>

#include "../core.h"
#include "../pinconf.h"
#include "pinctrl-msm.h"
#include "../pinctrl-utils.h"

#define MAX_NR_GPIO 300
#define MAX_NR_TILES 4
#define PS_HOLD_OFFSET 0x820

/**
 * struct msm_pinctrl - state for a pinctrl-msm device
 * @dev:            device handle.
 * @pctrl:          pinctrl handle.
 * @chip:           gpiochip handle.
 * @restart_nb:     restart notifier block.
 * @irq:            parent irq for the TLMM irq_chip.
 * @lock:           Spinlock to protect register resources as well
 *                  as msm_pinctrl data structures.
 * @enabled_irqs:   Bitmap of currently enabled irqs.
 * @dual_edge_irqs: Bitmap of irqs that need sw emulated dual edge
 *                  detection.
 * @soc;            Reference to soc_data of platform specific data.
 * @regs:           Base addresses for the TLMM tiles.
 */
struct msm_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctrl;
	struct gpio_chip chip;
	struct pinctrl_desc desc;
	struct notifier_block restart_nb;

	struct irq_chip irq_chip;
	int irq;

	raw_spinlock_t lock;

	DECLARE_BITMAP(dual_edge_irqs, MAX_NR_GPIO);
	DECLARE_BITMAP(enabled_irqs, MAX_NR_GPIO);

	const struct msm_pinctrl_soc_data *soc;
	void __iomem *regs[MAX_NR_TILES];
};

#define MSM_ACCESSOR(name) \
static u32 msm_readl_##name(struct msm_pinctrl *pctrl, \
			    const struct msm_pingroup *g) \
{ \
	return readl(pctrl->regs[g->tile] + g->name##_reg); \
} \
static void msm_writel_##name(u32 val, struct msm_pinctrl *pctrl, \
			      const struct msm_pingroup *g) \
{ \
	writel(val, pctrl->regs[g->tile] + g->name##_reg); \
}

MSM_ACCESSOR(ctl)
MSM_ACCESSOR(io)
MSM_ACCESSOR(intr_cfg)
MSM_ACCESSOR(intr_status)
MSM_ACCESSOR(intr_target)

static int msm_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->soc->ngroups;
}

static const char *msm_get_group_name(struct pinctrl_dev *pctldev,
				      unsigned group)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->soc->groups[group].name;
}

static int msm_get_group_pins(struct pinctrl_dev *pctldev,
			      unsigned group,
			      const unsigned **pins,
			      unsigned *num_pins)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	*pins = pctrl->soc->groups[group].pins;
	*num_pins = pctrl->soc->groups[group].npins;
	return 0;
}

static const struct pinctrl_ops msm_pinctrl_ops = {
	.get_groups_count	= msm_get_groups_count,
	.get_group_name		= msm_get_group_name,
	.get_group_pins		= msm_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_group,
	.dt_free_map		= pinctrl_utils_free_map,
};

static int msm_pinmux_request(struct pinctrl_dev *pctldev, unsigned offset)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	struct gpio_chip *chip = &pctrl->chip;

	return gpiochip_line_is_valid(chip, offset) ? 0 : -EINVAL;
}

static int msm_get_functions_count(struct pinctrl_dev *pctldev)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->soc->nfunctions;
}

static const char *msm_get_function_name(struct pinctrl_dev *pctldev,
					 unsigned function)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->soc->functions[function].name;
}

static int msm_get_function_groups(struct pinctrl_dev *pctldev,
				   unsigned function,
				   const char * const **groups,
				   unsigned * const num_groups)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	*groups = pctrl->soc->functions[function].groups;
	*num_groups = pctrl->soc->functions[function].ngroups;
	return 0;
}

static int msm_pinmux_set_mux(struct pinctrl_dev *pctldev,
			      unsigned function,
			      unsigned group)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	const struct msm_pingroup *g;
	unsigned long flags;
	u32 val, mask;
	int i;

	g = &pctrl->soc->groups[group];
	mask = GENMASK(g->mux_bit + order_base_2(g->nfuncs) - 1, g->mux_bit);

	for (i = 0; i < g->nfuncs; i++) {
		if (g->funcs[i] == function)
			break;
	}

	if (WARN_ON(i == g->nfuncs))
		return -EINVAL;

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_ctl(pctrl, g);
	val &= ~mask;
	val |= i << g->mux_bit;
	msm_writel_ctl(val, pctrl, g);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int msm_pinmux_request_gpio(struct pinctrl_dev *pctldev,
				   struct pinctrl_gpio_range *range,
				   unsigned offset)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	const struct msm_pingroup *g = &pctrl->soc->groups[offset];

	/* No funcs? Probably ACPI so can't do anything here */
	if (!g->nfuncs)
		return 0;

	/* For now assume function 0 is GPIO because it always is */
	return msm_pinmux_set_mux(pctldev, g->funcs[0], offset);
}

static const struct pinmux_ops msm_pinmux_ops = {
	.request		= msm_pinmux_request,
	.get_functions_count	= msm_get_functions_count,
	.get_function_name	= msm_get_function_name,
	.get_function_groups	= msm_get_function_groups,
	.gpio_request_enable	= msm_pinmux_request_gpio,
	.set_mux		= msm_pinmux_set_mux,
};

static int msm_config_reg(struct msm_pinctrl *pctrl,
			  const struct msm_pingroup *g,
			  unsigned param,
			  unsigned *mask,
			  unsigned *bit)
{
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_BUS_HOLD:
	case PIN_CONFIG_BIAS_PULL_UP:
		*bit = g->pull_bit;
		*mask = 3;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		*bit = g->drv_bit;
		*mask = 7;
		break;
	case PIN_CONFIG_OUTPUT:
	case PIN_CONFIG_INPUT_ENABLE:
		*bit = g->oe_bit;
		*mask = 1;
		break;
	default:
		return -ENOTSUPP;
	}

	return 0;
}

#define MSM_NO_PULL		0
#define MSM_PULL_DOWN		1
#define MSM_KEEPER		2
#define MSM_PULL_UP_NO_KEEPER	2
#define MSM_PULL_UP		3

static unsigned msm_regval_to_drive(u32 val)
{
	return (val + 1) * 2;
}

static int msm_config_group_get(struct pinctrl_dev *pctldev,
				unsigned int group,
				unsigned long *config)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	unsigned param = pinconf_to_config_param(*config);
	unsigned mask;
	unsigned arg;
	unsigned bit;
	int ret;
	u32 val;

	g = &pctrl->soc->groups[group];

	ret = msm_config_reg(pctrl, g, param, &mask, &bit);
	if (ret < 0)
		return ret;

	val = msm_readl_ctl(pctrl, g);
	arg = (val >> bit) & mask;

	/* Convert register value to pinconf value */
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		if (arg != MSM_NO_PULL)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (arg != MSM_PULL_DOWN)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_BIAS_BUS_HOLD:
		if (pctrl->soc->pull_no_keeper)
			return -ENOTSUPP;

		if (arg != MSM_KEEPER)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (pctrl->soc->pull_no_keeper)
			arg = arg == MSM_PULL_UP_NO_KEEPER;
		else
			arg = arg == MSM_PULL_UP;
		if (!arg)
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		arg = msm_regval_to_drive(arg);
		break;
	case PIN_CONFIG_OUTPUT:
		/* Pin is not output */
		if (!arg)
			return -EINVAL;

		val = msm_readl_io(pctrl, g);
		arg = !!(val & BIT(g->in_bit));
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		/* Pin is output */
		if (arg)
			return -EINVAL;
		arg = 1;
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int msm_config_group_set(struct pinctrl_dev *pctldev,
				unsigned group,
				unsigned long *configs,
				unsigned num_configs)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	unsigned long flags;
	unsigned param;
	unsigned mask;
	unsigned arg;
	unsigned bit;
	int ret;
	u32 val;
	int i;

	g = &pctrl->soc->groups[group];

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		ret = msm_config_reg(pctrl, g, param, &mask, &bit);
		if (ret < 0)
			return ret;

		/* Convert pinconf values to register values */
		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			arg = MSM_NO_PULL;
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			arg = MSM_PULL_DOWN;
			break;
		case PIN_CONFIG_BIAS_BUS_HOLD:
			if (pctrl->soc->pull_no_keeper)
				return -ENOTSUPP;

			arg = MSM_KEEPER;
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			if (pctrl->soc->pull_no_keeper)
				arg = MSM_PULL_UP_NO_KEEPER;
			else
				arg = MSM_PULL_UP;
			break;
		case PIN_CONFIG_DRIVE_STRENGTH:
			/* Check for invalid values */
			if (arg > 16 || arg < 2 || (arg % 2) != 0)
				arg = -1;
			else
				arg = (arg / 2) - 1;
			break;
		case PIN_CONFIG_OUTPUT:
			/* set output value */
			raw_spin_lock_irqsave(&pctrl->lock, flags);
			val = msm_readl_io(pctrl, g);
			if (arg)
				val |= BIT(g->out_bit);
			else
				val &= ~BIT(g->out_bit);
			msm_writel_io(val, pctrl, g);
			raw_spin_unlock_irqrestore(&pctrl->lock, flags);

			/* enable output */
			arg = 1;
			break;
		case PIN_CONFIG_INPUT_ENABLE:
			/* disable output */
			arg = 0;
			break;
		default:
			dev_err(pctrl->dev, "Unsupported config parameter: %x\n",
				param);
			return -EINVAL;
		}

		/* Range-check user-supplied value */
		if (arg & ~mask) {
			dev_err(pctrl->dev, "config %x: %x is invalid\n", param, arg);
			return -EINVAL;
		}

		raw_spin_lock_irqsave(&pctrl->lock, flags);
		val = msm_readl_ctl(pctrl, g);
		val &= ~(mask << bit);
		val |= arg << bit;
		msm_writel_ctl(val, pctrl, g);
		raw_spin_unlock_irqrestore(&pctrl->lock, flags);
	}

	return 0;
}

static const struct pinconf_ops msm_pinconf_ops = {
	.is_generic		= true,
	.pin_config_group_get	= msm_config_group_get,
	.pin_config_group_set	= msm_config_group_set,
};

static int msm_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[offset];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_ctl(pctrl, g);
	val &= ~BIT(g->oe_bit);
	msm_writel_ctl(val, pctrl, g);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int msm_gpio_direction_output(struct gpio_chip *chip, unsigned offset, int value)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[offset];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_io(pctrl, g);
	if (value)
		val |= BIT(g->out_bit);
	else
		val &= ~BIT(g->out_bit);
	msm_writel_io(val, pctrl, g);

	val = msm_readl_ctl(pctrl, g);
	val |= BIT(g->oe_bit);
	msm_writel_ctl(val, pctrl, g);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int msm_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	u32 val;

	g = &pctrl->soc->groups[offset];

	val = msm_readl_ctl(pctrl, g);

	/* 0 = output, 1 = input */
	return val & BIT(g->oe_bit) ? 0 : 1;
}

static int msm_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	u32 val;

	g = &pctrl->soc->groups[offset];

	val = msm_readl_io(pctrl, g);
	return !!(val & BIT(g->in_bit));
}

static void msm_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[offset];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_io(pctrl, g);
	if (value)
		val |= BIT(g->out_bit);
	else
		val &= ~BIT(g->out_bit);
	msm_writel_io(val, pctrl, g);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

#ifdef CONFIG_DEBUG_FS
#include <linux/seq_file.h>

static void msm_gpio_dbg_show_one(struct seq_file *s,
				  struct pinctrl_dev *pctldev,
				  struct gpio_chip *chip,
				  unsigned offset,
				  unsigned gpio)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned func;
	int is_out;
	int drive;
	int pull;
	int val;
	u32 ctl_reg, io_reg;

	static const char * const pulls_keeper[] = {
		"no pull",
		"pull down",
		"keeper",
		"pull up"
	};

	static const char * const pulls_no_keeper[] = {
		"no pull",
		"pull down",
		"pull up",
	};

	if (!gpiochip_line_is_valid(chip, offset))
		return;

	g = &pctrl->soc->groups[offset];
	ctl_reg = msm_readl_ctl(pctrl, g);
	io_reg = msm_readl_io(pctrl, g);

	is_out = !!(ctl_reg & BIT(g->oe_bit));
	func = (ctl_reg >> g->mux_bit) & 7;
	drive = (ctl_reg >> g->drv_bit) & 7;
	pull = (ctl_reg >> g->pull_bit) & 3;

	if (is_out)
		val = !!(io_reg & BIT(g->out_bit));
	else
		val = !!(io_reg & BIT(g->in_bit));

	seq_printf(s, " %-8s: %-3s", g->name, is_out ? "out" : "in");
	seq_printf(s, " %-4s func%d", val ? "high" : "low", func);
	seq_printf(s, " %dmA", msm_regval_to_drive(drive));
	if (pctrl->soc->pull_no_keeper)
		seq_printf(s, " %s", pulls_no_keeper[pull]);
	else
		seq_printf(s, " %s", pulls_keeper[pull]);
	seq_puts(s, "\n");
}

static void msm_gpio_dbg_show(struct seq_file *s, struct gpio_chip *chip)
{
	unsigned gpio = chip->base;
	unsigned i;

	for (i = 0; i < chip->ngpio; i++, gpio++)
		msm_gpio_dbg_show_one(s, NULL, chip, i, gpio);
}

#else
#define msm_gpio_dbg_show NULL
#endif

static int msm_gpio_init_valid_mask(struct gpio_chip *gc,
				    unsigned long *valid_mask,
				    unsigned int ngpios)
{
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	int ret;
	unsigned int len, i;
	const int *reserved = pctrl->soc->reserved_gpios;
	u16 *tmp;

	/* Driver provided reserved list overrides DT and ACPI */
	if (reserved) {
		bitmap_fill(valid_mask, ngpios);
		for (i = 0; reserved[i] >= 0; i++) {
			if (i >= ngpios || reserved[i] >= ngpios) {
				dev_err(pctrl->dev, "invalid list of reserved GPIOs\n");
				return -EINVAL;
			}
			clear_bit(reserved[i], valid_mask);
		}

		return 0;
	}

	/* The number of GPIOs in the ACPI tables */
	len = ret = device_property_count_u16(pctrl->dev, "gpios");
	if (ret < 0)
		return 0;

	if (ret > ngpios)
		return -EINVAL;

	tmp = kmalloc_array(len, sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	ret = device_property_read_u16_array(pctrl->dev, "gpios", tmp, len);
	if (ret < 0) {
		dev_err(pctrl->dev, "could not read list of GPIOs\n");
		goto out;
	}

	bitmap_zero(valid_mask, ngpios);
	for (i = 0; i < len; i++)
		set_bit(tmp[i], valid_mask);

out:
	kfree(tmp);
	return ret;
}

static const struct gpio_chip msm_gpio_template = {
	.direction_input  = msm_gpio_direction_input,
	.direction_output = msm_gpio_direction_output,
	.get_direction    = msm_gpio_get_direction,
	.get              = msm_gpio_get,
	.set              = msm_gpio_set,
	.request          = gpiochip_generic_request,
	.free             = gpiochip_generic_free,
	.dbg_show         = msm_gpio_dbg_show,
};

/* For dual-edge interrupts in software, since some hardware has no
 * such support:
 *
 * At appropriate moments, this function may be called to flip the polarity
 * settings of both-edge irq lines to try and catch the next edge.
 *
 * The attempt is considered successful if:
 * - the status bit goes high, indicating that an edge was caught, or
 * - the input value of the gpio doesn't change during the attempt.
 * If the value changes twice during the process, that would cause the first
 * test to fail but would force the second, as two opposite
 * transitions would cause a detection no matter the polarity setting.
 *
 * The do-loop tries to sledge-hammer closed the timing hole between
 * the initial value-read and the polarity-write - if the line value changes
 * during that window, an interrupt is lost, the new polarity setting is
 * incorrect, and the first success test will fail, causing a retry.
 *
 * Algorithm comes from Google's msmgpio driver.
 */
static void msm_gpio_update_dual_edge_pos(struct msm_pinctrl *pctrl,
					  const struct msm_pingroup *g,
					  struct irq_data *d)
{
	int loop_limit = 100;
	unsigned val, val2, intstat;
	unsigned pol;

	do {
		val = msm_readl_io(pctrl, g) & BIT(g->in_bit);

		pol = msm_readl_intr_cfg(pctrl, g);
		pol ^= BIT(g->intr_polarity_bit);
		msm_writel_intr_cfg(pol, pctrl, g);

		val2 = msm_readl_io(pctrl, g) & BIT(g->in_bit);
		intstat = msm_readl_intr_status(pctrl, g);
		if (intstat || (val == val2))
			return;
	} while (loop_limit-- > 0);
	dev_err(pctrl->dev, "dual-edge irq failed to stabilize, %#08x != %#08x\n",
		val, val2);
}

static void msm_gpio_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_pingroup *g;
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[d->hwirq];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_intr_cfg(pctrl, g);
	/*
	 * There are two bits that control interrupt forwarding to the CPU. The
	 * RAW_STATUS_EN bit causes the level or edge sensed on the line to be
	 * latched into the interrupt status register when the hardware detects
	 * an irq that it's configured for (either edge for edge type or level
	 * for level type irq). The 'non-raw' status enable bit causes the
	 * hardware to assert the summary interrupt to the CPU if the latched
	 * status bit is set. There's a bug though, the edge detection logic
	 * seems to have a problem where toggling the RAW_STATUS_EN bit may
	 * cause the status bit to latch spuriously when there isn't any edge
	 * so we can't touch that bit for edge type irqs and we have to keep
	 * the bit set anyway so that edges are latched while the line is masked.
	 *
	 * To make matters more complicated, leaving the RAW_STATUS_EN bit
	 * enabled all the time causes level interrupts to re-latch into the
	 * status register because the level is still present on the line after
	 * we ack it. We clear the raw status enable bit during mask here and
	 * set the bit on unmask so the interrupt can't latch into the hardware
	 * while it's masked.
	 */
	if (irqd_get_trigger_type(d) & IRQ_TYPE_LEVEL_MASK)
		val &= ~BIT(g->intr_raw_status_bit);

	val &= ~BIT(g->intr_enable_bit);
	msm_writel_intr_cfg(val, pctrl, g);

	clear_bit(d->hwirq, pctrl->enabled_irqs);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

static void msm_gpio_irq_clear_unmask(struct irq_data *d, bool status_clear)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_pingroup *g;
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[d->hwirq];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	if (status_clear) {
		/*
		 * clear the interrupt status bit before unmask to avoid
		 * any erroneous interrupts that would have got latched
		 * when the interrupt is not in use.
		 */
		val = msm_readl_intr_status(pctrl, g);
		val &= ~BIT(g->intr_status_bit);
		msm_writel_intr_status(val, pctrl, g);
	}

	val = msm_readl_intr_cfg(pctrl, g);
	val |= BIT(g->intr_raw_status_bit);
	val |= BIT(g->intr_enable_bit);
	msm_writel_intr_cfg(val, pctrl, g);

	set_bit(d->hwirq, pctrl->enabled_irqs);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

static void msm_gpio_irq_enable(struct irq_data *d)
{

	msm_gpio_irq_clear_unmask(d, true);
}

static void msm_gpio_irq_unmask(struct irq_data *d)
{
	msm_gpio_irq_clear_unmask(d, false);
}

static void msm_gpio_irq_ack(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_pingroup *g;
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[d->hwirq];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_intr_status(pctrl, g);
	if (g->intr_ack_high)
		val |= BIT(g->intr_status_bit);
	else
		val &= ~BIT(g->intr_status_bit);
	msm_writel_intr_status(val, pctrl, g);

	if (test_bit(d->hwirq, pctrl->dual_edge_irqs))
		msm_gpio_update_dual_edge_pos(pctrl, g, d);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

static int msm_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_pingroup *g;
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[d->hwirq];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	/*
	 * For hw without possibility of detecting both edges
	 */
	if (g->intr_detection_width == 1 && type == IRQ_TYPE_EDGE_BOTH)
		set_bit(d->hwirq, pctrl->dual_edge_irqs);
	else
		clear_bit(d->hwirq, pctrl->dual_edge_irqs);

	/* Route interrupts to application cpu */
	val = msm_readl_intr_target(pctrl, g);
	val &= ~(7 << g->intr_target_bit);
	val |= g->intr_target_kpss_val << g->intr_target_bit;
	msm_writel_intr_target(val, pctrl, g);

	/* Update configuration for gpio.
	 * RAW_STATUS_EN is left on for all gpio irqs. Due to the
	 * internal circuitry of TLMM, toggling the RAW_STATUS
	 * could cause the INTR_STATUS to be set for EDGE interrupts.
	 */
	val = msm_readl_intr_cfg(pctrl, g);
	val |= BIT(g->intr_raw_status_bit);
	if (g->intr_detection_width == 2) {
		val &= ~(3 << g->intr_detection_bit);
		val &= ~(1 << g->intr_polarity_bit);
		switch (type) {
		case IRQ_TYPE_EDGE_RISING:
			val |= 1 << g->intr_detection_bit;
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_EDGE_FALLING:
			val |= 2 << g->intr_detection_bit;
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_EDGE_BOTH:
			val |= 3 << g->intr_detection_bit;
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_LEVEL_LOW:
			break;
		case IRQ_TYPE_LEVEL_HIGH:
			val |= BIT(g->intr_polarity_bit);
			break;
		}
	} else if (g->intr_detection_width == 1) {
		val &= ~(1 << g->intr_detection_bit);
		val &= ~(1 << g->intr_polarity_bit);
		switch (type) {
		case IRQ_TYPE_EDGE_RISING:
			val |= BIT(g->intr_detection_bit);
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_EDGE_FALLING:
			val |= BIT(g->intr_detection_bit);
			break;
		case IRQ_TYPE_EDGE_BOTH:
			val |= BIT(g->intr_detection_bit);
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_LEVEL_LOW:
			break;
		case IRQ_TYPE_LEVEL_HIGH:
			val |= BIT(g->intr_polarity_bit);
			break;
		}
	} else {
		BUG();
	}
	msm_writel_intr_cfg(val, pctrl, g);

	if (test_bit(d->hwirq, pctrl->dual_edge_irqs))
		msm_gpio_update_dual_edge_pos(pctrl, g, d);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		irq_set_handler_locked(d, handle_level_irq);
	else if (type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING))
		irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static int msm_gpio_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	irq_set_irq_wake(pctrl->irq, on);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int msm_gpio_irq_reqres(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	int ret;

	if (!try_module_get(gc->owner))
		return -ENODEV;

	ret = msm_pinmux_request_gpio(pctrl->pctrl, NULL, d->hwirq);
	if (ret)
		goto out;
	msm_gpio_direction_input(gc, d->hwirq);

	if (gpiochip_lock_as_irq(gc, d->hwirq)) {
		dev_err(gc->parent,
			"unable to lock HW IRQ %lu for IRQ\n",
			d->hwirq);
		ret = -EINVAL;
		goto out;
	}
	return 0;
out:
	module_put(gc->owner);
	return ret;
}

static void msm_gpio_irq_relres(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);

	gpiochip_unlock_as_irq(gc, d->hwirq);
	module_put(gc->owner);
}

static void msm_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int irq_pin;
	int handled = 0;
	u32 val;
	int i;

	chained_irq_enter(chip, desc);

	/*
	 * Each pin has it's own IRQ status register, so use
	 * enabled_irq bitmap to limit the number of reads.
	 */
	for_each_set_bit(i, pctrl->enabled_irqs, pctrl->chip.ngpio) {
		g = &pctrl->soc->groups[i];
		val = msm_readl_intr_status(pctrl, g);
		if (val & BIT(g->intr_status_bit)) {
			irq_pin = irq_find_mapping(gc->irq.domain, i);
			generic_handle_irq(irq_pin);
			handled++;
		}
	}

	/* No interrupts were flagged */
	if (handled == 0)
		handle_bad_irq(desc);

	chained_irq_exit(chip, desc);
}

static bool msm_gpio_needs_valid_mask(struct msm_pinctrl *pctrl)
{
	if (pctrl->soc->reserved_gpios)
		return true;

	return device_property_count_u16(pctrl->dev, "gpios") > 0;
}

static int msm_gpio_init(struct msm_pinctrl *pctrl)
{
	struct gpio_chip *chip;
	struct gpio_irq_chip *girq;
	int ret;
	unsigned ngpio = pctrl->soc->ngpios;

	if (WARN_ON(ngpio > MAX_NR_GPIO))
		return -EINVAL;

	chip = &pctrl->chip;
	chip->base = -1;
	chip->ngpio = ngpio;
	chip->label = dev_name(pctrl->dev);
	chip->parent = pctrl->dev;
	chip->owner = THIS_MODULE;
	chip->of_node = pctrl->dev->of_node;
	if (msm_gpio_needs_valid_mask(pctrl))
		chip->init_valid_mask = msm_gpio_init_valid_mask;

	pctrl->irq_chip.name = "msmgpio";
	pctrl->irq_chip.irq_enable = msm_gpio_irq_enable;
	pctrl->irq_chip.irq_mask = msm_gpio_irq_mask;
	pctrl->irq_chip.irq_unmask = msm_gpio_irq_unmask;
	pctrl->irq_chip.irq_ack = msm_gpio_irq_ack;
	pctrl->irq_chip.irq_set_type = msm_gpio_irq_set_type;
	pctrl->irq_chip.irq_set_wake = msm_gpio_irq_set_wake;
	pctrl->irq_chip.irq_request_resources = msm_gpio_irq_reqres;
	pctrl->irq_chip.irq_release_resources = msm_gpio_irq_relres;

	girq = &chip->irq;
	girq->chip = &pctrl->irq_chip;
	girq->parent_handler = msm_gpio_irq_handler;
	girq->num_parents = 1;
	girq->parents = devm_kcalloc(pctrl->dev, 1, sizeof(*girq->parents),
				     GFP_KERNEL);
	if (!girq->parents)
		return -ENOMEM;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_bad_irq;
	girq->parents[0] = pctrl->irq;

	ret = gpiochip_add_data(&pctrl->chip, pctrl);
	if (ret) {
		dev_err(pctrl->dev, "Failed register gpiochip\n");
		return ret;
	}

	/*
	 * For DeviceTree-supported systems, the gpio core checks the
	 * pinctrl's device node for the "gpio-ranges" property.
	 * If it is present, it takes care of adding the pin ranges
	 * for the driver. In this case the driver can skip ahead.
	 *
	 * In order to remain compatible with older, existing DeviceTree
	 * files which don't set the "gpio-ranges" property or systems that
	 * utilize ACPI the driver has to call gpiochip_add_pin_range().
	 */
	if (!of_property_read_bool(pctrl->dev->of_node, "gpio-ranges")) {
		ret = gpiochip_add_pin_range(&pctrl->chip,
			dev_name(pctrl->dev), 0, 0, chip->ngpio);
		if (ret) {
			dev_err(pctrl->dev, "Failed to add pin range\n");
			gpiochip_remove(&pctrl->chip);
			return ret;
		}
	}

	return 0;
}

static int msm_ps_hold_restart(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct msm_pinctrl *pctrl = container_of(nb, struct msm_pinctrl, restart_nb);

	writel(0, pctrl->regs[0] + PS_HOLD_OFFSET);
	mdelay(1000);
	return NOTIFY_DONE;
}

static struct msm_pinctrl *poweroff_pctrl;

static void msm_ps_hold_poweroff(void)
{
	msm_ps_hold_restart(&poweroff_pctrl->restart_nb, 0, NULL);
}

static void msm_pinctrl_setup_pm_reset(struct msm_pinctrl *pctrl)
{
	int i;
	const struct msm_function *func = pctrl->soc->functions;

	for (i = 0; i < pctrl->soc->nfunctions; i++)
		if (!strcmp(func[i].name, "ps_hold")) {
			pctrl->restart_nb.notifier_call = msm_ps_hold_restart;
			pctrl->restart_nb.priority = 128;
			if (register_restart_handler(&pctrl->restart_nb))
				dev_err(pctrl->dev,
					"failed to setup restart handler.\n");
			poweroff_pctrl = pctrl;
			pm_power_off = msm_ps_hold_poweroff;
			break;
		}
}

static __maybe_unused int msm_pinctrl_suspend(struct device *dev)
{
	struct msm_pinctrl *pctrl = dev_get_drvdata(dev);

	return pinctrl_force_sleep(pctrl->pctrl);
}

static __maybe_unused int msm_pinctrl_resume(struct device *dev)
{
	struct msm_pinctrl *pctrl = dev_get_drvdata(dev);

	return pinctrl_force_default(pctrl->pctrl);
}

SIMPLE_DEV_PM_OPS(msm_pinctrl_dev_pm_ops, msm_pinctrl_suspend,
		  msm_pinctrl_resume);

EXPORT_SYMBOL(msm_pinctrl_dev_pm_ops);

int msm_pinctrl_probe(struct platform_device *pdev,
		      const struct msm_pinctrl_soc_data *soc_data)
{
	struct msm_pinctrl *pctrl;
	struct resource *res;
	int ret;
	int i;

	pctrl = devm_kzalloc(&pdev->dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	pctrl->dev = &pdev->dev;
	pctrl->soc = soc_data;
	pctrl->chip = msm_gpio_template;

	raw_spin_lock_init(&pctrl->lock);

	if (soc_data->tiles) {
		for (i = 0; i < soc_data->ntiles; i++) {
			res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							   soc_data->tiles[i]);
			pctrl->regs[i] = devm_ioremap_resource(&pdev->dev, res);
			if (IS_ERR(pctrl->regs[i]))
				return PTR_ERR(pctrl->regs[i]);
		}
	} else {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		pctrl->regs[0] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pctrl->regs[0]))
			return PTR_ERR(pctrl->regs[0]);
	}

	msm_pinctrl_setup_pm_reset(pctrl);

	pctrl->irq = platform_get_irq(pdev, 0);
	if (pctrl->irq < 0)
		return pctrl->irq;

	pctrl->desc.owner = THIS_MODULE;
	pctrl->desc.pctlops = &msm_pinctrl_ops;
	pctrl->desc.pmxops = &msm_pinmux_ops;
	pctrl->desc.confops = &msm_pinconf_ops;
	pctrl->desc.name = dev_name(&pdev->dev);
	pctrl->desc.pins = pctrl->soc->pins;
	pctrl->desc.npins = pctrl->soc->npins;

	pctrl->pctrl = devm_pinctrl_register(&pdev->dev, &pctrl->desc, pctrl);
	if (IS_ERR(pctrl->pctrl)) {
		dev_err(&pdev->dev, "Couldn't register pinctrl driver\n");
		return PTR_ERR(pctrl->pctrl);
	}

	ret = msm_gpio_init(pctrl);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, pctrl);

	dev_dbg(&pdev->dev, "Probed Qualcomm pinctrl driver\n");

	return 0;
}
EXPORT_SYMBOL(msm_pinctrl_probe);

int msm_pinctrl_remove(struct platform_device *pdev)
{
	struct msm_pinctrl *pctrl = platform_get_drvdata(pdev);

	gpiochip_remove(&pctrl->chip);

	unregister_restart_handler(&pctrl->restart_nb);

	return 0;
}
EXPORT_SYMBOL(msm_pinctrl_remove);

