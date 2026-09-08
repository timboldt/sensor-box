//! Bosch BME280 temperature / humidity / pressure sensor.
//!
//! Wired at I2C address 0x77 (the Adafruit breakout's default).

use bme280::i2c::BME280;
use embassy_time::Delay;

use crate::board::I2cBusDevice;
use crate::sensors::Reading;

pub struct Climate {
    dev: Option<BME280<I2cBusDevice>>,
    delay: Delay,
}

impl Climate {
    /// Initialise the sensor at 0x77. A missing sensor is logged and left as
    /// `None`; its `Reading` fields then stay `None`.
    pub fn new(i2c: I2cBusDevice) -> Self {
        let mut delay = Delay;
        let mut dev = BME280::new_secondary(i2c);
        match dev.init(&mut delay) {
            Ok(()) => {
                esp_println::println!("bme280: initialised");
                Self {
                    dev: Some(dev),
                    delay,
                }
            }
            Err(err) => {
                esp_println::println!("bme280: init failed: {err:?}");
                Self { dev: None, delay }
            }
        }
    }

    /// Measure and merge temperature, humidity and pressure into `reading`.
    pub fn read_into(&mut self, reading: &mut Reading) {
        let Some(dev) = self.dev.as_mut() else { return };
        match dev.measure(&mut self.delay) {
            Ok(m) => {
                reading.temp_c = Some(m.temperature);
                reading.rh = Some(m.humidity);
                reading.pressure_hpa = Some(m.pressure / 100.0);
            }
            Err(err) => {
                esp_println::println!("bme280: measure failed: {err:?}");
                reading.temp_c = None;
                reading.rh = None;
                reading.pressure_hpa = None;
            }
        }
    }
}
