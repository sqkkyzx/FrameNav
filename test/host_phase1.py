#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Compile selected *actual* C functions with tiny host stubs, not BLE emulation.

Run: python3 test/host_phase1.py
Use --upstream to reproduce the UUID/fake-fix regressions in the cloned commit.
These checks do not claim camera compatibility or EXIF success.
"""

import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parents[1]
UPSTREAM = "--upstream" in sys.argv
if UPSTREAM:
    sys.argv.remove("--upstream")


def function_source(path, name):
    if UPSTREAM:
        source = subprocess.check_output(
            ["git", "show", f"43f5ea52ec157c2f33e9831f8f91065f56f800ac:{path}"],
            cwd=ROOT, text=True,
        )
    else:
        source = (ROOT / path).read_text()
    # Match a definition, not an earlier forward declaration with the same name.
    match = re.search(
        rf"^(?:static )?(?:bool|void|int) {name}\([^;{{}}]*\)\s*\{{", source, re.M)
    if not match:
        raise AssertionError(f"Function not found: {name}")
    brace = source.index("{", match.start())
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def run_c(source):
    with tempfile.TemporaryDirectory(prefix="alphaloc-host-test-") as directory:
        binary = str(pathlib.Path(directory) / "check")
        compiled = subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
             "-x", "c", "-", "-o", binary],
            input=source, text=True, capture_output=True,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stderr)
        result = subprocess.run([binary], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr or f"C check exited {result.returncode}")


PRELUDE = """
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "gps.h"
"""


def clock_source(start):
    epoch = int(datetime(*start, tzinfo=timezone.utc).timestamp())
    return (f"\n#define ALPHALOC_FAKE_GPS_EPOCH {epoch}LL\n" +
            function_source("src/fake_gps_clock.c", "gps_fake_apply_clock"))


class Phase1Checks(unittest.TestCase):
    @unittest.skipIf(UPSTREAM, "Stepping-clock experiment was added locally")
    def test_clock_uses_elapsed_seconds_and_payload_utc_advances(self):
        run_c(PRELUDE + clock_source((2026, 9, 3, 12, 0, 58)) +
              function_source("src/ble_client.c", "build_location_payload") + """
int main(void) {
    gps_fix_t fix = {.valid=true, .lat_deg=48.137154, .lon_deg=11.576124};
    assert(!gps_fake_apply_clock(NULL, 0));
    assert(!gps_fake_apply_clock(&fix, -1));
    assert(gps_fake_apply_clock(&fix, 0));
    assert(fix.time_valid && fix.year == 2026 && fix.month == 9 && fix.day == 3);
    assert(fix.hour == 12 && fix.minute == 0 && fix.second == 58);
    assert(gps_fake_apply_clock(&fix, 999999));
    assert(fix.second == 58); // multiple reads do not artificially advance time
    assert(gps_fake_apply_clock(&fix, 2000000));
    assert(fix.hour == 12 && fix.minute == 1 && fix.second == 0);
    assert(gps_fake_apply_clock(&fix, 65000000));
    assert(fix.hour == 12 && fix.minute == 2 && fix.second == 3);
    assert(gps_fake_apply_clock(&fix, 601000000)); // reconnect after long gap
    assert(fix.hour == 12 && fix.minute == 10 && fix.second == 59);
    assert(gps_fake_apply_clock(&fix, 601000000));
    assert(fix.second == 59);
    uint8_t payload[95];
    size_t length;
    assert(build_location_payload(&fix, false, 480, 0, payload, &length));
    assert(length == 95 && payload[19] == 7 && payload[20] == 234);
    assert(payload[21] == 9 && payload[22] == 3 && payload[23] == 12);
    assert(payload[24] == 10 && payload[25] == 59);
    assert(payload[91] == 1 && payload[92] == 224); // UTC+8 still separate
    assert(fix.lat_deg == 48.137154 && fix.lon_deg == 11.576124 && fix.valid);
    return 0;
}
""")

    @unittest.skipIf(UPSTREAM, "Stepping-clock experiment was added locally")
    def test_clock_handles_day_month_year_and_leap_day_rollover(self):
        cases = [
            ((2026, 9, 3, 23, 59, 59), (2026, 9, 4)),
            ((2026, 12, 31, 23, 59, 59), (2027, 1, 1)),
            ((2027, 2, 28, 23, 59, 59), (2027, 3, 1)),
            ((2028, 2, 28, 23, 59, 59), (2028, 2, 29)),
            ((2028, 2, 29, 23, 59, 59), (2028, 3, 1)),
            ((2040, 12, 31, 23, 59, 59), (2041, 1, 1)),
        ]
        for start, expected in cases:
            with self.subTest(start=start):
                run_c(PRELUDE + clock_source(start) + f"""
int main(void) {{
    gps_fix_t fix = {{0}};
    assert(gps_fake_apply_clock(&fix, 1000000));
    assert(fix.year == {expected[0]} && fix.month == {expected[1]} &&
           fix.day == {expected[2]});
    assert(fix.hour == 0 && fix.minute == 0 && fix.second == 0);
    return 0;
}}
""")

    @unittest.skipIf(UPSTREAM, "Stepping-clock experiment was added locally")
    def test_fake_latest_uses_stepping_clock_after_ten_minutes(self):
        run_c(PRELUDE + clock_source((2026, 9, 3, 12, 0, 0)) + """
#define ALPHALOC_FAKE_GPS 1
#define ALPHALOC_FAKE_GPS_STEPPING 1
#define pdTRUE 1
#define pdMS_TO_TICKS(value) (value)
#define xSemaphoreTake(mutex, ticks) (1)
#define xSemaphoreGive(mutex) ((void)0)
#define esp_timer_get_time() (601000000LL)
static int s_fix_mutex = 1;
static gps_fix_t s_latest_fix;
""" + function_source("src/gps.c", "gps_get_latest") + """
int main(void) {
    s_latest_fix.valid = true;
    s_latest_fix.lat_deg = 48.137154;
    gps_fix_t fix;
    assert(gps_get_latest(&fix));
    assert(fix.valid && fix.time_valid && fix.year == 2026);
    assert(fix.hour == 12 && fix.minute == 10 && fix.second == 1);
    assert(fix.last_fix_time_us == 601000000LL);
    assert(fix.last_update_time_us == fix.last_fix_time_us);
    assert(fix.lat_deg == 48.137154);
    return 0;
}
""")

    @unittest.skipIf(UPSTREAM, "Stepping-clock experiment was added locally")
    def test_fake_fallback_never_reverts_to_frozen_2024(self):
        run_c(PRELUDE + clock_source((2026, 9, 3, 12, 0, 0)) + """
#define ALPHALOC_FAKE_GPS 1
#define ALPHALOC_FAKE_GPS_STEPPING 1
#define FAKE_LAT_DEG 48.137154
#define FAKE_LON_DEG 11.576124
#define FAKE_YEAR 2024
#define FAKE_MONTH 1
#define FAKE_DAY 1
#define FAKE_HOUR 12
#define FAKE_MINUTE 0
#define FAKE_SECOND 0
static int64_t now_us = 65000000;
#define esp_timer_get_time() (now_us)
bool gps_get_latest(gps_fix_t *fix) { (void)fix; return false; }
""" + function_source("src/main.c", "get_location_for_send") + """
int main(void) {
    gps_fix_t fix;
    assert(get_location_for_send(&fix));
    assert(fix.valid && fix.time_valid && fix.year == 2026);
    assert(fix.hour == 12 && fix.minute == 1 && fix.second == 5);
    assert(fix.lat_deg == 48.137154 && fix.lon_deg == 11.576124);
    now_us = -1;
    assert(!get_location_for_send(&fix));
    return 0;
}
""")

    @unittest.skipIf(UPSTREAM, "Diagnostic callbacks were added locally")
    def test_write_diagnostics_do_not_change_recovery_behavior(self):
        run_c(PRELUDE + """
#define VLOGI(...) ((void)printf(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)printf(__VA_ARGS__))
struct ble_gatt_error { int status; };
struct ble_gatt_attr { unsigned handle; };
static bool s_location_enabled = true;
""" + function_source("src/ble_client.c", "location_enable_write_cb") +
              function_source("src/ble_client.c", "location_write_cb") + """
int main(void) {
    struct ble_gatt_error error = {.status=0};
    assert(location_enable_write_cb(7, &error, NULL, "DD30") == 0);
    error.status = 0x103;
    assert(location_enable_write_cb(7, &error, NULL, "DD31") == 0);
    assert(location_write_cb(7, &error, NULL, NULL) == 0);
    assert(s_location_enabled); // known legacy behavior deliberately unchanged
    return 0;
}
""")

    @unittest.skipIf(UPSTREAM, "XIAO identity was added locally")
    def test_name_uses_ble_mac_suffix_in_display_order(self):
        run_c(PRELUDE + function_source("src/ble_client.c", "format_device_name") + """
int main(void) {
    const uint8_t addr[6] = {0x56, 0x34, 0x12, 0x00, 0x00, 0x02};
    char name[sizeof("FrameNav-FFFFFF")];
    format_device_name(addr, name, sizeof(name));
    assert(strcmp(name, "FrameNav-123456") == 0);
    const uint8_t zeros[6] = {0x01, 0x02, 0x03, 0x00, 0x00, 0x02};
    format_device_name(zeros, name, sizeof(name));
    assert(strcmp(name, "FrameNav-030201") == 0);
    char short_name[5] = {0};
    format_device_name(addr, short_name, sizeof(short_name));
    assert(strcmp(short_name, "Fram") == 0);
    return 0;
}
""")

    @unittest.skipIf(UPSTREAM, "XIAO heartbeat was added locally")
    def test_led_blinks_each_second_then_stays_on_while_connected(self):
        run_c(PRELUDE + """
#define pdMS_TO_TICKS(ms) ((ms) / 10)
static unsigned ticks, event_ticks[16], event_count;
static bool levels[16], usb_hosts[16];
static bool led_on;
static void vTaskDelay(unsigned delay_ticks) { ticks += delay_ticks; }
static void xiao_status_led_set(bool on, bool usb_host) {
    assert(event_count < 16);
    event_ticks[event_count] = ticks;
    usb_hosts[event_count] = usb_host;
    levels[event_count++] = on;
    led_on = on;
}
""" + function_source("src/main.c", "xiao_status_led_cycle") + """
int main(void) {
    xiao_status_led_cycle(false, false);
    assert(ticks == 100 && !led_on && event_count == 2);
    assert(levels[0] && event_ticks[0] == 0 && !usb_hosts[0]);
    assert(!levels[1] && event_ticks[1] == 10 && !usb_hosts[1]);
    xiao_status_led_cycle(true, false);
    assert(ticks == 200 && led_on && event_count == 3);
    assert(levels[2] && event_ticks[2] == 100 && !usb_hosts[2]);
    xiao_status_led_cycle(true, true);
    assert(ticks == 300 && led_on && event_count == 4);
    assert(levels[3] && event_ticks[3] == 200 && usb_hosts[3]); // no OFF gap
    xiao_status_led_cycle(false, true);
    assert(ticks == 400 && !led_on && event_count == 6);
    assert(levels[4] && event_ticks[4] == 300 && usb_hosts[4]);
    assert(!levels[5] && event_ticks[5] == 310 && usb_hosts[5]);
    xiao_status_led_cycle(false, false);
    assert(ticks == 500 && !led_on && event_count == 8);
    assert(levels[6] && event_ticks[6] == 400 && !usb_hosts[6]);
    assert(!levels[7] && event_ticks[7] == 410 && !usb_hosts[7]);
    return 0;
}
""")

    @unittest.skipIf(UPSTREAM, "XIAO heartbeat was added locally")
    def test_led_uses_full_duty_for_usb_host_and_dim_fallback(self):
        run_c(PRELUDE + """
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_CHANNEL_0 0
#define ESP_ERROR_CHECK(result) assert((result) == 0)
static unsigned last_duty;
static unsigned updates;
static int ledc_set_duty(int mode, int channel, unsigned duty) {
    assert(mode == 0 && channel == 0);
    last_duty = duty;
    return 0;
}
static int ledc_update_duty(int mode, int channel) {
    assert(mode == 0 && channel == 0);
    ++updates;
    return 0;
}
""" + function_source("src/main.c", "xiao_status_led_set") + """
int main(void) {
    xiao_status_led_set(true, false);
    assert(last_duty == 26 && updates == 1);
    xiao_status_led_set(false, false);
    assert(last_duty == 0 && updates == 2);
    xiao_status_led_set(true, true);
    assert(last_duty == 256 && updates == 3);
    xiao_status_led_set(false, true);
    assert(last_duty == 0 && updates == 4);
    xiao_status_led_set(true, true);
    xiao_status_led_set(true, false);
    assert(last_duty == 26 && updates == 6);
    return 0;
}
""")

    def test_sony_uuid_type_and_bytes(self):
        run_c(PRELUDE + """
#define BLE_UUID_TYPE_128 128
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
""" + function_source("src/ble_client.c", "make_sony_uuid") + """
int main(void) {
    ble_uuid128_t uuid;
    memset(&uuid, 0xA5, sizeof(uuid));
    make_sony_uuid(0xDD00, 0xDD00, &uuid);
    assert(uuid.u.type == BLE_UUID_TYPE_128);
    const uint8_t expected[16] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0x00,0xdd,0x00,0xdd,0x00,0x80
    };
    assert(memcmp(uuid.value, expected, 16) == 0);
    make_sony_uuid(0xDD11, 0xDD00, &uuid);
    assert(uuid.u.type == BLE_UUID_TYPE_128);
    assert(uuid.value[12] == 0x11 && uuid.value[13] == 0xdd);
    return 0;
}
""")

    def check_fix_age(self, fake):
        run_c(PRELUDE + f"\n#define ALPHALOC_FAKE_GPS {int(fake)}\n" + """
#define pdTRUE 1
#define pdMS_TO_TICKS(value) (value)
#define xSemaphoreTake(mutex, ticks) (1)
#define xSemaphoreGive(mutex) ((void)0)
#define esp_timer_get_time() (601000000LL)
static int s_fix_mutex = 1;
static gps_fix_t s_latest_fix;
""" + function_source("src/gps.c", "gps_get_latest") + """
int main(void) {
    s_latest_fix.valid = true;
    s_latest_fix.lat_deg = 48.137154;
    s_latest_fix.last_fix_time_us = 1000000;
    gps_fix_t fix;
    assert(!gps_get_latest(NULL));
    assert(gps_get_latest(&fix));
    assert(fix.valid && fix.lat_deg == 48.137154);
#if ALPHALOC_FAKE_GPS
    assert(fix.last_fix_time_us == 601000000LL);
    assert(fix.last_update_time_us == fix.last_fix_time_us);
#else
    assert(fix.last_fix_time_us == 1000000);
#endif
    s_fix_mutex = 0;
    assert(!gps_get_latest(&fix));
    return 0;
}
""")

    def test_fake_fix_is_fresh_after_ten_minutes(self):
        self.check_fix_age(True)

    def test_real_fix_age_is_not_refreshed(self):
        self.check_fix_age(False)

    def test_location_payload_keeps_upstream_layout(self):
        run_c(PRELUDE + function_source("src/ble_client.c", "build_location_payload") + """
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
int main(void) {
    gps_fix_t fix = {.valid=true, .time_valid=true, .lat_deg=48.137154,
                     .lon_deg=11.576124, .year=2024, .month=1, .day=1, .hour=12};
    uint8_t out[95];
    size_t length = 0;
    assert(build_location_payload(&fix, false, 480, 0, out, &length));
    assert(length == 95 && out[0] == 0 && out[1] == 93 && out[5] == 3);
    assert(be32(out + 11) == 481371540 && be32(out + 15) == 115761240);
    assert(out[19] == 7 && out[20] == 232 && out[21] == 1 && out[22] == 1);
    assert(out[23] == 12 && out[24] == 0 && out[25] == 0);
    for (int i = 26; i < 91; ++i) assert(out[i] == 0);
    assert(out[91] == 1 && out[92] == 224 && out[93] == 0 && out[94] == 0);
    assert(build_location_payload(&fix, false, 0, 0, out, &length));
    assert(length == 91 && out[1] == 89 && out[5] == 0);
    assert(build_location_payload(&fix, true, 0, 0, out, &length));
    assert(length == 95);
    fix.valid = false;
    assert(!build_location_payload(&fix, true, 0, 0, out, &length));
    assert(!build_location_payload(NULL, true, 0, 0, out, &length));
    return 0;
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
