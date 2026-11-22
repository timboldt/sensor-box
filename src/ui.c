/*
 * TFT dashboard (LVGL) for the 320x240 ILI9341 FeatherWing.
 *
 * SPDX-License-Identifier: MIT
 *
 *   +--------------------------------------------+
 *   | light          9:41 PM      wifi  battery |  status bar
 *   +--------------+--------------+--------------+
 *   |     Temp     |   Humidity   |   Pressure   |  title
 *   |     70.5     |     9.8      |    30.07     |  current value
 *   |   warming    |              |    rising    |  what it means, in words
 *   |  +0.8 in 1h  |  -0.2 in 1h  | +0.06 in 3h  |  change over the window
 *   +--------------+--------------+--------------+  that matters most
 *   |  Particles   |     CO2      |     TVOC     |  air-quality values,
 *   |     3500     |     400      |       0      |  CO2 and TVOC coloured
 *   |    rising    |     good     |     good     |  by band
 *   | +2.1k in 15m |  +60 in 15m  |   0 in 15m   |
 *   +--------------+--------------+--------------+
 *
 * Each tile shows one change, over the window where it means most: 3 h for
 * pressure (the forecasters' "tendency": beyond ~1.6 hPa, 0.05 inHg, is
 * rising or falling), 1 h for temperature and humidity, 15 min for the
 * pollutants (a window opened, someone cooking). It shows in grey while
 * within that reading's "steady" band (so sensor noise stays quiet),
 * otherwise in white, or for the pollutants green when falling and orange
 * when rising. "--" means there isn't that much history yet (it's in RAM;
 * after a reboot it comes back from the SD card). Charts are on the web page.
 *
 * The line of words above the change (see describe()): pressure's tendency
 * on the forecasters' scale (steady / rising / rising fast / rapid rise),
 * warming or cooling, damper or drier, particles rising or clearing
 * (blank while steady), and CO2's and TVOC's air-quality band.
 *
 * Units are left off to keep the tiles uncluttered: temperature is in F or C
 * (CONFIG_SENSOR_BOX_US_UNITS), humidity absolute (g/m3: it follows the air's
 * moisture, where %RH swings with temperature), pressure sea-level (MSL) inHg
 * or hPa, particles >0.3 um per litre (far livelier than PM2.5 in clean
 * indoor air), CO2 ppm, TVOC ppb.
 *
 * LVGL runs on its own workqueue (CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE), and
 * ui_update is called from the sensor loop, so every LVGL call is under
 * lvgl_lock().
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "history.h"
#include "timesync.h"
#include "ui.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#define STATUS_H 28
#define GAP      4
#define PAD      6
#define LINE_H   16	/* a line of Montserrat 14 */

#define US IS_ENABLED(CONFIG_SENSOR_BOX_US_UNITS)

/* Palette (dark theme). */
#define COL_BG     lv_color_hex(0x000000)
#define COL_TILE   lv_color_hex(0x1c1f24)
#define COL_TEXT   lv_color_hex(0xf2f2f2)
#define COL_MUTED  lv_color_hex(0x8a9099)
#define COL_GOOD   lv_color_hex(0x3ec46d)
#define COL_FAIR   lv_color_hex(0xf2c94c)
#define COL_POOR   lv_color_hex(0xf2994a)
#define COL_BAD    lv_color_hex(0xeb5757)

enum tile_id {
	T_TEMP,
	T_AH,
	T_PRESS,
	T_PC03,
	T_CO2,
	T_TVOC,
	T_COUNT,
};

static const struct {
	const char *title;
	int decimals;			/* current value */
	int delta_decimals;		/* change (at most) */
	enum history_series series;
	bool pollutant;			/* falling is good: colour changes green / orange */
	/*
	 * The change compares the average of the newest `avg` minutes with the
	 * average of `avg` minutes ending `minutes` ago, so one noisy reading
	 * doesn't decide it.
	 */
	const char *window;		/* shown after the change: "in 3h" */
	int minutes;
	int avg;
	/*
	 * A change is "steady" (grey) within max(steady, steady_rel x the larger
	 * of the two values), in metric units (degC, hPa, ...).
	 */
	float steady;
	float steady_rel;
} tile_def[T_COUNT] = {
	[T_TEMP] = { "Temp", 1, 1, H_TEMP, false, "in 1h", 60, 5, 0.5f },
	[T_AH] = { "Humidity", 1, 1, H_AH, false, "in 1h", 60, 5, 0.5f },
	[T_PRESS] = { "Pressure", US ? 2 : 0, US ? 2 : 1, H_PRESS, false, "in 3h", 180, 10, 1.6f },
	[T_PC03] = { "Particles", 0, 0, H_PC03, true, "in 15m", 15, 3, 300, 0.3f },
	[T_CO2] = { "CO2", 0, 0, H_CO2, true, "in 15m", 15, 3, 50 },
	[T_TVOC] = { "TVOC", 0, 0, H_TVOC, true, "in 15m", 15, 3, 30, 0.3f },
};

struct tile {
	lv_obj_t *value;
	lv_obj_t *word;			/* what the change or level means, in words */
	lv_obj_t *trend;		/* the change line: delta and its window */
	lv_obj_t *delta;
	lv_obj_t *note;			/* shown instead of trend, e.g. "warming up" */
};

static struct tile tiles[T_COUNT];
static lv_obj_t *light_label;
static lv_obj_t *clock_label;
static lv_obj_t *batt_label;

/* Metric (as stored in readings/history) -> what the tile shows. */
static float to_display(enum tile_id id, float v)
{
	if (US && id == T_TEMP) {
		return v * 9.0f / 5.0f + 32.0f;
	}
	if (US && id == T_PRESS) {
		return v / 33.8639f;	/* hPa -> inHg */
	}
	return v;
}

/* Why a reading has no value: shown instead of its changes, or NULL. */
static const char *status_note(enum tile_id id, const struct readings *r)
{
	switch (id) {
	case T_PC03:
		return r->pm_warming ? "warming up" : NULL;
	case T_CO2:
		return !r->co2_present ? "no sensor" : r->co2_warming ? "warming up" : NULL;
	case T_TVOC:
		return r->gas_warming ? "warming up" : NULL;
	default:
		return NULL;
	}
}

/* Current value of a tile, in display units. False if not available. */
static bool current_value(enum tile_id id, const struct readings *r, float *v)
{
	switch (id) {
	case T_TEMP:
		*v = to_display(id, (float)r->temp_c);
		return r->climate_ok;
	case T_AH:
		*v = (float)r->ah_g_m3;
		return r->climate_ok;
	case T_PRESS:
		*v = to_display(id, (float)r->press_sl_hpa);
		return r->climate_ok;
	case T_PC03:
		*v = (float)r->pc03_per_l;
		return r->pm_ok;
	case T_CO2:
		*v = (float)r->co2_ppm;
		return r->co2_ok;
	case T_TVOC:
		*v = (float)r->tvoc_ppb;
		return r->gas_ok;
	default:
		return false;
	}
}

/*
 * Air-quality bands -> colour; plain text colour for the others.
 * Upper bounds of good / fair / poor; anything above the last is bad.
 *   CO2:   common indoor ventilation guidance.
 *   TVOC:  German UBA indoor-air levels, in ppb.
 */
static int band(enum tile_id id, float v)
{
	static const float bands[T_COUNT][3] = {
		[T_CO2] = { 800, 1200, 2000 },
		[T_TVOC] = { 220, 660, 2200 },
	};

	if (id != T_CO2 && id != T_TVOC) {
		return -1;
	}
	return v <= bands[id][0] ? 0 : v <= bands[id][1] ? 1 : v <= bands[id][2] ? 2 : 3;
}

static const char *const band_word[] = { "good", "fair", "poor", "bad" };

static lv_color_t band_colour(int b)
{
	return b == 0 ? COL_GOOD : b == 1 ? COL_FAIR : b == 2 ? COL_POOR : COL_BAD;
}

static lv_color_t value_colour(enum tile_id id, float v)
{
	int b = band(id, v);

	return b < 0 ? COL_TEXT : band_colour(b);
}

static void format(enum tile_id id, float v, char *buf, size_t len)
{
	if (id == T_PC03 && v >= 10000.0f) {
		snprintf(buf, len, "%.0fk", (double)(v / 1000.0f));
		return;
	}
	snprintf(buf, len, "%.*f", tile_def[id].decimals, (double)v);
}

/* A change, signed: "+0.06", "-1.2", "+150", "+2.1k", "-12k"; "0" when it rounds to nothing. */
static void format_delta(float d, int decimals, char *buf, size_t len)
{
	float a = fabsf(d);
	const char *sign = d > 0.0f ? "+" : "-";

	if (a >= 9999.5f) {
		snprintf(buf, len, "%s%.0fk", sign, (double)(a / 1000.0f));
		return;
	}
	if (a >= 999.5f) {
		snprintf(buf, len, "%s%.1fk", sign, (double)(a / 1000.0f));
		return;
	}
	if (a >= 9.95f) {
		decimals = 0;
	} else if (a >= 0.995f) {
		decimals = MIN(decimals, 1);
	}
	if (roundf(a * powf(10.0f, (float)decimals)) == 0.0f) {
		snprintf(buf, len, "0");
		return;
	}
	snprintf(buf, len, "%s%.*f", sign, decimals, (double)a);
}

/* Average of the valid points in [from, to]; NAN if none. */
static float average(const float *pts, int from, int to)
{
	float sum = 0.0f;
	int n = 0;

	for (int i = from; i <= to; i++) {
		if (!isnan(pts[i])) {
			sum += pts[i];
			n++;
		}
	}
	return n > 0 ? sum / n : NAN;
}

/*
 * The words for a tile, and their colour: from the change d (metric units)
 * if have_d, outside the steady band; for CO2 and TVOC from the current
 * value v if have_v. "" for nothing to say.
 */
static const char *describe(enum tile_id id, bool have_d, float d, float steady, bool have_v,
			    float v, lv_color_t *col)
{
	bool up = d > 0.0f;
	bool moving = have_d && fabsf(d) >= steady;

	*col = COL_TEXT;
	switch (id) {
	case T_PRESS:
		/*
		 * Met Office / WMO tendency over 3 h: steady (here below 1.6 hPa,
		 * where the daily pressure tide alone moves it), rising/falling to
		 * 3.5, quickly to 6, very rapidly beyond.
		 */
		if (!have_d) {
			return "";
		}
		if (fabsf(d) < 1.6f) {
			*col = COL_MUTED;
			return "steady";
		}
		if (fabsf(d) < 3.6f) {
			return up ? "rising" : "falling";
		}
		*col = fabsf(d) <= 6.0f ? COL_FAIR : COL_POOR;
		return fabsf(d) <= 6.0f ? (up ? "rising fast" : "falling fast")
					: (up ? "rapid rise" : "rapid fall");
	case T_TEMP:
		return !moving ? "" : up ? "warming" : "cooling";
	case T_AH:
		return !moving ? "" : up ? "damper" : "drier";
	case T_PC03:
		*col = up ? COL_POOR : COL_GOOD;
		return !moving ? "" : up ? "rising" : "clearing";
	case T_CO2:
	case T_TVOC:
		if (!have_v) {
			return "";
		}
		*col = band_colour(band(id, v));
		return band_word[band(id, v)];
	default:
		return "";
	}
}

/*
 * Fill in a tile's change (from the 24 h history) and words. v is the
 * current value in display units, if have_v. Call under lvgl_lock().
 */
static void update_change(enum tile_id id, bool have_v, float v)
{
	static float pts[HISTORY_LEN];	/* only used under lvgl_lock */
	struct tile *t = &tiles[id];
	int n = history_get(HW_LONG, tile_def[id].series, pts, HISTORY_LEN, 1);
	int64_t covered_s = history_covered_s(HW_LONG);
	int k = tile_def[id].avg;
	int then_end = n - 1 - tile_def[id].minutes;
	int then_start = then_end - k + 1;
	float now = NAN;
	float then = NAN;
	char buf[24];

	/* Only compare against real history, not the boot-time backfill. */
	if (then_start >= 0 && covered_s >= (int64_t)(n - 1 - then_start) * HISTORY_PERIOD_S) {
		now = average(pts, n - k, n - 1);
		then = average(pts, then_start, then_end);
	}
	bool have_d = !isnan(now) && !isnan(then);
	float d = have_d ? now - then : 0.0f;	/* metric units */
	float steady = have_d ? MAX(tile_def[id].steady,
				    tile_def[id].steady_rel * MAX(fabsf(now), fabsf(then)))
			      : 0.0f;
	lv_color_t col;

	lv_label_set_text_static(t->word, describe(id, have_d, d, steady, have_v, v, &col));
	lv_obj_set_style_text_color(t->word, col, 0);

	if (!have_d) {
		lv_label_set_text_static(t->delta, "--");
		lv_obj_set_style_text_color(t->delta, COL_MUTED, 0);
		return;
	}

	col = fabsf(d) < steady ? COL_MUTED
	      : !tile_def[id].pollutant ? COL_TEXT
	      : d > 0.0f ? COL_POOR : COL_GOOD;
	format_delta(to_display(id, now) - to_display(id, then), tile_def[id].delta_decimals,
		     buf, sizeof(buf));
	lv_label_set_text(t->delta, buf);
	lv_obj_set_style_text_color(t->delta, col, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text_static(label, "");
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, colour, 0);
	return label;
}

static lv_obj_t *make_tile(lv_obj_t *parent, enum tile_id id, int32_t w, int32_t h)
{
	struct tile *t = &tiles[id];
	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, w, h);
	lv_obj_set_style_bg_color(box, COL_TILE, 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(box, 6, 0);
	lv_obj_set_style_pad_all(box, PAD, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = make_label(box, &lv_font_montserrat_14, COL_MUTED);

	lv_label_set_text_static(title, tile_def[id].title);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

	t->value = make_label(box, &lv_font_montserrat_28, COL_TEXT);
	lv_label_set_text_static(t->value, "--");
	lv_obj_align(t->value, LV_ALIGN_TOP_MID, 0, 16);

	/*
	 * The words, between the value (which ends 46 px down) and the change
	 * line. The widest, "falling fast", is 74 px.
	 */
	t->word = make_label(box, &lv_font_montserrat_14, COL_TEXT);
	lv_obj_align(t->word, LV_ALIGN_BOTTOM_MID, 0, -(LINE_H + 3));

	/*
	 * The change line, at the very bottom: "+0.06" (coloured) then "in 3h"
	 * (grey), centred. It spans the whole tile, padding included: the
	 * widest, "+444k in 15m", is 95 px.
	 */
	t->trend = lv_obj_create(box);
	lv_obj_remove_style_all(t->trend);
	lv_obj_set_size(t->trend, w, LINE_H);
	lv_obj_align(t->trend, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_remove_flag(t->trend, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(t->trend, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
	lv_obj_set_flex_flow(t->trend, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(t->trend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(t->trend, 5, 0);

	t->delta = make_label(t->trend, &lv_font_montserrat_14, COL_MUTED);
	lv_label_set_text_static(t->delta, "--");

	lv_obj_t *window = make_label(t->trend, &lv_font_montserrat_14, COL_MUTED);

	lv_label_set_text_static(window, tile_def[id].window);

	t->note = make_label(box, &lv_font_montserrat_14, COL_MUTED);
	lv_obj_align(t->note, LV_ALIGN_BOTTOM_MID, 0, -(LINE_H + 3) / 2);

	return box;
}

int ui_init(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return -ENODEV;
	}

	struct display_capabilities caps;

	display_get_capabilities(display, &caps);
	LOG_INF("display: %ux%u, orientation %d", caps.x_resolution,
		caps.y_resolution, caps.current_orientation);

	lvgl_lock();

	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, COL_BG, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	int32_t scr_w = lv_obj_get_width(scr);
	int32_t scr_h = lv_obj_get_height(scr);

	light_label = make_label(scr, &lv_font_montserrat_14, COL_MUTED);
	lv_obj_align(light_label, LV_ALIGN_TOP_LEFT, GAP, (STATUS_H - 14) / 2);
	batt_label = make_label(scr, &lv_font_montserrat_14, COL_MUTED);
	lv_obj_align(batt_label, LV_ALIGN_TOP_RIGHT, -GAP, (STATUS_H - 14) / 2);
	clock_label = make_label(scr, &lv_font_montserrat_14, COL_TEXT);
	lv_obj_align(clock_label, LV_ALIGN_TOP_MID, 0, (STATUS_H - 14) / 2);

	/* 3 x 2 grid of tiles under the status bar. */
	int32_t tile_w = (scr_w - 4 * GAP) / 3;
	int32_t tile_h = (scr_h - STATUS_H - 2 * GAP) / 2;

	for (int i = 0; i < T_COUNT; i++) {
		lv_obj_t *box = make_tile(scr, i, tile_w, tile_h);
		int col = i % 3;
		int row = i / 3;

		lv_obj_set_pos(box, GAP + col * (tile_w + GAP), STATUS_H + row * (tile_h + GAP));
	}

	lvgl_unlock();

	display_blanking_off(display);
	return 0;
}

static void update_tile(enum tile_id id, const struct readings *r)
{
	struct tile *t = &tiles[id];
	char buf[32];
	float v = 0.0f;
	bool have_v = current_value(id, r, &v);

	if (have_v) {
		format(id, v, buf, sizeof(buf));
		lv_label_set_text(t->value, buf);
		lv_obj_set_style_text_color(t->value, value_colour(id, v), 0);
	} else {
		lv_label_set_text_static(t->value, "--");
		lv_obj_set_style_text_color(t->value, COL_MUTED, 0);
	}

	/* During a sensor's warm-up (or without one), say so instead of the change. */
	const char *note = status_note(id, r);

	lv_label_set_text_static(t->note, note ? note : "");
	if (note) {
		lv_obj_add_flag(t->trend, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(t->word, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(t->trend, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(t->word, LV_OBJ_FLAG_HIDDEN);
		update_change(id, have_v, v);
	}
}

void ui_update(const struct readings *r)
{
	char buf[32];

	lvgl_lock();

	for (int i = 0; i < T_COUNT; i++) {
		update_tile(i, r);
	}

	/* Clock: blank until the first SNTP sync. 12-hour with US units. */
	struct tm now;

	if (timesync_local(&now)) {
		if (US) {
			int h12 = now.tm_hour % 12 == 0 ? 12 : now.tm_hour % 12;

			snprintf(buf, sizeof(buf), "%d:%02d %s", h12, now.tm_min,
				 now.tm_hour < 12 ? "AM" : "PM");
		} else {
			snprintf(buf, sizeof(buf), "%02d:%02d", now.tm_hour, now.tm_min);
		}
		lv_label_set_text(clock_label, buf);
	} else {
		lv_label_set_text_static(clock_label, "");
	}

	if (r->light_ok) {
		/* Whole lux in a lit room, two significant digits in the dark. */
		snprintf(buf, sizeof(buf), r->lux >= 10.0 ? "%.0f lux" : "%.2g lux", r->lux);
		lv_label_set_text(light_label, buf);
	} else {
		lv_label_set_text_static(light_label, "-- lux");
	}

	if (r->batt_ok) {
		/* The MAX17048 can report a little over 100% while charging. */
		unsigned int pct = MIN(r->batt_pct, 100U);
		const char *icon = pct >= 90 ? LV_SYMBOL_BATTERY_FULL
				 : pct >= 65 ? LV_SYMBOL_BATTERY_3
				 : pct >= 40 ? LV_SYMBOL_BATTERY_2
				 : pct >= 15 ? LV_SYMBOL_BATTERY_1
				 : LV_SYMBOL_BATTERY_EMPTY;

		/* No voltage here: it would run into the clock (it's in the log). */
		snprintf(buf, sizeof(buf), "%s%u%%  %s", r->wifi_ok ? LV_SYMBOL_WIFI "   " : "",
			 pct, icon);
		lv_label_set_text(batt_label, buf);
	} else {
		lv_label_set_text(batt_label, r->wifi_ok ? LV_SYMBOL_WIFI "   " LV_SYMBOL_BATTERY_EMPTY
						      " --"
						    : LV_SYMBOL_BATTERY_EMPTY " --");
	}

	lvgl_unlock();
}
