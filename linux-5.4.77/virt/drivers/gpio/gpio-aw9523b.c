// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/platform_data/aw9523b.h>
#include <linux/of.h>
#include <linux/delay.h>

#define REG_PIN0 0x00
#define REG_PIN1 0x01
#define REG_PORT0 0x02
#define REG_PORT1 0x03
#define REG_DDR0 0x04
#define REG_DDR1 0x05
#define REG_EIMSK0 0x06
#define REG_EIMSK1 0x07

#define REG_ID 0x10
#define REG_GCTL 0x11
#define REG_RST 0x7F

struct aw9523b_chip {
	struct gpio_chip gpio_chip;
	struct i2c_client *client;
	struct mutex lock;

	struct mutex irq_lock;
	uint16_t irq_mask;
	uint16_t irq_mask_cur;
	uint16_t irq_trig_raise;
	uint16_t irq_trig_fall;
};

static void aw9523b_get_regbit(const uint8_t *choice, const unsigned off,
			       uint8_t *reg, uint8_t *bit)
{
	if (off < 8) {
		*reg = choice[0];
		*bit = off;
	} else {
		*reg = choice[1];
		*bit = off - 8;
	}
}

static int aw9523b_gpio_get_value(struct gpio_chip *gc, unsigned off)
{
	struct aw9523b_chip *chip = gpiochip_get_data(gc);
	uint8_t choice[] = { REG_PIN0, REG_PIN1 };
	uint8_t reg, bit;
	int ret;
	aw9523b_get_regbit(choice, off, &reg, &bit);

	ret = i2c_smbus_read_byte_data(chip->client, reg);
	return !!(ret & (1 << bit));
}

static void aw9523b_gpio_set_value(struct gpio_chip *gc, unsigned off, int val)
{
	struct aw9523b_chip *chip = gpiochip_get_data(gc);
	uint8_t choice[] = { REG_PORT0, REG_PORT1 };
	uint8_t reg, bit;
	int ret;
	aw9523b_get_regbit(choice, off, &reg, &bit);

	ret = i2c_smbus_read_byte_data(chip->client, reg);
	if (val) {
		ret |= (1 << bit);
	} else {
		ret &= ~(1 << bit);
	}
	i2c_smbus_write_byte_data(chip->client, reg, ret);
}

static int aw9523b_gpio_direction_input(struct gpio_chip *gc, unsigned off)
{
	struct aw9523b_chip *chip = gpiochip_get_data(gc);
	uint8_t choice[] = { REG_DDR0, REG_DDR1 };
	uint8_t reg, bit;
	int ret;
	aw9523b_get_regbit(choice, off, &reg, &bit);

	ret = i2c_smbus_read_byte_data(chip->client, reg);
	ret |= (1 << bit);
	ret = i2c_smbus_write_byte_data(chip->client, reg, ret);

	return 0;
}

static int aw9523b_gpio_direction_output(struct gpio_chip *gc, unsigned off,
					 int val)
{
	struct aw9523b_chip *chip = gpiochip_get_data(gc);
	uint8_t choice[] = { REG_DDR0, REG_DDR1 };
	uint8_t reg, bit;
	int ret;
	aw9523b_get_regbit(choice, off, &reg, &bit);

	ret = i2c_smbus_read_byte_data(chip->client, reg);
	ret &= ~(1 << bit);
	ret = i2c_smbus_write_byte_data(chip->client, reg, ret);

	aw9523b_gpio_set_value(gc, off, val);

	return 0;
}

/*
static int aw9523b_gpio_get_multiple(struct gpio_chip *gc, unsigned long *mask,
				     unsigned long *bits)
{
	struct aw9523b_chip *chip = gpiochip_get_data(gc);
	uint8_t choice[] = { REG_PIN0, REG_PIN1 };
	uint8_t reg, bit;
	int ret;
	aw9523b_get_regbit(choice, 0, &reg, &bit);

	mutex_lock(&chip->lock);
	// TODO
	mutex_unlock(&chip->lock);
}

static void aw9523b_gpio_set_multiple(struct gpio_chip *gc, unsigned long *mask,
				      unsigned long *bits)
{
	struct aw9523b_chip *chip = gpiochip_get_data(gc);
	uint8_t choice[] = { REG_PORT0, REG_PORT1 };
	uint8_t reg, bit;
	int ret;
	aw9523b_get_regbit(choice, 0, &reg, &bit);

	mutex_lock(&chip->lock);
	// TODO
	mutex_unlock(&chip->lock);
}
*/

static void aw9523b_setup_gpio(struct aw9523b_chip *chip,
			       const struct i2c_device_id *id,
			       unsigned gpio_start)
{
	struct gpio_chip *gc = &chip->gpio_chip;

	gc->direction_input = aw9523b_gpio_direction_input;
	gc->direction_output = aw9523b_gpio_direction_output;
	gc->get = aw9523b_gpio_get_value;
	gc->set = aw9523b_gpio_set_value;
	// gc->set_multiple = aw9523b_gpio_set_multiple;
	// gc->get_multiple = aw9523b_gpio_get_multiple;
	gc->can_sleep = true;

	gc->base = gpio_start;
	gc->ngpio = 16;
	gc->label = chip->client->name;
	gc->parent = &chip->client->dev;
	gc->owner = THIS_MODULE;
}

static void aw9523b_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct aw9523b_chip *chip = gpiochip_get_data(gc);

	chip->irq_mask_cur &= ~(1 << d->hwirq);
}

static void aw9523b_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct aw9523b_chip *chip = gpiochip_get_data(gc);

	chip->irq_mask_cur |= 1 << d->hwirq;
}

static void aw9523b_irq_bus_lock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct aw9523b_chip *chip = gpiochip_get_data(gc);

	mutex_lock(&chip->irq_lock);
	chip->irq_mask_cur = chip->irq_mask;
}

static void aw9523b_irq_bus_sync_unlock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct aw9523b_chip *chip = gpiochip_get_data(gc);
	uint16_t msg = ~chip->irq_mask_cur;

	if (chip->irq_mask != chip->irq_mask_cur) {
		chip->irq_mask = chip->irq_mask_cur;

		mutex_lock(&chip->lock);

		i2c_smbus_write_byte_data(chip->client, REG_EIMSK0, msg & 0xff);
		i2c_smbus_write_byte_data(chip->client, REG_EIMSK1, msg >> 8);

		mutex_unlock(&chip->lock);
	}

	mutex_unlock(&chip->irq_lock);
}

static int aw9523b_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct aw9523b_chip *chip = gpiochip_get_data(gc);
	uint16_t mask = 1 << d->hwirq;

	if (!(type & IRQ_TYPE_EDGE_BOTH)) {
		dev_err(&chip->client->dev, "irq %d: unsupported type %d\n",
			d->irq, type);
		return -EINVAL;
	}

	if (type & IRQ_TYPE_EDGE_FALLING)
		chip->irq_trig_fall |= mask;
	else
		chip->irq_trig_fall &= ~mask;

	if (type & IRQ_TYPE_EDGE_RISING)
		chip->irq_trig_raise |= mask;
	else
		chip->irq_trig_raise &= ~mask;

	return 0;
}

static int aw9523b_irq_set_wake(struct irq_data *data, unsigned int on)
{
	struct aw9523b_chip *chip = irq_data_get_irq_chip_data(data);

	irq_set_irq_wake(chip->client->irq, on);
	return 0;
}

static struct irq_chip aw9523b_irq_chip = {
	.name = "aw9523b_irq",
	.irq_mask = aw9523b_irq_mask,
	.irq_unmask = aw9523b_irq_unmask,
	.irq_bus_lock = aw9523b_irq_bus_lock,
	.irq_bus_sync_unlock = aw9523b_irq_bus_sync_unlock,
	.irq_set_type = aw9523b_irq_set_type,
	.irq_set_wake = aw9523b_irq_set_wake,
};

static uint16_t aw9523b_irq_pending(struct aw9523b_chip *chip)
{
	uint16_t status;
	uint16_t pending;

	status = i2c_smbus_read_byte_data(chip->client, REG_PIN1) << 8;
	status |= i2c_smbus_read_byte_data(chip->client, REG_PIN0);

	pending = (chip->irq_trig_fall & ~status) |
		  (chip->irq_trig_raise & status);

	return pending;
}

static irqreturn_t aw9523b_irq_handler(int irq, void *devid)
{
	struct aw9523b_chip *chip = devid;
	uint16_t pending;
	uint8_t level;

	pending = aw9523b_irq_pending(chip);

	if (!pending)
		return IRQ_HANDLED;

	do {
		level = __ffs(pending);

		handle_nested_irq(
			irq_find_mapping(chip->gpio_chip.irq.domain, level));

		pending &= ~(1 << level);
	} while (pending);

	return IRQ_HANDLED;
}

static int aw9523b_irq_setup(struct aw9523b_chip *chip,
			     const struct i2c_device_id *id)
{
	struct i2c_client *client = chip->client;
	struct aw9523b_platform_data *pdata = dev_get_platdata(&client->dev);
	int irq_base = 0;
	int ret;

	if ((pdata && pdata->irq_base) || client->irq) {
		if (pdata)
			irq_base = pdata->irq_base;
		mutex_init(&chip->irq_lock);

		ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
						aw9523b_irq_handler,
						IRQF_ONESHOT | IRQF_TRIGGER_LOW,
						dev_name(&client->dev), chip);
		if (ret) {
			dev_err(&client->dev, "failed to request irq %d\n",
				client->irq);
			return ret;
		}
		ret = gpiochip_irqchip_add_nested(&chip->gpio_chip,
						  &aw9523b_irq_chip, irq_base,
						  handle_simple_irq,
						  IRQ_TYPE_NONE);
		if (ret) {
			dev_err(&client->dev,
				"could not connect irqchip to gpiochip\n");
			return ret;
		}
		gpiochip_set_nested_irqchip(&chip->gpio_chip, &aw9523b_irq_chip,
					    client->irq);
	}

	return 0;
}

static void aw9523_init_seq(struct aw9523b_chip *chip)
{
	i2c_smbus_write_byte_data(chip->client, REG_RST, 0x00);
	mdelay(1);
	i2c_smbus_write_byte_data(chip->client, REG_GCTL, 0x10);
	i2c_smbus_write_byte_data(chip->client, REG_DDR0, 0xff);
	i2c_smbus_write_byte_data(chip->client, REG_DDR1, 0xff);
	i2c_smbus_write_byte_data(chip->client, REG_EIMSK0, 0xff);
	i2c_smbus_write_byte_data(chip->client, REG_EIMSK1, 0xff);
	i2c_smbus_read_byte_data(chip->client, REG_PIN0);
	i2c_smbus_read_byte_data(chip->client, REG_PIN1);
}

static int aw9523b_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct aw9523b_platform_data *pdata;
	struct device_node *node;
	struct aw9523b_chip *chip;
	int ret;

	pdata = dev_get_platdata(&client->dev);
	node = client->dev.of_node;

	if (!pdata && node) {
		pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
		if (!pdata) {
			dev_dbg(&client->dev, "no platform data\n");
			return -EINVAL;
		}

		pdata->gpio_base = -1;
	}

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;
	chip->client = client;

	aw9523_init_seq(chip);

	aw9523b_setup_gpio(chip, id, pdata->gpio_base);
	chip->gpio_chip.parent = &client->dev;

	mutex_init(&chip->lock);

	ret = devm_gpiochip_add_data(&client->dev, &chip->gpio_chip, chip);
	if (ret)
		return ret;

	ret = aw9523b_irq_setup(chip, id);
	if (ret)
		return ret;

	if (pdata && pdata->setup) {
		ret = pdata->setup(client, chip->gpio_chip.base,
				   chip->gpio_chip.ngpio, pdata->context);
		if (ret < 0)
			dev_warn(&client->dev, "setup failed, %d\n", ret);
	}

	i2c_set_clientdata(client, chip);

	ret = i2c_smbus_read_byte_data(chip->client, REG_ID);
	dev_info(&client->dev, "with id 0x%02x registered.\n", ret);

	return 0;
}

static int aw9523b_remove(struct i2c_client *client)
{
	struct aw9523b_platform_data *pdata = dev_get_platdata(&client->dev);
	struct aw9523b_chip *chip = i2c_get_clientdata(client);

	if (pdata && pdata->teardown) {
		int ret;

		ret = pdata->teardown(client, chip->gpio_chip.base,
				      chip->gpio_chip.ngpio, pdata->context);
		if (ret < 0) {
			dev_err(&client->dev, "%s failed, %d\n", "teardown",
				ret);
			return ret;
		}
	}

	return 0;
}

static const struct i2c_device_id aw9523b_id[] = {
	{ "aw9523b", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, aw9523b_id);

#ifdef CONFIG_OF
static const struct of_device_id aw9523b_of_table[] = {
	{ .compatible = "awinic,aw9523b-gpio" },
	{}
};
MODULE_DEVICE_TABLE(of, aw9523b_of_table);
#endif

static struct i2c_driver aw9523b_driver = {
	.driver = {
		.name		= "aw9523b_gpio",
		.of_match_table	= of_match_ptr(aw9523b_of_table),
	},
	.probe		= aw9523b_probe,
	.remove		= aw9523b_remove,
	.id_table	= aw9523b_id,
};

static int __init aw9523b_init(void)
{
	return i2c_add_driver(&aw9523b_driver);
}
arch_initcall(aw9523b_init);

static void __exit aw9523b_exit(void)
{
	i2c_del_driver(&aw9523b_driver);
}
module_exit(aw9523b_exit);

MODULE_AUTHOR("Aodzip <aodzip@gmail.com>");
MODULE_DESCRIPTION("GPIO expander driver for AW9523B");
MODULE_LICENSE("GPL");
