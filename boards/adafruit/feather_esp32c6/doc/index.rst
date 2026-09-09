.. _adafruit_feather_esp32c6:

Adafruit Feather ESP32-C6
#########################

Overview
********

Out-of-tree board definition for the `Adafruit ESP32-C6 Feather
<https://www.adafruit.com/product/5933>`_, used by the ``sensor-box`` firmware.
The board carries an ESP32-C6-MINI-1 module (RISC-V, Wi-Fi 6, BLE 5,
802.15.4, 4 MB flash, no PSRAM), a STEMMA QT / Qwiic connector, a LiPo
charger, a NeoPixel and a user LED.

Supported Features
******************

- Native USB-Serial/JTAG console (default ``zephyr,console``)
- GPIO, I2C (STEMMA QT, ``i2c0``), SPI (Feather header, ``spi2``)
- UART on the Feather header (``uart1``, TX=IO16 / RX=IO17)
- Watchdog, TRNG

Not yet described in devicetree: full Feather-header nexus (A0/A1/A4/A5 and
the D-pin positions), STEMMA QT power switch, NeoPixel. See the TODO notes in
``feather_connector.dtsi`` and ``adafruit_feather_esp32c6_hpcore.dts``.

Building and Flashing
*********************

.. code-block:: console

   west build -b adafruit_feather_esp32c6/esp32c6/hpcore <app>
   west flash
   west espressif monitor -p /dev/cu.usbmodemXXXX

When building an app outside this repo, add
``-DBOARD_ROOT=/path/to/sensor-box``.
