# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

# This repo ships its own board (boards/adafruit/feather_esp32c6). Zephyr only
# looks under directories on BOARD_ROOT, and it does not add the application
# dir automatically — so register it here, before find_package(Zephyr).
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensor_box LANGUAGES C)

target_sources(app PRIVATE src/main.c src/ui.c src/backlight.c src/history.c src/baseline.c src/wifi.c src/timesync.c src/ltlog.c src/web.c src/light.c src/sdcard.c src/sdlog.c)

# The dashboard page, gzipped into a C array that src/web.c includes.
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)
generate_inc_file_for_target(app web/index.html ${gen_dir}/index.html.gz.inc --gzip)
zephyr_linker_sources(SECTIONS web/sections-rom.ld)
