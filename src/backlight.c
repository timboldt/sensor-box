/*
 * TFT backlight, driven from ambient light (TSL2591).
 *
 * SPDX-License-Identifier: MIT
 *
 * Brightness follows the room light:
 *
 *   lux < DARK_OFF_LUX           -> off
 *   DARK_ON_LUX .. FULL_LUX      -> MIN_PM rising to full, evenly per
 *                                   decade of lux (the eye's scale)
 *   lux >= FULL_LUX              -> full brightness
 *
 * Off and back on have different thresholds (hysteresis), so light hovering
 * around the cutoff does not make it flicker, and while on, the target only
 * moves when it changes by more than 1/RETARGET_FRACTION, so sensor noise
 * doesn't keep it fading. Every change fades over FADE_MS rather than
 * snapping.
 *
 * backlight_wake() (the Boot button) turns it on, at MIN_PM, for WAKE_MS even
 * in a dark room, so the readings can be checked at night without the room
 * lights.
 *
 * Callers are the sensor loop, the input thread and the shell; the fade and
 * wake-expiry run on the system workqueue. All state is under `lock`.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "backlight.h"

LOG_MODULE_REGISTER(backlight, LOG_LEVEL_INF);

#define DARK_OFF_LUX 0.01
#define DARK_ON_LUX  0.02
#define FULL_LUX     15.0
#define MIN_PM       10		/* brightness at DARK_ON_LUX (1%) */
#define RETARGET_FRACTION 8	/* ignore changes under 1/8 (12.5%) */

#define FADE_MS      1500
#define FADE_STEP_MS 20
#define WAKE_MS      30000
/*
 * Missed readings in a row before giving up on the sensor and going to full
 * brightness. A single miss is routine: the reading after each gain change is
 * dropped. Readings come every 5 s, so 6 is about 30 s.
 */
#define MISSES_BEFORE_FAIL 6

static const struct pwm_dt_spec pwm = PWM_DT_SPEC_GET(DT_NODELABEL(tft_backlight));

static K_MUTEX_DEFINE(lock);

/* Brightness in permille, for smooth fades. */
static int current_pm;
static int target_pm;
static int step_pm;

/* Inputs to the on/off decision. */
static bool dark;
static bool last_lux_ok;	/* false: no reading for MISSES_BEFORE_FAIL */
static int misses;		/* missed readings in a row */
static double last_lux;
static int manual_pm = -1;	/* from the shell: fixed permille, or -1 for auto */
static bool woken;		/* button pressed in the last WAKE_MS */

static void fade_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(fade_work, fade_step);

static void wake_expired(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(wake_work, wake_expired);

static void apply(int pm)
{
	uint32_t pulse = (uint32_t)(((uint64_t)pwm.period * pm) / 1000U);

	pwm_set_pulse_dt(&pwm, pulse);
}

static void fade_step(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&lock, K_FOREVER);

	if (current_pm < target_pm) {
		current_pm = MIN(current_pm + step_pm, target_pm);
	} else if (current_pm > target_pm) {
		current_pm = MAX(current_pm - step_pm, target_pm);
	}

	apply(current_pm);

	if (current_pm != target_pm) {
		k_work_reschedule(&fade_work, K_MSEC(FADE_STEP_MS));
	}

	k_mutex_unlock(&lock);
}

/*
 * Brightness (permille) for a lit room: MIN_PM at DARK_ON_LUX up to 1000 at
 * FULL_LUX, a constant ratio per decade of lux.
 */
static int lux_to_pm(double lux)
{
	if (lux >= FULL_LUX) {
		return 1000;
	}
	if (lux <= DARK_ON_LUX) {
		return MIN_PM;
	}

	double f = log(lux / DARK_ON_LUX) / log(FULL_LUX / DARK_ON_LUX);

	return (int)lround(MIN_PM * pow(1000.0 / MIN_PM, f));
}

/* Fade to whatever the current inputs call for. Call with `lock` held. */
static void reevaluate(void)
{
	int new_target;

	if (manual_pm >= 0) {
		new_target = manual_pm;
	} else if (!last_lux_ok) {
		/* No light reading: fail bright rather than dark. */
		new_target = 1000;
	} else if (dark) {
		new_target = woken ? MIN_PM : 0;
	} else {
		new_target = lux_to_pm(last_lux);
		/*
		 * Small changes while on are noise: hold the current level,
		 * except to reach either end of the scale exactly.
		 */
		if (target_pm > 0 && new_target != 1000 && new_target != MIN_PM &&
		    abs(new_target - target_pm) * RETARGET_FRACTION < target_pm) {
			new_target = target_pm;
		}
	}

	if (new_target == target_pm) {
		return;
	}

	/* Fade the whole distance in FADE_MS, whatever its size. */
	int steps = FADE_MS / FADE_STEP_MS;

	target_pm = new_target;
	step_pm = MAX(1, abs(target_pm - current_pm) / steps);
	k_work_reschedule(&fade_work, K_NO_WAIT);
}

static void wake_expired(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&lock, K_FOREVER);
	woken = false;
	reevaluate();
	k_mutex_unlock(&lock);
}

int backlight_init(void)
{
	if (!pwm_is_ready_dt(&pwm)) {
		LOG_ERR("backlight PWM not ready");
		return -ENODEV;
	}

	k_mutex_lock(&lock, K_FOREVER);
	current_pm = target_pm = 1000;
	last_lux_ok = false;
	apply(current_pm);
	k_mutex_unlock(&lock);
	return 0;
}

void backlight_set_ambient(bool lux_ok, double lux)
{
	if (!pwm_is_ready_dt(&pwm)) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);

	if (!lux_ok) {
		/* Hold the last level through a brief gap; then fail bright. */
		if (++misses >= MISSES_BEFORE_FAIL) {
			last_lux_ok = false;
			dark = false;
		}
	} else {
		misses = 0;
		last_lux_ok = true;
		last_lux = lux;
		if (dark && lux >= DARK_ON_LUX) {
			dark = false;
			LOG_INF("room light (%.3g lux): backlight on", lux);
		} else if (!dark && lux < DARK_OFF_LUX) {
			dark = true;
			LOG_INF("room dark (%.3g lux): backlight off", lux);
		}
	}
	reevaluate();

	k_mutex_unlock(&lock);
}

bool backlight_wake(void)
{
	if (!pwm_is_ready_dt(&pwm)) {
		return false;
	}

	k_mutex_lock(&lock, K_FOREVER);

	bool was_off = target_pm == 0;

	woken = true;
	reevaluate();
	k_work_reschedule(&wake_work, K_MSEC(WAKE_MS));

	k_mutex_unlock(&lock);
	return was_off;
}

/* Shell: "backlight <0-100>" fixes the brightness, "backlight auto" resumes. */
static int cmd_backlight(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "%s, now %d%%, target %d%%",
			    manual_pm >= 0 ? "manual" : "auto", current_pm / 10, target_pm / 10);
		return 0;
	}

	int new_manual;

	if (strcmp(argv[1], "auto") == 0) {
		new_manual = -1;
	} else {
		long pct = strtol(argv[1], NULL, 10);

		if (pct < 0 || pct > 100) {
			shell_error(sh, "usage: backlight [0-100|auto]");
			return -EINVAL;
		}
		new_manual = (int)pct * 10;
	}

	k_mutex_lock(&lock, K_FOREVER);
	manual_pm = new_manual;
	reevaluate();
	k_mutex_unlock(&lock);
	return 0;
}

SHELL_CMD_ARG_REGISTER(backlight, NULL, "Show, set (0-100) or auto backlight", cmd_backlight, 1,
		       1);
