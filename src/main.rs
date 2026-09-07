//! Environmental sensor box firmware for the Adafruit ESP32-C6 Feather.
//!
//! Reads a set of I2C environmental sensors, shows live values and recent history
//! on a TFT FeatherWing, and logs timestamped CSV to the SD card.

#![no_std]
#![no_main]

use embassy_executor::Spawner;
use embassy_time::{Duration, Timer};
use esp_backtrace as _;
use esp_hal::timer::timg::TimerGroup;
use esp_println::println;

esp_bootloader_esp_idf::esp_app_desc!();

#[esp_hal::main]
async fn main(spawner: Spawner) {
    let peripherals = esp_hal::init(esp_hal::Config::default());

    let timg0 = TimerGroup::new(peripherals.TIMG0);
    esp_rtos::start(timg0.timer0, peripherals.FROM_CPU_INTR0);

    println!("sensor-box: booted on ESP32-C6");

    spawner.spawn(heartbeat().unwrap());

    let mut secs = 0u32;
    loop {
        println!("main alive, t={secs}s");
        secs += 5;
        Timer::after(Duration::from_secs(5)).await;
    }
}

#[embassy_executor::task]
async fn heartbeat() {
    let mut tick = 0u32;
    loop {
        println!("heartbeat #{tick}");
        tick += 1;
        Timer::after(Duration::from_secs(1)).await;
    }
}
