/*
 * LAN web dashboard (HTTP on port 80).
 *
 * SPDX-License-Identifier: MIT
 *
 *   /                      the dashboard page (web/index.html, gzipped)
 *   /api/now               current readings
 *   /api/recent[?window=short]
 *                          the 6-hour RAM history (3-minute averages), or with
 *                          window=short the 15-minute one (10 s); points
 *                          from before "uptime" seconds ago are the boot-time
 *                          backfill, not measurements
 *   /api/history?since=T   the long-term hourly log from flash, from UTC T on
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>

#include "history.h"
#include "ltlog.h"
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
	jprintf(&j, ",\"press\":");
	jnum(&j, r.climate_ok, r.press_sl_hpa, 1);
	jprintf(&j, ",\"pm1\":");
	jnum(&j, r.pm_ok, r.pm1_0, 0);
	jprintf(&j, ",\"pm25\":");
	jnum(&j, r.pm_ok, r.pm2_5, 0);
	jprintf(&j, ",\"pm10\":");
	jnum(&j, r.pm_ok, r.pm10, 0);
	jprintf(&j, ",\"eco2\":");
	jnum(&j, r.gas_ok, r.eco2_ppm, 0);
	jprintf(&j, ",\"tvoc\":");
	jnum(&j, r.gas_ok, r.tvoc_ppb, 0);
	jprintf(&j, ",\"lux\":");
	jprintf(&j, r.light_ok ? "%.4g" : "null", r.lux);
	jprintf(&j, ",\"batt_v\":");
	jnum(&j, r.batt_ok, r.batt_v, 3);
	jprintf(&j, ",\"batt_pct\":");
	jnum(&j, r.batt_ok, MIN(r.batt_pct, 100), 0);
	jprintf(&j, ",\"pm_warming\":%s,\"gas_warming\":%s}", r.pm_warming ? "true" : "false",
		r.gas_warming ? "true" : "false");

	return respond_json(rsp, &j);
}

/* ---- /api/recent ---- */

static int recent_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *req, struct http_response_ctx *rsp,
			  void *user_data)
{
	static const struct {
		const char *name;
		enum history_series s;
		int decimals;
	} series[] = {
		{ "temp", H_TEMP, 2 }, { "rh", H_RH, 1 },     { "press", H_PRESS, 1 },
		{ "pm25", H_PM25, 1 }, { "eco2", H_ECO2, 0 }, { "tvoc", H_TVOC, 0 },
		{ "lux", H_LUX, 5 },
	};
	static char buf[8192];
	static float pts[HISTORY_LEN];
	struct jbuf j = { buf, sizeof(buf), 0 };

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	/* "?window=short": the 15-minute window the screen's tiles use. */
	char q[32] = "";

	memcpy(q, req->data, MIN(req->data_len, sizeof(q) - 1));
	bool short_window = strstr(q, "window=short") != NULL;
	int len = short_window ? HISTORY_SHORT_LEN : HISTORY_LEN;

	/* Points are oldest first, "period" seconds apart, the last one "now". */
	/* uptime lets the page tell real points from the boot-time backfill. */
	jprintf(&j, "{\"period\":%d,\"uptime\":%lld,\"t\":",
		short_window ? HISTORY_SHORT_PERIOD_S : HISTORY_PERIOD_S, k_uptime_get() / 1000);
	jnum(&j, timesync_valid(), (double)timesync_now(), 0);
	for (size_t i = 0; i < ARRAY_SIZE(series); i++) {
		int n = short_window ? history_get_short(series[i].s, pts, len)
				     : history_get(series[i].s, pts, len);

		jprintf(&j, ",\"%s\":[", series[i].name);
		for (int k = 0; k < n; k++) {
			jprintf(&j, k ? "," : "");
			jnum(&j, !isnan(pts[k]), pts[k], series[i].decimals);
		}
		jprintf(&j, "]");
	}
	jprintf(&j, "}");

	return respond_json(rsp, &j);
}

/* ---- /api/history?since=T (streamed in chunks) ---- */

#define HIST_ROWS_PER_CHUNK 24

static int history_handler(struct http_client_ctx *client, enum http_transaction_status status,
			   const struct http_request_ctx *req, struct http_response_ctx *rsp,
			   void *user_data)
{
	static char buf[HIST_ROWS_PER_CHUNK * 96 + 128];
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
			    "\"pm25\",\"eco2\",\"tvoc\",\"lux\"],\"rows\":[");
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
		jprintf(&j, "%s[%u,", first_row ? "" : ",", rec.t);
		first_row = false;
		jnum(&j, rec.valid & LTLOG_TEMP, rec.temp_c100 / 100.0, 2);
		jprintf(&j, ",");
		jnum(&j, rec.valid & LTLOG_RH, rec.rh10 / 10.0, 1);
		jprintf(&j, ",");
		jnum(&j, rec.valid & LTLOG_PRESS, rec.press10 / 10.0, 1);
		jprintf(&j, ",");
		jnum(&j, rec.valid & LTLOG_PM25, rec.pm25, 0);
		jprintf(&j, ",");
		jnum(&j, rec.valid & LTLOG_ECO2, rec.eco2, 0);
		jprintf(&j, ",");
		jnum(&j, rec.valid & LTLOG_TVOC, rec.tvoc, 0);
		jprintf(&j, ",");
		jprintf(&j, (rec.valid & LTLOG_LUX) ? "%.4g" : "null", ltlog_lux(&rec));
		jprintf(&j, "]");
		rows++;
	}
	if (done) {
		jprintf(&j, "]}");
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

static uint16_t http_port = 80;

HTTP_SERVICE_DEFINE(dashboard, NULL, &http_port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 4, NULL, NULL,
		    NULL);
HTTP_RESOURCE_DEFINE(index_res, dashboard, "/", &index_detail);
HTTP_RESOURCE_DEFINE(now_res, dashboard, "/api/now", &now_detail);
HTTP_RESOURCE_DEFINE(recent_res, dashboard, "/api/recent", &recent_detail);
HTTP_RESOURCE_DEFINE(history_res, dashboard, "/api/history", &history_detail);

void web_init(void)
{
	int ret = http_server_start();

	if (ret != 0) {
		LOG_ERR("HTTP server failed to start (%d)", ret);
	} else {
		LOG_INF("dashboard on port %u", http_port);
	}
}
