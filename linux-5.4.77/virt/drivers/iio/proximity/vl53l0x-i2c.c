// SPDX-License-Identifier: GPL-2.0
/*
 * Support for ST VL53L0X FlightSense ToF Ranging Sensor on a i2c bus.
 *
 * Copyright (C) 2016 STMicroelectronics Imaging Division.
 * Copyright (C) 2018 Song Qiang <songqiang1304521@gmail.com>
 *
 * Datasheet available at
 * <https://www.st.com/resource/en/datasheet/vl53l0x.pdf>
 *
 * Default 7-bit i2c slave address 0x29.
 *
 * TODO: FIFO buffer, continuous mode, interrupts, range selection,
 * sensor ID check.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include <linux/iio/iio.h>

#define VL_REG_SYSRANGE_START				0x00

#define VL_REG_SYSRANGE_MODE_MASK			GENMASK(3, 0)
#define VL_REG_SYSRANGE_MODE_SINGLESHOT			0x00
#define VL_REG_SYSRANGE_MODE_START_STOP			BIT(0)
#define VL_REG_SYSRANGE_MODE_BACKTOBACK			BIT(1)
#define VL_REG_SYSRANGE_MODE_TIMED			BIT(2)
#define VL_REG_SYSRANGE_MODE_HISTOGRAM			BIT(3)

#define VL_REG_RESULT_INT_STATUS			0x13
#define VL_REG_RESULT_RANGE_STATUS			0x14
#define VL_REG_RESULT_RANGE_STATUS_COMPLETE		BIT(0)

struct vl53l0x_data {
	struct i2c_client *client;
};

static int vl53l0x_read_proximity(struct vl53l0x_data *data,
				  const struct iio_chan_spec *chan,
				  int *val)
{
	struct i2c_client *client = data->client;
	u16 tries = 20;
	u8 buffer[12];
	int ret;

	ret = i2c_smbus_write_byte_data(client, VL_REG_SYSRANGE_START, 1);
	if (ret < 0)
		return ret;

	do {
		ret = i2c_smbus_read_byte_data(client,
					       VL_REG_RESULT_RANGE_STATUS);
		if (ret < 0)
			return ret;

		if (ret & VL_REG_RESULT_RANGE_STATUS_COMPLETE)
			break;

		usleep_range(1000, 5000);
	} while (--tries);
	if (!tries)
		return -ETIMEDOUT;

	ret = i2c_smbus_read_i2c_block_data(client, VL_REG_RESULT_RANGE_STATUS,
					    12, buffer);
	if (ret < 0)
		return ret;
	else if (ret != 12)
		return -EREMOTEIO;

	/* Values should be between 30~1200 in millimeters. */
	*val = (buffer[10] << 8) + buffer[11];

	return 0;
}

static const struct iio_chan_spec vl53l0x_channels[] = {
	{
		.type = IIO_DISTANCE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int vl53l0x_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			    int *val, int *val2, long mask)
{
	struct vl53l0x_data *data = iio_priv(indio_dev);
	int ret;

	if (chan->type != IIO_DISTANCE)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = vl53l0x_read_proximity(data, chan, val);
		if (ret < 0)
			return ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 1000;

		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info vl53l0x_info = {
	.read_raw = vl53l0x_read_raw,
};

static int vl53l0x_probe(struct i2c_client *client)
{
	struct vl53l0x_data *data;
	struct iio_dev *indio_dev;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	i2c_set_clientdata(client, indio_dev);

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_READ_I2C_BLOCK |
				     I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;

	indio_dev->dev.parent = &client->dev;
	indio_dev->name = "vl53l0x";
	indio_dev->info = &vl53l0x_info;
	indio_dev->channels = vl53l0x_channels;
	indio_dev->num_channels = ARRAY_SIZE(vl53l0x_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct of_device_id st_vl53l0x_dt_match[] = {
	{ .compatible = "st,vl53l0x", },
	{ }
};
MODULE_DEVICE_TABLE(of, st_vl53l0x_dt_match);

static struct i2c_driver vl53l0x_driver = {
	.driver = {
		.name = "vl53l0x-i2c",
		.of_match_table = st_vl53l0x_dt_match,
	},
	.probe_new = vl53l0x_probe,
};
module_i2c_driver(vl53l0x_driver);

MODULE_AUTHOR("Song Qiang <songqiang1304521@gmail.com>");
MODULE_DESCRIPTION("ST vl53l0x ToF ranging sensor driver");
MODULE_LICENSE("GPL v2");
