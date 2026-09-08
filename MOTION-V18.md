# FrameNav 0.18.0-test4: motion and deep sleep

Experimental firmware. Gesture recognition requires physical calibration; synthetic tests do not establish walking false-positive performance.

test3 fixes early gesture termination during gentle circles: accumulated change
from a quiet reference replaces the adjacent-sample stop test. Gesture trimming
uses the last moving sample/time instead of a fixed 35-sample assumption. Host
regression covers 0.15g, three circles in six seconds at both 50Hz and ~33Hz.
USB-only sample diagnostics run for the first 180 seconds after boot.
test4 searches bounded subwindows for the circle, retaining quality thresholds
while tolerating pickup/putdown contamination. Physical test3 capture was
rejected as a whole; its central portion had circular phase progression.
Synthetic tests cover pickup/putdown, linear oscillation and incoherent noise.

## Wiring

- BMI160 SDA: D9 / GPIO20; SCL: D10 / GPIO18.
- BMI160 INT1: D0 / GPIO0, active-high push-pull latched any-motion.
- GNSS RX: D8 / GPIO19 (MCU TX); GNSS TX: D7 / GPIO17.
- Sensor must remain powered during MCU deep sleep.

## Behavior

- Existing GNSS duty cycling is unchanged during normal operation. No stationary-location caching or inertial distance estimation.
- No camera BLE connection, no associated Wi-Fi clients (or STA IP), no recent Web activity, and 300 seconds without motion permits deep sleep. USB data-host SOF vetoes sleep; charger power alone does not.
- Confirm BMI160 health and interrupt configuration, request duration-zero GNSS backup, confirm UART silence, quiesce track writes, stop Wi-Fi and BLE, hold GNSS TX high and LED off, then enter EXT1-only sleep. No periodic timer wake.
- Motion wakes the MCU. Boot sends a UART wake byte to the GNSS and restores normal operation. A failed preparation resumes normal operation; a late interruption during radio shutdown restarts safely.
- On battery/MI-shoe power: after a quiet second, make roughly 2–3 consistent circles and stop. Detection is based on the acceleration trajectory, not reconstructed position. After deep sleep first move to wake, pause, then gesture.
- A recognized circle starts the AP immediately, with a 60-second gesture cooldown. Close after 30 seconds with no Web activity, even if a client remains associated. A visible configuration page sends a heartbeat every 10 seconds; captive redirect probes do not extend it.
- Computer/charger automatic Wi-Fi behavior remains unchanged. Gesture timeout applies to gesture-started sessions.
- NVS namespace `motion_diag` stores entry-attempt and EXT1-wake counters for checking after reconnecting USB. These are diagnostics, not measured power consumption.

## Validation

Build target: `pio run -e xiao-c6-gnss-power`.
Host tests: `python3 -m unittest discover -s test -p 'host_*.py'`.
Physical checks still required include gesture AP after deep-sleep wake, walking false positives, and charging current during deep sleep. Do not infer success from compile/host tests alone.

2026-09-08 test4 USB gesture check: user's 3–4-circle action was accepted,
353 captured points / 312 moving points, 8120ms capture, 1094ms classifier
processing. Main received the event (`Circle recognized; USB source already
manages WiFi`). 135 host tests passed. This verifies gesture recognition and
event delivery, NOT battery AP activation or real-world walking rejection.

2026-09-08 diagnostic-target checks: two battery gesture sessions successfully
started the AP; a separate battery power-on session also recognized the gesture
and started the AP. Persistent events confirmed one deep-sleep entry followed
by an EXT1 motion wake. USB was attached shortly after that wake, so gesture AP
activation after deep sleep remains unverified. Original 135 host tests plus
three diagnostic-isolation checks passed. Raw hardware logs are kept private.
