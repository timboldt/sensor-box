/*
 * TFT dashboard (LVGL) for the 320x240 ILI9341 FeatherWing.
 *
 * SPDX-License-Identifier: MIT
 *
 * Two kinds of page, cycled with the Boot button (ui_next_page):
 *
 * Dashboard (default):
 *   +--------------------------------------------+
 *   | light                       wifi  battery |  status bar (all pages)
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
 * Chart page, one per reading, plus one for light (lux, log scale):
 *   | Temperature                       70.5 °F  |
 *   |  72.0 +--------------------------------+   |
 *   |       |        ___/‾‾‾\___             |   |
 *   |  70.0 |-------/-----------\------------|   |
 *   |       |  ____/             \____       |   |
 *   |  68.0 +--------------------------------+   |
 *   |        -6h            -3h            now   |
 *
 * After RETURN_MS without a press it goes back to the dashboard.
 *
 * On the dashboard units are left off to keep the tiles uncluttered:
 * temperature is in F or C (CONFIG_SENSOR_BOX_US_UNITS), humidity %RH,
 * pressure sea-level (MSL) inHg or hPa, PM2.5 ug/m3, eCO2 ppm, TVOC ppb.
 * The chart pages show the unit.
 *
 * Each trend line auto-scales to its own data, with a minimum span so sensor
 * noise does not look like a big swing.
 *
 * LVGL runs on its own workqueue (CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE), and
 * this module is called from the sensor loop, the input thread and the
 * system workqueue, so every LVGL call and all page state is under
 * lvgl_lock().
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
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

/* Chart page geometry, relative to the page container. */
#define CHART_X  48
#define CHART_Y  40
#define CHART_H  150
#define AXIS_W   44

#define RETURN_MS 60000

#define US IS_ENABLED(CONFIG_SENSOR_BOX_US_UNITS)

/* Palette (dark theme). */
#define COL_BG     lv_color_hex(0x000000)
#define COL_TILE   lv_color_hex(0x1c1f24)
#define COL_GRID   lv_color_hex(0x30353d)
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
	T_TILES,		/* the readings above have dashboard tiles */
	T_LIGHT = T_TILES,	/* chart page only; lux is in the status bar */
	T_COUNT,
};

/* "\xc2\xb0" is the degree sign, which Montserrat includes. */
static const struct {
	const char *title;		/* dashboard tile */
	const char *long_title;		/* chart page */
	const char *unit;		/* chart page */
	int decimals;
	enum history_series series;
	float min_span;			/* smallest trend y-range, display units */
	bool log_scale;			/* chart log10(value); min_span in decades */
} tile_def[T_COUNT] = {
	[T_TEMP] = { "Temp", "Temperature", US ? "\xc2\xb0" "F" : "\xc2\xb0" "C",
		     1, H_TEMP, US ? 2.0f : 1.0f },
	[T_RH] = { "Humidity", "Humidity", "%RH", 0, H_RH, 5.0f },
	[T_PRESS] = { "Pressure", "Pressure (sea level)", US ? "inHg" : "hPa",
		      US ? 2 : 0, H_PRESS, US ? 0.05f : 2.0f },
	[T_PM25] = { "PM2.5", "PM2.5", "ug/m3", 0, H_PM25, 10.0f },
	[T_ECO2] = { "eCO2", "eCO2", "ppm", 0, H_ECO2, 200.0f },
	[T_TVOC] = { "TVOC", "TVOC", "ppb", 0, H_TVOC, 50.0f },
	[T_LIGHT] = { "Light", "Light (log scale)", "lux", 0, H_LUX, 1.0f, true },
};

/* Floor for log-scale charts: 0 lux (a dark room) plots as this. */
#define LOG_FLOOR 0.1f

struct tile {
	lv_obj_t *value;
	lv_obj_t *trend;
	lv_obj_t *note;		/* shown over the trend area, e.g. "warming up" */
	int32_t trend_vals[HISTORY_LEN];
};

static struct tile tiles[T_TILES];
static lv_obj_t *light_label;
static lv_obj_t *batt_label;

/* Pages. page 0 is the dashboard; page k (1..T_COUNT) charts tile k - 1. */
static lv_obj_t *dash_page;
static lv_obj_t *chart_page;
static int page;

static struct {
	lv_obj_t *title;
	lv_obj_t *value;
	lv_obj_t *chart;
	lv_obj_t *y_max;
	lv_obj_t *y_mid;
	lv_obj_t *y_min;
	lv_obj_t *note;
	int32_t vals[HISTORY_LEN];
} detail;

/* The latest readings, so a chart page can show the current value at once. */
static struct readings last;
static bool have_last;

static void return_to_dashboard(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(return_work, return_to_dashboard);

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

/* Whether a reading's sensor is still in its warm-up. */
static bool warming(enum tile_id id, const struct readings *r)
{
	switch (id) {
	case T_PM25:
		return r->pm_warming;
	case T_ECO2:
	case T_TVOC:
		return r->gas_warming;
	default:
		return false;
	}
}

/* Current value of a tile, in display units. False if not available. */
static bool current_value(enum tile_id id, const struct readings *r, float *v)
{
	switch (id) {
	case T_TEMP:
		*v = to_display(id, (float)r->temp_c);
		return r->climate_ok;
	case T_RH:
		*v = (float)r->rh_pct;
		return r->climate_ok;
	case T_PRESS:
		*v = to_display(id, (float)r->press_sl_hpa);
		return r->climate_ok;
	case T_PM25:
		*v = (float)r->pm2_5;
		return r->pm_ok;
	case T_ECO2:
		*v = (float)r->eco2_ppm;
		return r->gas_ok;
	case T_TVOC:
		*v = (float)r->tvoc_ppb;
		return r->gas_ok;
	case T_LIGHT:
		*v = (float)r->lux;
		return r->light_ok;
	default:
		return false;
	}
}

/*
 * Air-quality bands -> colour; plain text colour for the climate readings.
 * Upper bounds of good / fair / poor; anything above the last is bad.
 *   PM2.5: US EPA AQI breakpoints (2024).
 *   eCO2:  common indoor ventilation guidance.
 *   TVOC:  German UBA indoor-air levels, in ppb.
 */
static lv_color_t value_colour(enum tile_id id, float v)
{
	static const float bands[T_COUNT][3] = {
		[T_PM25] = { 9, 35, 55 },
		[T_ECO2] = { 800, 1200, 2000 },
		[T_TVOC] = { 220, 660, 2200 },
	};

	if (id != T_PM25 && id != T_ECO2 && id != T_TVOC) {
		return COL_TEXT;
	}
	if (v <= bands[id][0]) {
		return COL_GOOD;
	}
	if (v <= bands[id][1]) {
		return COL_FAIR;
	}
	if (v <= bands[id][2]) {
		return COL_POOR;
	}
	return COL_BAD;
}

static void format(enum tile_id id, float v, char *buf, size_t len)
{
	int decimals = tile_def[id].decimals;

	if (id == T_LIGHT && v < 10.0f) {
		decimals = 1;
	}
	snprintf(buf, len, "%.*f", decimals, (double)v);
}

/* Display value -> chart y (log10 for log-scale charts). */
static float to_chart(enum tile_id id, float v)
{
	return tile_def[id].log_scale ? log10f(MAX(v, LOG_FLOOR)) : v;
}

/* Chart y -> display value, for axis labels. */
static float from_chart(enum tile_id id, float y)
{
	return tile_def[id].log_scale ? powf(10.0f, y) : y;
}

/*
 * Load a tile's history into vals (HISTORY_LEN points, newest at the right)
 * and set the chart's y range to fit, with margin_frac headroom above and
 * below. Returns the y range in chart units (see to_chart) via lo/hi;
 * false if no data.
 */
static bool fill_trend(enum tile_id id, lv_obj_t *chart, int32_t *vals, float margin_frac,
		       float *lo_out, float *hi_out)
{
	static float pts[HISTORY_LEN];	/* only used under lvgl_lock */
	int n = history_get(tile_def[id].series, pts, HISTORY_LEN);
	int offset = HISTORY_LEN - n;
	float lo = INFINITY;
	float hi = -INFINITY;

	for (int i = 0; i < HISTORY_LEN; i++) {
		vals[i] = LV_CHART_POINT_NONE;
	}
	for (int i = 0; i < n; i++) {
		if (isnan(pts[i])) {
			continue;
		}
		float v = to_chart(id, to_display(id, pts[i]));

		lo = MIN(lo, v);
		hi = MAX(hi, v);
		vals[offset + i] = (int32_t)lroundf(v * TREND_SCALE);
	}

	bool have_data = lo <= hi;

	if (have_data) {
		/* Centre the data in at least min_span, so noise stays flat. */
		float span = MAX(hi - lo, tile_def[id].min_span);
		float mid = (hi + lo) / 2.0f;

		span *= 1.0f + 2.0f * margin_frac;
		*lo_out = mid - span / 2.0f;
		*hi_out = mid + span / 2.0f;
		lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y,
					(int32_t)lroundf(*lo_out * TREND_SCALE),
					(int32_t)lroundf(*hi_out * TREND_SCALE));
	}
	lv_chart_refresh(chart);
	return have_data;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text_static(label, "");
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, colour, 0);
	return label;
}

/* A fixed-width label with aligned text, placed at (x, y) in its parent. */
static lv_obj_t *make_axis_label(lv_obj_t *parent, int32_t x, int32_t y, int32_t w,
				 lv_text_align_t align, const char *text)
{
	lv_obj_t *label = make_label(parent, &lv_font_montserrat_14, COL_MUTED);

	lv_obj_set_width(label, w);
	lv_obj_set_style_text_align(label, align, 0);
	lv_obj_set_pos(label, x, y);
	if (text != NULL) {
		lv_label_set_text_static(label, text);
	}
	return label;
}

/* A bare line chart for HISTORY_LEN points over an external array. */
static lv_obj_t *make_chart(lv_obj_t *parent, int32_t *vals)
{
	lv_obj_t *chart = lv_chart_create(parent);

	lv_obj_remove_style_all(chart);
	lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
	lv_chart_set_div_line_count(chart, 0, 0);
	lv_chart_set_point_count(chart, HISTORY_LEN);
	lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
	lv_obj_set_style_line_rounded(chart, true, LV_PART_ITEMS);
	lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);

	lv_chart_series_t *ser = lv_chart_add_series(chart, COL_TREND, LV_CHART_AXIS_PRIMARY_Y);

	for (int i = 0; i < HISTORY_LEN; i++) {
		vals[i] = LV_CHART_POINT_NONE;
	}
	lv_chart_set_series_ext_y_array(chart, ser, vals);
	return chart;
}

static lv_obj_t *make_page(lv_obj_t *scr, int32_t w, int32_t h)
{
	lv_obj_t *p = lv_obj_create(scr);

	lv_obj_remove_style_all(p);
	lv_obj_set_size(p, w, h);
	lv_obj_set_pos(p, 0, STATUS_H);
	lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
	return p;
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

	t->trend = make_chart(box, t->trend_vals);
	lv_obj_set_size(t->trend, LV_PCT(100), TREND_H);
	lv_obj_align(t->trend, LV_ALIGN_BOTTOM_MID, 0, 0);

	t->note = make_label(box, &lv_font_montserrat_14, COL_MUTED);
	lv_obj_align(t->note, LV_ALIGN_BOTTOM_MID, 0, -(TREND_H - 14) / 2);

	return box;
}

static void make_chart_page(lv_obj_t *p, int32_t w)
{
	int32_t chart_w = w - CHART_X - 2 * GAP;

	detail.title = make_label(p, &lv_font_montserrat_14, COL_TEXT);
	lv_obj_align(detail.title, LV_ALIGN_TOP_LEFT, GAP + 2, 10);

	detail.value = make_label(p, &lv_font_montserrat_28, COL_TEXT);
	lv_obj_align(detail.value, LV_ALIGN_TOP_RIGHT, -GAP - 2, 0);

	detail.chart = make_chart(p, detail.vals);
	lv_obj_set_size(detail.chart, chart_w, CHART_H);
	lv_obj_set_pos(detail.chart, CHART_X, CHART_Y);
	lv_obj_set_style_bg_color(detail.chart, COL_TILE, 0);
	lv_obj_set_style_bg_opa(detail.chart, LV_OPA_COVER, 0);
	/* Grid lines at the top, middle and bottom, matching the y labels. */
	lv_chart_set_div_line_count(detail.chart, 3, 0);
	lv_obj_set_style_line_color(detail.chart, COL_GRID, LV_PART_MAIN);
	lv_obj_set_style_line_width(detail.chart, 1, LV_PART_MAIN);

	/* y labels, right-aligned against the chart, centred on the grid lines. */
	int32_t half = 8;

	detail.y_max = make_axis_label(p, 0, CHART_Y - half, AXIS_W, LV_TEXT_ALIGN_RIGHT, NULL);
	detail.y_mid = make_axis_label(p, 0, CHART_Y + CHART_H / 2 - half, AXIS_W,
				       LV_TEXT_ALIGN_RIGHT, NULL);
	detail.y_min = make_axis_label(p, 0, CHART_Y + CHART_H - half, AXIS_W,
				       LV_TEXT_ALIGN_RIGHT, NULL);

	/* Time axis. HISTORY_LEN x HISTORY_PERIOD_S = 6 h. */
	int32_t time_y = CHART_Y + CHART_H + 4;

	make_axis_label(p, CHART_X, time_y, 40, LV_TEXT_ALIGN_LEFT, "-6h");
	make_axis_label(p, CHART_X + chart_w / 2 - 20, time_y, 40, LV_TEXT_ALIGN_CENTER, "-3h");
	make_axis_label(p, CHART_X + chart_w - 40, time_y, 40, LV_TEXT_ALIGN_RIGHT, "now");

	detail.note = make_label(p, &lv_font_montserrat_14, COL_MUTED);
	lv_obj_align_to(detail.note, detail.chart, LV_ALIGN_CENTER, 0, 0);
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

	/* Dashboard: 3 x 2 grid of tiles under the status bar. */
	dash_page = make_page(scr, scr_w, scr_h - STATUS_H);

	int32_t tile_w = (scr_w - 4 * GAP) / 3;
	int32_t tile_h = (scr_h - STATUS_H - 2 * GAP) / 2;

	for (int i = 0; i < T_TILES; i++) {
		lv_obj_t *box = make_tile(dash_page, i, tile_w, tile_h);
		int col = i % 3;
		int row = i / 3;

		lv_obj_set_pos(box, GAP + col * (tile_w + GAP), row * (tile_h + GAP));
	}

	chart_page = make_page(scr, scr_w, scr_h - STATUS_H);
	make_chart_page(chart_page, scr_w);
	lv_obj_add_flag(chart_page, LV_OBJ_FLAG_HIDDEN);

	lvgl_unlock();

	display_blanking_off(display);
	return 0;
}

/* Redraw the chart page for the current page. Call under lvgl_lock(). */
static void render_chart_page(void)
{
	enum tile_id id = page - 1;
	char buf[32];
	float v;
	float lo;
	float hi;

	lv_label_set_text_static(detail.title, tile_def[id].long_title);

	if (have_last && current_value(id, &last, &v)) {
		format(id, v, buf, sizeof(buf));
		lv_label_set_text_fmt(detail.value, "%s %s", buf, tile_def[id].unit);
		lv_obj_set_style_text_color(detail.value, value_colour(id, v), 0);
	} else {
		lv_label_set_text_fmt(detail.value, "-- %s", tile_def[id].unit);
		lv_obj_set_style_text_color(detail.value, COL_MUTED, 0);
	}

	if (fill_trend(id, detail.chart, detail.vals, 0.05f, &lo, &hi)) {
		format(id, from_chart(id, hi), buf, sizeof(buf));
		lv_label_set_text(detail.y_max, buf);
		format(id, from_chart(id, (lo + hi) / 2.0f), buf, sizeof(buf));
		lv_label_set_text(detail.y_mid, buf);
		format(id, from_chart(id, lo), buf, sizeof(buf));
		lv_label_set_text(detail.y_min, buf);
		lv_label_set_text_static(detail.note, "");
	} else {
		lv_label_set_text_static(detail.y_max, "");
		lv_label_set_text_static(detail.y_mid, "");
		lv_label_set_text_static(detail.y_min, "");
		lv_label_set_text_static(detail.note,
					 have_last && warming(id, &last) ? "warming up" : "no data yet");
	}
	lv_obj_align_to(detail.note, detail.chart, LV_ALIGN_CENTER, 0, 0);
}

/* Switch page. Call under lvgl_lock(). */
static void show_page(int p)
{
	page = p;
	if (page == 0) {
		lv_obj_add_flag(chart_page, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(dash_page, LV_OBJ_FLAG_HIDDEN);
	} else {
		render_chart_page();
		lv_obj_add_flag(dash_page, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(chart_page, LV_OBJ_FLAG_HIDDEN);
	}
}

static void return_to_dashboard(struct k_work *work)
{
	ARG_UNUSED(work);

	lvgl_lock();
	show_page(0);
	lvgl_unlock();
}

void ui_next_page(void)
{
	if (dash_page == NULL) {
		return;		/* ui_init failed */
	}

	lvgl_lock();
	show_page((page + 1) % (T_COUNT + 1));
	bool on_chart = page != 0;
	lvgl_unlock();

	if (on_chart) {
		k_work_reschedule(&return_work, K_MSEC(RETURN_MS));
	} else {
		k_work_cancel_delayable(&return_work);
	}
}

static void update_tile(enum tile_id id, const struct readings *r)
{
	struct tile *t = &tiles[id];
	char buf[32];
	float v;
	float lo;
	float hi;

	if (current_value(id, r, &v)) {
		format(id, v, buf, sizeof(buf));
		lv_label_set_text(t->value, buf);
		lv_obj_set_style_text_color(t->value, value_colour(id, v), 0);
	} else {
		lv_label_set_text_static(t->value, "--");
		lv_obj_set_style_text_color(t->value, COL_MUTED, 0);
	}

	fill_trend(id, t->trend, t->trend_vals, 0.0f, &lo, &hi);
}

void ui_update(const struct readings *r)
{
	char buf[32];

	lvgl_lock();

	last = *r;
	have_last = true;

	for (int i = 0; i < T_TILES; i++) {
		update_tile(i, r);
		/* During a sensor's warm-up, say so where the trend line will be. */
		lv_label_set_text_static(tiles[i].note, warming(i, r) ? "warming up" : "");
	}

	if (page != 0) {
		render_chart_page();
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

		snprintf(buf, sizeof(buf), "%s%.2f V  %u%%  %s",
			 r->wifi_ok ? LV_SYMBOL_WIFI "   " : "", r->batt_v, pct, icon);
		lv_label_set_text(batt_label, buf);
	} else {
		lv_label_set_text(batt_label, r->wifi_ok ? LV_SYMBOL_WIFI "   " LV_SYMBOL_BATTERY_EMPTY
						      " --"
						    : LV_SYMBOL_BATTERY_EMPTY " --");
	}

	lvgl_unlock();
}
