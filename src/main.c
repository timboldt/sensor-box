/*
 * sensor-box firmware (Zephyr) — environmental monitor for the
 * Adafruit ESP32-C6 Feather.
 *
 * SPDX-License-Identifier: MIT
 *
 * Step 2: 1 Hz heartbeat + blink the on-board red LED (IO15) to prove
 * GPIO control. Sensors, display and SD logging come in later steps.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor_box, LOG_LEVEL_INF);

/* How many 1 Hz ticks between "report" lines (matches the old Rust cadence). */
#define REPORT_EVERY_TICKS 5U

/* The board devicetree aliases led0 -> &red_led (gpio0 15, active high). */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
	LOG_INF("sensor-box starting on %s", CONFIG_BOARD_TARGET);

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED GPIO device not ready");
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("failed to configure LED pin: %d", ret);
		return ret;
	}

	uint32_t tick = 0;

	while (1) {
		gpio_pin_toggle_dt(&led);

		if (tick % REPORT_EVERY_TICKS == 0) {
			LOG_INF("tick %u", tick);
		}

		tick++;
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
