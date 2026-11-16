/*
 * Recent history of the main readings, for trend lines and charts.
 *
 * SPDX-License-Identifier: MIT
 *
 * Readings are averaged into HISTORY_PERIOD_S buckets; the last HISTORY_LEN
 * buckets are kept (6 hours at the defaults). Values are stored in the same
 * metric units as struct readings. Lost on reboot; at boot each series is
 * backfilled with its first reading (see history.c).
 */

#ifndef SENSOR_BOX_HISTORY_H_
#define SENSOR_BOX_HISTORY_H_

#include "readings.h"

#define HISTORY_LEN 120

/* Overridable for testing, e.g. west build -- -DEXTRA_CFLAGS=-DHISTORY_PERIOD_S=10 */
#ifndef HISTORY_PERIOD_S
#define HISTORY_PERIOD_S 180
#endif

enum history_series {
	H_TEMP,		/* degC */
	H_RH,		/* %RH */
	H_PRESS,	/* hPa, sea level */
	H_PM25,		/* ug/m3 */
	H_ECO2,		/* ppm */
	H_TVOC,		/* ppb */
	H_COUNT,
};

/* Fold one set of readings into the current bucket. Call on every report. */
void history_add(const struct readings *r);

/*
 * Copy up to max points of a series, oldest first, into out. The last point
 * is the still-filling current bucket, so the line moves as soon as a reading
 * comes in. Buckets with no valid reading are NAN. Returns the point count.
 */
int history_get(enum history_series s, float *out, int max);

#endif /* SENSOR_BOX_HISTORY_H_ */
