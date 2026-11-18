/*
 * microSD card in the TFT FeatherWing's slot. See sdcard.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * The card shares the SPI bus with the display (src/ui.c); the SD driver
 * locks the bus for each command, so the two take turns. It's FAT
 * formatted (FAT32) and mounted at SDCARD_MNT at boot, if present.
 *
 * A card that won't mount (no card, or a filesystem other than FAT, e.g. a
 * new >32 GB card's exFAT) is left alone: it's only ever formatted by
 * "sdcard format yes" on the console.
 *
 * The card gets its first reset at boot, before the display's first SPI
 * traffic (sdcard_early_init). A card that sees other traffic on the bus
 * first can stay in SD (not SPI) mode until it loses power, and nothing
 * here can power-cycle it: so insert or swap cards with the box off (USB
 * and battery unplugged), then power up.
 *
 * Console: "sdcard info", "sdcard mount", "sdcard unmount",
 * "sdcard format yes", "sdcard test". Files: "fs ls /SD:" etc.
 */

#include <stdio.h>
#include <string.h>

#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/disk_access.h>

#include "sdcard.h"
#include "timesync.h"

LOG_MODULE_REGISTER(sdcard, LOG_LEVEL_INF);

#define DISK_NAME "SD"

static K_MUTEX_DEFINE(lock);
static FATFS fat;
static struct fs_mount_t mnt = {
	.type = FS_FATFS,
	.mnt_point = SDCARD_MNT,
	.fs_data = &fat,
	/* Never format on a failed mount (see CONFIG_FS_FATFS_MOUNT_MKFS). */
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};
static bool mounted;
static void (*unmount_hook)(void);

/*
 * Wake the card the way the SD spec asks: at least 74 clocks at <= 400 kHz
 * with its chip-select high (deselected). Zephyr's SD driver sends them
 * with chip-select low, which some cards don't accept. Under `lock`.
 */
static void wake_card(void)
{
	const struct device *spi = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(sdhc0)));
	/* No chip-select: every CS line on the bus stays high. */
	const struct spi_config cfg = {
		.frequency = 400000,
		.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8),
		.slave = 7,	/* no hardware CS pad either */
	};
	uint8_t ones[10];

	memset(ones, 0xff, sizeof(ones));

	const struct spi_buf buf = { .buf = ones, .len = sizeof(ones) };
	const struct spi_buf_set tx = { .buffers = &buf, .count = 1 };

	if (!device_is_ready(spi) || spi_write(spi, &cfg, &tx) != 0) {
		LOG_WRN("couldn't send the card its wake-up clocks");
	}
}

/* Mount the card. Under `lock`. */
static int do_mount(void)
{
	if (mounted) {
		return 0;
	}

	wake_card();

	int ret = fs_mount(&mnt);

	if (ret == 0) {
		mounted = true;
	}
	return ret;
}

static int do_unmount(void)
{
	if (!mounted) {
		return 0;
	}

	int ret = fs_unmount(&mnt);

	if (ret == 0) {
		mounted = false;
	}
	return ret;
}

/*
 * Before the display starts using the shared SPI bus (its init priority is
 * 85): give the card its wake-up clocks and its first reset, which puts it
 * in SPI mode. A card that first sees other traffic may not answer until
 * it's power-cycled. Mounting comes later, from sdcard_init().
 */
static int sdcard_early_init(void)
{
	wake_card();
	int ret = disk_access_init(DISK_NAME);

	if (ret != 0) {
		LOG_INF("no card at boot (%d)", ret);
	}
	return 0;
}

SYS_INIT(sdcard_early_init, POST_KERNEL, 83);

void sdcard_init(void)
{
	k_mutex_lock(&lock, K_FOREVER);

	int ret = do_mount();

	k_mutex_unlock(&lock);
	if (ret == 0) {
		LOG_INF("card mounted at %s", SDCARD_MNT);
	} else {
		/* -EIO etc.: no card; -EINVAL / -ENODEV: not FAT (e.g. exFAT). */
		LOG_WRN("no usable card (%d): insert a FAT card, or format with "
			"\"sdcard format yes\" (erases it)", ret);
	}
}

bool sdcard_ready(void)
{
	return mounted;
}

void sdcard_set_unmount_hook(void (*hook)(void))
{
	unmount_hook = hook;
}

int sdcard_append(const char *dir, const char *path, const char *header, const char *data,
		  size_t len)
{
	struct fs_dirent st;
	struct fs_file_t f;
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	if (!mounted) {
		k_mutex_unlock(&lock);
		return -ENODEV;
	}

	if (fs_stat(dir, &st) == -ENOENT) {
		ret = fs_mkdir(dir);
		if (ret != 0 && ret != -EEXIST) {
			goto out;
		}
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (ret != 0) {
		goto out;
	}

	ssize_t n = 0;

	/* An empty file (new, or created just before a crash) gets the header. */
	if (header != NULL && fs_seek(&f, 0, FS_SEEK_END) == 0 && fs_tell(&f) == 0) {
		n = fs_write(&f, header, strlen(header));
	}
	if (n >= 0) {
		n = fs_write(&f, data, len);
		if (n >= 0 && (size_t)n != len) {
			n = -ENOSPC;	/* card full */
		}
	}
	ret = fs_close(&f);
	if (n < 0) {
		ret = (int)n;
	}
out:
	k_mutex_unlock(&lock);
	return ret;
}

/* ---- console ---- */

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t sectors = 0;
	uint32_t sector_size = 0;

	k_mutex_lock(&lock, K_FOREVER);
	/* Initialises the card if nothing has yet (harmless if it has). */
	wake_card();
	int ret = disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);

	if (ret == 0) {
		disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
		disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);
	}

	struct fs_statvfs st;
	int sret = mounted ? fs_statvfs(SDCARD_MNT, &st) : -ENOENT;

	k_mutex_unlock(&lock);

	if (ret != 0) {
		shell_error(sh, "no card (%d)", ret);
		return ret;
	}
	shell_print(sh, "card: %llu MB (%u sectors of %u bytes)",
		    (unsigned long long)sectors * sector_size / 1000000, sectors, sector_size);
	if (sret == 0) {
		uint64_t total = (uint64_t)st.f_frsize * st.f_blocks;
		uint64_t free = (uint64_t)st.f_frsize * st.f_bfree;

		shell_print(sh, "mounted at %s: %llu MB free of %llu MB, %lu-byte clusters",
			    SDCARD_MNT, free / 1000000, total / 1000000,
			    (unsigned long)st.f_frsize);
	} else {
		shell_print(sh, "not mounted (no FAT filesystem? see \"sdcard format\")");
	}
	return 0;
}

static int cmd_mount(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lock, K_FOREVER);
	int ret = do_mount();

	k_mutex_unlock(&lock);
	if (ret != 0) {
		shell_error(sh, "mount failed (%d)", ret);
		return ret;
	}
	shell_print(sh, "mounted at %s", SDCARD_MNT);
	return 0;
}

static int cmd_unmount(const struct shell *sh, size_t argc, char **argv)
{
	if (unmount_hook != NULL) {
		unmount_hook();
	}
	k_mutex_lock(&lock, K_FOREVER);
	int ret = do_unmount();

	k_mutex_unlock(&lock);
	if (ret != 0) {
		shell_error(sh, "unmount failed (%d)", ret);
		return ret;
	}
	shell_print(sh, "unmounted: safe to remove the card");
	return 0;
}

static int cmd_format(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "yes") != 0) {
		shell_error(sh, "this erases the whole card: \"sdcard format yes\" to go ahead");
		return -EINVAL;
	}

	/* FAT32 in an MBR partition, two FAT copies, 32 KB clusters. */
	MKFS_PARM opt = {
		.fmt = FM_FAT32,
		.n_fat = 2,
		.au_size = 32768,
	};

	shell_print(sh, "formatting (FAT32); this can take a minute...");
	k_mutex_lock(&lock, K_FOREVER);
	int64_t start = k_uptime_get();
	int ret = do_unmount();

	if (ret == 0) {
		ret = fs_mkfs(FS_FATFS, (uintptr_t)DISK_NAME ":", &opt, 0);
	}
	if (ret == 0) {
		ret = do_mount();
	}
	k_mutex_unlock(&lock);

	if (ret != 0) {
		shell_error(sh, "format failed (%d)", ret);
		return ret;
	}
	shell_print(sh, "formatted and mounted in %lld s", (k_uptime_get() - start) / 1000);
	return 0;
}

static int cmd_test(const struct shell *sh, size_t argc, char **argv)
{
	static const char path[] = SDCARD_MNT "/TEST.TXT";
	char line[80];
	char back[80] = "";
	struct fs_file_t f;
	int ret;

	snprintf(line, sizeof(line), "sensor-box test: uptime %lld s, UTC %lld\n",
		 k_uptime_get() / 1000, timesync_valid() ? (long long)timesync_now() : 0LL);

	k_mutex_lock(&lock, K_FOREVER);
	if (!mounted) {
		k_mutex_unlock(&lock);
		shell_error(sh, "no card mounted");
		return -ENODEV;
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret == 0) {
		ssize_t n = fs_write(&f, line, strlen(line));

		ret = n < 0 ? (int)n : 0;
		fs_close(&f);
	}
	if (ret == 0) {
		fs_file_t_init(&f);
		ret = fs_open(&f, path, FS_O_READ);
		if (ret == 0) {
			ssize_t n = fs_read(&f, back, sizeof(back) - 1);

			ret = n < 0 ? (int)n : 0;
			fs_close(&f);
		}
	}
	k_mutex_unlock(&lock);

	if (ret != 0) {
		shell_error(sh, "test failed (%d)", ret);
		return ret;
	}
	shell_print(sh, "wrote and read back %s:\n%s", path, back);
	return strcmp(line, back) == 0 ? 0 : -EIO;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sdcard_cmds,
	SHELL_CMD_ARG(info, NULL, "Card size and free space", cmd_info, 1, 0),
	SHELL_CMD_ARG(mount, NULL, "Mount the card (e.g. after inserting it)", cmd_mount, 1, 0),
	SHELL_CMD_ARG(unmount, NULL, "Unmount, so the card can be removed", cmd_unmount, 1, 0),
	SHELL_CMD_ARG(format, NULL, "Erase and format as FAT32: format yes", cmd_format, 1, 1),
	SHELL_CMD_ARG(test, NULL, "Write and read back " SDCARD_MNT "/TEST.TXT", cmd_test, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sdcard, &sdcard_cmds, "microSD card", NULL);
