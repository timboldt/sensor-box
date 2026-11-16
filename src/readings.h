/*
 * One snapshot of every sensor on the box.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_READINGS_H_
#define SENSOR_BOX_READINGS_H_

#include <stdbool.h>
#include <stdint.h>

struct readings {
	/* BME280 */
	bool climate_ok;
	double temp_c;
	double rh_pct;
	double press_hpa;	/* station (absolute) pressure */
	double press_sl_hpa;	/* adjusted to sea level */

	/* TSL2591 */
	bool light_ok;
	double lux;

	/* PMSA003I, ug/m3. pm_warming: alive but still in its ~30 s warm-up. */
	bool pm_ok;
	bool pm_warming;
	int32_t pm1_0;
	int32_t pm2_5;
	int32_t pm10;

	/* SGP30. gas_warming: alive but still in its ~15 s warm-up. */
	bool gas_ok;
	bool gas_warming;
	int32_t eco2_ppm;
	int32_t tvoc_ppb;

	/* Network */
	bool wifi_ok;

	/* MAX17048 */
	bool batt_ok;
	double batt_v;
	uint8_t batt_pct;
};

#endif /* SENSOR_BOX_READINGS_H_ */
