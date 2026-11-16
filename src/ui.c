/*
 * TFT dashboard (LVGL) for the 320x240 ILI9341 FeatherWing.
 *
 * SPDX-License-Identifier: MIT
 *
 * Layout (landscape):
 *
 *   +--------------------------------------------+
 *   | light                              battery |  status bar
 *   +--------------+--------------+--------------+
 *   |     Temp     |   Humidity   |   Pressure   |  title
 *   |     70.5     |      64      |    30.07     |  current value
 *   |  ___/‾‾\__   |  ‾‾\___/‾    |  ____/‾‾‾    |  trend, last 6 h
 *   +--------------+--------------+--------------+
 *   |    PM2.5     |     eCO2     |     TVOC     |  air-quality values
 *   |       6      |     400      |       0      |  coloured by band
 *   |  _/\___/\_   |  __/‾‾\__    |  ___/\___    |
 *   +--------------+--------------+--------------+
 *
 * Units are left off to keep the tiles uncluttered: temperature is in F or C
 * (CONFIG_SENSOR_BOX_US_UNITS), humidity %RH, pressure sea-level (MSL) inHg
 * or hPa, PM2.5 ug/m3, eCO2 ppm, TVOC ppb.
 *
 * Each trend line auto-scales to its own data, with a minimum span so sensor
 * noise does not look like a big swing.
 *
 * LVGL runs on its own workqueue (CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE), so
 * every call in here is made under lvgl_lock().
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "history.h"
#include "ui.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#define STATUS_H 28
#define GAP      4
#define PAD      6
#define TREND_H  30

#define US IS_ENABLED(CONFIG_SENSOR_BOX_US_UNITS)

/* Palette (dark theme). */
#define COL_BG     lv_color_hex(0x000000)
#define COL_TILE   lv_color_hex(0x1c1f24)
#define COL_TEXT   lv_color_hex(0xf2f2f2)
#define COL_MUTED  lv_color_hex(0x8a9099)
#define COL_TREND  lv_color_hex(0x5aa9e6)
#define COL_GOOD   lv_color_hex(0x3ec46d)
#define COL_FAIR   lv_color_hex(0xf2c94c)
#define COL_POOR   lv_color_hex(0xf2994a)
#define COL_BAD    lv_color_hex(0xeb5757)

/* Trend values are stored as display units x TREND_SCALE (LVGL charts are int). */
#define TREND_SCALE 100

enum tile_id {
	T_TEMP,
	T_RH,
	T_PRESS,
	T_PM25,
	T_ECO2,
	T_TVOC,
	T_COUNT,
};

static const struct {
	const char *title;
	enum history_series series;
	float min_span;			/* smallest trend y-range, display units */
} tile_def[T_COUNT] = {
	[T_TEMP] = { "Temp", H_TEMP, US ? 2.0f : 1.0f },
	[T_RH] = { "Humidity", H_RH, 5.0f },
	[T_PRESS] = { "Pressure", H_PRESS, US ? 0.05f : 2.0f },
	[T_PM25] = { "PM2.5", H_PM25, 10.0f },
	[T_ECO2] = { "eCO2", H_ECO2, 200.0f },
	[T_TVOC] = { "TVOC", H_TVOC, 50.0f },
};

struct tile {
	lv_obj_t *value;
	lv_obj_t *trend;
	lv_chart_series_t *trend_ser;
	lv_obj_t *note;		/* shown over the trend area, e.g. "warming up" */
	int32_t trend_vals[HISTORY_LEN];
};

static struct tile tiles[T_COUNT];
static lv_obj_t *light_label;
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

/*
 * Air-quality bands -> colour. Upper bounds of good / fair / poor; anything
 * above the last is bad.
 *   PM2.5: US EPA AQI breakpoints (2024).
 *   eCO2:  common indoor ventilation guidance.
 *   TVOC:  German UBA indoor-air levels, in ppb.
 */
static lv_color_t band_colour(int32_t v, int32_t good, int32_t fair, int32_t poor)
{
	if (v <= good) {
		return COL_GOOD;
	}
	if (v <= fair) {
		return COL_FAIR;
	}
	if (v <= poor) {
		return COL_POOR;
	}
	return COL_BAD;
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

	/* Trend line: a bare line chart, no axes, grid or point markers. */
	t->trend = lv_chart_create(box);
	lv_obj_remove_style_all(t->trend);
	lv_obj_set_size(t->trend, LV_PCT(100), TREND_H);
	lv_obj_align(t->trend, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_chart_set_type(t->trend, LV_CHART_TYPE_LINE);
	lv_chart_set_div_line_count(t->trend, 0, 0);
	lv_chart_set_point_count(t->trend, HISTORY_LEN);
	lv_obj_set_style_line_width(t->trend, 2, LV_PART_ITEMS);
	lv_obj_set_style_line_rounded(t->trend, true, LV_PART_ITEMS);
	lv_obj_set_style_size(t->trend, 0, 0, LV_PART_INDICATOR);
	t->trend_ser = lv_chart_add_series(t->trend, COL_TREND, LV_CHART_AXIS_PRIMARY_Y);
	for (int i = 0; i < HISTORY_LEN; i++) {
		t->trend_vals[i] = LV_CHART_POINT_NONE;
	}
	lv_chart_set_series_ext_y_array(t->trend, t->trend_ser, t->trend_vals);

	t->note = make_label(box, &lv_font_montserrat_14, COL_MUTED);
	lv_obj_align(t->note, LV_ALIGN_BOTTOM_MID, 0, -(TREND_H - 14) / 2);

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

	/* 3 x 2 grid of tiles under the status bar. */
	int32_t tile_w = (scr_w - 4 * GAP) / 3;
	int32_t tile_h = (scr_h - STATUS_H - 2 * GAP) / 2;

	for (int i = 0; i < T_COUNT; i++) {
		lv_obj_t *box = make_tile(scr, i, tile_w, tile_h);
		int col = i % 3;
		int row = i / 3;

		lv_obj_set_pos(box, GAP + col * (tile_w + GAP),
			       STATUS_H + row * (tile_h + GAP));
	}

	lvgl_unlock();

	display_blanking_off(display);
	return 0;
}

static void set_value(enum tile_id id, const char *text, lv_color_t colour)
{
	lv_label_set_text(tiles[id].value, text);
	lv_obj_set_style_text_color(tiles[id].value, colour, 0);
}

static void set_missing(enum tile_id id)
{
	set_value(id, "--", COL_MUTED);
}

/* Redraw a tile's trend line from history, right-aligned (newest at right). */
static void update_trend(enum tile_id id)
{
	static float pts[HISTORY_LEN];
	struct tile *t = &tiles[id];
	int n = history_get(tile_def[id].series, pts, HISTORY_LEN);
	int offset = HISTORY_LEN - n;
	float lo = INFINITY;
	float hi = -INFINITY;

	for (int i = 0; i < HISTORY_LEN; i++) {
		t->trend_vals[i] = LV_CHART_POINT_NONE;
	}
	for (int i = 0; i < n; i++) {
		if (isnan(pts[i])) {
			continue;
		}
		float v = to_display(id, pts[i]);

		lo = MIN(lo, v);
		hi = MAX(hi, v);
		t->trend_vals[offset + i] = (int32_t)lroundf(v * TREND_SCALE);
	}

	if (lo <= hi) {
		/* Centre the data in at least min_span, so noise stays flat. */
		float span = MAX(hi - lo, tile_def[id].min_span);
		float mid = (hi + lo) / 2.0f;

		lv_chart_set_axis_range(t->trend, LV_CHART_AXIS_PRIMARY_Y,
					(int32_t)lroundf((mid - span / 2.0f) * TREND_SCALE),
					(int32_t)lroundf((mid + span / 2.0f) * TREND_SCALE));
	}
	lv_chart_refresh(t->trend);
}

void ui_update(const struct readings *r)
{
	char buf[32];

	lvgl_lock();

	if (r->climate_ok) {
		snprintf(buf, sizeof(buf), "%.1f", (double)to_display(T_TEMP, r->temp_c));
		set_value(T_TEMP, buf, COL_TEXT);
		snprintf(buf, sizeof(buf), "%.0f", r->rh_pct);
		set_value(T_RH, buf, COL_TEXT);
		snprintf(buf, sizeof(buf), US ? "%.2f" : "%.0f",
			 (double)to_display(T_PRESS, r->press_sl_hpa));
		set_value(T_PRESS, buf, COL_TEXT);
	} else {
		set_missing(T_TEMP);
		set_missing(T_RH);
		set_missing(T_PRESS);
	}

	if (r->pm_ok) {
		snprintf(buf, sizeof(buf), "%d", r->pm2_5);
		set_value(T_PM25, buf, band_colour(r->pm2_5, 9, 35, 55));
	} else {
		set_missing(T_PM25);
	}

	if (r->gas_ok) {
		snprintf(buf, sizeof(buf), "%d", r->eco2_ppm);
		set_value(T_ECO2, buf, band_colour(r->eco2_ppm, 800, 1200, 2000));
		snprintf(buf, sizeof(buf), "%d", r->tvoc_ppb);
		set_value(T_TVOC, buf, band_colour(r->tvoc_ppb, 220, 660, 2200));
	} else {
		set_missing(T_ECO2);
		set_missing(T_TVOC);
	}
	/* During SGP30 warm-up, say so where the trend line will be. */
	lv_label_set_text_static(tiles[T_ECO2].note, r->gas_warming ? "warming up" : "");
	lv_label_set_text_static(tiles[T_TVOC].note, r->gas_warming ? "warming up" : "");

	for (int i = 0; i < T_COUNT; i++) {
		update_trend(i);
	}

	if (r->light_ok) {
		snprintf(buf, sizeof(buf), "%.0f lux", r->lux);
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

		snprintf(buf, sizeof(buf), "%.2f V  %u%%  %s", r->batt_v, pct, icon);
		lv_label_set_text(batt_label, buf);
	} else {
		lv_label_set_text_static(batt_label, LV_SYMBOL_BATTERY_EMPTY " --");
	}

	lvgl_unlock();
}
