/*
 * Minute-by-minute log of every reading, as daily CSV files on the SD card.
 * See sdlog.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_SDLOG_H_
#define SENSOR_BOX_SDLOG_H_

#include "readings.h"

void sdlog_init(void);

/* Fold one set of readings into the current minute. Call on every report. */
void sdlog_add(const struct readings *r);

/* Write out the buffered lines now (also done every 5 minutes). */
int sdlog_flush(void);

#endif /* SENSOR_BOX_SDLOG_H_ */
