#!/usr/bin/env python3
"""Record motion-test serial output; print one-second summaries, no device writes."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import datetime
import math
import re
import time
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True)
parser.add_argument('--log', required=True)
parser.add_argument('--duration', type=float, default=1850)
args = parser.parse_args()
pattern = re.compile(r'x=([-\d.]+)g y=([-\d.]+)g z=([-\d.]+)g')
port = serial.Serial(port=None, baudrate=115200, timeout=.2)
port.dtr = False
port.rts = False
port.port = args.port
with open(args.log, 'x', buffering=1) as log:
    port.open()
    points = []
    last = time.monotonic()
    end = last + args.duration
    try:
        while time.monotonic() < end:
            line = port.readline().decode(errors='replace').strip()
            now = datetime.datetime.now().astimezone().isoformat(timespec='milliseconds')
            if line:
                log.write(now + ' ' + line + '\n')
                match = pattern.search(line)
                if match:
                    points.append(tuple(map(float, match.groups())))
                elif any(tag in line for tag in ('test_debug:', 'bmi160_probe', 'Track store ready', 'motion:', 'Circle', 'FrameNav 0.')):
                    print(now, line, flush=True)
            if time.monotonic() - last >= 1:
                if points:
                    means = [sum(p[k] for p in points)/len(points) for k in range(3)]
                    rms = math.sqrt(sum(sum((p[k]-means[k])**2 for k in range(3)) for p in points)/len(points))
                    norms = [math.sqrt(sum(v*v for v in p)) for p in points]
                    print(f'{now} n={len(points)} mean={means[0]:.3f},{means[1]:.3f},{means[2]:.3f} vector_rms={rms:.4f}g norm_min/max={min(norms):.3f}/{max(norms):.3f}g', flush=True)
                points = []
                last = time.monotonic()
    except KeyboardInterrupt:
        pass
    finally:
        port.close()
