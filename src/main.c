/*
 * sensor-box firmware (Zephyr) — environmental monitor for the
 * Adafruit ESP32-C6 Feather.
 *
 * SPDX-License-Identifier: MIT
 *
 * Reads every sensor on the box each REPORT_EVERY_TICKS:
 *   - BME280   temp / humidity / pressure   (sensor API)
 *   - TSL2591  ambient light / IR           (sensor API)
 *   - PMSA003I PM1.0 / 2.5 / 10             (sensor API, out-of-tree)
 *   - SGP30    eCO2 / TVOC                  (sensor API, out-of-tree)
 *   - MAX17048 battery voltage / charge     (fuel-gauge API)
 * and shows them on the TFT (ui.c), switching its backlight off when the room
 * is dark (backlight.c), plus the 1 Hz heartbeat + LED blink. The Boot
 * button pages through full-screen charts, and wakes the screen in a dark
 * room. A missing device is logged and skipped, never fatal. The BME280
 * temp/RH is fed to the SGP30 as absolute humidity for its on-chip
 * compensation, and its IAQ baseline is saved to flash (baseline.c). Wi-Fi
 * connects to the network stored on the device (wifi.c).
 */

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include <sensor_box/sgp30.h>

#include "backlight.h"
#include "baseline.h"
#include "history.h"
#include "readings.h"
#include "ui.h"
#include "wifi.h"

LOG_MODULE_REGISTER(sensor_box, LOG_LEVEL_INF);

/* How many 1 Hz ticks between full readings / report lines. */
#define REPORT_EVERY_TICKS 5U

/* led0 -> &red_led (gpio0 15, active high) from the board devicetree. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* I2C devices from app.overlay; *_OR_NULL keeps the build fine if removed. */
static const struct device *const bme280 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(bme280));
static const struct device *const tsl2591 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(tsl2591));
static const struct device *const max17048 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(max17048));
static const struct device *const pmsa003i = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pmsa003i));
static const struct device *const sgp30 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(sgp30));

/*
 * Absolute humidity in g/m^3 from temperature (degC) and relative humidity (%),
 * via the Magnus approximation of saturation vapour pressure.
 */
static double abs_humidity_g_m3(double t_c, double rh_pct)
{
	double sat_hpa = 6.112 * exp((17.62 * t_c) / (243.12 + t_c));
	double vp_hpa = (rh_pct / 100.0) * sat_hpa;

	return 216.7 * vp_hpa / (273.15 + t_c);
}

/*
 * Station pressure -> sea-level pressure (hPa), for an altitude in metres and
 * the current temperature in degC. Hypsometric formula with the standard
 * lapse rate, as used for weather-report (QNH-style) pressure.
 */
static double sea_level_hpa(double station_hpa, double alt_m, double t_c)
{
	double lapse = 0.0065 * alt_m;

	return station_hpa * pow(1.0 - lapse / (t_c + lapse + 273.15), -5.257);
}

/* Push absolute humidity to the SGP30 for on-chip compensation. */
static void feed_sgp30_humidity(double temp_c, double rh_pct)
{
	if (sgp30 == NULL || !device_is_ready(sgp30)) {
		return;
	}

	struct sensor_value ah;

	sensor_value_from_double(&ah, abs_humidity_g_m3(temp_c, rh_pct));
	sensor_attr_set(sgp30, SENSOR_CHAN_ALL,
			(enum sensor_attribute)SENSOR_ATTR_SGP30_ABS_HUMIDITY, &ah);
}

static void report_bme280(struct readings *r)
{
	if (bme280 == NULL || !device_is_ready(bme280)) {
		LOG_WRN("bme280: not ready, skipping");
		return;
	}

	if (sensor_sample_fetch(bme280) != 0) {
		LOG_WRN("bme280: sample fetch failed");
		return;
	}

	struct sensor_value temp, press, humidity;

	sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(bme280, SENSOR_CHAN_PRESS, &press);
	sensor_channel_get(bme280, SENSOR_CHAN_HUMIDITY, &humidity);

	/* Zephyr reports temp in degC, pressure in kPa, humidity in %RH. */
	double temp_c = sensor_value_to_double(&temp);
	double rh_pct = sensor_value_to_double(&humidity);

	double press_hpa = sensor_value_to_double(&press) * 10.0;
	double press_sl_hpa = sea_level_hpa(press_hpa, CONFIG_SENSOR_BOX_ALTITUDE_M, temp_c);

	LOG_INF("bme280:  %.2f C  %.2f %%RH  %.2f hPa (%.2f hPa MSL)  (AH %.2f g/m3)",
		temp_c, rh_pct, press_hpa, press_sl_hpa,
		abs_humidity_g_m3(temp_c, rh_pct));

	r->climate_ok = true;
	r->temp_c = temp_c;
	r->rh_pct = rh_pct;
	r->press_hpa = press_hpa;
	r->press_sl_hpa = press_sl_hpa;

	feed_sgp30_humidity(temp_c, rh_pct);
}

static void report_tsl2591(struct readings *r)
{
	if (tsl2591 == NULL || !device_is_ready(tsl2591)) {
		LOG_WRN("tsl2591: not ready, skipping");
		return;
	}

	/* Fetch everything; SENSOR_CHAN_LIGHT is the lux approximation. */
	if (sensor_sample_fetch(tsl2591) != 0) {
		LOG_WRN("tsl2591: sample fetch failed");
		return;
	}

	struct sensor_value light, ir;

	sensor_channel_get(tsl2591, SENSOR_CHAN_LIGHT, &light);
	sensor_channel_get(tsl2591, SENSOR_CHAN_IR, &ir);

	LOG_INF("tsl2591: %.2f lux  (ir %.0f)",
		sensor_value_to_double(&light),
		sensor_value_to_double(&ir));

	r->light_ok = true;
	r->lux = sensor_value_to_double(&light);
}

static void report_max17048(struct readings *r)
{
	if (max17048 == NULL || !device_is_ready(max17048)) {
		LOG_WRN("max17048: not ready, skipping");
		return;
	}

	union fuel_gauge_prop_val v_uv, soc_pct;

	if (fuel_gauge_get_prop(max17048, FUEL_GAUGE_VOLTAGE, &v_uv) != 0 ||
	    fuel_gauge_get_prop(max17048, FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE,
				&soc_pct) != 0) {
		LOG_WRN("max17048: read failed");
		return;
	}

	/* voltage is microvolts; relative_state_of_charge is whole percent. */
	LOG_INF("battery: %.3f V  %u%%",
		(double)v_uv.voltage / 1e6, soc_pct.relative_state_of_charge);

	r->batt_ok = true;
	r->batt_v = (double)v_uv.voltage / 1e6;
	r->batt_pct = soc_pct.relative_state_of_charge;
}

static void report_pmsa003i(struct readings *r)
{
	if (pmsa003i == NULL || !device_is_ready(pmsa003i)) {
		LOG_WRN("pmsa003i: not ready, skipping");
		return;
	}

	int ret = sensor_sample_fetch(pmsa003i);

	if (ret == -EAGAIN) {
		LOG_INF("pmsa003i: warming up");
		r->pm_warming = true;
		return;
	}
	if (ret != 0) {
		LOG_WRN("pmsa003i: sample fetch failed: %d", ret);
		return;
	}

	struct sensor_value pm1, pm25, pm10;

	sensor_channel_get(pmsa003i, SENSOR_CHAN_PM_1_0, &pm1);
	sensor_channel_get(pmsa003i, SENSOR_CHAN_PM_2_5, &pm25);
	sensor_channel_get(pmsa003i, SENSOR_CHAN_PM_10, &pm10);

	LOG_INF("pmsa003i: PM1.0 %d  PM2.5 %d  PM10 %d  ug/m3",
		pm1.val1, pm25.val1, pm10.val1);

	r->pm_ok = true;
	r->pm1_0 = pm1.val1;
	r->pm2_5 = pm25.val1;
	r->pm10 = pm10.val1;
}

static void report_sgp30(struct readings *r)
{
	if (sgp30 == NULL || !device_is_ready(sgp30)) {
		LOG_WRN("sgp30: not ready, skipping");
		return;
	}

	int ret = sensor_sample_fetch(sgp30);

	if (ret == -EAGAIN) {
		LOG_INF("sgp30: warming up");
		r->gas_warming = true;
		return;
	}
	if (ret != 0) {
		LOG_WRN("sgp30: sample fetch failed: %d", ret);
		return;
	}

	struct sensor_value eco2, tvoc;

	sensor_channel_get(sgp30, SENSOR_CHAN_CO2, &eco2);
	sensor_channel_get(sgp30, SENSOR_CHAN_VOC, &tvoc);

	LOG_INF("sgp30:   eCO2 %d ppm  TVOC %d ppb", eco2.val1, tvoc.val1);

	r->gas_ok = true;
	r->eco2_ppm = eco2.val1;
	r->tvoc_ppb = tvoc.val1;
}

/*
 * Boot button (sw0, IO9). A press while the screen is dark only wakes it;
 * otherwise it moves to the next page. Runs on the input thread.
 */
static void on_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}
	LOG_INF("button: %s", evt->value ? "pressed" : "released");
	if (evt->value != 1) {
		return;
	}
	if (backlight_wake()) {
		return;
	}
	ui_next_page();
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(DT_ALIAS(sw0))), on_input, NULL);

int main(void)
{
	LOG_INF("sensor-box starting on %s", CONFIG_BOARD_TARGET);

	if (!gpio_is_ready_dt(&led) ||
	    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_ERR("LED GPIO not available");
	}

	/* A missing display is logged and skipped, like any other device. */
	bool have_ui = ui_init() == 0;

	backlight_init();
	baseline_init();
	wifi_init();

	uint32_t tick = 0;

	while (1) {
		gpio_pin_toggle_dt(&led);

		if (tick % REPORT_EVERY_TICKS == 0) {
			struct readings r = { 0 };

			LOG_INF("--- tick %u ---", tick);
			report_bme280(&r);
			report_tsl2591(&r);
			report_pmsa003i(&r);
			report_sgp30(&r);
			report_max17048(&r);
			r.wifi_ok = wifi_is_connected();

			backlight_set_ambient(r.light_ok, r.lux);
			history_add(&r);
			baseline_tick();

			if (have_ui) {
				ui_update(&r);
			}
		}

		tick++;
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
