# sensor-box

Firmware for a small environmental monitor built on the Adafruit ESP32-C6 Feather.
It reads a handful of I2C environmental sensors and shows live values and recent
history on a TFT FeatherWing. A LAN web dashboard is planned for a later phase.

Built on [Zephyr RTOS](https://zephyrproject.org/).

**Status:** early work in progress. All sensors are read, logged to the
console and shown live on the TFT with a 6-hour trend line per reading.
It joins Wi-Fi, sets its clock from SNTP and serves a web dashboard on the
local network.

## Using it

- The display shows a dashboard of current readings, each with its change
  over the window that matters most for it: 3 hours for pressure (the
  forecasters' tendency: beyond ~1.6 hPa, 0.05 inHg, it's rising or
  falling), 1 hour for temperature and humidity, 15 minutes for particles,
  CO2 and TVOC. A change is grey while steady, otherwise white, or for the
  pollutants green when falling and orange when rising. The status bar has
  the light level and, once the clock has been set over the network, the
  local time. Charts are on the web page.
- **Boot button:** wakes the screen when it's dark. Don't hold Boot while
  resetting or plugging in (that enters the ESP32-C6 download mode).
- **Warm-up:** the particle readings show "warming up" for the first 30 s after power-up
  (fan spin-up), TVOC for the SGP30's first ~15 s, and CO2 until the SCD-43's
  first measurement (~5 s).
- **CO2** comes from an SCD-43 (true CO2, 400–5000 ppm), corrected with the
  BME280's pressure. Without one the CO2 tile says "no sensor". The SGP30's
  eCO2 is only an estimate from TVOC, so it's no longer shown (it's still in
  the console log).
- **SGP30 baseline:** the TVOC sensor learns a clean-air baseline over
  ~12 h. The firmware saves it to flash (first after 12 h of uptime, then
  hourly) and restores it at boot, so a reboot doesn't restart the learning.
  Reflashing keeps it. Each save is timestamped, and at boot the baseline is
  restored once the clock is set, unless it's over 7 days old (Sensirion's
  limit); with no network for 2 minutes it's restored without the check.
  `sgp30 baseline [save|clear]` on the console shows, saves or clears it.
- **Long-term history:** every reading is averaged per clock hour, and the
  peak PM2.5, CO2 and TVOC of each hour are kept too, in flash (128 KB,
  about 4 months, oldest dropped first), kept across reboots and reflashes.
  `ltlog show [n]` lists the newest records (UTC), `ltlog clear` erases them.
- **Particles:** besides PM2.5, the count of particles larger than 0.3 µm
  per litre is shown (on the screen in place of PM2.5, and as a tile and
  log-scale chart on the web page). In clean indoor air PM2.5 sits near 0 while this count still
  reacts to cooking, candles and dust. The web PM2.5 tile also
  notes PM1 and PM10, which indoors track PM2.5 closely.
- **microSD card** (in the TFT FeatherWing's slot, FAT32): mounted at boot
  at `/SD:`. Every minute's readings go to one CSV file per UTC day,
  `LOG/YYYYMMDD.CSV`: the time (start of the minute, UTC), each reading's
  average over the minute and the pollutants' peaks; empty = no reading.
  Lines are written every 5 minutes, so a crash or reboot loses at most
  the last few; logging pauses while the battery is nearly flat. At boot,
  once the clock is set, the last 24 hours are read back from the files,
  so the charts (screen and web) carry on across reboots and reflashes. `sdlog`
  shows the state, `sdlog flush` writes now (`sdcard unmount` does too:
  use it before pulling the card while running). Insert or swap cards with the
  box **fully off** (USB and battery unplugged): a card that first sees
  the display's traffic on the shared SPI bus may not answer until it's
  power-cycled. Console: `sdcard info`, `sdcard mount` / `unmount`,
  `sdcard format yes` (erases the card; the firmware never formats on its
  own), `sdcard test`, and `fs ls /SD:` etc. Cards over 32 GB come as
  exFAT: format them first, here or on a computer (macOS: Disk Utility,
  "MS-DOS (FAT)").
- **Battery current:** an INA219 in the battery lead measures the current
  into (charging, positive) or out of (on battery, negative) the LiPo. It's
  on the web page (tile and chart) and in the hourly log.
- **Humidity** is shown as absolute humidity (g/m³ of water vapour), which
  follows the air's actual moisture; relative humidity swings with
  temperature. %RH is still logged, and noted on the web page's tile.
- **Light level** is shown in lux (the screen's status bar; a log-scale
  chart on the web page). The TSL2591's gain is adjusted automatically (x1
  to x9200), so it reads from daylight down to about 0.0002 lux. The hourly
  log stores it in decibels relative to 1 lux (0 dB = 1 lux, -20 dB =
  0.01 lux).
- **Backlight:** dims with the room light, from full brightness at 15 lux
  and up to 1% at 0.02 lux, and switches off below 0.01 lux. In a dark room a
  press of Boot turns it on, dimly, for 30 s.

## Hardware

- Adafruit ESP32-C6 Feather (STEMMA QT), with on-board MAX17048 battery gauge
- Adafruit INA219 current sensor, inline in the battery lead (optional)
- microSD card in the TFT FeatherWing's slot, FAT32 (optional)
- Adafruit 2.4" 320x240 TFT FeatherWing (ILI9341), on SPI. Its touch
  controller and microSD slot are unused, but share the SPI bus, so their
  chip-selects must be held inactive.
  - **Hand-wired mod:** the FeatherWing's LITE pad is soldered to Feather A5
    (GPIO2), so the firmware can switch the backlight off when the room is
    dark. Without that wire the backlight is always on.
- I2C sensors, daisy-chained over STEMMA QT: PMSA003I (PM), BME280
  (temp/humidity/pressure), TSL2591 (light), SGP30 (TVOC), SCD-43 (CO2)

No RTC: wall-clock time, when needed, will come from SNTP over Wi-Fi.

## Layout

| Path | Contents |
|------|----------|
| `src/main.c` | Application: sensor loop |
| `src/ui.c` | TFT dashboard (LVGL) |
| `src/history.c` | 24-hour history (1-minute averages) and 15-minute history (10 s) for the trend lines and charts |
| `src/baseline.c` | Saves/restores the SGP30 baseline in flash |
| `src/wifi.c` | Joins the Wi-Fi network stored on the device |
| `src/timesync.c` | Sets the clock (UTC) from SNTP once online, then every 6 h |
| `src/ltlog.c` | Hourly long-term history in flash (FCB) |
| `src/web.c`, `web/` | Web dashboard: HTTP server, JSON API and the page |
| `src/light.c` | TSL2591 auto-gain and lux calculation |
| `src/backlight.c` | Backlight brightness from ambient light |
| `app.overlay` | Devicetree for the I2C sensors and the SPI display |
| `prj.conf`, `Kconfig` | Application Kconfig and its app-specific options |
| `boards/adafruit/feather_esp32c6/` | Board definition (also holds the FeatherWing pin wiring) |
| `drivers/sensor/` | Out-of-tree drivers for PMSA003I and SGP30 |
| `dts/bindings/` | Devicetree bindings for those drivers |
| `west.yml` | West manifest (pins the Zephyr version) |

## Web dashboard

Once on Wi-Fi, open **http://sensor-box.local** (or the IP address the box
logs at boot) on any device on the same network. It shows the current
readings and trend charts for the last 10 minutes, hour, 4 hours, day,
week, month or 3 months, in your browser's time zone. Click a chart to zoom
in to the next shorter range around that moment; ← and → step through time
and Now returns to the present. There is no login: anyone
on the network can view it, and nothing can be changed from it.

The page is `web/index.html`, built into the firmware. The JSON behind it
can also be used directly (values are metric; missing values are `null`):

| Endpoint | Contents |
|----------|----------|
| `/api/now` | Current readings |
| `/api/recent` | Last 24 hours, 1-minute averages, oldest first (`?points=N` for the last N; `&group=G` averages every G into one point; `?window=short`: last 15 minutes, 10 s); includes `covered`, how many seconds back the real data goes (the uptime, or more if restored from the SD card; older points are a placeholder), and `age`, how long ago the newest point ended |
| `/api/history?since=<unix time>` | Hourly averages and peaks from flash, plus the hour so far (`current`) |
| `/api/minutes?since=<unix time>&until=<unix time>` | The SD card's minute log for that span (at most 2 days), as CSV straight from the card, each day's file after its own header row; empty without a card. `&step=S` (S dividing 1440) sends S-minute averages of the charted columns instead |

## Wi-Fi

The Wi-Fi network is stored on the device, not in this repo or the build.
Add it once from a serial terminal (e.g. `screen /dev/cu.usbmodem1101 115200`;
quit with Ctrl-A K):

```
wifi cred add -s "YourSSID" -k 1 -p "YourPassword"    # -k 1 = WPA2; -k 3 = WPA3-only
wifi cred auto_connect                                 # or just reset
```

The box then joins it at every boot and logs its IP address; a Wi-Fi icon
shows in the status bar while connected. The ESP32-C6 is 2.4 GHz only. The
credentials are kept in the flash `storage` partition, so reflashing keeps
them. `wifi cred list` / `wifi cred delete` manage them, and `wifi status`
shows the connection. Once connected, the clock is set from SNTP (UTC); there
is no RTC, so until then the time is unknown. `date get` shows it.

## Configuration

Site-specific settings live in `prj.conf` (defined in `Kconfig`):

| Option | Meaning |
|--------|---------|
| `CONFIG_SENSOR_BOX_ALTITUDE_M` | Altitude of the box in metres, for sea-level pressure |
| `CONFIG_SENSOR_BOX_US_UNITS` | Show °F and inHg on the display (logs stay metric) |
| `CONFIG_SENSOR_BOX_NTP_SERVER` | SNTP server (default `pool.ntp.org`) |
| `CONFIG_SENSOR_BOX_UTC_OFFSET_MIN` | Standard-time UTC offset for the on-screen clock (Pacific: -480) |
| `CONFIG_SENSOR_BOX_US_DST` | Apply US daylight saving time to the on-screen clock |

## Setup

This repo is the west manifest repo. Clone it into an empty workspace
directory, then let west fetch Zephyr and its modules next to it:

```sh
mkdir zephyr-ws && cd zephyr-ws
git clone <this repo> sensor-box
cd sensor-box

# Python env (uv-managed Python, so OS/Homebrew upgrades can't break it).
uv venv --managed-python --python 3.13 .venv
source .venv/bin/activate
uv pip install west

west init -l .
west update
west blobs fetch hal_espressif

# Zephyr's requirements plus each module's (esptool, imgtool, ...).
uv pip install $(west packages pip)
```

You also need the Zephyr SDK; see Zephyr's
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

## Build & flash

From `sensor-box/`:

```sh
west build -b adafruit_feather_esp32c6/esp32c6/hpcore
west flash
west espressif monitor   # serial console + Zephyr shell
```

Helpers that use `.venv` without activating it and find the board's port
(`/dev/cu.usbmodem*`, or `ESP_PORT`) on their own:

```sh
scripts/flash.sh                                     # west flash
.venv/bin/python scripts/console.py "kernel uptime"   # run shell commands
.venv/bin/python scripts/console.py --watch 30        # watch the log
```

`console.py` leaves the board running; some ways of opening the ESP32-C6's
USB serial port reset it.

## Wishlist

Ideas and loose ends (new sensors, smaller improvements) are kept in
[WISHLIST.md](WISHLIST.md).

## License

MIT — see [LICENSE](LICENSE).
