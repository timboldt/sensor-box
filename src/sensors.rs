//! Sensor drivers and the unified [`Reading`] they fill in.
//!
//! Each sensor has a thin wrapper with a `read_into(&mut Reading)` method. A
//! failing or absent sensor logs a warning and leaves its fields as `None`
//! rather than bringing the whole box down.

mod climate;
mod gas;
mod pm;

pub use climate::Climate;
pub use gas::Gas;
pub use pm::Pm;

/// A snapshot of every sensor value. Fields are `None` until read, or when the
/// owning sensor is missing or erroring.
#[derive(Clone, Copy, Debug, Default)]
pub struct Reading {
    /// PM1.0 mass concentration, µg/m³ (atmospheric-environment calibration).
    pub pm1_0: Option<u16>,
    /// PM2.5 mass concentration, µg/m³ (atmospheric-environment calibration).
    pub pm2_5: Option<u16>,
    /// PM10 mass concentration, µg/m³ (atmospheric-environment calibration).
    pub pm10: Option<u16>,
    /// US EPA Air Quality Index: the worse of the PM2.5 and PM10 sub-indices.
    /// `None` if a concentration is off the top of the AQI scale (> 500).
    pub aqi: Option<u16>,

    /// Air temperature, °C.
    pub temp_c: Option<f32>,
    /// Relative humidity, %.
    pub rh: Option<f32>,
    /// Barometric pressure, hPa.
    pub pressure_hpa: Option<f32>,

    /// Equivalent CO₂, ppm (SGP30 model estimate, not a real CO₂ measurement).
    pub eco2_ppm: Option<u16>,
    /// Total volatile organic compounds, ppb.
    pub tvoc_ppb: Option<u16>,
}
