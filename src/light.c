/*
 * Ambient light from the TSL2591, with automatic gain.
 *
 * SPDX-License-Identifier: MIT
 *
 * The Zephyr driver starts at gain x1 / 100 ms and never changes it, where
 * one count is ~4 lux, so anything dimmer than a lamp reads 0. This module
 * (after the auto-gain in the earlier Rust firmware, iot-central tsl.rs):
 *
 *  - integrates for 200 ms, so a channel can reach the full 16-bit range
 *    (at 100 ms it saturates at 36863 counts);
 *  - picks the gain from x1, x25, x400 and x9200 when the full-spectrum
 *    channel (ch0) leaves GAIN_UP_BELOW..GAIN_DOWN_ABOVE counts: the highest
 *    gain at which this reading would have been at most GAIN_TARGET counts,
 *    so a big change (room lights on or off) is one jump, not one step per
 *    reading. If either channel clips, the true count is unknown, so it drops
 *    straight to x1. ch0 alone decides: it always includes the IR in ch1, and
 *    requiring ch1 too (as the Rust code did) can oscillate under LED light,
 *    where ch1 is ~0;
 *  - a gain change restarts the sensor's integration (the driver powers it
 *    down and up), so a reading within SETTLE_MS of one is dropped. Read
 *    once a second, that never happens;
 *  - computes lux from the raw counts with the Adafruit / Zephyr formula
 *    (DF = 408): lux = (ch0 - ch1) * (1 - ch1/ch0) * 408 / (ms * gain).
 *
 * At x9200 / 200 ms one count is ~0.0002 lux (about -37 dB). Below one count
 * the reading is reported as half a count rather than 0, so the log scale
 * stays finite; the dashboard's dB floor sits near there.
 *
 * Counts are recovered from the driver's scaled channel values
 * (SENSOR_CHAN_LIGHT / _IR = count * 408 / (ms * gain)), so the driver's own
 * gain bookkeeping is used throughout.
 */

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/tsl2591.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "light.h"

LOG_MODULE_REGISTER(light, LOG_LEVEL_INF);

#define INTEGRATION_MS   200
#define LUX_DF           408.0
#define MAX_COUNT        65535
#define GAIN_UP_BELOW    1000
#define GAIN_DOWN_ABOVE  50000
#define GAIN_TARGET      20000
#define SETTLE_MS        (2 * INTEGRATION_MS + 100)

static const struct device *const dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(tsl2591));

static const struct {
	enum sensor_gain_tsl2591 mode;
	double factor;		/* matches the driver's TSL2591_GAIN_SCALE_* */
	const char *name;
} gains[] = {
	{ TSL2591_SENSOR_GAIN_LOW, 1.0, "x1" },
	{ TSL2591_SENSOR_GAIN_MED, 25.0, "x25" },
	{ TSL2591_SENSOR_GAIN_HIGH, 400.0, "x400" },
	{ TSL2591_SENSOR_GAIN_MAX, 9200.0, "x9200" },
};

static int gain_idx = 1;	/* start at x25: right for most rooms */
static int read_gain_idx = 1;	/* the gain the last reading was taken at */
static bool ok;
static int64_t gain_changed_at;	/* uptime (ms) of the last gain change */

static bool set_gain(int idx)
{
	struct sensor_value v = { .val1 = gains[idx].mode };

	if (sensor_attr_set(dev, SENSOR_CHAN_ALL, (enum sensor_attribute)SENSOR_ATTR_GAIN_MODE,
			    &v) != 0) {
		LOG_WRN("setting gain %s failed", gains[idx].name);
		return false;
	}
	gain_idx = idx;
	gain_changed_at = k_uptime_get();
	return true;
}

void light_init(void)
{
	if (dev == NULL || !device_is_ready(dev)) {
		LOG_WRN("TSL2591 not ready");
		return;
	}

	struct sensor_value t = { .val1 = INTEGRATION_MS };

	ok = sensor_attr_set(dev, SENSOR_CHAN_ALL,
			     (enum sensor_attribute)SENSOR_ATTR_INTEGRATION_TIME, &t) == 0 &&
	     set_gain(gain_idx);
	if (!ok) {
		LOG_ERR("TSL2591 configuration failed");
	}
}

/* Driver channel value back to the raw 16-bit count. */
static double to_count(const struct sensor_value *v)
{
	return sensor_value_to_double(v) * INTEGRATION_MS * gains[gain_idx].factor / LUX_DF;
}

const char *light_gain_name(void)
{
	return gains[read_gain_idx].name;
}

bool light_read(double *lux)
{
	if (!ok) {
		return false;
	}

	struct sensor_value v0, v1;

	if (sensor_sample_fetch(dev) != 0 ||
	    sensor_channel_get(dev, SENSOR_CHAN_LIGHT, &v0) != 0 ||
	    sensor_channel_get(dev, SENSOR_CHAN_IR, &v1) != 0) {
		LOG_WRN("TSL2591 read failed");
		return false;
	}

	if (k_uptime_get() - gain_changed_at < SETTLE_MS) {
		/* No complete integration at the new gain yet. */
		return false;
	}

	read_gain_idx = gain_idx;

	double ch0 = round(to_count(&v0));
	double ch1 = round(to_count(&v1));
	double per_count = LUX_DF / (INTEGRATION_MS * gains[gain_idx].factor);
	bool clipped = ch0 >= MAX_COUNT || ch1 >= MAX_COUNT;

	/* Lux, before any gain change (the counts belong to the current gain). */
	double counts = (ch0 > ch1 && ch0 > 0) ? (ch0 - ch1) * (1.0 - ch1 / ch0) : 0.0;

	*lux = MAX(counts, 0.5) * per_count;

	LOG_DBG("ch0 %.0f ch1 %.0f gain %s -> %.4g lux", ch0, ch1, gains[gain_idx].name, *lux);

	/* Gain for the next reading. */
	bool valid = !clipped || gain_idx == 0;	/* clipped at x1: a lower bound */
	int want = gain_idx;

	if (clipped) {
		want = 0;
	} else if (ch0 > GAIN_DOWN_ABOVE || ch0 < GAIN_UP_BELOW) {
		/* The highest gain that would put this light at <= GAIN_TARGET. */
		double c = MAX(ch0, 1.0) / gains[gain_idx].factor;

		want = 0;
		for (int i = ARRAY_SIZE(gains) - 1; i > 0; i--) {
			if (c * gains[i].factor <= GAIN_TARGET) {
				want = i;
				break;
			}
		}
	}
	if (want != gain_idx) {
		set_gain(want);
	}

	return valid;
}
