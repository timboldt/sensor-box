/*
 * Long-term log: hourly averages of every reading, kept in flash.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_LTLOG_H_
#define SENSOR_BOX_LTLOG_H_

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/fs/fcb.h>
#include <zephyr/toolchain.h>

#include "readings.h"

/* Bits in ltlog_record.valid: the field had at least one reading that hour. */
enum {
	LTLOG_TEMP = BIT(0),
	LTLOG_RH = BIT(1),
	LTLOG_PRESS = BIT(2),
	LTLOG_PM25 = BIT(3),
	LTLOG_ECO2 = BIT(4),
	LTLOG_TVOC = BIT(5),
	LTLOG_LUX = BIT(6),
};

/* One stored hour (20 bytes). Decode with the ltlog_*() helpers below. */
struct ltlog_record {
	uint32_t t;		/* UTC seconds at the start of the hour */
	int16_t temp_c100;	/* degC x 100 */
	uint16_t rh10;		/* %RH x 10 */
	uint16_t press10;	/* sea-level hPa x 10 */
	uint16_t pm25;		/* ug/m3 */
	uint16_t eco2;		/* ppm */
	uint16_t tvoc;		/* ppb */
	uint16_t light;		/* v2: int16 dB re 1 lux x 100; v1: (log10(lux) + 2) x 1000 */
	uint8_t valid;		/* LTLOG_* bits */
	uint8_t version;	/* LTLOG_RECORD_VERSION */
} __packed;

/*
 * Record format. v1 (2026-11-16) stored light as log10(lux) with a 0.01 lux
 * floor; v2 stores dB re 1 lux, reaching far below 1 lux. Both are read.
 */
#define LTLOG_RECORD_VERSION 2

static inline bool ltlog_record_known(const struct ltlog_record *r)
{
	return r->version == 1 || r->version == 2;
}

/* Light in dB re 1 lux (0 dB = 1 lux). */
static inline double ltlog_light_db(const struct ltlog_record *r)
{
	if (r->version == 1) {
		return 10.0 * (r->light / 1000.0 - 2.0);
	}
	return (int16_t)r->light / 100.0;
}

static inline double ltlog_lux(const struct ltlog_record *r)
{
	return pow(10.0, ltlog_light_db(r) / 10.0);
}

/* Open (or create) the log in the "history" flash partition. */
int ltlog_init(void);

/* Fold one set of readings into the current hour; write the hour when it ends. */
void ltlog_add(const struct readings *r);

/*
 * Call cb for each stored record, oldest first, until it returns false.
 * Returns the number of records visited, or a negative error.
 */
int ltlog_walk(bool (*cb)(const struct ltlog_record *rec, void *arg), void *arg);

/*
 * Resumable iteration, for streaming the log out in pieces: zero-initialise
 * a cursor, then call ltlog_next() until it returns -ENOENT (end). Returns 0
 * with *rec filled, or a negative error.
 */
struct ltlog_cursor {
	struct fcb_entry loc;
};

int ltlog_next(struct ltlog_cursor *c, struct ltlog_record *rec);

#endif /* SENSOR_BOX_LTLOG_H_ */
