/*
 * Wi-Fi station: connect to the network stored on the device.
 *
 * SPDX-License-Identifier: MIT
 *
 * Credentials are never in the source or the build. They are stored on the
 * device (Zephyr wifi_credentials, in the settings/NVS "storage" partition,
 * which reflashing does not touch), added once from the serial console:
 *
 *   wifi cred add -s "SSID" -k 1 -p "password"     (-k 1 = WPA2-PSK)
 *   wifi cred auto_connect                         (or just reset)
 *
 * At boot this connects to the stored network; the ESP32 driver then runs
 * DHCP. After a failed attempt or a disconnect it retries every RETRY.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>

#include "wifi.h"

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

#define FIRST_CONNECT K_SECONDS(2)
#define RETRY         K_SECONDS(60)

#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static atomic_t connected;

static void connect(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(connect_work, connect);

static void connect(struct k_work *work)
{
	ARG_UNUSED(work);

	struct net_if *iface = net_if_get_wifi_sta();

	if (iface == NULL) {
		LOG_ERR("no Wi-Fi interface");
		return;
	}
	if (wifi_credentials_is_empty()) {
		LOG_INF("no network stored; add one on the console with "
			"'wifi cred add -s \"SSID\" -k 1 -p \"password\"', then reset");
		return;
	}

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0);

	if (ret != 0) {
		LOG_WRN("connect request failed (%d); retrying", ret);
		k_work_reschedule(&connect_work, RETRY);
	}
}

static void on_wifi_event(struct net_mgmt_event_callback *cb, uint64_t event,
			  struct net_if *iface)
{
	const struct wifi_status *status = cb->info;

	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		if (status != NULL && status->status != 0) {
			LOG_WRN("connect failed (%d); retrying", status->status);
			k_work_reschedule(&connect_work, RETRY);
		} else if (!atomic_get(&connected)) {
			/* The address event can arrive first; only log if it hasn't. */
			LOG_INF("associated; waiting for DHCP");
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		atomic_set(&connected, 0);
		LOG_WRN("disconnected; retrying");
		k_work_reschedule(&connect_work, RETRY);
	}
}

static void on_ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event,
			  struct net_if *iface)
{
	if (event != NET_EVENT_IPV4_ADDR_ADD || cb->info == NULL) {
		return;
	}

	char addr[NET_IPV4_ADDR_LEN];

	net_addr_ntop(AF_INET, cb->info, addr, sizeof(addr));
	atomic_set(&connected, 1);
	LOG_INF("connected, IP %s", addr);
}

void wifi_init(void)
{
	net_mgmt_init_event_callback(&wifi_cb, on_wifi_event, WIFI_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	k_work_reschedule(&connect_work, FIRST_CONNECT);
}

bool wifi_is_connected(void)
{
	return atomic_get(&connected) != 0;
}
