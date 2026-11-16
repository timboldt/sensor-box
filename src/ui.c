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
 *   | Temp         | Humidity     | Pressure     |
 *   |     70.5     |      64      |    30.07     |
 *   |      F       |      %       |   inHg MSL   |
 *   +--------------+--------------+--------------+
 *   | PM2.5        | eCO2         | TVOC         |  values coloured by
 *   |       6      |     400      |       0      |  air-quality band
 *   |    ug/m3     |     ppm      |      ppb     |
 *   +--------------+--------------+--------------+
 *
 * LVGL runs on its own workqueue (CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE), so
 * every call in here is made under lvgl_lock().
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "ui.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#define STATUS_H 28
#define GAP      4

/* Palette (dark theme). */
#define COL_BG     lv_color_hex(0x000000)
#define COL_TILE   lv_color_hex(0x1c1f24)
#define COL_TEXT   lv_color_hex(0xf2f2f2)
#define COL_MUTED  lv_color_hex(0x8a9099)
#define COL_GOOD   lv_color_hex(0x3ec46d)
#define COL_FAIR   lv_color_hex(0xf2c94c)
#define COL_POOR   lv_color_hex(0xf2994a)
#define COL_BAD    lv_color_hex(0xeb5757)

struct tile {
	lv_obj_t *value;
	lv_obj_t *unit;
};

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
	const char *unit;
} tile_def[T_COUNT] = {
	/* "\xc2\xb0" is the degree sign, which Montserrat includes. */
	[T_TEMP] = { "Temp", IS_ENABLED(CONFIG_SENSOR_BOX_US_UNITS) ? "\xc2\xb0" "F"
								    : "\xc2\xb0" "C" },
	[T_RH] = { "Humidity", "%RH" },
	[T_PRESS] = { "Pressure", IS_ENABLED(CONFIG_SENSOR_BOX_US_UNITS) ? "inHg MSL"
									 : "hPa MSL" },
	[T_PM25] = { "PM2.5", "ug/m3" },
	[T_ECO2] = { "eCO2", "ppm" },
	[T_TVOC] = { "TVOC", "ppb" },
};

static struct tile tiles[T_COUNT];
static lv_obj_t *light_label;
static lv_obj_t *batt_label;

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

static lv_obj_t *make_tile(lv_obj_t *parent, enum tile_id id, int32_t w, int32_t h)
{
	lv_obj_t *box = lv_obj_create(parent);

	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, w, h);
	lv_obj_set_style_bg_color(box, COL_TILE, 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(box, 6, 0);
	lv_obj_set_style_pad_all(box, 6, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(box);

	lv_label_set_text_static(title, tile_def[id].title);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(title, COL_MUTED, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

	tiles[id].value = lv_label_create(box);
	lv_label_set_text_static(tiles[id].value, "--");
	lv_obj_set_style_text_font(tiles[id].value, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(tiles[id].value, COL_TEXT, 0);
	lv_obj_align(tiles[id].value, LV_ALIGN_CENTER, 0, 2);

	tiles[id].unit = lv_label_create(box);
	lv_label_set_text_static(tiles[id].unit, tile_def[id].unit);
	lv_obj_set_style_text_font(tiles[id].unit, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(tiles[id].unit, COL_MUTED, 0);
	lv_obj_align(tiles[id].unit, LV_ALIGN_BOTTOM_MID, 0, 0);

	return box;
}

static lv_obj_t *make_status_label(lv_obj_t *parent, lv_align_t align, int32_t x_ofs)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text_static(label, "");
	lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(label, COL_MUTED, 0);
	lv_obj_align(label, align, x_ofs, (STATUS_H - 14) / 2);
	return label;
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

	light_label = make_status_label(scr, LV_ALIGN_TOP_LEFT, GAP);
	batt_label = make_status_label(scr, LV_ALIGN_TOP_RIGHT, -GAP);

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

void ui_update(const struct readings *r)
{
	char buf[32];

	lvgl_lock();

	if (r->climate_ok) {
		if (IS_ENABLED(CONFIG_SENSOR_BOX_US_UNITS)) {
			snprintf(buf, sizeof(buf), "%.1f", r->temp_c * 9.0 / 5.0 + 32.0);
		} else {
			snprintf(buf, sizeof(buf), "%.1f", r->temp_c);
		}
		set_value(T_TEMP, buf, COL_TEXT);
		snprintf(buf, sizeof(buf), "%.0f", r->rh_pct);
		set_value(T_RH, buf, COL_TEXT);
		if (IS_ENABLED(CONFIG_SENSOR_BOX_US_UNITS)) {
			/* 1 inHg = 33.8639 hPa */
			snprintf(buf, sizeof(buf), "%.2f", r->press_sl_hpa / 33.8639);
		} else {
			snprintf(buf, sizeof(buf), "%.0f", r->press_sl_hpa);
		}
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
	/* During SGP30 warm-up, say so in place of the unit. */
	lv_label_set_text_static(tiles[T_ECO2].unit,
				 r->gas_warming ? "warming up" : tile_def[T_ECO2].unit);
	lv_label_set_text_static(tiles[T_TVOC].unit,
				 r->gas_warming ? "warming up" : tile_def[T_TVOC].unit);

	if (r->light_ok) {
		snprintf(buf, sizeof(buf), "%.0f lux", r->lux);
		lv_label_set_text(light_label, buf);
	} else {
		lv_label_set_text_static(light_label, "-- lux");
	}

	if (r->batt_ok) {
		const char *icon = r->batt_pct >= 90 ? LV_SYMBOL_BATTERY_FULL
				 : r->batt_pct >= 65 ? LV_SYMBOL_BATTERY_3
				 : r->batt_pct >= 40 ? LV_SYMBOL_BATTERY_2
				 : r->batt_pct >= 15 ? LV_SYMBOL_BATTERY_1
				 : LV_SYMBOL_BATTERY_EMPTY;

		snprintf(buf, sizeof(buf), "%.2f V  %u%%  %s", r->batt_v, r->batt_pct, icon);
		lv_label_set_text(batt_label, buf);
	} else {
		lv_label_set_text_static(batt_label, LV_SYMBOL_BATTERY_EMPTY " --");
	}

	lvgl_unlock();
}
