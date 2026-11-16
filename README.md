# sensor-box

Firmware for a small environmental monitor built on the Adafruit ESP32-C6 Feather.
It reads a handful of I2C environmental sensors and shows live values and recent
history on a TFT FeatherWing. A LAN web dashboard is planned for a later phase.

Built on [Zephyr RTOS](https://zephyrproject.org/).

**Status:** early work in progress. All sensors are read, logged to the
console and shown live on the TFT with a 6-hour trend line per reading.
It joins Wi-Fi and sets its clock (UTC) from SNTP; the web dashboard is not
yet implemented.

## Using it

- The display shows a dashboard of current readings with 6-hour trends.
- **Boot button:** press to page through a full-screen chart for each
  reading, plus light level (lux, on a log scale); after the last it returns
  to the dashboard, and it also returns on its own after a minute. Don't
  hold Boot while resetting or plugging in (that enters the ESP32-C6
  download mode).
- **Warm-up:** PM2.5 shows "warming up" for the first 30 s after power-up
  (fan spin-up), and eCO2/TVOC for the SGP30's first ~15 s.
- **SGP30 baseline:** the eCO2/TVOC sensor learns a clean-air baseline over
  ~12 h. The firmware saves it to flash (first after 12 h of uptime, then
  hourly) and restores it at boot, so a reboot doesn't restart the learning.
  Reflashing keeps it. `sgp30 baseline [save|clear]` on the console shows,
  saves or clears it. There's no clock, so a baseline is restored however
  long the box was off (Sensirion suggests discarding ones over 7 days old).
- **Dark room:** the backlight switches off below 1 lux. A press of Boot
  turns it on for 30 s (that first press only wakes the screen).

## Hardware

- Adafruit ESP32-C6 Feather (STEMMA QT), with on-board MAX17048 battery gauge
- Adafruit 2.4" 320x240 TFT FeatherWing (ILI9341), on SPI. Its touch
  controller and microSD slot are unused, but share the SPI bus, so their
  chip-selects must be held inactive.
  - **Hand-wired mod:** the FeatherWing's LITE pad is soldered to Feather A5
    (GPIO2), so the firmware can switch the backlight off when the room is
    dark. Without that wire the backlight is always on.
- I2C sensors, daisy-chained over STEMMA QT: PMSA003I (PM), BME280
  (temp/humidity/pressure), TSL2591 (light), SGP30 (eCO2/TVOC)

No RTC: wall-clock time, when needed, will come from SNTP over Wi-Fi.

## Layout

| Path | Contents |
|------|----------|
| `src/main.c` | Application: sensor loop |
| `src/ui.c` | TFT dashboard (LVGL) |
| `src/history.c` | 6-hour history (3-minute averages) for the trend lines |
| `src/baseline.c` | Saves/restores the SGP30 baseline in flash |
| `src/wifi.c` | Joins the Wi-Fi network stored on the device |
| `src/timesync.c` | Sets the clock (UTC) from SNTP once online, then every 6 h |
| `src/backlight.c` | Backlight on/off from ambient light |
| `app.overlay` | Devicetree for the I2C sensors and the SPI display |
| `prj.conf`, `Kconfig` | Application Kconfig and its app-specific options |
| `boards/adafruit/feather_esp32c6/` | Board definition (also holds the FeatherWing pin wiring) |
| `drivers/sensor/` | Out-of-tree drivers for PMSA003I and SGP30 |
| `dts/bindings/` | Devicetree bindings for those drivers |
| `west.yml` | West manifest (pins the Zephyr version) |

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

## License

MIT — see [LICENSE](LICENSE).
