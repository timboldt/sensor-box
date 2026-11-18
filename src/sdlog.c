/*
 * Minute-by-minute log of every reading on the SD card. See sdlog.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * One CSV file per UTC day, /SD:/LOG/YYYYMMDD.CSV, starting with a header
 * row (see cols[]). Each line is one UTC minute: its start time
 * (2026-11-18T04:33:00Z), the average of each reading over the minute and,
 * for the pollutants, the minute's highest reading. A reading with nothing
 * that minute is left empty. New columns only ever go at the end, so older
 * files stay readable.
 *
 * Lines are buffered in RAM and appended every 5 minutes (on the wall
 * clock's 5-minute marks), and at the end of each day, so a crash or
 * reboot loses at most the last few minutes. A torn last line (power cut
 * mid-write) is just a short line: readers should skip lines with the
 * wrong number of fields. Nothing is written while the battery is nearly
 * flat and not charging, so a dying battery can't cut power mid-write.
 *
 * Nothing is logged before the clock is set (no RTC), as with ltlog.c.
 * Without a card the lines are dropped. "sdlog" on the console shows the
 * state; "sdlog flush" writes out the buffer now ("sdcard unmount" does
 * too).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "sdcard.h"
#include "sdlog.h"
#include "timesync.h"

LOG_MODULE_REGISTER(sdlog, LOG_LEVEL_INF);

#define LOG_DIR      SDCARD_MNT "/LOG"
#define FLUSH_MIN    5		/* write out every 5 minutes */
#define LOW_BATT_PCT 5		/* stop writing below this, unless charging */

/* The columns after "time". decimals < 0: 4 significant digits. */
enum col {
	C_TEMP, C_RH, C_AH, C_PRESS, C_PM1, C_PM25, C_PM10, C_PC03, C_CO2, C_TVOC,
	C_LUX, C_BATT_V, C_BATT_PCT, C_BATT_MA, C_COUNT,
};

static const struct {
	const char *name;
	const char *peak;	/* name of its peak column, if it has one */
	int decimals;
} cols[C_COUNT] = {
	[C_TEMP] = { "temp_c", NULL, 2 },
	[C_RH] = { "rh_pct", NULL, 1 },
	[C_AH] = { "ah_g_m3", NULL, 2 },
	[C_PRESS] = { "press_hpa", NULL, 1 },
	[C_PM1] = { "pm1", NULL, 1 },
	[C_PM25] = { "pm25", "pm25_max", 1 },
	[C_PM10] = { "pm10", NULL, 1 },
	[C_PC03] = { "pc03_per_l", "pc03_max", 0 },
	[C_CO2] = { "co2_ppm", "co2_max", 0 },
	[C_TVOC] = { "tvoc_ppb", "tvoc_max", 0 },
	[C_LUX] = { "lux", NULL, -1 },
	[C_BATT_V] = { "batt_v", NULL, 3 },
	[C_BATT_PCT] = { "batt_pct", NULL, 0 },
	[C_BATT_MA] = { "batt_ma", NULL, 1 },
};

static K_MUTEX_DEFINE(lock);

/* The minute being accumulated. */
static struct {
	time_t minute;		/* UTC start; 0 = none */
	double sum[C_COUNT];
	double max[C_COUNT];
	int n[C_COUNT];
} acc;

/* Lines not yet written, all for the same UTC day. */
static char buf[4096];
static size_t used;
static time_t buf_day;		/* UTC midnight of the buffered lines */
static bool low_batt;		/* paused: battery nearly flat */

/* For "sdlog": what happened last. */
static uint32_t lines_written;
static uint32_t lines_dropped;
static int last_err;
static char last_file[48];

static char header[320];

static void build_header(void)
{
	size_t len = snprintf(header, sizeof(header), "time");

	for (int c = 0; c < C_COUNT; c++) {
		len += snprintf(header + len, sizeof(header) - len, ",%s", cols[c].name);
		if (cols[c].peak != NULL) {
			len += snprintf(header + len, sizeof(header) - len, ",%s", cols[c].peak);
		}
	}
	snprintf(header + len, sizeof(header) - len, "\n");
}

static void flush_before_unmount(void)
{
	(void)sdlog_flush();
}

void sdlog_init(void)
{
	build_header();
	sdcard_set_unmount_hook(flush_before_unmount);
}

/* Write out the buffer. Under `lock`. */
static int flush_locked(void)
{
	if (used == 0) {
		return 0;
	}

	int lines = 0;

	for (size_t i = 0; i < used; i++) {
		lines += buf[i] == '\n';
	}

	int ret;

	if (low_batt) {
		ret = -EBUSY;
	} else {
		struct tm tm;
		char path[sizeof(last_file)];

		gmtime_r(&buf_day, &tm);
		snprintf(path, sizeof(path), LOG_DIR "/%04d%02d%02d.CSV", tm.tm_year + 1900,
			 tm.tm_mon + 1, tm.tm_mday);
		ret = sdcard_append(LOG_DIR, path, header, buf, used);
		if (ret == 0) {
			strcpy(last_file, path);
		}
	}

	if (ret == 0) {
		lines_written += lines;
	} else {
		/* Dropped: no card, card full, low battery. Say so once per cause. */
		if (ret != last_err && ret != -ENODEV) {
			LOG_WRN("%d line(s) not written (%d)", lines, ret);
		}
		lines_dropped += lines;
	}
	last_err = ret;
	used = 0;
	return ret;
}

int sdlog_flush(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	int ret = flush_locked();

	k_mutex_unlock(&lock);
	return ret;
}

static void append_value(char *line, size_t *len, size_t size, enum col c, double v, bool ok)
{
	*len += snprintf(line + *len, size - *len, ",");
	if (!ok) {
		return;
	}
	if (cols[c].decimals < 0) {
		*len += snprintf(line + *len, size - *len, "%.4g", v);
	} else {
		*len += snprintf(line + *len, size - *len, "%.*f", cols[c].decimals, v);
	}
}

/* Turn the accumulated minute into a line in the buffer. Under `lock`. */
static void emit_minute(void)
{
	char line[320];
	struct tm tm;
	size_t len;
	time_t day = acc.minute - acc.minute % 86400;

	/* A new day goes in a new file. */
	if (used > 0 && day != buf_day) {
		flush_locked();
	}
	buf_day = day;

	gmtime_r(&acc.minute, &tm);
	len = snprintf(line, sizeof(line), "%04d-%02d-%02dT%02d:%02d:00Z", tm.tm_year + 1900,
		       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
	for (int c = 0; c < C_COUNT; c++) {
		bool ok = acc.n[c] > 0;

		append_value(line, &len, sizeof(line), c, ok ? acc.sum[c] / acc.n[c] : 0.0, ok);
		if (cols[c].peak != NULL) {
			append_value(line, &len, sizeof(line), c, acc.max[c], ok);
		}
	}
	len += snprintf(line + len, sizeof(line) - len, "\n");

	if (used + len > sizeof(buf)) {
		flush_locked();
	}
	memcpy(buf + used, line, len);
	used += len;

	/* Write out on the clock's 5-minute marks (after :04, :09, ...). */
	if ((acc.minute / 60 + 1) % FLUSH_MIN == 0) {
		flush_locked();
	}
}

static void add(enum col c, bool ok, double v)
{
	if (ok) {
		acc.max[c] = acc.n[c] == 0 ? v : MAX(acc.max[c], v);
		acc.sum[c] += v;
		acc.n[c]++;
	}
}

void sdlog_add(const struct readings *r)
{
	if (!timesync_valid()) {
		return;
	}

	time_t now = timesync_now();
	time_t minute = now - now % 60;

	k_mutex_lock(&lock, K_FOREVER);

	if (acc.minute != minute) {
		if (acc.minute != 0) {
			emit_minute();
		}
		memset(&acc, 0, sizeof(acc));
		acc.minute = minute;
	}

	/* Pause while the battery is nearly flat and not charging. */
	bool charging = r->batt_i_ok && r->batt_ma > 0.0;
	bool low = r->batt_ok && r->batt_pct < LOW_BATT_PCT && !charging;

	if (low != low_batt) {
		LOG_INF("battery %s: SD logging %s", low ? "nearly flat" : "OK",
			low ? "paused" : "resumed");
		low_batt = low;
	}

	add(C_TEMP, r->climate_ok, r->temp_c);
	add(C_RH, r->climate_ok, r->rh_pct);
	add(C_AH, r->climate_ok, r->ah_g_m3);
	add(C_PRESS, r->climate_ok, r->press_sl_hpa);
	add(C_PM1, r->pm_ok, r->pm1_0);
	add(C_PM25, r->pm_ok, r->pm2_5);
	add(C_PM10, r->pm_ok, r->pm10);
	add(C_PC03, r->pm_ok, r->pc03_per_l);
	add(C_CO2, r->co2_ok, r->co2_ppm);
	add(C_TVOC, r->gas_ok, r->tvoc_ppb);
	add(C_LUX, r->light_ok, r->lux);
	add(C_BATT_V, r->batt_ok, r->batt_v);
	add(C_BATT_PCT, r->batt_ok, r->batt_pct);
	add(C_BATT_MA, r->batt_i_ok, r->batt_ma);

	k_mutex_unlock(&lock);
}

/* ---- console ---- */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lock, K_FOREVER);
	int buffered = 0;

	for (size_t i = 0; i < used; i++) {
		buffered += buf[i] == '\n';
	}
	shell_print(sh, "card %s; %d line(s) buffered; %u written, %u dropped%s",
		    sdcard_ready() ? "mounted" : "not mounted", buffered, lines_written,
		    lines_dropped, low_batt ? "; paused (battery nearly flat)" : "");
	if (last_file[0] != '\0') {
		shell_print(sh, "last file: %s", last_file);
	}
	if (last_err != 0) {
		shell_print(sh, "last write failed (%d)", last_err);
	}
	k_mutex_unlock(&lock);
	return 0;
}

static int cmd_flush(const struct shell *sh, size_t argc, char **argv)
{
	int ret = sdlog_flush();

	if (ret != 0) {
		shell_error(sh, "flush failed (%d)", ret);
	} else {
		shell_print(sh, "written");
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sdlog_cmds,
	SHELL_CMD_ARG(flush, NULL, "Write the buffered lines now", cmd_flush, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sdlog, &sdlog_cmds, "Minute log on the SD card (no argument: status)",
		   cmd_status);
