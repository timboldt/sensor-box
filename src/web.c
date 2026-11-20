/*
 * LAN web dashboard (HTTP on port 80).
 *
 * SPDX-License-Identifier: MIT
 *
 *   /                      the dashboard page (web/index.html, gzipped)
 *   /api/now               current readings
 *   /api/recent[?window=short][&points=N][&group=G]
 *                          the last N points of the 24-hour RAM history
 *                          (1-minute averages), or with window=short the
 *                          15-minute one (10 s); with group=G each point
 *                          averages G of those (period G times as long), so
 *                          a day's chart needn't fetch every minute; points
 *                          from before
 *                          "covered" seconds ago (the uptime, or more if
 *                          history was restored from the SD card at boot)
 *                          are the boot-time backfill, not measurements
 *   /api/history?since=T   the long-term hourly log from flash, from UTC T on,
 *                          plus the hour so far ("current", or null)
 *   /api/minutes?since=T&until=U[&step=S]
 *                          the SD card's minute log (CSV, as on the card),
 *                          the lines from UTC T to U (at most 2 days), each
 *                          file's lines after its own header row; empty
 *                          without a card. The last few minutes may not be
 *                          on the card yet. With step=S (a divisor of a
 *                          day's 1440 minutes), S-minute averages of just
 *                          the charted columns instead, each row at its
 *                          slot's start.
 *
 * Values are metric; "us_units" in /api/now tells the page how to display
 * them. Missing values are null. There is no login: anyone on the LAN can
 * read the data, and nothing can be changed.
 *
 * All handlers run on the HTTP server's thread, one request at a time, so
 * their static buffers are not shared.
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>

#include "history.h"
#include "ltlog.h"
#include "sdcard.h"
#include "sdlog.h"
#include "timesync.h"
#include "web.h"

LOG_MODULE_REGISTER(web, LOG_LEVEL_INF);

/* ---- latest readings, shared with the sensor loop ---- */

static struct k_spinlock lock;
static struct readings latest;
static bool have_latest;

void web_update(const struct readings *r)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	latest = *r;
	have_latest = true;
	k_spin_unlock(&lock, key);
}

/* ---- tiny JSON writer into a fixed buffer ---- */

struct jbuf {
	char *buf;
	size_t size;
	size_t len;
};

static void jprintf(struct jbuf *j, const char *fmt, ...)
{
	va_list ap;

	if (j->len >= j->size) {
		return;
	}
	va_start(ap, fmt);
	int n = vsnprintf(j->buf + j->len, j->size - j->len, fmt, ap);

	va_end(ap);
	j->len = MIN(j->len + (n > 0 ? n : 0), j->size);
}

/* A number, or null if missing / not finite. */
static void jnum(struct jbuf *j, bool ok, double v, int decimals)
{
	if (ok && isfinite(v)) {
		jprintf(j, "%.*f", decimals, v);
	} else {
		jprintf(j, "null");
	}
}

static const struct http_header json_headers[] = {
	{ .name = "Content-Type", .value = "application/json" },
	{ .name = "Cache-Control", .value = "no-store" },
};

/* Common shape of a single-chunk JSON response. */
static int respond_json(struct http_response_ctx *rsp, const struct jbuf *j)
{
	if (j->len >= j->size) {
		LOG_ERR("JSON response truncated");
		return -ENOMEM;
	}
	rsp->status = HTTP_200_OK;
	rsp->headers = json_headers;
	rsp->header_count = ARRAY_SIZE(json_headers);
	rsp->body = (const uint8_t *)j->buf;
	rsp->body_len = j->len;
	rsp->final_chunk = true;
	return 0;
}

/* ---- /api/now ---- */

static int now_handler(struct http_client_ctx *client, enum http_transaction_status status,
		       const struct http_request_ctx *req, struct http_response_ctx *rsp,
		       void *user_data)
{
	static char buf[640];
	struct jbuf j = { buf, sizeof(buf), 0 };
	struct readings r;
	bool have;

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	r = latest;
	have = have_latest;
	k_spin_unlock(&lock, key);

	jprintf(&j, "{\"t\":");
	jnum(&j, timesync_valid(), (double)timesync_now(), 0);
	jprintf(&j, ",\"uptime\":%lld,\"us_units\":%s,\"ready\":%s", k_uptime_get() / 1000,
		IS_ENABLED(CONFIG_SENSOR_BOX_US_UNITS) ? "true" : "false",
		have ? "true" : "false");
	jprintf(&j, ",\"temp\":");
	jnum(&j, r.climate_ok, r.temp_c, 2);
	jprintf(&j, ",\"rh\":");
	jnum(&j, r.climate_ok, r.rh_pct, 1);
	jprintf(&j, ",\"ah\":");
	jnum(&j, r.climate_ok, r.ah_g_m3, 2);
	jprintf(&j, ",\"press\":");
	jnum(&j, r.climate_ok, r.press_sl_hpa, 1);
	jprintf(&j, ",\"pm1\":");
	jnum(&j, r.pm_ok, r.pm1_0, 0);
	jprintf(&j, ",\"pm25\":");
	jnum(&j, r.pm_ok, r.pm2_5, 0);
	jprintf(&j, ",\"pm10\":");
	jnum(&j, r.pm_ok, r.pm10, 0);
	jprintf(&j, ",\"pc03\":");
	jnum(&j, r.pm_ok, r.pc03_per_l, 0);
	jprintf(&j, ",\"co2\":");
	jnum(&j, r.co2_ok, r.co2_ppm, 0);
	jprintf(&j, ",\"tvoc\":");
	jnum(&j, r.gas_ok, r.tvoc_ppb, 0);
	jprintf(&j, ",\"lux\":");
	jprintf(&j, r.light_ok ? "%.4g" : "null", r.lux);
	jprintf(&j, ",\"batt_v\":");
	jnum(&j, r.batt_ok, r.batt_v, 3);
	jprintf(&j, ",\"batt_ma\":");
	jnum(&j, r.batt_i_ok, r.batt_ma, 1);
	jprintf(&j, ",\"batt_pct\":");
	jnum(&j, r.batt_ok, MIN(r.batt_pct, 100), 0);
	jprintf(&j, ",\"pm_warming\":%s,\"gas_warming\":%s,\"co2_warming\":%s"
		    ",\"co2_present\":%s}",
		r.pm_warming ? "true" : "false", r.gas_warming ? "true" : "false",
		r.co2_warming ? "true" : "false", r.co2_present ? "true" : "false");

	return respond_json(rsp, &j);
}

/* ---- /api/recent[?window=short][&points=N][&group=G] (streamed in chunks) ---- */

#define RECENT_POINTS_PER_CHUNK 240
#define RECENT_MAX_GROUP        60

static int recent_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *req, struct http_response_ctx *rsp,
			  void *user_data)
{
	static const struct {
		const char *name;
		enum history_series s;
		int decimals;		/* -1: 4 significant digits */
	} series[] = {
		{ "temp", H_TEMP, 2 }, { "ah", H_AH, 2 },     { "press", H_PRESS, 1 },
		{ "pm25", H_PM25, 1 }, { "co2", H_CO2, 0 } , { "tvoc", H_TVOC, 0 }, { "pc03", H_PC03, 0 }, { "batt_ma", H_BATT_I, 1 },
		{ "lux", H_LUX, -1 },
	};
	static char buf[RECENT_POINTS_PER_CHUNK * 12 + 256];
	static float raw[RECENT_MAX_GROUP];
	/* Where the response has got to, between chunks. */
	static bool started;
	static enum history_window w;
	static uint32_t first;		/* sequence number of the oldest point sent */
	static int count;		/* points per series */
	static int group;		/* buckets averaged into each point */
	static size_t si;		/* series being sent */
	static int pos;			/* next point of that series */
	struct jbuf j = { buf, sizeof(buf), 0 };

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		started = false;
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (!started) {
		/* First call: the query string (if any) is in req->data. */
		char q[48] = "";
		const char *p;

		memcpy(q, req->data, MIN(req->data_len, sizeof(q) - 1));
		w = strstr(q, "window=short") != NULL ? HW_SHORT : HW_LONG;
		int len = w == HW_SHORT ? HISTORY_SHORT_LEN : HISTORY_LEN;

		p = strstr(q, "group=");
		group = p ? CLAMP(atoi(p + 6), 1, RECENT_MAX_GROUP) : 1;
		p = strstr(q, "points=");
		count = p ? CLAMP(atoi(p + 7), 1, len / group) : len / group;

		/*
		 * Every series is read up to the same completed bucket, so they
		 * line up even if a bucket closes while this is being sent.
		 */
		int64_t age_ms;
		uint32_t end = history_snapshot(w, &age_ms);

		count = MIN(count, (int)(end / group));
		first = end - count * group;
		si = 0;
		pos = 0;
		started = true;

		rsp->status = HTTP_200_OK;
		rsp->headers = json_headers;
		rsp->header_count = ARRAY_SIZE(json_headers);
		/*
		 * Points are oldest first, "period" seconds apart; the last one
		 * ended "age" seconds before "t". "covered" lets the page tell
		 * real points from the boot-time backfill.
		 */
		jprintf(&j, "{\"period\":%d,\"uptime\":%lld,\"covered\":%lld,\"age\":%lld,"
			    "\"t\":",
			(w == HW_SHORT ? HISTORY_SHORT_PERIOD_S : HISTORY_PERIOD_S) * group,
			k_uptime_get() / 1000, history_covered_s(w), age_ms / 1000);
		jnum(&j, timesync_valid(), (double)timesync_now(), 0);
	}

	/* Up to one chunk's worth of points, across series boundaries. */
	int sent = 0;

	while (si < ARRAY_SIZE(series) && sent < RECENT_POINTS_PER_CHUNK) {
		if (pos == 0) {
			jprintf(&j, ",\"%s\":[", series[si].name);
		}
		int n = MIN(count - pos, RECENT_POINTS_PER_CHUNK - sent);

		for (int k = 0; k < n; k++) {
			/* The average of the point's buckets that have a value. */
			double sum = 0.0;
			int got = 0;

			history_get_seq(w, series[si].s, first + (uint32_t)(pos + k) * group, group,
					raw);
			for (int g = 0; g < group; g++) {
				if (!isnan(raw[g])) {
					sum += (double)raw[g];
					got++;
				}
			}
			float v = got ? (float)(sum / got) : NAN;

			jprintf(&j, pos + k ? "," : "");
			if (series[si].decimals < 0) {
				jprintf(&j, isnan(v) ? "null" : "%.4g", (double)v);
			} else {
				jnum(&j, true, v, series[si].decimals);
			}
		}
		pos += n;
		sent += n;
		if (pos >= count) {
			jprintf(&j, "]");
			si++;
			pos = 0;
		}
	}
	bool done = si >= ARRAY_SIZE(series);

	if (done) {
		jprintf(&j, "}");
	}
	if (j.len >= j.size) {
		LOG_ERR("recent chunk truncated");
		return -ENOMEM;
	}

	rsp->body = (const uint8_t *)buf;
	rsp->body_len = j.len;
	rsp->final_chunk = done;
	return 0;
}

/* ---- /api/history?since=T (streamed in chunks) ---- */

/* One /api/history row: [t, temp, rh, ...] in the order of "fields". */
static void history_row(struct jbuf *j, const struct ltlog_record *r)
{
	jprintf(j, "[%u,", r->t);
	jnum(j, r->valid & LTLOG_TEMP, r->temp_c100 / 100.0, 2);
	jprintf(j, ",");
	jnum(j, r->valid & LTLOG_RH, r->rh10 / 10.0, 1);
	jprintf(j, ",");
	jnum(j, r->valid & LTLOG_PRESS, r->press10 / 10.0, 1);
	jprintf(j, ",");
	jnum(j, r->valid & LTLOG_PM25, r->pm25, 0);
	jprintf(j, ",");
	jnum(j, r->valid & LTLOG_CO2, r->co2, 0);
	jprintf(j, ",");
	jnum(j, r->valid & LTLOG_TVOC, r->tvoc, 0);
	jprintf(j, ",");
	jprintf(j, (r->valid & LTLOG_LUX) ? "%.4g" : "null", ltlog_lux(r));
	/* Peaks: null for records written before peaks were kept. */
	bool peaks = ltlog_has_peaks(r);

	jprintf(j, ",");
	jnum(j, peaks && (r->valid & LTLOG_PM25), r->pm25_max, 0);
	jprintf(j, ",");
	jnum(j, peaks && (r->valid & LTLOG_CO2), r->co2_max, 0);
	jprintf(j, ",");
	jnum(j, peaks && (r->valid & LTLOG_TVOC), r->tvoc_max, 0);
	/* Particle count, per litre (stored per 0.1 L); null before v5. */
	jprintf(j, ",");
	jnum(j, r->valid & LTLOG_PC03, r->pc03 * 10.0, 0);
	jprintf(j, ",");
	jnum(j, r->valid & LTLOG_PC03, r->pc03_max * 10.0, 0);
	jprintf(j, ",");
	jnum(j, r->batt_ma10 != LTLOG_NO_CURRENT, r->batt_ma10 / 10.0, 1);
	jprintf(j, "]");
}

#define HIST_ROWS_PER_CHUNK 24

static int history_handler(struct http_client_ctx *client, enum http_transaction_status status,
			   const struct http_request_ctx *req, struct http_response_ctx *rsp,
			   void *user_data)
{
	static char buf[(HIST_ROWS_PER_CHUNK + 1) * 140 + 200];
	static struct ltlog_cursor cur;
	static uint32_t since;
	static bool started;
	static bool first_row;
	struct jbuf j = { buf, sizeof(buf), 0 };

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		started = false;
		return 0;
	}

	if (!started) {
		/* First call: the query string (if any) is in req->data. */
		char q[48] = "";
		const char *p;

		memcpy(q, req->data, MIN(req->data_len, sizeof(q) - 1));
		p = strstr(q, "since=");
		since = p ? (uint32_t)strtoul(p + 6, NULL, 10) : 0;
		memset(&cur, 0, sizeof(cur));
		started = true;
		first_row = true;

		rsp->status = HTTP_200_OK;
		rsp->headers = json_headers;
		rsp->header_count = ARRAY_SIZE(json_headers);
		jprintf(&j, "{\"period\":3600,\"fields\":[\"t\",\"temp\",\"rh\",\"press\","
			    "\"pm25\",\"co2\",\"tvoc\",\"lux\",\"pm25_max\",\"co2_max\","
			    "\"tvoc_max\",\"pc03\",\"pc03_max\",\"batt_ma\"],\"rows\":[");
	}

	struct ltlog_record rec;
	int rows = 0;
	bool done = false;

	while (rows < HIST_ROWS_PER_CHUNK) {
		if (ltlog_next(&cur, &rec) != 0) {
			done = true;
			break;
		}
		if (rec.t < since) {
			continue;
		}
		jprintf(&j, "%s", first_row ? "" : ",");
		first_row = false;
		history_row(&j, &rec);
		rows++;
	}
	if (done) {
		/* The hour so far, so a chart's peaks can reach the present. */
		jprintf(&j, "],\"current\":");
		if (ltlog_current(&rec)) {
			history_row(&j, &rec);
		} else {
			jprintf(&j, "null");
		}
		jprintf(&j, "}");
	}
	if (j.len >= j.size) {
		LOG_ERR("history chunk truncated");
		return -ENOMEM;
	}

	rsp->body = (const uint8_t *)buf;
	rsp->body_len = j.len;
	rsp->final_chunk = done;
	return 0;
}

/* ---- /api/minutes?since=T&until=U (CSV, streamed in chunks) ---- */

#define MINUTES_MAX_SPAN (2 * 86400)
#define MINUTES_CHUNK    3072	/* send about this much at a time */
#define MINUTES_READ     4096	/* read the card this much at a time */
#define MINUTES_LINE     512

static const struct http_header csv_headers[] = {
	{ .name = "Content-Type", .value = "text/csv" },
	{ .name = "Cache-Control", .value = "no-store" },
};

/*
 * The columns averaged with step=S, as named in the log's header, and how
 * they're printed (decimals < 0: 4 significant digits).
 */
static const struct {
	const char *name;
	int decimals;
} step_cols[] = {
	{ "temp_c", 2 }, { "ah_g_m3", 2 }, { "press_hpa", 1 }, { "pm25", 1 }, { "co2_ppm", 0 },
	{ "tvoc_ppb", 0 }, { "pc03_per_l", 0 }, { "lux", -1 }, { "batt_ma", 1 },
};

#define STEP_COLS  ARRAY_SIZE(step_cols)
#define MAX_FIELDS 48

/* The slot being averaged (step=S), between lines and chunks. */
static struct {
	int step;		/* minutes; 1 = send the log's lines as they are */
	int nfields;		/* in this file's header */
	int col[STEP_COLS];	/* field index of each column, -1 if absent */
	time_t slot;		/* start of the slot being summed; 0 = none */
	double sum[STEP_COLS];
	int n[STEP_COLS];
} agg;

/* Split a CSV line in place. Returns the number of fields. */
static int split_fields(char *line, char *field[], int max)
{
	int n = 0;
	char *p = line;

	while (n < max) {
		field[n++] = p;
		p = strchr(p, ',');
		if (p == NULL) {
			break;
		}
		*p++ = '\0';
	}
	return n;
}

/* "2026-11-18T04:33:00Z": the log's time format, which sorts as text. */
static void iso_minute(time_t t, char *out, size_t size)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:00Z", tm.tm_year + 1900, tm.tm_mon + 1,
		 tm.tm_mday, tm.tm_hour, tm.tm_min);
}

/*
 * A line start in the file `path` at or before its first minute >= iso, so
 * reading needn't start from the top: a binary search on the lines' times,
 * which are in order. 0 if nothing better is found.
 */
static size_t seek_minute(const char *path, const char *iso)
{
	static char probe[320];
	size_t lo = 0, hi = 4 * 1024 * 1024;	/* far more than a day's file */

	while (hi - lo > sizeof(probe)) {
		size_t mid = lo + (hi - lo) / 2;
		int n = sdcard_read_at(path, mid, probe, sizeof(probe));

		if (n <= 0) {
			hi = mid;	/* past the end */
			continue;
		}

		const char *nl = memchr(probe, '\n', n);

		if (nl == NULL || probe + n - (nl + 1) < 20) {
			hi = mid;	/* no whole timestamp here: look earlier */
			continue;
		}
		if (strncmp(nl + 1, iso, 20) < 0) {
			lo = mid + (nl + 1 - probe);	/* that line is before iso */
		} else {
			hi = mid;
		}
	}
	return lo;
}

/* Append the slot being averaged, if any, as a CSV row; then clear it. */
static void agg_emit(char *out, size_t *len)
{
	if (agg.slot == 0) {
		return;
	}

	char t[40];

	iso_minute(agg.slot, t, sizeof(t));
	*len += sprintf(out + *len, "%s", t);
	for (size_t c = 0; c < STEP_COLS; c++) {
		out[(*len)++] = ',';
		if (agg.n[c] == 0) {
			continue;
		}
		double v = agg.sum[c] / agg.n[c];

		if (step_cols[c].decimals < 0) {
			*len += sprintf(out + *len, "%.4g", v);
		} else {
			*len += sprintf(out + *len, "%.*f", step_cols[c].decimals, v);
		}
	}
	out[(*len)++] = '\n';
	memset(&agg.sum, 0, sizeof(agg.sum));
	memset(&agg.n, 0, sizeof(agg.n));
	agg.slot = 0;
}

/* A file's header row (step=S): find the columns, send our own header. */
static void agg_header(char *line, char *out, size_t *len)
{
	char *field[MAX_FIELDS];

	agg.nfields = split_fields(line, field, MAX_FIELDS);
	*len += sprintf(out + *len, "time");
	for (size_t c = 0; c < STEP_COLS; c++) {
		agg.col[c] = -1;
		for (int i = 0; i < agg.nfields; i++) {
			if (strcmp(field[i], step_cols[c].name) == 0) {
				agg.col[c] = i;
			}
		}
		*len += sprintf(out + *len, ",%s", step_cols[c].name);
	}
	out[(*len)++] = '\n';
}

/* A minute in range (step=S): add it to its slot, sending the last one. */
static void agg_line(char *line, char *out, size_t *len)
{
	char *field[MAX_FIELDS];
	int y, mo, d, h, mi;

	if (split_fields(line, field, MAX_FIELDS) != agg.nfields ||
	    sscanf(field[0], "%4d-%2d-%2dT%2d:%2d", &y, &mo, &d, &h, &mi) != 5) {
		return;		/* torn or damaged line */
	}

	time_t t = (time_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60);
	time_t slot = t - t % (agg.step * 60);

	if (slot != agg.slot) {
		agg_emit(out, len);
		agg.slot = slot;
	}
	for (size_t c = 0; c < STEP_COLS; c++) {
		const char *f = agg.col[c] >= 0 ? field[agg.col[c]] : "";

		if (*f != '\0') {
			agg.sum[c] += strtod(f, NULL);
			agg.n[c]++;
		}
	}
}

static int minutes_handler(struct http_client_ctx *client, enum http_transaction_status status,
			   const struct http_request_ctx *req, struct http_response_ctx *rsp,
			   void *user_data)
{
	static char out[MINUTES_CHUNK + MINUTES_READ + MINUTES_LINE];
	static char piece[MINUTES_READ];
	/* Where the response has got to, between chunks. */
	static bool started;
	static char from_iso[40], to_iso[40];
	static time_t day, first_day, last_day;	/* the file being read; the range */
	static size_t offset;		/* in that file */
	static char line[MINUTES_LINE];
	static size_t line_len;
	static bool overlong;		/* the current line didn't fit: skip it */
	static bool header_seen;	/* in this file */
	size_t len = 0;
	bool done = false;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		started = false;
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (!started) {
		/* First call: the query string (if any) is in req->data. */
		char q[80] = "";
		const char *p;

		memcpy(q, req->data, MIN(req->data_len, sizeof(q) - 1));
		p = strstr(q, "until=");
		time_t until = p ? (time_t)strtoul(p + 6, NULL, 10) : timesync_now();
		p = strstr(q, "since=");
		time_t since = p ? (time_t)strtoul(p + 6, NULL, 10) : until - 86400;
		p = strstr(q, "step=");
		int step = p ? atoi(p + 5) : 1;

		memset(&agg, 0, sizeof(agg));
		agg.step = step >= 1 && step <= 1440 && 1440 % step == 0 ? step : 1;

		since = MAX(since, until - MINUTES_MAX_SPAN);
		iso_minute(since, from_iso, sizeof(from_iso));
		iso_minute(until, to_iso, sizeof(to_iso));
		day = first_day = since - since % 86400;
		last_day = until - until % 86400;
		offset = 0;
		line_len = 0;
		overlong = false;
		header_seen = false;
		started = true;

		rsp->status = HTTP_200_OK;
		rsp->headers = csv_headers;
		rsp->header_count = ARRAY_SIZE(csv_headers);
	}

	while (len < MINUTES_CHUNK) {
		if (day > last_day || !sdcard_ready()) {
			agg_emit(out, &len);
			done = true;
			break;
		}

		char path[48];

		sdlog_path(day, path, sizeof(path));

		if (!header_seen) {
			/* Send the file's header row, then skip to the range's start. */
			int n = sdcard_read_at(path, 0, piece, sizeof(piece));
			const char *nl = n > 0 ? memchr(piece, '\n', n) : NULL;

			if (nl == NULL) {
				day += 86400;	/* no such file, or no header */
				continue;
			}
			size_t hl = nl - piece;

			if (hl > 0 && piece[hl - 1] == '\r') {
				hl--;
			}
			if (agg.step > 1) {
				hl = MIN(hl, sizeof(line) - 1);
				memcpy(line, piece, hl);
				line[hl] = '\0';
				agg_header(line, out, &len);
			} else {
				memcpy(out + len, piece, hl);
				len += hl;
				out[len++] = '\n';
			}
			header_seen = true;
			offset = nl + 1 - piece;
			if (day == first_day) {
				offset = MAX(offset, seek_minute(path, from_iso));
			}
			continue;
		}

		int n = sdcard_read_at(path, offset, piece, sizeof(piece));

		if (n <= 0) {
			/* End of this file, or none for that day: on to the next. */
			agg_emit(out, &len);
			day += 86400;
			offset = 0;
			line_len = 0;
			overlong = false;
			header_seen = false;
			continue;
		}
		offset += n;

		for (int i = 0; i < n; i++) {
			char ch = piece[i];

			if (ch == '\r') {
				continue;
			}
			if (ch != '\n') {
				if (line_len < sizeof(line) - 1) {
					line[line_len++] = ch;
				} else {
					overlong = true;
				}
				continue;
			}

			/* A whole line: the header, or a minute in range. */
			line[line_len] = '\0';
			bool keep = false;

			if (overlong || line_len == 0) {
				/* skip */
			} else if (strncmp(line, from_iso, 20) >= 0 &&
				   strncmp(line, to_iso, 20) <= 0) {
				keep = true;
			} else if (strncmp(line, to_iso, 20) > 0) {
				/* Lines are in time order: the rest of the file is later. */
				offset = SIZE_MAX;
			}
			if (keep && agg.step > 1) {
				agg_line(line, out, &len);
			} else if (keep) {
				memcpy(out + len, line, line_len);
				len += line_len;
				out[len++] = '\n';
			}
			line_len = 0;
			overlong = false;
			if (offset == SIZE_MAX) {
				break;
			}
		}
	}

	rsp->body = (const uint8_t *)out;
	rsp->body_len = len;
	rsp->final_chunk = done;
	return 0;
}

/* ---- the page and the service ---- */

static const uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};

static struct http_resource_detail_static index_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/html; charset=utf-8",
	},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};

#define DYNAMIC(name, handler)                                                         \
	static struct http_resource_detail_dynamic name = {                            \
		.common = {                                                            \
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,                            \
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),            \
		},                                                                     \
		.cb = handler,                                                         \
	}

DYNAMIC(now_detail, now_handler);
DYNAMIC(recent_detail, recent_handler);
DYNAMIC(history_detail, history_handler);
DYNAMIC(minutes_detail, minutes_handler);

static uint16_t http_port = 80;

HTTP_SERVICE_DEFINE(dashboard, NULL, &http_port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 4, NULL, NULL,
		    NULL);
HTTP_RESOURCE_DEFINE(index_res, dashboard, "/", &index_detail);
HTTP_RESOURCE_DEFINE(now_res, dashboard, "/api/now", &now_detail);
HTTP_RESOURCE_DEFINE(recent_res, dashboard, "/api/recent", &recent_detail);
HTTP_RESOURCE_DEFINE(history_res, dashboard, "/api/history", &history_detail);
HTTP_RESOURCE_DEFINE(minutes_res, dashboard, "/api/minutes", &minutes_detail);

void web_init(void)
{
	int ret = http_server_start();

	if (ret != 0) {
		LOG_ERR("HTTP server failed to start (%d)", ret);
	} else {
		LOG_INF("dashboard on port %u", http_port);
	}
}
