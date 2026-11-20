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
 * Feed the latest ambient light reading. The backlight dims with the room
 * light, from full brightness (15 lux and up) to 1%, and fades off when the
 * room is dark (below 0.01 lux). With lux_ok false (no reading), it holds
 * its level; after about 30 s without one (sensor missing or failed), it goes
 * to full brightness.
 */
void backlight_set_ambient(bool lux_ok, double lux);

/*
 * Turn the backlight on (dimly) for a while (30 s) even if the room is dark, e.g. on
 * a button press; each call restarts the timer. Returns true if the backlight
 * was off (or fading off), so the caller can treat the press as "wake only".
 */
bool backlight_wake(void);

#endif /* SENSOR_BOX_BACKLIGHT_H_ */
