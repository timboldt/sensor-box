/*
 * Extended sensor attributes for the out-of-tree SGP30 driver.
 *
 * Copyright (c) 2026 Tim Boldt
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_SGP30_H_
#define SENSOR_BOX_SGP30_H_

#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SGP30-specific sensor attributes. */
enum sensor_attribute_sgp30 {
	/**
	 * Absolute humidity for the SGP30's on-chip compensation, in g/m^3
	 * (sensor_value: val1 = whole g/m^3, val2 = micro-g/m^3).
	 *
	 * Set with sensor_attr_set(dev, SENSOR_CHAN_ALL, ...). A value of 0
	 * disables compensation (the sensor's power-on default). The driver
	 * pushes the value to the sensor on its next 1 Hz measurement cycle,
	 * and only when it has changed.
	 */
	SENSOR_ATTR_SGP30_ABS_HUMIDITY = SENSOR_ATTR_PRIV_START,

	/**
	 * The sensor's IAQ baseline, as the raw words the SGP30 uses
	 * (sensor_value: val1 = eCO2 baseline, val2 = TVOC baseline).
	 *
	 * sensor_attr_get() returns the latest baseline, which the driver reads
	 * from the sensor once a minute (-EAGAIN until the first read).
	 * sensor_attr_set() restores a saved baseline; the driver sends it
	 * before its next measurement. Per Sensirion, save the baseline
	 * periodically and restore it after Init_air_quality (i.e. soon after
	 * boot), so the sensor need not relearn it from scratch.
	 */
	SENSOR_ATTR_SGP30_BASELINE,
};

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_BOX_SGP30_H_ */
