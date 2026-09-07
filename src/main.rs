//! Environmental sensor box firmware for the Adafruit ESP32-C6 Feather.
//!
//! Reads a set of I2C environmental sensors, shows live values and recent history
//! on a TFT FeatherWing, and logs timestamped CSV to the SD card.

#![no_std]
#![no_main]

mod board;

use embassy_executor::Spawner;
use embassy_time::{Duration, Timer};
use esp_backtrace as _;
use esp_println::println;

use crate::board::Board;

esp_bootloader_esp_idf::esp_app_desc!();

#[esp_hal::main]
async fn main(_spawner: Spawner) {
    let mut board = Board::init();
    println!("sensor-box: board initialised");

    board.i2c_scan().await;

    loop {
        Timer::after(Duration::from_secs(10)).await;
        println!("sensor-box: idle");
    }
}
