//! Sensirion SGP30 VOC / equivalent-CO₂ gas sensor (I2C, 0x58).
//!
//! Two quirks drive the design here:
//! - it must be measured at **1 Hz** for its baseline algorithm to converge, so
//!   [`Gas::sample`] is called every tick, separately from the slower main cycle;
//! - it reports 400 ppm eCO₂ / 0 ppb TVOC for the first ~15 s after init.
//!
//! Baseline persistence across reboots is a later step.

use embassy_time::Delay;
use sgp30::{Humidity, Sgp30};

use crate::board::I2cBusDevice;
use crate::sensors::Reading;

pub struct Gas {
    dev: Option<Sgp30<I2cBusDevice, Delay>>,
    eco2_ppm: Option<u16>,
    tvoc_ppb: Option<u16>,
}

impl Gas {
    pub fn new(i2c: I2cBusDevice) -> Self {
        let mut dev = Sgp30::new(i2c, 0x58, Delay);
        match dev.init() {
            Ok(()) => {
                esp_println::println!("sgp30: initialised");
                Self {
                    dev: Some(dev),
                    eco2_ppm: None,
                    tvoc_ppb: None,
                }
            }
            Err(err) => {
                esp_println::println!("sgp30: init failed: {err:?}");
                Self {
                    dev: None,
                    eco2_ppm: None,
                    tvoc_ppb: None,
                }
            }
        }
    }

    /// Take one air-quality measurement. Must be called once per second.
    pub fn sample(&mut self) {
        let Some(dev) = self.dev.as_mut() else { return };
        match dev.measure() {
            Ok(m) => {
                self.eco2_ppm = Some(m.co2eq_ppm);
                self.tvoc_ppb = Some(m.tvoc_ppb);
            }
            Err(err) => {
                esp_println::println!("sgp30: measure failed: {err:?}");
                self.eco2_ppm = None;
                self.tvoc_ppb = None;
            }
        }
    }

    /// Feed the on-chip humidity compensation from the latest climate values.
    pub fn set_humidity(&mut self, temp_c: f32, rh_pct: f32) {
        let Some(dev) = self.dev.as_mut() else { return };
        if let Ok(humidity) = Humidity::from_f32(absolute_humidity(temp_c, rh_pct)) {
            if let Err(err) = dev.set_humidity(Some(&humidity)) {
                esp_println::println!("sgp30: set_humidity failed: {err:?}");
            }
        }
    }

    /// Copy the latest gas values into `reading`.
    pub fn read_into(&self, reading: &mut Reading) {
        reading.eco2_ppm = self.eco2_ppm;
        reading.tvoc_ppb = self.tvoc_ppb;
    }
}

/// Absolute humidity in g/m³ from temperature (°C) and relative humidity (%),
/// using the Magnus formula the SGP30 datasheet recommends.
fn absolute_humidity(temp_c: f32, rh_pct: f32) -> f32 {
    let saturation = 6.112 * libm::expf((17.62 * temp_c) / (243.12 + temp_c));
    216.7 * (rh_pct / 100.0 * saturation) / (273.15 + temp_c)
}
