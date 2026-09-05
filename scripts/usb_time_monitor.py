#!/usr/bin/env python3
"""Phase 1 USB clock handshake + timestamped monitor. No Wi-Fi / GNSS required.

Copyright 2026 FrameNav contributors
SPDX-License-Identifier: Apache-2.0

Opening the serial port can reset XIAO on macOS. Keep this process open during
camera-only restart tests. Each new ESP boot requests a fresh host UTC sample.
"""

import argparse
from datetime import datetime, timezone
from pathlib import Path
import re
import sys
import time

MIN_MS = 1577836800000
MAX_MS = 4102444800000
REQUEST = re.compile(r"ALPHALOC_TIME_REQUEST_V1 ([0-9a-fA-F]{16})(?:\s|$)")
GPS_SAMPLE = re.compile(r"DD11 sample:.*UTC=(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)")


def time_command(nonce, epoch_ms):
    if not re.fullmatch(r"[0-9a-fA-F]{16}", nonce) or int(nonce, 16) == 0:
        raise ValueError("Invalid board challenge")
    if not isinstance(epoch_ms, int) or not MIN_MS <= epoch_ms < MAX_MS:
        raise ValueError("Host UTC must be between 2020 and 2100; check Mac clock")
    return f"ALPHALOC_TIME_SET_V1 {nonce} {epoch_ms:013d}\n".encode("ascii")


def matching_device(port, expected_serial, devices):
    return any(item.device == port and item.vid == 0x303A and item.pid == 0x1001
               and (item.serial_number or "").upper() == expected_serial.upper()
               for item in devices)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbmodem3101")
    parser.add_argument("--serial-number", required=True, help="expected board USB serial number")
    parser.add_argument("--seconds", type=int, default=900, help="bounded capture, max 3600 s")
    parser.add_argument("--delay-sync", type=int, default=0,
                        help="test only: withhold host time for this many seconds after open")
    parser.add_argument("--log", type=Path, required=True, help="new log file; never overwritten")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or not 0 <= args.delay_sync <= 300:
        parser.error("--seconds must be 1..3600; --delay-sync must be 0..300")
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        parser.error("Use the PlatformIO pipx Python environment (contains pyserial)")
    time_command("0000000000000001", time.time_ns() // 1000000)
    if not matching_device(args.port, args.serial_number, list_ports.comports()):
        parser.error("Port does not match the expected XIAO USB VID/PID and serial number")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open("x", encoding="utf-8", buffering=1) as log:
        def emit(message):
            output = f"{datetime.now(timezone.utc).isoformat(timespec='milliseconds')} {message}"
            print(output, flush=True)
            log.write(output + "\n")

        deadline = time.monotonic() + args.seconds
        failures = 0
        emit("HOST_CAPTURE_START v7: opening USB may reset ESP; host answers fresh challenges only")
        try:
            while time.monotonic() < deadline:
                port = serial.Serial(port=None, baudrate=115200, timeout=.2, write_timeout=2)
                port.dtr = False
                port.rts = False
                port.port = args.port
                try:
                    if not matching_device(args.port, args.serial_number, list_ports.comports()):
                        raise serial.SerialException("Expected XIAO is not present")
                    port.open()
                    emit("HOST_PORT_OPEN (kept open until disconnect, Ctrl+C or deadline)")
                    allow_sync_at = time.monotonic() + args.delay_sync
                    buffer = b""
                    sent_nonce = None
                    while time.monotonic() < deadline:
                        data = port.read(port.in_waiting or 1)
                        if not data:
                            continue
                        buffer += data
                        while b"\n" in buffer:
                            raw, buffer = buffer.split(b"\n", 1)
                            line = raw.replace(b"\r", b"").decode("utf-8", errors="replace")
                            if not line:
                                continue
                            emit(line)
                            request = REQUEST.search(line)
                            if request and time.monotonic() >= allow_sync_at:
                                nonce = request.group(1)
                                if nonce != sent_nonce:
                                    epoch_ms = time.time_ns() // 1000000
                                    command = time_command(nonce, epoch_ms)
                                    if port.write(command) != len(command):
                                        raise serial.SerialException("Incomplete host time write")
                                    sent_nonce = nonce
                                    emit(f"HOST_TIME_SENT nonce={nonce} epoch_ms={epoch_ms}")
                            sample = GPS_SAMPLE.search(line)
                            if sample:
                                gps_ms = int(datetime.fromisoformat(
                                    sample.group(1).replace("Z", "+00:00")).timestamp() * 1000)
                                delta_ms = time.time_ns() // 1000000 - gps_ms
                                emit(f"HOST_GPS_TIME_DELTA_MS={delta_ms} "
                                     "(includes whole-second GPS quantization and log transport)")
                        if len(buffer) > 16384:
                            emit("HOST_WARNING overlong serial line discarded")
                            buffer = b""
                except (serial.SerialException, OSError) as exc:
                    failures += 1
                    emit(f"HOST_USB_ERROR attempt={failures}: {exc}")
                    if failures >= 3:
                        raise
                    # Only reopen after an actual USB error, never to poll status.
                    port.close()
                    time.sleep(1)
                finally:
                    port.close()
        except KeyboardInterrupt:
            emit("HOST_CAPTURE_STOP requested; firmware clock continues until ESP reset")
        finally:
            emit("HOST_CAPTURE_END")


if __name__ == "__main__":
    main()
