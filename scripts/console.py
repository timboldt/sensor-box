"""Talk to the board's Zephyr shell, or watch its log, over USB serial.

    .venv/bin/python scripts/console.py "kernel uptime" "backlight"
    .venv/bin/python scripts/console.py --watch 30

Runs each shell command and prints its output, or with --watch prints the
log for that many seconds. Does not reset the board: on macOS, opening the
ESP32-C6 USB-Serial-JTAG port with pyserial's default DTR/RTS leaves it
running, whereas setting dtr/rts = False before open() resets it.
The port defaults to the first /dev/cu.usbmodem*; override with ESP_PORT.
"""

import argparse
import glob
import os
import re
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|uart:~\$ ")


def clean(raw: bytes) -> str:
    return ANSI.sub("", raw.decode(errors="replace"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("commands", nargs="*", help="shell commands to run")
    ap.add_argument("--watch", type=float, metavar="SECONDS",
                    help="print the log for this long instead")
    args = ap.parse_args()

    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    port = os.environ.get("ESP_PORT") or (ports[0] if ports else None)
    if port is None:
        sys.exit("no /dev/cu.usbmodem* found; is the board plugged in?")

    with serial.Serial(port, 115200, timeout=0.3) as s:
        if args.watch:
            end = time.time() + args.watch
            while time.time() < end:
                chunk = s.read(4096)
                if chunk:
                    print(clean(chunk), end="", flush=True)
            print()
            return 0

        s.read(65536)  # drop pending log output
        for cmd in args.commands:
            s.write(cmd.encode() + b"\r\n")
            time.sleep(0.8)
            out = clean(s.read(65536))
            # Keep the command's output; skip interleaved periodic sensor logs.
            print("\n".join(l for l in out.splitlines()
                            if l.strip() and "<inf> sensor_box" not in l))
    return 0


if __name__ == "__main__":
    sys.exit(main())
