/*
 * Sensirion SGP30 indoor air quality sensor (eCO2 / TVOC).
 *
 * Copyright (c) 2026 Tim Boldt
 * SPDX-License-Identifier: MIT
 *
 * The SGP30's on-chip baseline compensation assumes it is measured exactly
 * once per second. Rather than push that requirement onto the application,
 * this driver clocks itself: a k_work_delayable on the system workqueue
 * issues Measure_air_quality every second and caches the result.
 *
 *   sensor_sample_fetch() - no-op (returns -EAGAIN until the first reading)
 *   sensor_channel_get(SENSOR_CHAN_CO2) - cached eCO2 in ppm
 *   sensor_channel_get(SENSOR_CHAN_VOC) - cached TVOC in ppb
 *
 * For ~15 s after power-up the sensor returns a fixed 400 ppm / 0 ppb while
 * it warms up; that is expected, not an error.
 *
 * The IAQ baseline (SENSOR_ATTR_SGP30_BASELINE) is also handled in the work
 * handler, so its commands never collide with a measurement: a baseline set
 * by the application is sent before the next measurement, and the current
 * baseline is read back once a minute for sensor_attr_get().
 *
 * I2C wire format: every command is a big-endian u16; every returned data
 * word is 2 bytes followed by a CRC-8 (poly 0x31, init 0xFF).
 */

#define DT_DRV_COMPAT sensirion_sgp30

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <sensor_box/sgp30.h>

LOG_MODULE_REGISTER(SGP30, CONFIG_SENSOR_LOG_LEVEL);

#define SGP30_CMD_IAQ_INIT       0x2003
#define SGP30_CMD_IAQ_MEASURE    0x2008
#define SGP30_CMD_SET_HUMIDITY   0x2061
#define SGP30_CMD_GET_FEATURESET 0x202F
#define SGP30_CMD_GET_BASELINE   0x2015
#define SGP30_CMD_SET_BASELINE   0x201E

/* Sentinel meaning "no humidity value has been pushed by attr_set yet". */
#define SGP30_AH_UNSET           UINT32_MAX

#define SGP30_CRC_POLY           0x31
#define SGP30_CRC_INIT           0xFF

/* Datasheet max durations, rounded up. */
#define SGP30_INIT_WAIT_MS       10
#define SGP30_MEASURE_WAIT_MS    12
#define SGP30_MEASURE_PERIOD     K_SECONDS(1)
#define SGP30_BASELINE_WAIT_MS   10

/* Read the baseline back every this many measurement cycles. */
#define SGP30_BASELINE_READ_EVERY 60

struct sgp30_config {
	struct i2c_dt_spec i2c;
};

struct sgp30_data {
	const struct device *dev;
	struct k_work_delayable measure_work;
	uint16_t eco2_ppm;
	uint16_t tvoc_ppb;
	bool valid;   /* at least one good measurement cached */

	/*
	 * Absolute humidity as 8.8 fixed-point g/m^3, set from attr_set()
	 * (main thread) and consumed by the work handler (sysworkq). Holds
	 * SGP30_AH_UNSET until the application pushes a value.
	 */
	atomic_t abs_humidity_8_8;
	uint16_t ah_last_sent;   /* work-handler-private */

	/* Baseline, shared between the work handler and attr_get/attr_set. */
	struct k_spinlock lock;
	bool baseline_pending;       /* set_baseline queued by attr_set */
	uint16_t pending_eco2, pending_tvoc;
	bool baseline_valid;         /* read back at least once */
	uint16_t baseline_eco2, baseline_tvoc;
	unsigned int cycles;         /* work-handler-private */
};

static uint8_t sgp30_crc(const uint8_t *data)
{
	return crc8(data, 2, SGP30_CRC_POLY, SGP30_CRC_INIT, false);
}

static int sgp30_send_cmd(const struct device *dev, uint16_t cmd)
{
	const struct sgp30_config *cfg = dev->config;
	uint8_t buf[2];

	sys_put_be16(cmd, buf);

	return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

/* Read n_words data words, verifying the CRC after each; store the raw u16s. */
static int sgp30_read_words(const struct device *dev, uint16_t *words, size_t n_words)
{
	const struct sgp30_config *cfg = dev->config;
	uint8_t buf[9];   /* max 3 words * 3 bytes */
	int ret;

	if (n_words > 3) {
		return -EINVAL;
	}

	ret = i2c_read_dt(&cfg->i2c, buf, n_words * 3);
	if (ret < 0) {
		return ret;
	}

	for (size_t i = 0; i < n_words; i++) {
		const uint8_t *w = &buf[i * 3];

		if (sgp30_crc(w) != w[2]) {
			LOG_ERR("CRC error on word %zu", i);
			return -EIO;
		}
		words[i] = sys_get_be16(w);
	}

	return 0;
}

/* Send Set_humidity: command word + 1 data word (abs humidity, 8.8 g/m^3). */
static int sgp30_send_humidity(const struct device *dev, uint16_t ah_8_8)
{
	const struct sgp30_config *cfg = dev->config;
	uint8_t buf[5];

	sys_put_be16(SGP30_CMD_SET_HUMIDITY, buf);
	sys_put_be16(ah_8_8, &buf[2]);
	buf[4] = sgp30_crc(&buf[2]);

	return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

/* Send Set_baseline. Note the order: TVOC word first, then eCO2. */
static int sgp30_send_baseline(const struct device *dev, uint16_t eco2, uint16_t tvoc)
{
	const struct sgp30_config *cfg = dev->config;
	uint8_t buf[8];

	sys_put_be16(SGP30_CMD_SET_BASELINE, buf);
	sys_put_be16(tvoc, &buf[2]);
	buf[4] = sgp30_crc(&buf[2]);
	sys_put_be16(eco2, &buf[5]);
	buf[7] = sgp30_crc(&buf[5]);

	return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

/* Restore a queued baseline, if any. Work-handler context. */
static void sgp30_restore_baseline(struct sgp30_data *data)
{
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	bool pending = data->baseline_pending;
	uint16_t eco2 = data->pending_eco2;
	uint16_t tvoc = data->pending_tvoc;

	data->baseline_pending = false;
	k_spin_unlock(&data->lock, key);

	if (!pending) {
		return;
	}
	if (sgp30_send_baseline(data->dev, eco2, tvoc) == 0) {
		LOG_INF("baseline restored: eCO2 0x%04x TVOC 0x%04x", eco2, tvoc);
		k_sleep(K_MSEC(SGP30_BASELINE_WAIT_MS));
	} else {
		LOG_WRN("Set_baseline failed");
	}
}

/* Read the current baseline into the cache. Work-handler context. */
static void sgp30_read_baseline(struct sgp30_data *data)
{
	uint16_t words[2];

	if (sgp30_send_cmd(data->dev, SGP30_CMD_GET_BASELINE) < 0) {
		LOG_WRN("Get_baseline command failed");
		return;
	}
	k_sleep(K_MSEC(SGP30_BASELINE_WAIT_MS));
	if (sgp30_read_words(data->dev, words, 2) < 0) {
		LOG_WRN("Get_baseline read failed");
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	data->baseline_eco2 = words[0];
	data->baseline_tvoc = words[1];
	data->baseline_valid = true;
	k_spin_unlock(&data->lock, key);
}

static void sgp30_measure_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct sgp30_data *data = CONTAINER_OF(dwork, struct sgp30_data, measure_work);
	const struct device *dev = data->dev;
	uint16_t words[2];
	int ret;

	/* Reschedule first so a transient error doesn't stop the 1 Hz cadence. */
	k_work_reschedule(&data->measure_work, SGP30_MEASURE_PERIOD);

	/* Push a new absolute-humidity value to the sensor if it changed. */
	atomic_val_t ah = atomic_get(&data->abs_humidity_8_8);

	if (ah != SGP30_AH_UNSET && (uint16_t)ah != data->ah_last_sent) {
		if (sgp30_send_humidity(dev, (uint16_t)ah) == 0) {
			data->ah_last_sent = (uint16_t)ah;
			k_sleep(K_MSEC(SGP30_MEASURE_WAIT_MS));
		} else {
			LOG_WRN("Set_humidity failed");
		}
	}

	sgp30_restore_baseline(data);

	ret = sgp30_send_cmd(dev, SGP30_CMD_IAQ_MEASURE);
	if (ret < 0) {
		LOG_WRN("measure command failed: %d", ret);
		return;
	}

	k_sleep(K_MSEC(SGP30_MEASURE_WAIT_MS));

	ret = sgp30_read_words(dev, words, 2);
	if (ret < 0) {
		LOG_WRN("measure read failed: %d", ret);
		return;
	}

	data->eco2_ppm = words[0];
	data->tvoc_ppb = words[1];
	data->valid = true;

	if (++data->cycles % SGP30_BASELINE_READ_EVERY == 0) {
		sgp30_read_baseline(data);
	}
}

static int sgp30_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct sgp30_data *data = dev->data;

	if (chan != SENSOR_CHAN_ALL &&
	    chan != SENSOR_CHAN_CO2 && chan != SENSOR_CHAN_VOC) {
		return -ENOTSUP;
	}

	/* The work item keeps the cache fresh; nothing to do here. */
	return data->valid ? 0 : -EAGAIN;
}

static int sgp30_channel_get(const struct device *dev, enum sensor_channel chan,
			     struct sensor_value *val)
{
	const struct sgp30_data *data = dev->data;

	val->val2 = 0;

	switch (chan) {
	case SENSOR_CHAN_CO2:
		val->val1 = data->eco2_ppm;
		break;
	case SENSOR_CHAN_VOC:
		val->val1 = data->tvoc_ppb;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int sgp30_attr_set(const struct device *dev, enum sensor_channel chan,
			  enum sensor_attribute attr, const struct sensor_value *val)
{
	struct sgp30_data *data = dev->data;

	if ((int)attr == SENSOR_ATTR_SGP30_BASELINE) {
		if (val->val1 < 0 || val->val1 > UINT16_MAX ||
		    val->val2 < 0 || val->val2 > UINT16_MAX) {
			return -EINVAL;
		}

		k_spinlock_key_t key = k_spin_lock(&data->lock);

		data->pending_eco2 = (uint16_t)val->val1;
		data->pending_tvoc = (uint16_t)val->val2;
		data->baseline_pending = true;
		k_spin_unlock(&data->lock, key);
		return 0;
	}

	if ((int)attr != SENSOR_ATTR_SGP30_ABS_HUMIDITY) {
		return -ENOTSUP;
	}

	/* g/m^3 (val1.val2) -> 8.8 fixed-point, clamped to the u16 range. */
	int64_t ah_8_8 = (int64_t)val->val1 * 256 +
			 ((int64_t)val->val2 * 256) / 1000000;

	ah_8_8 = CLAMP(ah_8_8, 0, UINT16_MAX);
	atomic_set(&data->abs_humidity_8_8, (atomic_val_t)ah_8_8);

	return 0;
}

static int sgp30_attr_get(const struct device *dev, enum sensor_channel chan,
			  enum sensor_attribute attr, struct sensor_value *val)
{
	struct sgp30_data *data = dev->data;
	int ret = 0;

	if ((int)attr != SENSOR_ATTR_SGP30_BASELINE) {
		return -ENOTSUP;
	}

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (data->baseline_valid) {
		val->val1 = data->baseline_eco2;
		val->val2 = data->baseline_tvoc;
	} else {
		ret = -EAGAIN;
	}
	k_spin_unlock(&data->lock, key);
	return ret;
}

static DEVICE_API(sensor, sgp30_api) = {
	.sample_fetch = sgp30_sample_fetch,
	.channel_get = sgp30_channel_get,
	.attr_set = sgp30_attr_set,
	.attr_get = sgp30_attr_get,
};

static int sgp30_init(const struct device *dev)
{
	const struct sgp30_config *cfg = dev->config;
	struct sgp30_data *data = dev->data;
	uint16_t featureset;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	data->dev = dev;
	atomic_set(&data->abs_humidity_8_8, SGP30_AH_UNSET);
	data->ah_last_sent = 0;

	/* Probe: read the feature set (also confirms CRC wiring). */
	ret = sgp30_send_cmd(dev, SGP30_CMD_GET_FEATURESET);
	if (ret == 0) {
		k_sleep(K_MSEC(SGP30_INIT_WAIT_MS));
		ret = sgp30_read_words(dev, &featureset, 1);
	}
	if (ret < 0) {
		LOG_ERR("SGP30 not responding: %d", ret);
		return ret;
	}
	LOG_DBG("SGP30 feature set 0x%04x", featureset);

	ret = sgp30_send_cmd(dev, SGP30_CMD_IAQ_INIT);
	if (ret < 0) {
		LOG_ERR("Init_air_quality failed: %d", ret);
		return ret;
	}
	k_sleep(K_MSEC(SGP30_INIT_WAIT_MS));

	k_work_init_delayable(&data->measure_work, sgp30_measure_work);
	k_work_reschedule(&data->measure_work, SGP30_MEASURE_PERIOD);

	return 0;
}

#define SGP30_DEFINE(inst)								\
	static struct sgp30_data sgp30_data_##inst;					\
											\
	static const struct sgp30_config sgp30_config_##inst = {				\
		.i2c = I2C_DT_SPEC_INST_GET(inst),					\
	};										\
											\
	SENSOR_DEVICE_DT_INST_DEFINE(inst, sgp30_init, NULL,				\
				    &sgp30_data_##inst, &sgp30_config_##inst,		\
				    POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,		\
				    &sgp30_api);

DT_INST_FOREACH_STATUS_OKAY(SGP30_DEFINE)
