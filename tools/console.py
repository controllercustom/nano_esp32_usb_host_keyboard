#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Console monitor for the Nano ESP32 USB-host-keyboard demo.

The sketch's console is Serial1 (hardware UART1) on D0 (RX, GPIO44) and
D1 (TX, GPIO43). The Prolific PL2303 USB-UART cable plugs into those pins and
shows up on this host as a plain serial port (legacy mode), so pyserial reads
it directly:

  /dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller-if00-port0
  /dev/ttyUSB0

Only Rx/Tx/GND are wired, so there is no DTR/RTS reset circuit — to catch the
boot log, start the monitor first, then press the board's RESET button.

Note: the kernel's pl2303 legacy converter always reports 0 bytes waiting, so
pyserial's in_waiting cannot be used here — reads must block (timeout=0.2).

  tools/console.py                    # 15 s, then exit
  tools/console.py --seconds 0        # until Ctrl-C
  tools/console.py --port /dev/ttyUSB0 --baud 115200
"""
import argparse
import os
import sys
import time

import serial

BY_ID = "/dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller-if00-port0"


def default_port() -> str:
    return BY_ID if os.path.exists(BY_ID) else "/dev/ttyUSB0"


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--port", default=default_port())
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=15.0, help="0 = run until Ctrl-C")
    args = ap.parse_args()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as exc:
        print(f"cannot open {args.port}: {exc}", file=sys.stderr)
        print("is the PL2303 cable plugged in? (ls /dev/serial/by-id/)", file=sys.stderr)
        return 1

    pending = ""
    deadline = None if args.seconds == 0 else time.time() + args.seconds
    try:
        while deadline is None or time.time() < deadline:
            # The pl2303 legacy converter never reports bytes waiting, so block-
            # read with a short inter-byte timeout: read() returns whatever came
            # in once the gap exceeds timeout, or b"" when the line is idle.
            try:
                chunk = port.read(1024)
            except serial.SerialException as exc:
                # "device reports readiness to read but returned no data" is
                # what the converter raises when another reader holds the same
                # tty (or the backing device is re-created): drop our handle
                # and take the port again rather than dying.
                print(f"[monitor] {exc} — reopening {args.port}", file=sys.stderr, flush=True)
                try:
                    port.close()
                except Exception:
                    pass
                try:
                    port = serial.Serial(args.port, args.baud, timeout=0.2)
                except serial.SerialException:
                    time.sleep(0.5)
                continue
            if not chunk:
                continue
            pending += chunk.decode("utf-8", "replace")
            while "\n" in pending:
                line, _, pending = pending.partition("\n")
                print(line.rstrip("\r"), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            port.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
