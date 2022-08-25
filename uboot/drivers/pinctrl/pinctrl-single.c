// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) EETS GmbH, 2017, Felix Brack <f.brack@eets.ch>
 */

#include <common.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/pinctrl.h>
#include <linux/libfdt.h>
#include <asm/io.h>

struct single_pdata {
	fdt_addr_t base;	/* first configuration register */
	int offset;		/* index of last configuration register */
	u32 mask;		/* configuration-value mask bits */
	int width;		/* configuration register bit width */
	bool bits_per_mux;
};

struct single_fdt_pin_cfg {
	fdt32_t reg;		/* configuration register offset */
	fdt32_t val;		/* configuration register value */
};

struct single_fdt_bits_cfg {
	fdt32_t reg;		/* configuration register offset */
	fdt32_t val;		/* configuration register value */
	fdt32_t mask;		/* configuration register mask */
};

/**
 * single_configure_pins() - Configure pins based on FDT data
 *
 * @dev: Pointer to single pin configuration device which is the parent of
 *       the pins node holding the pin configuration data.
 * @pins: Pointer to the first element of an array of register/value pairs
 *        of type 'struct single_fdt_pin_cfg'. Each such pair describes the
 *        the pin to be configured and the value to be used for configuration.
 *        This pointer points to a 'pinctrl-single,pins' property in the
 *        device-tree.
 * @size: Size of the 'pins' array in bytes.
 *        The number of register/value pairs in the 'pins' array therefore
 *        equals to 'size / sizeof(struct single_fdt_pin_cfg)'.
 */
static int single_configure_pins(struct udevice *dev,
				 const struct single_fdt_pin_cfg *pins,
				 int size)
{
	struct single_pdata *pdata = dev->platdata;
	int count = size / sizeof(struct single_fdt_pin_cfg);
	phys_addr_t n, reg;
	u32 val;

	for (n = 0; n < count; n++, pins++) {
		reg = fdt32_to_cpu(pins->reg);
		if ((reg < 0) || (reg > pdata->offset)) {
			dev_dbg(dev, "  invalid register offset 0x%pa\n", &reg);
			continue;
		}
		reg += pdata->base;
		val = fdt32_to_cpu(pins->val) & pdata->mask;
		switch (pdata->width) {
		case 16:
			writew((readw(reg) & ~pdata->mask) | val, reg);
			break;
		case 32:
			writel((readl(reg) & ~pdata->mask) | val, reg);
			break;
		default:
			dev_warn(dev, "unsupported register width %i\n",
				 pdata->width);
			continue;
		}
		dev_dbg(dev, "  reg/val 0x%pa/0x%08x\n", &reg, val);
	}
	return 0;
}

static int single_configure_bits(struct udevice *dev,
				 const struct single_fdt_bits_cfg *pins,
				 int size)
{
	struct single_pdata *pdata = dev->platdata;
	int count = size / sizeof(struct single_fdt_bits_cfg);
	phys_addr_t n, reg;
	u32 val, mask;

	for (n = 0; n < count; n++, pins++) {
		reg = fdt32_to_cpu(pins->reg);
		if ((reg < 0) || (reg > pdata->offset)) {
			dev_dbg(dev, "  invalid register offset 0x%pa\n", &reg);
			continue;
		}
		reg += pdata->base;

		mask = fdt32_to_cpu(pins->mask);
		val = fdt32_to_cpu(pins->val) & mask;

		switch (pdata->width) {
		case 16:
			writew((readw(reg) & ~mask) | val, reg);
			break;
		case 32:
			writel((readl(reg) & ~mask) | val, reg);
			break;
		default:
			dev_warn(dev, "unsupported register width %i\n",
				 pdata->width);
			continue;
		}
		dev_dbg(dev, "  reg/val 0x%pa/0x%08x\n", &reg, val);
	}
	return 0;
}
static int single_set_state(struct udevice *dev,
			    struct udevice *config)
{
	const struct single_fdt_pin_cfg *prop;
	const struct single_fdt_bits_cfg *prop_bits;
	int len;

	prop = dev_read_prop(config, "pinctrl-single,pins", &len);

	if (prop) {
		dev_dbg(dev, "configuring pins for %s\n", config->name);
		if (len % sizeof(struct single_fdt_pin_cfg)) {
			dev_dbg(dev, "  invalid pin configuration in fdt\n");
			return -FDT_ERR_BADSTRUCTURE;
		}
		single_configure_pins(dev, prop, len);
		return 0;
	}

	/* pinctrl-single,pins not found so check for pinctrl-single,bits */
	prop_bits = dev_read_prop(config, "pinctrl-single,bits", &len);
	if (prop_bits) {
		dev_dbg(dev, "configuring pins for %s\n", config->name);
		if (len % sizeof(struct single_fdt_bits_cfg)) {
			dev_dbg(dev, "  invalid bits configuration in fdt\n");
			return -FDT_ERR_BADSTRUCTURE;
		}
		single_configure_bits(dev, prop_bits, len);
		return 0;
	}

	/* Neither 'pinctrl-single,pins' nor 'pinctrl-single,bits' were found */
	return len;
}

static int single_ofdata_to_platdata(struct udevice *dev)
{
	fdt_addr_t addr;
	u32 of_reg[2];
	int res;
	struct single_pdata *pdata = dev->platdata;

	pdata->width =
		dev_read_u32_default(dev, "pinctrl-single,register-width", 0);

	res = dev_read_u32_array(dev, "reg", of_reg, 2);
	if (res)
		return res;
	pdata->offset = of_reg[1] - pdata->width / 8;

	addr = dev_read_addr(dev);
	if (addr == FDT_ADDR_T_NONE) {
		dev_dbg(dev, "no valid base register address\n");
		return -EINVAL;
	}
	pdata->base = addr;

	pdata->mask = dev_read_u32_default(dev, "pinctrl-single,function-mask",
					   0xffffffff);
	pdata->bits_per_mux = dev_read_bool(dev, "pinctrl-single,bit-per-mux");

	return 0;
}

const struct pinctrl_ops single_pinctrl_ops = {
	.set_state = single_set_state,
};

static const struct udevice_id single_pinctrl_match[] = {
	{ .compatible = "pinctrl-single" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(single_pinctrl) = {
	.name = "single-pinctrl",
	.id = UCLASS_PINCTRL,
	.of_match = single_pinctrl_match,
	.ops = &single_pinctrl_ops,
	.platdata_auto_alloc_size = sizeof(struct single_pdata),
	.ofdata_to_platdata = single_ofdata_to_platdata,
};
