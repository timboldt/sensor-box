/*
 * TFT backlight, driven from ambient light.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_BACKLIGHT_H_
#define SENSOR_BOX_BACKLIGHT_H_

#include <stdbool.h>

/* Set up the PWM and start at full brightness. Returns 0, or -ENODEV. */
int backlight_init(void);

/*
 * Feed the latest ambient light reading. The backlight fades off when the
 * room is dark and back on when it is not. With lux_ok false (sensor
 * missing or failed), it stays on.
 */
void backlight_set_ambient(bool lux_ok, double lux);

#endif /* SENSOR_BOX_BACKLIGHT_H_ */
