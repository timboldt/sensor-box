/*
 * LAN web dashboard (HTTP on port 80).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_WEB_H_
#define SENSOR_BOX_WEB_H_

#include "readings.h"

/* Start the HTTP server. */
void web_init(void);

/* Publish the latest readings for /api/now. Call from the sensor loop. */
void web_update(const struct readings *r);

#endif /* SENSOR_BOX_WEB_H_ */
