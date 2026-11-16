/*
 * Persist the SGP30's IAQ baseline across reboots (Zephyr settings / NVS).
 *
 * SPDX-License-Identifier: MIT
 *
 * Following Sensirion's guidance for the SGP30:
 *  - With no saved baseline, the sensor needs ~12 h of operation to learn
 *    one, so the first save waits FIRST_SAVE_FRESH.
 *  - After restoring a saved baseline, saves start after FIRST_SAVE_RESTORED.
 *  - From then on the baseline is saved every SAVE_PERIOD (only if changed,
 *    to spare the flash).
 *
 * Sensirion also says to discard a baseline more than 7 days old. Without an
 * RTC or network time the box cannot tell how long it was off, so the saved
 * baseline is always restored. (With SNTP, store a timestamp alongside.)
 *
 * The baseline lives in the "storage" flash partition, which flashing the
 * firmware does not touch.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include <sensor_box/sgp30.h>

#include "baseline.h"

LOG_MODULE_REGISTER(baseline, LOG_LEVEL_INF);

#define FIRST_SAVE_FRESH    K_HOURS(12)
#define FIRST_SAVE_RESTORED K_HOURS(1)
#define SAVE_PERIOD         K_HOURS(1)

#define SETTINGS_KEY "sgp30/baseline"

struct baseline {
	uint16_t eco2;
	uint16_t tvoc;
};

static const struct device *const sgp30 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(sgp30));

/* Only touched by the sensor-loop thread and the shell, both under `lock`. */
static K_MUTEX_DEFINE(lock);
static struct baseline saved;
static bool have_saved;
static int64_t next_save_ms;

static int settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(key, "baseline") != 0) {
		return -ENOENT;
	}
	if (len != sizeof(saved)) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, &saved, sizeof(saved)) != sizeof(saved)) {
		return -EIO;
	}
	have_saved = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sgp30, "sgp30", NULL, settings_set, NULL, NULL);

static bool sgp30_ready(void)
{
	return sgp30 != NULL && device_is_ready(sgp30);
}

static int read_current(struct baseline *b)
{
	struct sensor_value v;
	int ret = sensor_attr_get(sgp30, SENSOR_CHAN_ALL,
				  (enum sensor_attribute)SENSOR_ATTR_SGP30_BASELINE, &v);

	if (ret == 0) {
		b->eco2 = (uint16_t)v.val1;
		b->tvoc = (uint16_t)v.val2;
	}
	return ret;
}

/* Save the current baseline if it differs from the saved one. Under `lock`. */
static int save_now(void)
{
	struct baseline cur;
	int ret = read_current(&cur);

	if (ret != 0) {
		return ret;
	}
	if (have_saved && cur.eco2 == saved.eco2 && cur.tvoc == saved.tvoc) {
		return 0;
	}

	ret = settings_save_one(SETTINGS_KEY, &cur, sizeof(cur));
	if (ret == 0) {
		saved = cur;
		have_saved = true;
		LOG_INF("saved: eCO2 0x%04x TVOC 0x%04x", cur.eco2, cur.tvoc);
	}
	return ret;
}

void baseline_init(void)
{
	if (!sgp30_ready()) {
		return;
	}

	int ret = settings_subsys_init();

	if (ret == 0) {
		ret = settings_load_subtree("sgp30");
	}
	if (ret != 0) {
		LOG_ERR("settings unavailable (%d); baseline will not persist", ret);
	}

	k_mutex_lock(&lock, K_FOREVER);

	k_timeout_t first_save = FIRST_SAVE_FRESH;

	if (have_saved) {
		struct sensor_value v = { .val1 = saved.eco2, .val2 = saved.tvoc };

		ret = sensor_attr_set(sgp30, SENSOR_CHAN_ALL,
				      (enum sensor_attribute)SENSOR_ATTR_SGP30_BASELINE, &v);
		if (ret == 0) {
			LOG_INF("restoring saved: eCO2 0x%04x TVOC 0x%04x", saved.eco2,
				saved.tvoc);
			first_save = FIRST_SAVE_RESTORED;
		} else {
			LOG_WRN("restore failed: %d", ret);
		}
	} else {
		LOG_INF("no saved baseline; first save after 12 h of operation");
	}
	next_save_ms = k_uptime_get() + k_ticks_to_ms_floor64(first_save.ticks);

	k_mutex_unlock(&lock);
}

void baseline_tick(void)
{
	if (!sgp30_ready()) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);

	if (k_uptime_get() >= next_save_ms) {
		int ret = save_now();

		if (ret != 0) {
			LOG_WRN("save failed: %d", ret);
		}
		next_save_ms += k_ticks_to_ms_floor64(SAVE_PERIOD.ticks);
	}

	k_mutex_unlock(&lock);
}

/* Shell: "sgp30 baseline [save|clear]". */
static int cmd_baseline(const struct shell *sh, size_t argc, char **argv)
{
	if (!sgp30_ready()) {
		shell_error(sh, "SGP30 not ready");
		return -ENODEV;
	}

	int ret = 0;

	k_mutex_lock(&lock, K_FOREVER);

	if (argc > 1 && strcmp(argv[1], "save") == 0) {
		ret = save_now();
		if (ret != 0) {
			shell_error(sh, "save failed: %d", ret);
		}
	} else if (argc > 1 && strcmp(argv[1], "clear") == 0) {
		ret = settings_delete(SETTINGS_KEY);
		if (ret == 0) {
			/* Back to the fresh-start rule: don't save an unlearned one. */
			have_saved = false;
			next_save_ms = k_uptime_get() +
				       k_ticks_to_ms_floor64(FIRST_SAVE_FRESH.ticks);
			shell_print(sh, "saved baseline cleared; reset the board to "
				    "restart the sensor's learning");
		} else {
			shell_error(sh, "clear failed: %d", ret);
		}
	} else if (argc > 1) {
		shell_error(sh, "usage: sgp30 baseline [save|clear]");
		ret = -EINVAL;
	}

	struct baseline cur;

	if (read_current(&cur) == 0) {
		shell_print(sh, "current: eCO2 0x%04x TVOC 0x%04x", cur.eco2, cur.tvoc);
	} else {
		shell_print(sh, "current: not read yet (first read ~1 min after boot)");
	}
	if (have_saved) {
		shell_print(sh, "saved:   eCO2 0x%04x TVOC 0x%04x", saved.eco2, saved.tvoc);
	} else {
		shell_print(sh, "saved:   none");
	}
	shell_print(sh, "next save in %lld min", (next_save_ms - k_uptime_get()) / 60000);

	k_mutex_unlock(&lock);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sgp30_cmds,
	SHELL_CMD_ARG(baseline, NULL, "Show, save or clear the IAQ baseline", cmd_baseline, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sgp30, &sgp30_cmds, "SGP30 commands", NULL);
