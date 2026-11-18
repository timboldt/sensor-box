/*
 * Recent history of the main readings, for trend lines and charts.
 *
 * SPDX-License-Identifier: MIT
 *
 * Readings are averaged into fixed-length buckets in two windows:
 *
 *  - HW_LONG:  24 hours of 1-minute buckets, for the screen's chart pages
 *              (6 hours, averaged in threes) and the web page's 6 h and Day
 *              views;
 *  - HW_SHORT: 15 minutes of 10-second buckets, for the tiles' trend lines.
 *
 * Values are returned in the same metric units as struct readings. Lost on
 * reboot; at boot each series is backfilled with its first reading (see
 * history.c).
 */

#ifndef SENSOR_BOX_HISTORY_H_
#define SENSOR_BOX_HISTORY_H_

#include <stdint.h>
#include <time.h>

#include "readings.h"

/* Long window: 1440 x 60 s = 24 h. */
#define HISTORY_LEN 1440

#define HISTORY_PERIOD_S 60

/* Short window: 90 x 10 s = 15 min (two 5-second readings per point). */
#define HISTORY_SHORT_LEN      90
#define HISTORY_SHORT_PERIOD_S 10

enum history_window {
	HW_LONG,
	HW_SHORT,
};

enum history_series {
	H_TEMP,		/* degC */
	H_AH,		/* g/m3, absolute humidity */
	H_PRESS,	/* hPa, sea level */
	H_PM25,		/* ug/m3 */
	H_CO2,		/* ppm (SCD-4x) */
	H_TVOC,		/* ppb */
	H_PC03,		/* particles >0.3 um per litre */
	H_BATT_I,	/* battery current, mA, + = charging */
	H_LUX,		/* lux */
	H_COUNT,
};

/* Fold one set of readings into the current buckets. Call on every report. */
void history_add(const struct readings *r);

/*
 * Copy the newest points of a series, oldest first, into out (at most max).
 * The last point is the still-filling current bucket, so a line moves as soon
 * as a reading comes in. With group > 1, each point is the average of that
 * many buckets (aligned so the newest group ends now). Buckets with no valid
 * reading are NAN. Returns the point count.
 */
int history_get(enum history_window w, enum history_series s, float *out, int max, int group);

/*
 * For reading a window in pieces (e.g. a streamed web response) with every
 * series lined up: history_snapshot gives the sequence number just past the
 * newest completed bucket, and the milliseconds since that bucket ended.
 * history_get_seq then copies completed buckets [first, first + count) of a
 * series; ones no longer (or not yet) kept come back as NAN.
 */
uint32_t history_snapshot(enum history_window w, int64_t *age_ms);

/*
 * Restore one past minute from a saved log: v[] is each series' value for
 * the UTC minute starting at `minute` (NAN = none), `now` the current UTC
 * time. Only fills buckets from before this boot (they otherwise hold the
 * boot-time backfill); anything measured since boot is left alone.
 */
void history_restore(time_t minute, const float v[H_COUNT], time_t now);

/*
 * Seconds of real data in a window, back from now: the uptime, or more
 * if older data was restored.
 */
int64_t history_covered_s(enum history_window w);
void history_get_seq(enum history_window w, enum history_series s, uint32_t first, int count,
		     float *out);

#endif /* SENSOR_BOX_HISTORY_H_ */
