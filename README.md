# sensor-box

Firmware for a small environmental monitor built on the Adafruit ESP32-C6 Feather.
It reads a handful of I2C environmental sensors, shows live values and recent
history on a TFT FeatherWing, and logs timestamped CSV to an SD card. A LAN web
dashboard is planned for a later phase.

Built on [Zephyr RTOS](https://zephyrproject.org/).

**Status:** early work in progress. All sensors are read and logged to the
console; the display, SD logging and RTC are not yet implemented.

## Hardware

- Adafruit ESP32-C6 Feather (STEMMA QT), with on-board MAX17048 battery gauge
- Adafruit 2.4" 320x240 TFT FeatherWing (ILI9341) — display + microSD slot
- Adafruit DS3231 Precision RTC FeatherWing
- I2C sensors: PMSA003I (PM), BME280 (temp/humidity/pressure), TSL2591 (light),
  SGP30 (eCO2/TVOC)

## Layout

| Path | Contents |
|------|----------|
| `src/main.c` | Application |
| `app.overlay` | Devicetree for the devices on the I2C bus |
| `prj.conf` | Application Kconfig |
| `boards/adafruit/feather_esp32c6/` | Board definition (also holds the FeatherWing pin wiring) |
| `drivers/sensor/` | Out-of-tree drivers for PMSA003I and SGP30 |
| `dts/bindings/` | Devicetree bindings for those drivers |
| `west.yml` | West manifest (pins the Zephyr version) |

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

## License

MIT — see [LICENSE](LICENSE).
