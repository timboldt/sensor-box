/*
 * Plantower PMSA003I I2C particulate matter sensor.
 *
 * Copyright (c) 2026 Tim Boldt
 * SPDX-License-Identifier: MIT
 *
 * The PMSA003I streams a fixed 32-byte frame; an I2C read of 32 bytes
 * (no register/command byte) returns the most recent one:
 *
 *   off  field
 *    0   0x42  start byte 1
 *    1   0x4D  start byte 2
 *    2   frame length (big-endian u16, = 28)
 *    4   PM1.0  CF=1  (ug/m3)          } "standard particle"
 *    6   PM2.5  CF=1                   }
 *    8   PM10   CF=1                   }
 *   10   PM1.0  atmospheric (ug/m3)    } ambient-air calibration
 *   12   PM2.5  atmospheric            }
 *   14   PM10   atmospheric            }
 *   16   count >0.3um / 0.1L
 *   18   count >0.5um
 *   20   count >1.0um
 *   22   count >2.5um
 *   24   count >5.0um
 *   26   count >10um
 *   28   firmware version
 *   29   error code
 *   30   checksum (big-endian u16 = sum of bytes 0..29)
 *
 * All multi-byte values are big-endian.
 */

#define DT_DRV_COMPAT plantower_pmsa003i

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(PMSA003I, CONFIG_SENSOR_LOG_LEVEL);

#define PMSA003I_FRAME_LEN   32
#define PMSA003I_MAGIC_0     0x42
#define PMSA003I_MAGIC_1     0x4D

/* Byte offsets of each big-endian u16 field in the frame. */
enum {
	OFF_PM1_0_CF  = 4,
	OFF_PM2_5_CF  = 6,
	OFF_PM10_CF   = 8,
	OFF_PM1_0_ATM = 10,
	OFF_PM2_5_ATM = 12,
	OFF_PM10_ATM  = 14,
	OFF_CNT_0_3   = 16,
	OFF_CNT_0_5   = 18,
	OFF_CNT_1_0   = 20,
	OFF_CNT_2_5   = 22,
	OFF_CNT_5_0   = 24,
	OFF_CNT_10    = 26,
	OFF_CHECKSUM  = 30,
};

struct pmsa003i_config {
	struct i2c_dt_spec i2c;
};

struct pmsa003i_data {
	uint16_t pm_cf[3];   /* 1.0, 2.5, 10  CF=1 */
	uint16_t pm_atm[3];  /* 1.0, 2.5, 10  atmospheric */
	uint16_t count[6];   /* 0.3, 0.5, 1.0, 2.5, 5.0, 10 um */
};

static int pmsa003i_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct pmsa003i_config *cfg = dev->config;
	struct pmsa003i_data *data = dev->data;
	uint8_t buf[PMSA003I_FRAME_LEN];
	int ret;

	if (chan != SENSOR_CHAN_ALL) {
		return -ENOTSUP;
	}

	ret = i2c_read_dt(&cfg->i2c, buf, sizeof(buf));
	if (ret < 0) {
		LOG_ERR("I2C read failed: %d", ret);
		return ret;
	}

	if (buf[0] != PMSA003I_MAGIC_0 || buf[1] != PMSA003I_MAGIC_1) {
		LOG_ERR("bad frame header %02x %02x", buf[0], buf[1]);
		return -EBADMSG;
	}

	uint16_t sum = 0;

	for (int i = 0; i < OFF_CHECKSUM; i++) {
		sum += buf[i];
	}
	if (sum != sys_get_be16(&buf[OFF_CHECKSUM])) {
		LOG_ERR("checksum mismatch");
		return -EBADMSG;
	}

	data->pm_cf[0]  = sys_get_be16(&buf[OFF_PM1_0_CF]);
	data->pm_cf[1]  = sys_get_be16(&buf[OFF_PM2_5_CF]);
	data->pm_cf[2]  = sys_get_be16(&buf[OFF_PM10_CF]);
	data->pm_atm[0] = sys_get_be16(&buf[OFF_PM1_0_ATM]);
	data->pm_atm[1] = sys_get_be16(&buf[OFF_PM2_5_ATM]);
	data->pm_atm[2] = sys_get_be16(&buf[OFF_PM10_ATM]);
	data->count[0]  = sys_get_be16(&buf[OFF_CNT_0_3]);
	data->count[1]  = sys_get_be16(&buf[OFF_CNT_0_5]);
	data->count[2]  = sys_get_be16(&buf[OFF_CNT_1_0]);
	data->count[3]  = sys_get_be16(&buf[OFF_CNT_2_5]);
	data->count[4]  = sys_get_be16(&buf[OFF_CNT_5_0]);
	data->count[5]  = sys_get_be16(&buf[OFF_CNT_10]);

	return 0;
}

static int pmsa003i_channel_get(const struct device *dev, enum sensor_channel chan,
				struct sensor_value *val)
{
	struct pmsa003i_data *data = dev->data;

	/* All channels are integer counts / concentrations: val2 is always 0. */
	val->val2 = 0;

	switch ((int)chan) {
	case SENSOR_CHAN_PM_1_0:
		val->val1 = data->pm_atm[0];
		break;
	case SENSOR_CHAN_PM_2_5:
		val->val1 = data->pm_atm[1];
		break;
	case SENSOR_CHAN_PM_10:
		val->val1 = data->pm_atm[2];
		break;
	case SENSOR_CHAN_PM_1_0_CF:
		val->val1 = data->pm_cf[0];
		break;
	case SENSOR_CHAN_PM_2_5_CF:
		val->val1 = data->pm_cf[1];
		break;
	case SENSOR_CHAN_PM_10_CF:
		val->val1 = data->pm_cf[2];
		break;
	case SENSOR_CHAN_PM_0_3_COUNT:
		val->val1 = data->count[0];
		break;
	case SENSOR_CHAN_PM_0_5_COUNT:
		val->val1 = data->count[1];
		break;
	case SENSOR_CHAN_PM_1_0_COUNT:
		val->val1 = data->count[2];
		break;
	case SENSOR_CHAN_PM_2_5_COUNT:
		val->val1 = data->count[3];
		break;
	case SENSOR_CHAN_PM_5_COUNT:
		val->val1 = data->count[4];
		break;
	case SENSOR_CHAN_PM_10_COUNT:
		val->val1 = data->count[5];
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static DEVICE_API(sensor, pmsa003i_api) = {
	.sample_fetch = pmsa003i_sample_fetch,
	.channel_get = pmsa003i_channel_get,
};

static int pmsa003i_init(const struct device *dev)
{
	const struct pmsa003i_config *cfg = dev->config;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	/*
	 * The PMSA003I powers up in active mode streaming frames on its own;
	 * no configuration write is required. The fan needs ~30 s to give
	 * stable readings, but that is the application's concern, not init's.
	 */
	return 0;
}

#define PMSA003I_DEFINE(inst)								\
	static struct pmsa003i_data pmsa003i_data_##inst;				\
											\
	static const struct pmsa003i_config pmsa003i_config_##inst = {			\
		.i2c = I2C_DT_SPEC_INST_GET(inst),					\
	};										\
											\
	SENSOR_DEVICE_DT_INST_DEFINE(inst, pmsa003i_init, NULL,				\
				    &pmsa003i_data_##inst, &pmsa003i_config_##inst,	\
				    POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,		\
				    &pmsa003i_api);

DT_INST_FOREACH_STATUS_OKAY(PMSA003I_DEFINE)
