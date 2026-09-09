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

use embassy_embedded_hal::shared_bus::blocking::i2c::I2cDevice;
use embassy_embedded_hal::shared_bus::blocking::spi::SpiDeviceWithConfig;
use embassy_sync::blocking_mutex::{Mutex, raw::CriticalSectionRawMutex};
use embedded_hal::i2c::I2c as _;
use esp_hal::{
    Blocking,
    gpio::{Level, Output, OutputConfig},
    i2c::master::{BusTimeout, Config as I2cConfig, I2c, SoftwareTimeout},
    spi::{
        Mode,
        master::{Config as SpiConfig, Spi},
    },
    time::{Duration, Rate},
    timer::timg::TimerGroup,
};
use esp_println::println;
use static_cell::StaticCell;

/// The shared I2C bus, behind a critical-section mutex. The esp-hal I2C driver
/// blocks for the (sub-millisecond) duration of each transfer; that is fine at
/// our polling rates and keeps every sensor driver on the simpler blocking API.
type I2cBus = Mutex<CriticalSectionRawMutex, RefCell<I2c<'static, Blocking>>>;

/// One handle onto the shared I2C bus; each sensor driver gets its own.
pub type I2cBusDevice = I2cDevice<'static, CriticalSectionRawMutex, I2c<'static, Blocking>>;

static I2C_BUS: StaticCell<I2cBus> = StaticCell::new();

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
    /// Get a per-sensor handle with [`Board::i2c`].
    pub i2c_bus: &'static I2cBus,
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
            I2cConfig::default()
                .with_frequency(Rate::from_khz(100))
                // Without these, a stuck bus (e.g. a half-seated STEMMA
                // connector holding SDA low) hangs the first transfer forever.
                .with_timeout(BusTimeout::Maximum)
                .with_software_timeout(SoftwareTimeout::Transaction(Duration::from_millis(50))),
        )
        .unwrap()
        .with_sda(p.GPIO19)
        .with_scl(p.GPIO18);
        let i2c_bus: &'static I2cBus = I2C_BUS.init(Mutex::new(RefCell::new(i2c)));

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
            i2c_bus,
            display_spi,
            display_dc,
            sd_spi,
            touch_spi,
            _i2c_power: i2c_power,
        }
    }

    /// A fresh handle onto the shared I2C bus.
    pub fn i2c(&self) -> I2cBusDevice {
        I2cDevice::new(self.i2c_bus)
    }

    /// Probe every 7-bit address on the I2C bus and log which ones respond.
    /// Bring-up diagnostic only.
    pub fn i2c_scan(&self) {
        let mut dev = self.i2c();
        let mut found = 0u32;
        for addr in 0x08u8..=0x77 {
            if dev.write(addr, &[]).is_ok() {
                println!("i2c: device found at 0x{addr:02x}");
                found += 1;
            }
        }
        println!("i2c: scan complete, {found} device(s) responded");
    }
}
