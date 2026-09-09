/*
 * sensor-box firmware (Zephyr) — environmental monitor for the
 * Adafruit ESP32-C6 Feather.
 *
 * SPDX-License-Identifier: MIT
 *
 * Step 4: read the in-tree I2C devices every REPORT_EVERY_TICKS:
 *   - BME280   temp / humidity / pressure   (sensor API)
 *   - TSL2591  ambient light / IR           (sensor API)
 *   - MAX17048 battery voltage / charge     (fuel-gauge API)
 * plus the 1 Hz heartbeat + LED blink. A missing device is logged and
 * skipped, never fatal.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/logging/log.h>

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

static void report_bme280(void)
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
	LOG_INF("bme280:  %.2f C  %.2f %%RH  %.2f hPa",
		sensor_value_to_double(&temp),
		sensor_value_to_double(&humidity),
		sensor_value_to_double(&press) * 10.0);
}

static void report_tsl2591(void)
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
}

static void report_max17048(void)
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
}

static void report_pmsa003i(void)
{
	if (pmsa003i == NULL || !device_is_ready(pmsa003i)) {
		LOG_WRN("pmsa003i: not ready, skipping");
		return;
	}

	if (sensor_sample_fetch(pmsa003i) != 0) {
		LOG_WRN("pmsa003i: sample fetch failed");
		return;
	}

	struct sensor_value pm1, pm25, pm10;

	sensor_channel_get(pmsa003i, SENSOR_CHAN_PM_1_0, &pm1);
	sensor_channel_get(pmsa003i, SENSOR_CHAN_PM_2_5, &pm25);
	sensor_channel_get(pmsa003i, SENSOR_CHAN_PM_10, &pm10);

	LOG_INF("pmsa003i: PM1.0 %d  PM2.5 %d  PM10 %d  ug/m3",
		pm1.val1, pm25.val1, pm10.val1);
}

int main(void)
{
	LOG_INF("sensor-box starting on %s", CONFIG_BOARD_TARGET);

	if (!gpio_is_ready_dt(&led) ||
	    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_ERR("LED GPIO not available");
	}

	uint32_t tick = 0;

	while (1) {
		gpio_pin_toggle_dt(&led);

		if (tick % REPORT_EVERY_TICKS == 0) {
			LOG_INF("--- tick %u ---", tick);
			report_bme280();
			report_tsl2591();
			report_pmsa003i();
			report_max17048();
		}

		tick++;
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
