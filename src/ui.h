/*
 * TFT dashboard (LVGL).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_UI_H_
#define SENSOR_BOX_UI_H_

#include "readings.h"

/* Build the screen and turn the display on. Returns 0, or -ENODEV. */
int ui_init(void);

/* Show a new set of readings. Safe to call from any thread. */
void ui_update(const struct readings *r);

/*
 * Show the next page: dashboard -> one full-screen chart per reading ->
 * dashboard. Returns to the dashboard on its own after a minute. Safe to
 * call from any thread.
 */
void ui_next_page(void);

#endif /* SENSOR_BOX_UI_H_ */
