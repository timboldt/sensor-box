# Wishlist

Ideas and loose ends for sensor-box, roughly in order of interest. Nothing
here is promised; it's a place to keep them.

## Next up

- **SCD-43 CO2 sensor** (ordered, arriving ~2026-11-23). The firmware is
  ready for it (declared as an SCD41 at 0x62, fed the BME280's pressure).
  When it's plugged in:
  - check Zephyr's scd4x driver works with it (it's untested on the SCD43);
  - see whether the SGP30's "measure command failed: -14" boot warning goes
    away (it appeared once the SCD4x node was added, while the sensor is
    absent);
  - automatic self-calibration (ASC) is now turned off at boot (the factory
    calibration read spot on outdoors). If it drifts, add a console command
    for a forced recalibration by an open window;
  - watch for brownouts: its measurement peaks (~150-200 mA) share the 3.3 V
    rail with the PMSA003I fan and Wi-Fi bursts.

- **SD card logging, next steps** (minute CSV files are written to
  `/SD:/LOG/`, and the last 24 hours reload at boot): the web page should
  offer arbitrary intervals at sensible detail (1-minute from the files
  for recent days, coarser further back, "All" downsampled rather than
  everything), and a way to download a day's CSV.

## New hardware on hand

- **INA219, next steps** (it's fitted and charted on the web page): an
  "about N h left" battery estimate; measure what the backlight, Wi-Fi and
  sensors each cost; maybe show it on the device's status line.
- **AS7341 spectral sensor** (address 0x39, alongside the TSL2591, not
  instead of it: the TSL2591 stays for lux and the dark-room backlight).
  8 visible channels plus clear, near-IR and flicker detection. Ideas:
  colour temperature (warm evening lamps vs daylight), evening blue light
  (fits the sleep / CO2 tracking), a guess at the light source (daylight,
  LED, incandescent), 100/120 Hz LED flicker, and live spectrum bars for
  fun. Needs an out-of-tree driver (no Zephyr one); the fiddly part is its
  SMUX channel mapping, which takes two passes per full reading.
- **TCA4307 I2C buffer** with stuck-bus recovery and hot-swap. Not needed
  today (short bus, fine at 100 kHz). Use it if the bus misbehaves: put
  the power-hungry sensors (PMSA003I, SCD-43) on its far side so a
  brownout can't freeze the whole bus, or for a sensor on a long cable.
  It's a buffer, not a multiplexer: addresses must still be unique.

## Smaller improvements

- Enable IPv6 so mDNS answers AAAA queries: `sensor-box.local` currently
  takes ~5 s to resolve in some clients (by IP it's instant).
- Faster web Day view: the 1440-point `/api/recent` reply is ~49 KB and
  takes ~1.2 s; larger chunks or fetching only new points would help.

## Parked

- **MB85RC256V FRAM** (32 KB, I2C at 0x50; Zephyr driver
  `fujitsu,mb85rcxx`): superseded by SD card logging, which covers keeping
  the history across reboots and much more. Only worth it if losing the
  last few minutes ever matters. Its board has Dupont-style pins: connect
  it with a STEMMA QT to female-socket cable (Adafruit 4397; red VCC, black
  GND, blue SDA, yellow SCL; leave WP and A0-A2 unconnected).
- Faster backlight reaction to room light changes.
- Full-brightness flash at boot.
