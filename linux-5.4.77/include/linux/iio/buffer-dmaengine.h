/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2014-2015 Analog Devices Inc.
 *  Author: Lars-Peter Clausen <lars@metafoo.de>
 */

#ifndef __IIO_DMAENGINE_H__
#define __IIO_DMAENGINE_H__

struct iio_buffer;
struct device;

struct iio_buffer *iio_dmaengine_buffer_alloc(struct device *dev,
	const char *channel);
void iio_dmaengine_buffer_free(struct iio_buffer *buffer);

#endif
