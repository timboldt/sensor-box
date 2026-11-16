/*
 * Persist the SGP30's IAQ baseline across reboots.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_BASELINE_H_
#define SENSOR_BOX_BASELINE_H_

/* Load the saved baseline (if any) and restore it to the SGP30. */
void baseline_init(void);

/* Save the baseline when due. Call regularly from the sensor loop. */
void baseline_tick(void);

#endif /* SENSOR_BOX_BASELINE_H_ */
