/*
 * Wall-clock time from SNTP (UTC).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_TIMESYNC_H_
#define SENSOR_BOX_TIMESYNC_H_

#include <stdbool.h>
#include <time.h>

/* Sync the clock whenever the network comes up, and periodically after. */
void timesync_init(void);

/* True once the clock has been set from SNTP at least once since boot. */
bool timesync_valid(void);

/* Current UTC time (seconds since 1970). Only meaningful if timesync_valid(). */
time_t timesync_now(void);

/*
 * Current local time (CONFIG_SENSOR_BOX_UTC_OFFSET_MIN, plus US daylight
 * saving if CONFIG_SENSOR_BOX_US_DST). False if the clock isn't set yet.
 */
bool timesync_local(struct tm *tm);

#endif /* SENSOR_BOX_TIMESYNC_H_ */
