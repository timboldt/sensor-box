# sensor-box

Firmware for a small environmental monitor built on the Adafruit ESP32-C6 Feather.
It reads a handful of I2C environmental sensors, shows live values and recent
history on a TFT FeatherWing, and logs timestamped CSV to an SD card. A LAN web
dashboard is planned for a later phase.

**Status:** early work in progress. See `.claude/plans/` for the build plan.

## Hardware

- Adafruit ESP32-C6 Feather (STEMMA QT)
- Adafruit 2.4" 320x240 TFT FeatherWing (ILI9341) — display + microSD slot
- Adafruit DS3231 Precision RTC FeatherWing
- I2C sensors: PMSA003I (PM), BME280 (temp/humidity/pressure), TSL2591 (light),
  SGP30 (eCO2/TVOC)

## Toolchain

Stable Rust (no nightly, no `build-std`); target `riscv32imac-unknown-none-elf`,
installed automatically via `rust-toolchain.toml`. Flashing uses
[`espflash`](https://github.com/esp-rs/espflash):

```sh
cargo install espflash
```

## Build & run

```sh
cargo build
cargo run        # build, flash over USB, and open the serial monitor
```

## License

MIT — see [LICENSE](LICENSE).
