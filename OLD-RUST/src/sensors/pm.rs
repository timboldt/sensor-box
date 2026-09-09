//! Plantower PMSA003I particulate matter sensor (I2C, address 0x12).
//!
//! The sensor free-runs: its fan and laser need ~30 s after power-up before
//! readings settle, and it emits a fresh frame roughly once a second.
//!
//! We keep the "atmospheric environment" (`env_*`) concentrations rather than
//! the "standard particle" ones — that's the calibration consumer air monitors
//! report and feed to the AQI. Below ~50 µg/m³ the two are identical anyway.

use pmsa003i::Pmsa003i;

use crate::board::I2cBusDevice;
use crate::sensors::Reading;

pub struct Pm {
    dev: Pmsa003i<I2cBusDevice>,
}

impl Pm {
    pub fn new(i2c: I2cBusDevice) -> Self {
        Self {
            dev: Pmsa003i::new(i2c),
        }
    }

    /// Read the sensor and merge the PM fields into `reading`. On error, logs a
    /// warning and clears those fields.
    pub fn read_into(&mut self, reading: &mut Reading) {
        match self.dev.read() {
            Ok(frame) => {
                // esp_println::println!("pm: {frame:?}");
                reading.pm1_0 = Some(frame.env_pm1);
                reading.pm2_5 = Some(frame.env_pm2_5);
                reading.pm10 = Some(frame.env_pm10);
                reading.aqi = us_aqi(frame.env_pm2_5, frame.env_pm10);
            }
            Err(err) => {
                esp_println::println!("pm: read failed: {err:?}");
                reading.pm1_0 = None;
                reading.pm2_5 = None;
                reading.pm10 = None;
                reading.aqi = None;
            }
        }
    }
}

/// Overall US AQI from PM2.5 and PM10 concentrations (µg/m³): the worse of the
/// two sub-indices. `None` if either is past the top of the scale.
fn us_aqi(pm2_5: u16, pm10: u16) -> Option<u16> {
    let pm2_5 = aqi::pm2_5(f64::from(pm2_5)).ok()?.aqi();
    let pm10 = aqi::pm10(f64::from(pm10)).ok()?.aqi();
    Some(pm2_5.max(pm10) as u16)
}
