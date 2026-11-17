/*
 * Long-term log: hourly averages of every reading, kept in flash.
 *
 * SPDX-License-Identifier: MIT
 *
 * Readings are averaged per UTC clock hour and, when the hour ends, written
 * as one 20-byte record to an FCB (flash circular buffer) in the "history"
 * partition. When the buffer is full the oldest 4 KB sector is erased; at
 * 28 bytes per record in flash (length header and CRC, each padded to the
 * 4-byte write size) 128 KB holds about 6 months.
 *
 * Readings before the clock is first set (no RTC; ~15 s after boot) are not
 * recorded, since they can't be timestamped. A reboot loses the partial hour
 * in progress; the hour's record then covers only the time after the reboot.
 *
 * "ltlog flush" (testing) writes the partial hour early, and the hour is
 * written again when it ends, so readers should keep the last record for a
 * given t.
 *
 * ltlog_add runs on the sensor loop; the shell and (later) the web server
 * read the log, so everything is under `lock`.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/fs/fcb.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>

#include "light.h"
#include "ltlog.h"
#include "timesync.h"

LOG_MODULE_REGISTER(ltlog, LOG_LEVEL_INF);

#define AREA_ID      PARTITION_ID(history_partition)
#define MAX_SECTORS  32
#define FCB_MAGIC    0x4c544c47	/* "LTLG" */
#define HOUR         3600

static K_MUTEX_DEFINE(lock);
static struct fcb fcb;
static struct flash_sector sectors[MAX_SECTORS];
static bool ready;

/* The hour being accumulated. */
static struct {
	uint32_t hour;			/* UTC seconds at its start; 0 = none */
	double sum[7];
	uint32_t n[7];
} acc;

enum field { F_TEMP, F_RH, F_PRESS, F_PM25, F_ECO2, F_TVOC, F_LUX };

static int open_fcb(void)
{
	uint32_t cnt = ARRAY_SIZE(sectors);
	int ret = flash_area_get_sectors(AREA_ID, &cnt, sectors);

	if (ret != 0) {
		return ret;
	}

	memset(&fcb, 0, sizeof(fcb));
	fcb.f_magic = FCB_MAGIC;
	/* The FCB's own format version, fixed; records carry their own version. */
	fcb.f_version = 1;
	fcb.f_sector_cnt = (uint16_t)cnt;
	fcb.f_sectors = sectors;
	return fcb_init(AREA_ID, &fcb);
}

int ltlog_init(void)
{
	k_mutex_lock(&lock, K_FOREVER);

	int ret = open_fcb();

	if (ret != 0) {
		/* Never used, or not an FCB of ours (e.g. a format change): start over. */
		const struct flash_area *fa;

		LOG_WRN("history area unreadable (%d); erasing it", ret);
		ret = flash_area_open(AREA_ID, &fa);
		if (ret == 0) {
			ret = flash_area_erase(fa, 0, fa->fa_size);
			flash_area_close(fa);
		}
		if (ret == 0) {
			ret = open_fcb();
		}
	}

	ready = ret == 0;
	if (ready) {
		LOG_INF("history: %u sectors of %u bytes%s", fcb.f_sector_cnt,
			sectors[0].fs_size, fcb_is_empty(&fcb) ? ", empty" : "");
	} else {
		LOG_ERR("history unavailable (%d)", ret);
	}

	k_mutex_unlock(&lock);
	return ret;
}

static uint16_t clamp_u16(double v)
{
	return (uint16_t)CLAMP(lround(v), 0, UINT16_MAX);
}

/* Write the accumulated hour as a record. Under `lock`. */
static int write_hour(void)
{
	struct ltlog_record rec = {
		.t = acc.hour,
		.version = LTLOG_RECORD_VERSION,
	};
	double avg[7];

	for (int f = 0; f < 7; f++) {
		if (acc.n[f] > 0) {
			avg[f] = acc.sum[f] / acc.n[f];
			rec.valid |= BIT(f);	/* LTLOG_* bits follow enum field */
		}
	}
	if (rec.valid == 0) {
		return 0;
	}

	rec.temp_c100 = (int16_t)CLAMP(lround(avg[F_TEMP] * 100.0), INT16_MIN, INT16_MAX);
	rec.rh10 = clamp_u16(avg[F_RH] * 10.0);
	rec.press10 = clamp_u16(avg[F_PRESS] * 10.0);
	rec.pm25 = clamp_u16(avg[F_PM25]);
	rec.eco2 = clamp_u16(avg[F_ECO2]);
	rec.tvoc = clamp_u16(avg[F_TVOC]);
	rec.light = (uint16_t)(int16_t)CLAMP(lround(light_db(avg[F_LUX]) * 100.0), INT16_MIN,
					     INT16_MAX);

	struct fcb_entry loc;
	int ret = fcb_append(&fcb, sizeof(rec), &loc);

	if (ret == -ENOSPC) {
		/* Full: drop the oldest sector and try again. */
		ret = fcb_rotate(&fcb);
		if (ret == 0) {
			ret = fcb_append(&fcb, sizeof(rec), &loc);
		}
	}
	if (ret == 0) {
		ret = flash_area_write(fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc), &rec, sizeof(rec));
	}
	if (ret == 0) {
		ret = fcb_append_finish(&fcb, &loc);
	}
	if (ret != 0) {
		LOG_ERR("writing hour %u failed (%d)", rec.t, ret);
	}
	return ret;
}

static void add(enum field f, bool ok, double v)
{
	if (ok) {
		acc.sum[f] += v;
		acc.n[f]++;
	}
}

void ltlog_add(const struct readings *r)
{
	if (!timesync_valid()) {
		return;
	}

	uint32_t now = (uint32_t)timesync_now();
	uint32_t hour = now - now % HOUR;

	k_mutex_lock(&lock, K_FOREVER);

	if (acc.hour != hour) {
		if (acc.hour != 0 && ready) {
			write_hour();
		}
		memset(&acc, 0, sizeof(acc));
		acc.hour = hour;
	}

	add(F_TEMP, r->climate_ok, r->temp_c);
	add(F_RH, r->climate_ok, r->rh_pct);
	add(F_PRESS, r->climate_ok, r->press_sl_hpa);
	add(F_PM25, r->pm_ok, r->pm2_5);
	add(F_ECO2, r->gas_ok, r->eco2_ppm);
	add(F_TVOC, r->gas_ok, r->tvoc_ppb);
	add(F_LUX, r->light_ok, r->lux);

	k_mutex_unlock(&lock);
}

struct walk_ctx {
	bool (*cb)(const struct ltlog_record *rec, void *arg);
	void *arg;
	int count;
};

static int walk_entry(struct fcb_entry_ctx *ctx, void *arg)
{
	struct walk_ctx *w = arg;
	struct ltlog_record rec;

	if (ctx->loc.fe_data_len != sizeof(rec) ||
	    flash_area_read(ctx->fap, FCB_ENTRY_FA_DATA_OFF(ctx->loc), &rec, sizeof(rec)) != 0 ||
	    !ltlog_record_known(&rec)) {
		return 0;	/* skip anything unexpected */
	}
	w->count++;
	return w->cb(&rec, w->arg) ? 0 : 1;
}

int ltlog_walk(bool (*cb)(const struct ltlog_record *rec, void *arg), void *arg)
{
	struct walk_ctx w = { .cb = cb, .arg = arg };

	k_mutex_lock(&lock, K_FOREVER);

	int ret = ready ? fcb_walk(&fcb, NULL, walk_entry, &w) : -ENODEV;

	k_mutex_unlock(&lock);
	return ret < 0 ? ret : w.count;
}

int ltlog_next(struct ltlog_cursor *c, struct ltlog_record *rec)
{
	int ret = -ENOENT;

	k_mutex_lock(&lock, K_FOREVER);

	while (ready && fcb_getnext(&fcb, &c->loc) == 0) {
		if (c->loc.fe_data_len == sizeof(*rec) &&
		    flash_area_read(fcb.fap, FCB_ENTRY_FA_DATA_OFF(c->loc), rec,
				    sizeof(*rec)) == 0 &&
		    ltlog_record_known(rec)) {
			ret = 0;
			break;
		}
	}

	k_mutex_unlock(&lock);
	return ret;
}

/* Shell: "ltlog show [n]" lists the newest n records; "ltlog flush" writes now. */

#define SHOW_MAX 48

struct show_ctx {
	struct ltlog_record last[SHOW_MAX];
	int n;		/* how many to keep */
	int seen;
};

static bool keep_last(const struct ltlog_record *rec, void *arg)
{
	struct show_ctx *s = arg;

	s->last[s->seen % s->n] = *rec;
	s->seen++;
	return true;
}

/* Format a field, or "--" if it had no readings that hour. */
#define FIELD(buf, bit, fmt, val)                                                     \
	do {                                                                           \
		if (r->valid & (bit)) {                                                \
			snprintf(buf, sizeof(buf), fmt, val);                          \
		} else {                                                               \
			snprintf(buf, sizeof(buf), "--");                              \
		}                                                                      \
	} while (0)

static void print_record(const struct shell *sh, const struct ltlog_record *r)
{
	time_t t = r->t;
	struct tm tm;
	char when[24];
	char temp[12], rh[12], press[12], pm[12], eco2[12], tvoc[12], light[16];

	gmtime_r(&t, &tm);
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
	FIELD(temp, LTLOG_TEMP, "%.2f", r->temp_c100 / 100.0);
	FIELD(rh, LTLOG_RH, "%.1f", r->rh10 / 10.0);
	FIELD(press, LTLOG_PRESS, "%.1f", r->press10 / 10.0);
	FIELD(pm, LTLOG_PM25, "%u", r->pm25);
	FIELD(eco2, LTLOG_ECO2, "%u", r->eco2);
	FIELD(tvoc, LTLOG_TVOC, "%u", r->tvoc);
	FIELD(light, LTLOG_LUX, "%.1f", ltlog_light_db(r));
	shell_print(sh, "%s UTC  %6s C  %5s %%RH  %6s hPa  PM2.5 %3s  eCO2 %4s  TVOC %4s  "
		    "%s dB", when, temp, rh, press, pm, eco2, tvoc, light);
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	static struct show_ctx s;	/* too big for the shell stack */
	int n = argc > 1 ? atoi(argv[1]) : 10;

	s.n = CLAMP(n, 1, SHOW_MAX);
	s.seen = 0;

	int total = ltlog_walk(keep_last, &s);

	if (total < 0) {
		shell_error(sh, "history unavailable (%d)", total);
		return total;
	}

	int shown = MIN(s.seen, s.n);

	for (int i = s.seen - shown; i < s.seen; i++) {
		print_record(sh, &s.last[i % s.n]);
	}
	shell_print(sh, "%d record(s) stored; current hour has %u reading(s)", total,
		    acc.n[F_TEMP]);
	return 0;
}

static int cmd_flush(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lock, K_FOREVER);

	int ret = (ready && acc.hour != 0) ? write_hour() : -ENODATA;

	k_mutex_unlock(&lock);
	if (ret == 0) {
		shell_print(sh, "current partial hour written (it will be written again, "
			    "complete, when the hour ends)");
	} else {
		shell_error(sh, "nothing written (%d)", ret);
	}
	return ret;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lock, K_FOREVER);

	int ret = ready ? fcb_clear(&fcb) : -ENODEV;

	k_mutex_unlock(&lock);
	if (ret == 0) {
		shell_print(sh, "history cleared");
	} else {
		shell_error(sh, "clear failed (%d)", ret);
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ltlog_cmds,
	SHELL_CMD_ARG(show, NULL, "Show the newest records: show [n]", cmd_show, 1, 1),
	SHELL_CMD_ARG(flush, NULL, "Write the current partial hour now (testing)", cmd_flush,
		      1, 0),
	SHELL_CMD_ARG(clear, NULL, "Erase all stored history", cmd_clear, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ltlog, &ltlog_cmds, "Long-term hourly history", NULL);
