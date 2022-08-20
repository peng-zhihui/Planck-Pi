/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#ifndef __QCOM_TSENS_H__
#define __QCOM_TSENS_H__

#define ONE_PT_CALIB		0x1
#define ONE_PT_CALIB2		0x2
#define TWO_PT_CALIB		0x3
#define CAL_DEGC_PT1		30
#define CAL_DEGC_PT2		120
#define SLOPE_FACTOR		1000
#define SLOPE_DEFAULT		3200


#include <linux/thermal.h>
#include <linux/regmap.h>
#include <linux/slab.h>

struct tsens_priv;

enum tsens_ver {
	VER_0_1 = 0,
	VER_1_X,
	VER_2_X,
};

/**
 * struct tsens_sensor - data for each sensor connected to the tsens device
 * @priv: tsens device instance that this sensor is connected to
 * @tzd: pointer to the thermal zone that this sensor is in
 * @offset: offset of temperature adjustment curve
 * @id: Sensor ID
 * @hw_id: HW ID can be used in case of platform-specific IDs
 * @slope: slope of temperature adjustment curve
 * @status: 8960-specific variable to track 8960 and 8660 status register offset
 */
struct tsens_sensor {
	struct tsens_priv		*priv;
	struct thermal_zone_device	*tzd;
	int				offset;
	unsigned int			id;
	unsigned int			hw_id;
	int				slope;
	u32				status;
};

/**
 * struct tsens_ops - operations as supported by the tsens device
 * @init: Function to initialize the tsens device
 * @calibrate: Function to calibrate the tsens device
 * @get_temp: Function which returns the temp in millidegC
 * @enable: Function to enable (clocks/power) tsens device
 * @disable: Function to disable the tsens device
 * @suspend: Function to suspend the tsens device
 * @resume: Function to resume the tsens device
 * @get_trend: Function to get the thermal/temp trend
 */
struct tsens_ops {
	/* mandatory callbacks */
	int (*init)(struct tsens_priv *priv);
	int (*calibrate)(struct tsens_priv *priv);
	int (*get_temp)(struct tsens_priv *priv, int i, int *temp);
	/* optional callbacks */
	int (*enable)(struct tsens_priv *priv, int i);
	void (*disable)(struct tsens_priv *priv);
	int (*suspend)(struct tsens_priv *priv);
	int (*resume)(struct tsens_priv *priv);
	int (*get_trend)(struct tsens_priv *priv, int i, enum thermal_trend *trend);
};

#define REG_FIELD_FOR_EACH_SENSOR11(_name, _offset, _startbit, _stopbit) \
	[_name##_##0]  = REG_FIELD(_offset,      _startbit, _stopbit),	\
	[_name##_##1]  = REG_FIELD(_offset +  4, _startbit, _stopbit), \
	[_name##_##2]  = REG_FIELD(_offset +  8, _startbit, _stopbit), \
	[_name##_##3]  = REG_FIELD(_offset + 12, _startbit, _stopbit), \
	[_name##_##4]  = REG_FIELD(_offset + 16, _startbit, _stopbit), \
	[_name##_##5]  = REG_FIELD(_offset + 20, _startbit, _stopbit), \
	[_name##_##6]  = REG_FIELD(_offset + 24, _startbit, _stopbit), \
	[_name##_##7]  = REG_FIELD(_offset + 28, _startbit, _stopbit), \
	[_name##_##8]  = REG_FIELD(_offset + 32, _startbit, _stopbit), \
	[_name##_##9]  = REG_FIELD(_offset + 36, _startbit, _stopbit), \
	[_name##_##10] = REG_FIELD(_offset + 40, _startbit, _stopbit)

#define REG_FIELD_FOR_EACH_SENSOR16(_name, _offset, _startbit, _stopbit) \
	[_name##_##0]  = REG_FIELD(_offset,      _startbit, _stopbit),	\
	[_name##_##1]  = REG_FIELD(_offset +  4, _startbit, _stopbit), \
	[_name##_##2]  = REG_FIELD(_offset +  8, _startbit, _stopbit), \
	[_name##_##3]  = REG_FIELD(_offset + 12, _startbit, _stopbit), \
	[_name##_##4]  = REG_FIELD(_offset + 16, _startbit, _stopbit), \
	[_name##_##5]  = REG_FIELD(_offset + 20, _startbit, _stopbit), \
	[_name##_##6]  = REG_FIELD(_offset + 24, _startbit, _stopbit), \
	[_name##_##7]  = REG_FIELD(_offset + 28, _startbit, _stopbit), \
	[_name##_##8]  = REG_FIELD(_offset + 32, _startbit, _stopbit), \
	[_name##_##9]  = REG_FIELD(_offset + 36, _startbit, _stopbit), \
	[_name##_##10] = REG_FIELD(_offset + 40, _startbit, _stopbit), \
	[_name##_##11] = REG_FIELD(_offset + 44, _startbit, _stopbit), \
	[_name##_##12] = REG_FIELD(_offset + 48, _startbit, _stopbit), \
	[_name##_##13] = REG_FIELD(_offset + 52, _startbit, _stopbit), \
	[_name##_##14] = REG_FIELD(_offset + 56, _startbit, _stopbit), \
	[_name##_##15] = REG_FIELD(_offset + 60, _startbit, _stopbit)

/* reg_field IDs to use as an index into an array */
enum regfield_ids {
	/* ----- SROT ------ */
	/* HW_VER */
	VER_MAJOR = 0,
	VER_MINOR,
	VER_STEP,
	/* CTRL_OFFSET */
	TSENS_EN =  3,
	TSENS_SW_RST,
	SENSOR_EN,
	CODE_OR_TEMP,

	/* ----- TM ------ */
	/* STATUS */
	LAST_TEMP_0 = 7,	/* Last temperature reading */
	LAST_TEMP_1,
	LAST_TEMP_2,
	LAST_TEMP_3,
	LAST_TEMP_4,
	LAST_TEMP_5,
	LAST_TEMP_6,
	LAST_TEMP_7,
	LAST_TEMP_8,
	LAST_TEMP_9,
	LAST_TEMP_10,
	LAST_TEMP_11,
	LAST_TEMP_12,
	LAST_TEMP_13,
	LAST_TEMP_14,
	LAST_TEMP_15,
	VALID_0 = 23,		/* VALID reading or not */
	VALID_1,
	VALID_2,
	VALID_3,
	VALID_4,
	VALID_5,
	VALID_6,
	VALID_7,
	VALID_8,
	VALID_9,
	VALID_10,
	VALID_11,
	VALID_12,
	VALID_13,
	VALID_14,
	VALID_15,
	MIN_STATUS_0,		/* MIN threshold violated */
	MIN_STATUS_1,
	MIN_STATUS_2,
	MIN_STATUS_3,
	MIN_STATUS_4,
	MIN_STATUS_5,
	MIN_STATUS_6,
	MIN_STATUS_7,
	MIN_STATUS_8,
	MIN_STATUS_9,
	MIN_STATUS_10,
	MIN_STATUS_11,
	MIN_STATUS_12,
	MIN_STATUS_13,
	MIN_STATUS_14,
	MIN_STATUS_15,
	MAX_STATUS_0,		/* MAX threshold violated */
	MAX_STATUS_1,
	MAX_STATUS_2,
	MAX_STATUS_3,
	MAX_STATUS_4,
	MAX_STATUS_5,
	MAX_STATUS_6,
	MAX_STATUS_7,
	MAX_STATUS_8,
	MAX_STATUS_9,
	MAX_STATUS_10,
	MAX_STATUS_11,
	MAX_STATUS_12,
	MAX_STATUS_13,
	MAX_STATUS_14,
	MAX_STATUS_15,
	LOWER_STATUS_0,	/* LOWER threshold violated */
	LOWER_STATUS_1,
	LOWER_STATUS_2,
	LOWER_STATUS_3,
	LOWER_STATUS_4,
	LOWER_STATUS_5,
	LOWER_STATUS_6,
	LOWER_STATUS_7,
	LOWER_STATUS_8,
	LOWER_STATUS_9,
	LOWER_STATUS_10,
	LOWER_STATUS_11,
	LOWER_STATUS_12,
	LOWER_STATUS_13,
	LOWER_STATUS_14,
	LOWER_STATUS_15,
	UPPER_STATUS_0,	/* UPPER threshold violated */
	UPPER_STATUS_1,
	UPPER_STATUS_2,
	UPPER_STATUS_3,
	UPPER_STATUS_4,
	UPPER_STATUS_5,
	UPPER_STATUS_6,
	UPPER_STATUS_7,
	UPPER_STATUS_8,
	UPPER_STATUS_9,
	UPPER_STATUS_10,
	UPPER_STATUS_11,
	UPPER_STATUS_12,
	UPPER_STATUS_13,
	UPPER_STATUS_14,
	UPPER_STATUS_15,
	CRITICAL_STATUS_0,	/* CRITICAL threshold violated */
	CRITICAL_STATUS_1,
	CRITICAL_STATUS_2,
	CRITICAL_STATUS_3,
	CRITICAL_STATUS_4,
	CRITICAL_STATUS_5,
	CRITICAL_STATUS_6,
	CRITICAL_STATUS_7,
	CRITICAL_STATUS_8,
	CRITICAL_STATUS_9,
	CRITICAL_STATUS_10,
	CRITICAL_STATUS_11,
	CRITICAL_STATUS_12,
	CRITICAL_STATUS_13,
	CRITICAL_STATUS_14,
	CRITICAL_STATUS_15,
	/* TRDY */
	TRDY,
	/* INTERRUPT ENABLE */
	INT_EN,	/* Pre-V1, V1.x */
	LOW_INT_EN,	/* V2.x */
	UP_INT_EN,	/* V2.x */
	CRIT_INT_EN,	/* V2.x */

	/* Keep last */
	MAX_REGFIELDS
};

/**
 * struct tsens_features - Features supported by the IP
 * @ver_major: Major number of IP version
 * @crit_int: does the IP support critical interrupts?
 * @adc:      do the sensors only output adc code (instead of temperature)?
 * @srot_split: does the IP neatly splits the register space into SROT and TM,
 *              with SROT only being available to secure boot firmware?
 * @max_sensors: maximum sensors supported by this version of the IP
 */
struct tsens_features {
	unsigned int ver_major;
	unsigned int crit_int:1;
	unsigned int adc:1;
	unsigned int srot_split:1;
	unsigned int max_sensors;
};

/**
 * struct tsens_plat_data - tsens compile-time platform data
 * @num_sensors: Number of sensors supported by platform
 * @ops: operations the tsens instance supports
 * @hw_ids: Subset of sensors ids supported by platform, if not the first n
 * @feat: features of the IP
 * @fields: bitfield locations
 */
struct tsens_plat_data {
	const u32		num_sensors;
	const struct tsens_ops	*ops;
	unsigned int		*hw_ids;
	const struct tsens_features	*feat;
	const struct reg_field		*fields;
};

/**
 * struct tsens_context - Registers to be saved/restored across a context loss
 */
struct tsens_context {
	int	threshold;
	int	control;
};

/**
 * struct tsens_priv - private data for each instance of the tsens IP
 * @dev: pointer to struct device
 * @num_sensors: number of sensors enabled on this device
 * @tm_map: pointer to TM register address space
 * @srot_map: pointer to SROT register address space
 * @tm_offset: deal with old device trees that don't address TM and SROT
 *             address space separately
 * @rf: array of regmap_fields used to store value of the field
 * @ctx: registers to be saved and restored during suspend/resume
 * @feat: features of the IP
 * @fields: bitfield locations
 * @ops: pointer to list of callbacks supported by this device
 * @sensor: list of sensors attached to this device
 */
struct tsens_priv {
	struct device			*dev;
	u32				num_sensors;
	struct regmap			*tm_map;
	struct regmap			*srot_map;
	u32				tm_offset;
	struct regmap_field		*rf[MAX_REGFIELDS];
	struct tsens_context		ctx;
	const struct tsens_features	*feat;
	const struct reg_field		*fields;
	const struct tsens_ops		*ops;
	struct tsens_sensor		sensor[0];
};

char *qfprom_read(struct device *dev, const char *cname);
void compute_intercept_slope(struct tsens_priv *priv, u32 *pt1, u32 *pt2, u32 mode);
int init_common(struct tsens_priv *priv);
int get_temp_tsens_valid(struct tsens_priv *priv, int i, int *temp);
int get_temp_common(struct tsens_priv *priv, int i, int *temp);

/* TSENS target */
extern const struct tsens_plat_data data_8960;

/* TSENS v0.1 targets */
extern const struct tsens_plat_data data_8916, data_8974;

/* TSENS v1 targets */
extern const struct tsens_plat_data data_tsens_v1;

/* TSENS v2 targets */
extern const struct tsens_plat_data data_8996, data_tsens_v2;

#endif /* __QCOM_TSENS_H__ */
