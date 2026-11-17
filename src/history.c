/*
 * Recent history of the main readings. See history.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * Two windows are kept from the same readings: a long one (24 h of 1-minute
 * averages) for the chart pages and the web dashboard, and a short one
 * (15 min of 10-second averages) for the lively trend lines in the tiles.
 *
 * Buckets are stored as 16-bit codes (see enc[]) rather than floats, which
 * halves the long window to about 20 KB. Each series' resolution is well
 * below what's displayed (0.01 C, 0.01 %RH, 0.1 hPa, 0.1 ug/m3, 1 ppm/ppb,
 * 0.01 dB of light).
 *
 * At boot each window is backfilled: the first valid reading of each series
 * fills that series' whole history, so every trend line starts as a flat
 * line across the full width instead of an almost-empty strip. Real data
 * then scrolls in from the right. (This is invented data, chosen because a
 * flat "no change yet" line reads better than a mostly blank one; the web
 * page drops it using the uptime.)
 *
 * history_add runs on the sensor loop; the getters also run from the
 * button handler (chart pages) and the web server, so all take `lock`.
 */

#include <math.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "history.h"
#include "light.h"

#define NONE 0xFFFF	/* code for "no valid reading in this bucket" */

/* code = (value + offset) * scale; light is coded as dB re 1 lux. */
static const struct {
	float scale;
	float offset;
} enc[H_COUNT] = {
	[H_TEMP] = { 100.0f, 100.0f },	/* -100 .. 555 C */
	[H_RH] = { 100.0f, 0.0f },
	[H_PRESS] = { 10.0f, 0.0f },
	[H_PM25] = { 10.0f, 0.0f },
	[H_ECO2] = { 1.0f, 0.0f },	/* the SGP30 tops out at 60000 */
	[H_TVOC] = { 1.0f, 0.0f },
	[H_LUX] = { 100.0f, 100.0f },	/* -100 .. 555 dB */
};

static uint16_t encode(enum history_series s, double v)
{
	if (s == H_LUX) {
		v = light_db(v);
	}
	double c = round((v + (double)enc[s].offset) * (double)enc[s].scale);

	return (uint16_t)CLAMP(c, 0.0, (double)(NONE - 1));
}

static float decode(enum history_series s, uint16_t c)
{
	if (c == NONE) {
		return NAN;
	}
	float v = c / enc[s].scale - enc[s].offset;

	return s == H_LUX ? powf(10.0f, v / 10.0f) : v;
}

/* One averaging window: a ring of completed buckets plus the one filling. */
struct window {
	int len;
	int64_t period_ms;
	uint16_t (*buckets)[H_COUNT];	/* [len][H_COUNT] codes */
	/* Buckets completed so far; bucket k is at k % len, and [seq - len, seq)
	 * are kept. Starts at len, so the backfilled history counts as kept.
	 */
	uint32_t seq;
	/* The bucket currently being filled. */
	double sum[H_COUNT];
	int n[H_COUNT];
	bool open;
	int64_t start_ms;
	/* Whether each series has had its first valid reading (and been backfilled). */
	bool seeded[H_COUNT];
};

static uint16_t long_buckets[HISTORY_LEN][H_COUNT];
static uint16_t short_buckets[HISTORY_SHORT_LEN][H_COUNT];

static struct window windows[] = {
	[HW_LONG] = {
		.len = HISTORY_LEN,
		.period_ms = HISTORY_PERIOD_S * 1000LL,
		.buckets = long_buckets,
	},
	[HW_SHORT] = {
		.len = HISTORY_SHORT_LEN,
		.period_ms = HISTORY_SHORT_PERIOD_S * 1000LL,
		.buckets = short_buckets,
	},
};

static K_MUTEX_DEFINE(lock);

static void close_bucket(struct window *w)
{
	uint16_t *b = w->buckets[w->seq % w->len];

	for (int s = 0; s < H_COUNT; s++) {
		b[s] = w->n[s] > 0 ? encode(s, w->sum[s] / w->n[s]) : NONE;
		w->sum[s] = 0.0;
		w->n[s] = 0;
	}
	w->seq++;
}

static void add(struct window *w, enum history_series s, bool ok, double v)
{
	if (!ok) {
		return;
	}
	if (!w->seeded[s]) {
		uint16_t c = encode(s, v);

		for (int i = 0; i < w->len; i++) {
			w->buckets[i][s] = c;
		}
		w->seeded[s] = true;
	}
	w->sum[s] += v;
	w->n[s]++;
}

static void window_add(struct window *w, const struct readings *r, int64_t now)
{
	if (!w->open) {
		/* Full-length history from the start; series stay NONE until seeded. */
		for (int i = 0; i < w->len; i++) {
			for (int s = 0; s < H_COUNT; s++) {
				w->buckets[i][s] = NONE;
			}
		}
		w->seq = w->len;
		w->open = true;
		w->start_ms = now;
	} else {
		/* Close every bucket that has ended (more than one if a report was late). */
		while (now - w->start_ms >= w->period_ms) {
			close_bucket(w);
			w->start_ms += w->period_ms;
		}
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
	for (size_t i = 0; i < ARRAY_SIZE(windows); i++) {
		window_add(&windows[i], r, now);
	}
	k_mutex_unlock(&lock);
}

/*
 * Point i of a window's series, where points are the kept buckets (oldest
 * first) followed by the filling one: as a sum and count, for averaging.
 */
static void point(const struct window *w, enum history_series s, int i, double *sum, int *n)
{
	if (i < w->len) {
		float v = decode(s, w->buckets[(w->seq - w->len + i) % w->len][s]);

		if (!isnan(v)) {
			*sum += (double)v;
			(*n)++;
		}
	} else if (w->n[s] > 0) {
		*sum += w->sum[s] / w->n[s];
		(*n)++;
	}
}

int history_get(enum history_window wi, enum history_series s, float *out, int max, int group)
{
	const struct window *w = &windows[wi];
	int k = 0;

	group = MAX(group, 1);
	k_mutex_lock(&lock, K_FOREVER);
	if (w->open) {
		/* The filling bucket only counts once it has a reading. */
		int total = w->len + (w->n[s] > 0 ? 1 : 0);
		int count = MIN(max, (total + group - 1) / group);

		/* Group j (0 = oldest output) ends at point end_j; fill from the newest. */
		for (int j = count - 1; j >= 0; j--) {
			int end = total - 1 - (count - 1 - j) * group;
			double sum = 0.0;
			int n = 0;

			for (int i = MAX(end - group + 1, 0); i <= end; i++) {
				point(w, s, i, &sum, &n);
			}
			out[j] = n > 0 ? (float)(sum / n) : NAN;
		}
		k = count;
	}
	k_mutex_unlock(&lock);
	return k;
}

uint32_t history_snapshot(enum history_window wi, int64_t *age_ms)
{
	const struct window *w = &windows[wi];

	k_mutex_lock(&lock, K_FOREVER);
	uint32_t seq = w->open ? w->seq : 0;

	*age_ms = w->open ? k_uptime_get() - w->start_ms : 0;
	k_mutex_unlock(&lock);
	return seq;
}

void history_get_seq(enum history_window wi, enum history_series s, uint32_t first, int count,
		     float *out)
{
	const struct window *w = &windows[wi];

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < count; i++) {
		uint32_t k = first + i;
		bool kept = w->open && k < w->seq && w->seq - k <= (uint32_t)w->len;

		out[i] = kept ? decode(s, w->buckets[k % w->len][s]) : NAN;
	}
	k_mutex_unlock(&lock);
}
