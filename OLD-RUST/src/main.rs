//! Environmental sensor box firmware for the Adafruit ESP32-C6 Feather.
//!
//! Reads a set of I2C environmental sensors, shows live values and recent history
//! on a TFT FeatherWing, and logs timestamped CSV to the SD card.

#![no_std]
#![no_main]

mod board;
mod sensors;

use embassy_executor::Spawner;
use embassy_time::{Duration, Ticker};
use esp_backtrace as _;
use esp_println::println;

use crate::board::Board;
use crate::sensors::{Climate, Gas, Pm, Reading};

/// How many 1 Hz ticks between full readings / log lines.
const REPORT_EVERY_TICKS: u32 = 5;

esp_bootloader_esp_idf::esp_app_desc!();

#[esp_hal::main]
async fn main(_spawner: Spawner) {
    let board = Board::init();
    println!("sensor-box: board initialised");

    board.i2c_scan();

    let mut pm = Pm::new(board.i2c());
    let mut climate = Climate::new(board.i2c());
    let mut gas = Gas::new(board.i2c());

    // The SGP30 needs a steady 1 Hz measurement cadence; the rest of the sensors
    // and the log line ride along every REPORT_EVERY_TICKS.
    let mut ticker = Ticker::every(Duration::from_secs(1));
    let mut tick: u32 = 0;

    loop {
        gas.sample();

        if tick.is_multiple_of(REPORT_EVERY_TICKS) {
            let mut reading = Reading::default();
            pm.read_into(&mut reading);
            climate.read_into(&mut reading);
            gas.read_into(&mut reading);
            if let (Some(temp_c), Some(rh)) = (reading.temp_c, reading.rh) {
                gas.set_humidity(temp_c, rh);
            }
            println!("{reading:?}");
        }

        tick = tick.wrapping_add(1);
        ticker.next().await;
    }
}
