/*
 * Recent history of the main readings. See history.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * At boot the history is backfilled: the first valid reading of each series
 * fills that series' whole history, so every trend line starts as a flat
 * line across the full width instead of an almost-empty strip. Real data
 * then scrolls in from the right. (This is invented data, chosen because a
 * flat "no change yet" line reads better than a mostly blank one.)
 *
 * history_add runs on the sensor loop; history_get also runs from the
 * button handler (chart pages), so both take `lock`.
 */

#include <math.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "history.h"

/* Completed buckets, as a ring: oldest at (head - count), newest at head - 1. */
static float buckets[H_COUNT][HISTORY_LEN];
static int head;
static int count;

/* The bucket currently being filled. */
static double sum[H_COUNT];
static int n[H_COUNT];
static bool bucket_open;
static int64_t bucket_start_ms;

/* Whether each series has had its first valid reading (and been backfilled). */
static bool seeded[H_COUNT];

static K_MUTEX_DEFINE(lock);

static void close_bucket(void)
{
	for (int s = 0; s < H_COUNT; s++) {
		buckets[s][head] = n[s] > 0 ? (float)(sum[s] / n[s]) : NAN;
		sum[s] = 0.0;
		n[s] = 0;
	}
	head = (head + 1) % HISTORY_LEN;
	count = MIN(count + 1, HISTORY_LEN);
}

static void add(enum history_series s, bool ok, double v)
{
	if (ok && !seeded[s]) {
		for (int i = 0; i < HISTORY_LEN; i++) {
			buckets[s][i] = (float)v;
		}
		seeded[s] = true;
	}
	if (ok) {
		sum[s] += v;
		n[s]++;
	}
}

void history_add(const struct readings *r)
{
	int64_t now = k_uptime_get();

	k_mutex_lock(&lock, K_FOREVER);

	if (!bucket_open) {
		/* Full-length history from the start; series stay NAN until seeded. */
		for (int s = 0; s < H_COUNT; s++) {
			for (int i = 0; i < HISTORY_LEN; i++) {
				buckets[s][i] = NAN;
			}
		}
		count = HISTORY_LEN;
		bucket_open = true;
		bucket_start_ms = now;
	} else if (now - bucket_start_ms >= HISTORY_PERIOD_S * 1000LL) {
		close_bucket();
		bucket_start_ms += HISTORY_PERIOD_S * 1000LL;
	}

	add(H_TEMP, r->climate_ok, r->temp_c);
	add(H_RH, r->climate_ok, r->rh_pct);
	add(H_PRESS, r->climate_ok, r->press_sl_hpa);
	add(H_PM25, r->pm_ok, r->pm2_5);
	add(H_ECO2, r->gas_ok, r->eco2_ppm);
	add(H_TVOC, r->gas_ok, r->tvoc_ppb);

	k_mutex_unlock(&lock);
}

int history_get(enum history_series s, float *out, int max)
{
	k_mutex_lock(&lock, K_FOREVER);

	bool partial = n[s] > 0;
	int total = count + (partial ? 1 : 0);
	int skip = MAX(0, total - max);
	int k = 0;

	for (int i = skip; i < count; i++) {
		out[k++] = buckets[s][(head - count + i + HISTORY_LEN) % HISTORY_LEN];
	}
	if (partial && k < max) {
		out[k++] = (float)(sum[s] / n[s]);
	}

	k_mutex_unlock(&lock);
	return k;
}
