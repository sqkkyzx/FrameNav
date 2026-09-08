# Battery gesture diagnostic build

Only `xiao-c6-motion-debug` defines `FRAMENAV_TEST_DEBUG=1`.
Its separate 64 KiB NVS partition starts at 0x377000; existing NVS,
application and track partition addresses and sizes are unchanged.
The normal build excludes the logger and uses the original partition table.
No motion thresholds or Wi-Fi policy changes are made by this target.

The last 128 events persist across restarts. On USB boot, `test_debug`
prints saved history before current events. Boot IDs and uptime distinguish
old battery events from the new USB session. Writes are queued; wait at least
3 seconds after the action before unplugging. Flash writes affect power/timing,
so this build is not suitable for power measurements.

Source bits: SOF=1, D+=2, D-=4. These are observed signals, not guaranteed
identification of the attached supply.
SENSOR_STATE: a bits healthy=1 paused=2 initialized=4 armed=8 capturing=16 INT1=32;
b cooldown seconds, c successful samples since previous report, d quiet seconds.
CAPTURE_END: a accepted, b points, c moving points, d elapsed milliseconds.
CIRCLE: a automatic Wi-Fi allowed, b Wi-Fi running.
AP_START: a gesture=1 or automatic=0. AP_RESULT: a success, b gesture=1.
WIFI_STOP: a=1 web inactivity, a=2 supply policy.
SENSOR_ERROR: a register, b error, c I2C address, d write=1/read=0.
BOOT: a reset reason, b sleep wake cause.

No coordinates, credentials or raw IMU samples are persisted.
