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
 *  - steps the gain between x1, x25, x400 and x9200 to keep the full-spectrum
 *    channel (ch0) between GAIN_UP_BELOW and GAIN_DOWN_ABOVE counts, and
 *    steps down at once if either channel clips. ch0 alone decides "up":
 *    it always includes the IR in ch1, and requiring ch1 too (as the Rust code
 *    did) can oscillate under LED light, where ch1 is ~0;
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
static bool settling;		/* next reading may be from the old gain */

static bool set_gain(int idx)
{
	struct sensor_value v = { .val1 = gains[idx].mode };

	if (sensor_attr_set(dev, SENSOR_CHAN_ALL, (enum sensor_attribute)SENSOR_ATTR_GAIN_MODE,
			    &v) != 0) {
		LOG_WRN("setting gain %s failed", gains[idx].name);
		return false;
	}
	gain_idx = idx;
	settling = true;
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

	if (settling) {
		/* Registers may still hold an integration from before the change. */
		settling = false;
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
	if (clipped && gain_idx > 0) {
		set_gain(gain_idx - 1);
	} else if (ch0 > GAIN_DOWN_ABOVE && gain_idx > 0) {
		set_gain(gain_idx - 1);
	} else if (ch0 < GAIN_UP_BELOW && gain_idx < (int)ARRAY_SIZE(gains) - 1) {
		set_gain(gain_idx + 1);
	}

	/* A clipped reading at the lowest gain is still a (lower-bound) value. */
	return !clipped || gain_idx == 0;
}
