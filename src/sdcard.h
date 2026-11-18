/*
 * microSD card in the TFT FeatherWing's slot, FAT formatted, mounted at
 * SDCARD_MNT. See sdcard.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SENSOR_BOX_SDCARD_H_
#define SENSOR_BOX_SDCARD_H_

#include <stdbool.h>
#include <stddef.h>

#define SDCARD_MNT "/SD:"

/* Mount the card if one is present. Logs why not, otherwise. */
void sdcard_init(void);

/* Whether a card is mounted. */
bool sdcard_ready(void);

/*
 * Append len bytes of data to the file `path` (e.g. SDCARD_MNT "/LOG/X.CSV"),
 * creating it, and its directory `dir` if missing. A new (or empty) file
 * gets `header` first. The file is closed, so everything is on the card
 * when this returns. Returns 0 or a negative error (-ENODEV: no card).
 */
int sdcard_append(const char *dir, const char *path, const char *header, const char *data,
		  size_t len);

/*
 * Read the file `path` from the start, passing each piece to cb (which
 * returns false to stop). Returns 0, or a negative error (-ENOENT: no
 * such file; -ENODEV: no card).
 */
int sdcard_read(const char *path, bool (*cb)(const char *data, size_t len, void *arg),
		void *arg);

/* Called (once) before "sdcard unmount", e.g. to write out buffered data. */
void sdcard_set_unmount_hook(void (*hook)(void));

#endif /* SENSOR_BOX_SDCARD_H_ */
