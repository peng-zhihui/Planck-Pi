/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_I2C_AW9523B_H
#define __LINUX_I2C_AW9523B_H

/* platform data for the AW9523B 16-bit I/O expander driver */

struct aw9523b_platform_data {
	/* number of the first GPIO */
	unsigned	gpio_base;

	/* interrupt base */
	int		irq_base;

	void		*context;	/* param to setup/teardown */

	int		(*setup)(struct i2c_client *client,
				unsigned gpio, unsigned ngpio,
				void *context);
	int		(*teardown)(struct i2c_client *client,
				unsigned gpio, unsigned ngpio,
				void *context);
};
#endif /* __LINUX_I2C_AW9523B_H */
