/*
 * Wall-clock time from SNTP (UTC).
 *
 * SPDX-License-Identifier: MIT
 *
 * The box has no RTC, so the clock is unknown until the first sync. A sync
 * runs as soon as the network gets an IPv4 address, then every RESYNC; a
 * failed attempt retries after RETRY. The result sets Zephyr's
 * SYS_CLOCK_REALTIME, so `date get` on the shell shows it.
 *
 * Syncs run on their own work queue: an SNTP query can block for seconds,
 * which would stall the system workqueue (the SGP30's 1 Hz measurement and
 * the backlight fades run there).
 *
 * Local time for the on-screen clock is a fixed offset plus, optionally,
 * the US daylight-saving rule, computed here rather than relying on the C
 * library's TZ support.
 */

#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/sntp.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>

#include "timesync.h"

LOG_MODULE_REGISTER(timesync, LOG_LEVEL_INF);

#define QUERY_TIMEOUT_MS 5000
#define RESYNC           K_HOURS(6)
#define RETRY            K_SECONDS(60)
#define WQ_STACK_SIZE    3072
#define WQ_PRIORITY      K_LOWEST_APPLICATION_THREAD_PRIO

static K_THREAD_STACK_DEFINE(wq_stack, WQ_STACK_SIZE);
static struct k_work_q wq;
static struct net_mgmt_event_callback ipv4_cb;
static atomic_t valid;

static void sync(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sync_work, sync);

static void sync(struct k_work *work)
{
	ARG_UNUSED(work);

	struct sntp_time ts;
	int ret = sntp_simple(CONFIG_SENSOR_BOX_NTP_SERVER, QUERY_TIMEOUT_MS, &ts);

	if (ret != 0) {
		LOG_WRN("SNTP query to %s failed (%d); retrying", CONFIG_SENSOR_BOX_NTP_SERVER,
			ret);
		k_work_reschedule_for_queue(&wq, &sync_work, RETRY);
		return;
	}

	/* fraction is a 32-bit binary fraction of a second. */
	struct timespec tp = {
		.tv_sec = (time_t)ts.seconds,
		.tv_nsec = (long)(((uint64_t)ts.fraction * 1000000000ULL) >> 32),
	};

	ret = sys_clock_settime(SYS_CLOCK_REALTIME, &tp);
	if (ret != 0) {
		LOG_ERR("setting the clock failed (%d)", ret);
		k_work_reschedule_for_queue(&wq, &sync_work, RETRY);
		return;
	}

	struct tm tm;
	char buf[32];

	gmtime_r(&tp.tv_sec, &tm);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	LOG_INF("clock set from %s: %s UTC", CONFIG_SENSOR_BOX_NTP_SERVER, buf);

	atomic_set(&valid, 1);
	k_work_reschedule_for_queue(&wq, &sync_work, RESYNC);
}

static void on_ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event,
			  struct net_if *iface)
{
	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		/* Give DHCP's DNS server setting a moment to land. */
		k_work_reschedule_for_queue(&wq, &sync_work, K_SECONDS(1));
	}
}

void timesync_init(void)
{
	k_work_queue_init(&wq);
	k_work_queue_start(&wq, wq_stack, K_THREAD_STACK_SIZEOF(wq_stack), WQ_PRIORITY,
			   &(struct k_work_queue_config){ .name = "timesync" });

	net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);
}

bool timesync_valid(void)
{
	return atomic_get(&valid) != 0;
}

time_t timesync_now(void)
{
	struct timespec tp;

	sys_clock_gettime(SYS_CLOCK_REALTIME, &tp);
	return tp.tv_sec;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm). */
static int64_t days_from_civil(int64_t y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + doe - 719468;
}

/* Day number (as days_from_civil) of the n-th Sunday of a month. */
static int64_t nth_sunday(int64_t y, int m, int n)
{
	int64_t first = days_from_civil(y, m, 1);
	int64_t weekday = ((first % 7) + 7 + 4) % 7;	/* 1970-01-01 was a Thursday; 0 = Sun */

	return first + (7 - weekday) % 7 + 7 * (n - 1);
}

/* Is US daylight saving time in effect at this UTC instant? */
static bool us_dst(time_t utc)
{
	const int64_t std = (int64_t)CONFIG_SENSOR_BOX_UTC_OFFSET_MIN * 60;
	struct tm tm;
	time_t local_std = utc + std;

	gmtime_r(&local_std, &tm);

	int64_t year = tm.tm_year + 1900;
	/* Starts 2:00 local standard time; ends 2:00 local daylight time. */
	int64_t start = nth_sunday(year, 3, 2) * 86400 + 2 * 3600 - std;
	int64_t end = nth_sunday(year, 11, 1) * 86400 + 2 * 3600 - (std + 3600);

	return utc >= start && utc < end;
}

bool timesync_local(struct tm *tm)
{
	if (!timesync_valid()) {
		return false;
	}

	time_t utc = timesync_now();
	time_t local = utc + (time_t)CONFIG_SENSOR_BOX_UTC_OFFSET_MIN * 60;

	if (IS_ENABLED(CONFIG_SENSOR_BOX_US_DST) && us_dst(utc)) {
		local += 3600;
	}
	gmtime_r(&local, tm);
	return true;
}
