//! sensor-box — Phase 1 step 1: bare skeleton.
//!
//! Boots the Embassy executor on the ESP32-C6 and prints a heartbeat over the
//! built-in USB-Serial-JTAG so we can confirm the toolchain / build / flash path
//! before wiring up any peripherals.

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
