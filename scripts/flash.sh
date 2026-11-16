#!/bin/sh
# Flash the current build to the board.
#
#   scripts/flash.sh [extra west flash args]
#
# Uses the repo's .venv (for west and esptool) without needing it activated.
# The port defaults to the first /dev/cu.usbmodem*; override with ESP_PORT.
set -e
cd "$(dirname "$0")/.."
PATH="$PWD/.venv/bin:$PATH"
port=${ESP_PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1)}
exec west flash ${port:+--esp-device "$port"} "$@"
