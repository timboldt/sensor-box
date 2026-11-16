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
 * Sensirion also says to discard a baseline more than MAX_AGE (7 days) old.
 * Each save records the UTC time (from SNTP), so the restore waits until the
 * clock is set (~15 s after boot), then restores the baseline only if it is
 * recent enough. If the clock isn't set within CLOCK_WAIT (no network), or
 * the baseline was saved without a known time, it is restored anyway. The
 * SGP30 accepts a baseline at any point after Init_air_quality.
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

#include "timesync.h"

#include "baseline.h"

LOG_MODULE_REGISTER(baseline, LOG_LEVEL_INF);

#define FIRST_SAVE_FRESH    K_HOURS(12)
#define FIRST_SAVE_RESTORED K_HOURS(1)
#define SAVE_PERIOD         K_HOURS(1)
#define MAX_AGE_S           (7 * 24 * 3600)
#define CLOCK_WAIT_MS       (120 * 1000)

#define SETTINGS_KEY "sgp30/baseline"

struct baseline {
	uint16_t eco2;
	uint16_t tvoc;
	uint32_t saved_at;	/* UTC seconds, 0 = unknown */
};

/* Saves from before saved_at existed: just the two words. */
#define LEGACY_SIZE 4

static const struct device *const sgp30 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(sgp30));

/* Only touched by the sensor-loop thread and the shell, both under `lock`. */
static K_MUTEX_DEFINE(lock);
static struct baseline saved;
static bool have_saved;
static bool restore_decided;
static int64_t next_save_ms;

static int settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(key, "baseline") != 0) {
		return -ENOENT;
	}
	if (len != sizeof(saved) && len != LEGACY_SIZE) {
		return -EINVAL;
	}
	memset(&saved, 0, sizeof(saved));
	if (read_cb(cb_arg, &saved, len) != (ssize_t)len) {
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

	cur.saved_at = timesync_valid() ? (uint32_t)timesync_now() : 0;
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

	/* Fresh-start rule until (and unless) a saved baseline is restored. */
	next_save_ms = k_uptime_get() + k_ticks_to_ms_floor64(FIRST_SAVE_FRESH.ticks);
	if (have_saved) {
		LOG_INF("saved baseline found; restoring once the clock is set");
	} else {
		restore_decided = true;
		LOG_INF("no saved baseline; first save after 12 h of operation");
	}

	k_mutex_unlock(&lock);
}

/* Restore or discard the saved baseline, once its age can be judged. Under `lock`. */
static void decide_restore(void)
{
	bool clock = timesync_valid();

	if (!have_saved) {
		restore_decided = true;	/* cleared while waiting */
		return;
	}
	if (!clock && k_uptime_get() < CLOCK_WAIT_MS) {
		return;		/* keep waiting for SNTP */
	}
	restore_decided = true;

	if (clock && saved.saved_at != 0) {
		int64_t age = (int64_t)timesync_now() - saved.saved_at;

		if (age > MAX_AGE_S) {
			LOG_INF("saved baseline is %lld days old (limit 7); discarding",
				age / 86400);
			settings_delete(SETTINGS_KEY);
			have_saved = false;
			return;
		}
	} else {
		LOG_INF("restoring without an age check (%s)",
			clock ? "saved before the clock was known" : "clock not set");
	}

	struct sensor_value v = { .val1 = saved.eco2, .val2 = saved.tvoc };
	int ret = sensor_attr_set(sgp30, SENSOR_CHAN_ALL,
				  (enum sensor_attribute)SENSOR_ATTR_SGP30_BASELINE, &v);

	if (ret == 0) {
		LOG_INF("restoring saved: eCO2 0x%04x TVOC 0x%04x", saved.eco2, saved.tvoc);
		next_save_ms = k_uptime_get() + k_ticks_to_ms_floor64(FIRST_SAVE_RESTORED.ticks);
	} else {
		LOG_WRN("restore failed: %d", ret);
	}
}

void baseline_tick(void)
{
	if (!sgp30_ready()) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);

	if (!restore_decided) {
		decide_restore();
	}
	if (restore_decided && k_uptime_get() >= next_save_ms) {
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
			restore_decided = true;
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
		shell_print(sh, "saved:   eCO2 0x%04x TVOC 0x%04x%s", saved.eco2, saved.tvoc,
			    restore_decided ? "" : " (restore pending)");
		if (saved.saved_at != 0 && timesync_valid()) {
			shell_print(sh, "         %lld h old",
				    ((int64_t)timesync_now() - saved.saved_at) / 3600);
		}
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
