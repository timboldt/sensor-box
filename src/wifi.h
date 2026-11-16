/*
 * Wi-Fi station: connect to the network stored on the device.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_WIFI_H_
#define SENSOR_BOX_WIFI_H_

#include <stdbool.h>
#include <stddef.h>

/* Start connecting to the stored network (if any), and keep reconnecting. */
void wifi_init(void);

/* True once connected with an IPv4 address. */
bool wifi_is_connected(void);

#endif /* SENSOR_BOX_WIFI_H_ */
