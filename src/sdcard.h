/*
 * microSD card in the TFT FeatherWing's slot, FAT formatted, mounted at
 * SDCARD_MNT. See sdcard.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_SDCARD_H_
#define SENSOR_BOX_SDCARD_H_

#include <stdbool.h>

#define SDCARD_MNT "/SD:"

/* Mount the card if one is present. Logs why not, otherwise. */
void sdcard_init(void);

/* Whether a card is mounted. */
bool sdcard_ready(void);

#endif /* SENSOR_BOX_SDCARD_H_ */
