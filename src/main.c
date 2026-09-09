/*
 * sensor-box firmware (Zephyr) — environmental monitor for the
 * Adafruit ESP32-C6 Feather.
 *
 * SPDX-License-Identifier: MIT
 *
 * Step 3: read the BME280 (temp / humidity / pressure) off the I2C bus
 * every REPORT_EVERY_TICKS, alongside the 1 Hz heartbeat + LED blink.
 * A missing or failing sensor is logged and skipped, not fatal.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor_box, LOG_LEVEL_INF);

/* How many 1 Hz ticks between full readings / report lines. */
#define REPORT_EVERY_TICKS 5U

/* led0 -> &red_led (gpio0 15, active high) from the board devicetree. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* The bme280 node from app.overlay; NULL-safe if the node is absent. */
static const struct device *const bme280 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(bme280));

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
	LOG_INF("bme280: %.2f C  %.2f %%RH  %.2f hPa",
		sensor_value_to_double(&temp),
		sensor_value_to_double(&humidity),
		sensor_value_to_double(&press) * 10.0);
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
			LOG_INF("tick %u", tick);
			report_bme280();
		}

		tick++;
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
