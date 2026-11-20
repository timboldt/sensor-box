/*
 * Ambient light from the TSL2591, with automatic gain.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_LIGHT_H_
#define SENSOR_BOX_LIGHT_H_

#include <math.h>
#include <stdbool.h>

/* Configure the sensor (integration time, starting gain). */
void light_init(void);

/*
 * Read the light level in lux. Returns false if there's no valid reading
 * (sensor missing, I2C error, clipped, or within ~0.5 s of a gain change).
 * Adjusts the gain for the next reading as needed. Meant to be called about
 * once a second, so a gain change never costs a reading.
 */
bool light_read(double *lux);

/* The gain used for the last reading, e.g. "x400" (for logs). */
const char *light_gain_name(void);

/* Lux to decibels relative to 1 lux (0 dB = 1 lux, -30 dB = 0.001 lux). */
#define LIGHT_MIN_LUX 1e-5	/* floor for the log: -50 dB */

static inline double light_db(double lux)
{
	return 10.0 * log10(lux > LIGHT_MIN_LUX ? lux : LIGHT_MIN_LUX);
}

#endif /* SENSOR_BOX_LIGHT_H_ */
