# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

# This repo ships its own board (boards/adafruit/feather_esp32c6). Zephyr only
# looks under directories on BOARD_ROOT, and it does not add the application
# dir automatically — so register it here, before find_package(Zephyr).
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensor_box LANGUAGES C)

target_sources(app PRIVATE src/main.c)
