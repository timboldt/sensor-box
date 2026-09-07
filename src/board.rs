//! Board wiring for the Adafruit ESP32-C6 Feather stacked with the 2.4" TFT
//! FeatherWing (ILI9341 + microSD + STMPE610 touch) and the DS3231 RTC
//! FeatherWing.
//!
//! Feather silkscreen -> ESP32-C6 GPIO:
//!
//! | Signal                | Pin  | GPIO |
//! |-----------------------|------|------|
//! | I2C data              | SDA  | 19   |
//! | I2C clock             | SCL  | 18   |
//! | I2C / STEMMA QT power  | --   | 20   |
//! | SPI clock             | SCK  | 21   |
//! | SPI MOSI              | MOSI | 22   |
//! | SPI MISO              | MISO | 23   |
//! | Display chip select   | D9   | 7    |
//! | Display data/command  | D10  | 8    |
//! | microSD chip select   | D5   | 5    |
//! | Touch chip select     | D6   | 6    |
//!
//! The I2C bus is shared by the PMSA003I, BME280, TSL2591, SGP30 and the DS3231.
//! The SPI bus is shared by the display, the SD card and the touch controller;
//! each gets its own [`SpiBusDevice`] carrying its own clock/mode.

use core::cell::RefCell;

use embassy_embedded_hal::shared_bus::blocking::spi::SpiDeviceWithConfig;
use embassy_sync::blocking_mutex::{Mutex, raw::CriticalSectionRawMutex};
use embedded_hal_async::i2c::I2c as AsyncI2c;
use esp_hal::{
    Async,
    Blocking,
    gpio::{Level, Output, OutputConfig},
    i2c::master::{Config as I2cConfig, I2c},
    spi::{
        Mode,
        master::{Config as SpiConfig, Spi},
    },
    time::Rate,
    timer::timg::TimerGroup,
};
use esp_println::println;
use static_cell::StaticCell;

/// The shared blocking SPI bus, behind a critical-section mutex.
type SpiBus = Mutex<CriticalSectionRawMutex, RefCell<Spi<'static, Blocking>>>;

/// A single peripheral on the shared SPI bus, with its own clock/mode config
/// applied on every transaction.
pub type SpiBusDevice =
    SpiDeviceWithConfig<'static, CriticalSectionRawMutex, Spi<'static, Blocking>, Output<'static>>;

static SPI_BUS: StaticCell<SpiBus> = StaticCell::new();

/// Owns every configured bus and pin `main` needs to reach the hardware.
// The SPI devices are constructed here but not exercised until the display (step
// 4) and SD logging (step 5) land.
#[allow(dead_code)]
pub struct Board {
    /// Shared I2C bus: PMSA003I, BME280, TSL2591, SGP30, DS3231.
    pub i2c: I2c<'static, Async>,
    /// ILI9341 display, on the shared SPI bus.
    pub display_spi: SpiBusDevice,
    /// Display data/command select line.
    pub display_dc: Output<'static>,
    /// microSD slot on the TFT FeatherWing, on the shared SPI bus.
    pub sd_spi: SpiBusDevice,
    /// STMPE610 resistive touch controller, on the shared SPI bus.
    pub touch_spi: SpiBusDevice,
    /// Holds the STEMMA QT / I2C power rail on. Dropping it cuts sensor power.
    _i2c_power: Output<'static>,
}

impl Board {
    /// Take the peripherals, start the Embassy time driver, and bring up the
    /// I2C and SPI buses.
    pub fn init() -> Self {
        let p = esp_hal::init(esp_hal::Config::default());

        let timg0 = TimerGroup::new(p.TIMG0);
        esp_rtos::start(timg0.timer0, p.FROM_CPU_INTR0);

        // On this board GPIO20 gates power to the STEMMA QT connector and the
        // I2C pull-ups; it must be driven high before the bus will work.
        let i2c_power = Output::new(p.GPIO20, Level::High, OutputConfig::default());

        let i2c = I2c::new(
            p.I2C0,
            I2cConfig::default().with_frequency(Rate::from_khz(100)),
        )
        .unwrap()
        .with_sda(p.GPIO19)
        .with_scl(p.GPIO18)
        .into_async();

        let spi = Spi::new(
            p.SPI2,
            SpiConfig::default()
                .with_frequency(Rate::from_mhz(1))
                .with_mode(Mode::_0),
        )
        .unwrap()
        .with_sck(p.GPIO21)
        .with_mosi(p.GPIO22)
        .with_miso(p.GPIO23);
        let spi_bus: &'static SpiBus = SPI_BUS.init(Mutex::new(RefCell::new(spi)));

        let display_spi = SpiDeviceWithConfig::new(
            spi_bus,
            Output::new(p.GPIO7, Level::High, OutputConfig::default()),
            SpiConfig::default()
                .with_frequency(Rate::from_mhz(24))
                .with_mode(Mode::_0),
        );
        let display_dc = Output::new(p.GPIO8, Level::High, OutputConfig::default());

        // SD cards must be clocked at <= 400 kHz through the init handshake;
        // step 5 raises this once the card is ready.
        let sd_spi = SpiDeviceWithConfig::new(
            spi_bus,
            Output::new(p.GPIO5, Level::High, OutputConfig::default()),
            SpiConfig::default()
                .with_frequency(Rate::from_khz(400))
                .with_mode(Mode::_0),
        );

        let touch_spi = SpiDeviceWithConfig::new(
            spi_bus,
            Output::new(p.GPIO6, Level::High, OutputConfig::default()),
            SpiConfig::default()
                .with_frequency(Rate::from_mhz(1))
                .with_mode(Mode::_0),
        );

        Self {
            i2c,
            display_spi,
            display_dc,
            sd_spi,
            touch_spi,
            _i2c_power: i2c_power,
        }
    }

    /// Probe every 7-bit address on the I2C bus and log which ones respond.
    /// Bring-up diagnostic only.
    pub async fn i2c_scan(&mut self) {
        let mut found = 0u32;
        for addr in 0x08u8..=0x77 {
            if AsyncI2c::write(&mut self.i2c, addr, &[]).await.is_ok() {
                println!("i2c: device found at 0x{addr:02x}");
                found += 1;
            }
        }
        println!("i2c: scan complete, {found} device(s) responded");
    }
}
