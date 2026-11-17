/*
 * Recent history of the main readings. See history.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * Two windows are kept from the same readings: a long one (6 h of 3-minute
 * averages) for the chart pages and the web dashboard, and a short one
 * (15 min of 10-second averages) for the lively trend lines in the tiles.
 *
 * At boot each window is backfilled: the first valid reading of each series
 * fills that series' whole history, so every trend line starts as a flat
 * line across the full width instead of an almost-empty strip. Real data
 * then scrolls in from the right. (This is invented data, chosen because a
 * flat "no change yet" line reads better than a mostly blank one.)
 *
 * history_add runs on the sensor loop; history_get also runs from the
 * button handler (chart pages) and the web server, so all take `lock`.
 */

#include <math.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "history.h"

/* One averaging window: a ring of completed buckets plus the one filling. */
struct window {
	int len;			/* buckets kept, <= HISTORY_LEN */
	int64_t period_ms;
	/* Completed buckets: oldest at (head - count), newest at head - 1. */
	float buckets[H_COUNT][HISTORY_LEN];
	int head;
	int count;
	/* The bucket currently being filled. */
	double sum[H_COUNT];
	int n[H_COUNT];
	bool open;
	int64_t start_ms;
	/* Whether each series has had its first valid reading (and been backfilled). */
	bool seeded[H_COUNT];
};

static struct window long_win = {
	.len = HISTORY_LEN,
	.period_ms = HISTORY_PERIOD_S * 1000LL,
};
static struct window short_win = {
	.len = HISTORY_SHORT_LEN,
	.period_ms = HISTORY_SHORT_PERIOD_S * 1000LL,
};

static K_MUTEX_DEFINE(lock);

static void close_bucket(struct window *w)
{
	for (int s = 0; s < H_COUNT; s++) {
		w->buckets[s][w->head] = w->n[s] > 0 ? (float)(w->sum[s] / w->n[s]) : NAN;
		w->sum[s] = 0.0;
		w->n[s] = 0;
	}
	w->head = (w->head + 1) % w->len;
	w->count = MIN(w->count + 1, w->len);
}

static void add(struct window *w, enum history_series s, bool ok, double v)
{
	if (ok && !w->seeded[s]) {
		for (int i = 0; i < w->len; i++) {
			w->buckets[s][i] = (float)v;
		}
		w->seeded[s] = true;
	}
	if (ok) {
		w->sum[s] += v;
		w->n[s]++;
	}
}

static void window_add(struct window *w, const struct readings *r, int64_t now)
{
	if (!w->open) {
		/* Full-length history from the start; series stay NAN until seeded. */
		for (int s = 0; s < H_COUNT; s++) {
			for (int i = 0; i < w->len; i++) {
				w->buckets[s][i] = NAN;
			}
		}
		w->count = w->len;
		w->open = true;
		w->start_ms = now;
	} else if (now - w->start_ms >= w->period_ms) {
		close_bucket(w);
		w->start_ms += w->period_ms;
	}

	add(w, H_TEMP, r->climate_ok, r->temp_c);
	add(w, H_RH, r->climate_ok, r->rh_pct);
	add(w, H_PRESS, r->climate_ok, r->press_sl_hpa);
	add(w, H_PM25, r->pm_ok, r->pm2_5);
	add(w, H_ECO2, r->gas_ok, r->eco2_ppm);
	add(w, H_TVOC, r->gas_ok, r->tvoc_ppb);
	add(w, H_LUX, r->light_ok, r->lux);
}

void history_add(const struct readings *r)
{
	int64_t now = k_uptime_get();

	k_mutex_lock(&lock, K_FOREVER);
	window_add(&long_win, r, now);
	window_add(&short_win, r, now);
	k_mutex_unlock(&lock);
}

static int window_get(const struct window *w, enum history_series s, float *out, int max)
{
	bool partial = w->n[s] > 0;
	int total = w->count + (partial ? 1 : 0);
	int skip = MAX(0, total - max);
	int k = 0;

	for (int i = skip; i < w->count; i++) {
		out[k++] = w->buckets[s][(w->head - w->count + i + w->len) % w->len];
	}
	if (partial && k < max) {
		out[k++] = (float)(w->sum[s] / w->n[s]);
	}
	return k;
}

int history_get(enum history_series s, float *out, int max)
{
	k_mutex_lock(&lock, K_FOREVER);
	int k = window_get(&long_win, s, out, max);
	k_mutex_unlock(&lock);
	return k;
}

int history_get_short(enum history_series s, float *out, int max)
{
	k_mutex_lock(&lock, K_FOREVER);
	int k = window_get(&short_win, s, out, max);
	k_mutex_unlock(&lock);
	return k;
}
