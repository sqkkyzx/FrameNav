#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Host checks for real K18 NMEA; compile actual C, never access serial ports."""

import configparser
import re
import unittest

from host_phase1 import PRELUDE, ROOT, function_source, run_c


def nmea(body):
    checksum = 0
    for char in body:
        checksum ^= ord(char)
    return f"${body}*{checksum:02X}"


def source(*names):
    return "\n".join(function_source("src/gps.c", name) for name in names)


BASE = PRELUDE + """
#include <ctype.h>
#include <stdlib.h>
#define ALPHALOC_PHASE2 1
#define ALPHALOC_FAKE_GPS 0
#define GPS_LINE_MAX 128
#define GPS_UART_BUF_SIZE 2048
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define xSemaphoreTake(m,t) (1)
#define xSemaphoreGive(m) ((void)0)
#define VLOGI(...) ((void)0)
#define NMEALOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static int64_t now_us=6000000;
#define esp_timer_get_time() (now_us)
"""
RMC = source("parse_time_hms", "date_valid", "split_csv", "parse_deg_min", "parse_rmc")
STATE = """
static gps_fix_t s_latest_fix;
static gps_status_t s_status;
static int s_fix_mutex=1;
static int64_t s_last_no_fix_log_us;
""" + source("update_fix", "gps_get_latest", "gps_get_status")


class Phase2Checks(unittest.TestCase):
    def test_gp_and_gn_rmc_produce_real_coordinates_and_complete_utc(self):
        for talker in ("GP", "GN"):
            line = nmea(f"{talker}RMC,125959.50,A,2234.5678,N,11345.1234,E,0.0,0.0,030926,,,A,V")
            with self.subTest(talker=talker):
                run_c(BASE + RMC + source("hex_value", "nmea_checksum_valid") +
                      function_source("src/ble_client.c", "build_location_payload") + f"""
int main(void) {{
  const char *line="{line}";
  assert(nmea_checksum_valid(line));
  gps_fix_t fix={{0}};
  assert(parse_rmc(line,&fix));
  assert(fix.valid && fix.time_valid);
  assert(fabs(fix.lat_deg-(22+34.5678/60))<1e-9);
  assert(fabs(fix.lon_deg-(113+45.1234/60))<1e-9);
  assert(fix.year==2026 && fix.month==9 && fix.day==3);
  assert(fix.hour==12 && fix.minute==59 && fix.second==59);
  assert(fix.last_fix_time_us==now_us);
  uint8_t payload[95]; size_t len=0;
  assert(build_location_payload(&fix,true,480,0,payload,&len));
  assert(len==95 && payload[19]==7 && payload[20]==234 && payload[25]==59);
  assert(payload[91]==1 && payload[92]==224);
  return 0;
}}
""")

    def test_invalid_rmc_never_becomes_a_publishable_fix(self):
        bodies = [
            "GNRMC,120000.00,V,2234.5678,N,11345.1234,E,0,0,030926,,,N,V",
            "GNRMC,120000.00,AA,2234.5678,N,11345.1234,E,0,0,030926,,,A,V",
            "GNRMC,,A,2234.5678,N,11345.1234,E,0,0,030926,,,A,V",
            "GNRMC,120000.xx,A,2234.5678,N,11345.1234,E,0,0,030926,,,A,V",
            "GNRMC,120000.,A,2234.5678,N,11345.1234,E,0,0,030926,,,A,V",
            "GNRMC,250000.00,A,2234.5678,N,11345.1234,E,0,0,030926,,,A,V",
            "GNRMC,120000.00,A,2234.5678,N,11345.1234,E,0,0,,,,A,V",
            "GNRMC,120000.00,A,2234.5678,N,11345.1234,E,0,0,310226,,,A,V",
            "GNRMC,120000.00,A,2234.5678,N,11345.1234,E,0,0,0309,,,A,V",
            "GNRMC,120000.00,A,nan,N,11345.1234,E,0,0,030926,,,A,V",
            "GNRMC,120000.00,A,2234.5678,N,inf,E,0,0,030926,,,A,V",
            "GNRMC,120000.00,A,2260.0000,N,11345.1234,E,0,0,030926,,,A,V",
        ]
        array = ",\n".join(f'"{nmea(body)}"' for body in bodies)
        run_c(BASE + RMC + f"""
int main(void) {{
  const char *bad[]={{ {array} }};
  for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {{
    gps_fix_t fix={{0}}; assert(!parse_rmc(bad[i],&fix)); assert(!fix.valid);
  }}
  return 0;
}}
""")

    def test_checksum_rejects_corruption_and_incomplete_sentences(self):
        line = nmea("GNRMC,120000.00,A,2234.5678,N,11345.1234,E,0,0,030926,,,A,V")
        run_c(BASE + source("hex_value", "nmea_checksum_valid") + f"""
int main(void) {{
  (void)now_us;
  char line[]="{line}";
  assert(nmea_checksum_valid(line)); line[10]^=1; assert(!nmea_checksum_valid(line));
  assert(!nmea_checksum_valid("$GNRMC,foo"));
  assert(!nmea_checksum_valid("$GNRMC,foo*"));
  assert(!nmea_checksum_valid("$GNRMC,foo*A"));
  assert(!nmea_checksum_valid("$GNRMC,foo*ZZ"));
  assert(!nmea_checksum_valid(NULL));
  return 0;
}}
""")

    def test_no_fake_fallback_and_invalid_gga_stops_publication(self):
        run_c(BASE + STATE + source("split_csv", "parse_int_range", "update_status_gga") +
              function_source("src/main.c", "get_location_for_send") + """
int main(void) {
  gps_fix_t out={0}, input={.valid=true,.time_valid=true,.lat_deg=22.5,.lon_deg=113.5,
    .year=2026,.month=9,.day=3,.hour=12,.last_fix_time_us=now_us};
  gps_status_t status;
  assert(!get_location_for_send(&out)); // boot with no GNSS
  update_fix(&input,true); assert(get_location_for_send(&out));
  assert(out.lat_deg==22.5 && out.year==2026);
  input.time_valid=false; s_latest_fix.time_valid=false;
  assert(!get_location_for_send(&out));
  input.time_valid=true; update_fix(&input,true);
  update_status_gga("$GNGGA,120000.00,2234.0000,N,11345.0000,E,0,00,99.99,0.0,M,0.0,M,,");
  assert(!get_location_for_send(&out));
  assert(gps_get_status(&status)); assert(!status.has_lock && status.satellites==0);
  update_fix(&input,true); assert(get_location_for_send(&out));
  update_fix(&input,false); assert(!get_location_for_send(&out));
  assert(!s_latest_fix.time_valid);
  return 0;
}
""")

    def test_gga_altitude_is_parsed_and_survives_the_matching_rmc(self):
        run_c(BASE + STATE + source("split_csv", "parse_int_range",
                                    "update_status_gga") + """
int main(void) {
  gps_fix_t rmc={.valid=true,.time_valid=true,.lat_deg=22.5,.lon_deg=113.5,
    .year=2026,.month=9,.day=3,.hour=12,.last_fix_time_us=now_us};
  update_fix(&rmc,true);
  update_status_gga("$GNGGA,120000.00,2234.0000,N,11345.0000,E,1,12,0.8,123.45,M,0.0,M,,");
  gps_fix_t out;
  assert(gps_get_latest(&out));
  assert(out.altitude_valid && fabs(out.altitude_m-123.45)<1e-9);
  rmc.lon_deg=113.6;
  update_fix(&rmc,true);
  assert(gps_get_latest(&out));
  assert(out.altitude_valid && fabs(out.altitude_m-123.45)<1e-9);
  update_status_gga("$GNGGA,120001.00,2234.0000,N,11345.0000,E,0,00,99.9,0.0,M,0.0,M,,");
  assert(gps_get_latest(&out));
  assert(!out.valid && !out.altitude_valid);
  return 0;
}
""")

    def test_stale_fix_is_not_refreshed_or_sent_by_focus_callback(self):
        run_c(BASE + STATE + '\n#include "config.h"\n' + """
static int sent;
bool config_get_snapshot(const app_config_t *p,app_config_t *out) { *out=*p; return true; }
void config_validate(app_config_t *cfg) { (void)cfg; }
static bool ble_client_send_location(const gps_fix_t *fix) { assert(fix->valid); sent++; return true; }
""" + function_source("src/main.c", "get_location_for_send") +
              function_source("src/main.c", "focus_update_cb") + """
int main(void) {
  app_config_t cfg={.max_gps_age_s=5};
  gps_fix_t input={.valid=true,.time_valid=true,.lat_deg=22.5,.lon_deg=113.5,
    .year=2026,.month=9,.day=3,.hour=12,.last_fix_time_us=now_us};
  update_fix(&input,true); focus_update_cb(&cfg); assert(sent==1);
  now_us+=5000001; focus_update_cb(&cfg); assert(sent==1);
  gps_fix_t out; gps_status_t status;
  assert(gps_get_latest(&out)); assert(out.last_fix_time_us==6000000);
  assert(gps_get_status(&status));
  return 0;
}
""")

    def test_two_second_power_and_three_second_debug_publishers_keep_stale_guard(self):
        boot = function_source("src/main.c", "app_main")
        phase2 = boot.split("#if ALPHALOC_PHASE2\n", 1)[1].split("#else", 1)[0]
        assignments = re.findall(
            r"s_cfg\.gps_interval_ms\s*=\s*[^;]+;", phase2)
        self.assertEqual(len(assignments), 1)
        for power_save, interval_ms, expected_sends in ((1, 2000, 3), (0, 3000, 2)):
            with self.subTest(power_save=power_save, interval_ms=interval_ms):
                self.check_publisher_interval(assignments, power_save, interval_ms, expected_sends)

    def check_publisher_interval(self, assignments, power_save, interval_ms, expected_sends):
        run_c(BASE + STATE + f'\n#define ALPHALOC_POWER_SAVE {power_save}\n' +
              '#include "power_policy.h"\n' +
              f'#define EXPECTED_INTERVAL_MS {interval_ms}\n#define EXPECTED_SENDS {expected_sends}\n' +
              '\n#include "config.h"\n#include <setjmp.h>\n' + """
static app_config_t s_cfg;
static jmp_buf stop;
static int sent, waits;
static int64_t send_times[3];
bool config_get_snapshot(const app_config_t *p, app_config_t *out) { *out=*p; return true; }
void config_validate(app_config_t *cfg) { (void)cfg; }
static bool ble_client_send_location(const gps_fix_t *fix) {
  assert(fix->valid && sent<3); send_times[sent++]=now_us; return true;
}
static void vTaskDelay(uint32_t ms) {
  assert(ms==EXPECTED_INTERVAL_MS);
  if (++waits==4) longjmp(stop,1);
  now_us+=(int64_t)ms*1000;
}
""" + function_source("src/main.c", "get_location_for_send") +
              function_source("src/main.c", "location_publisher_task") + """
int main(void) {
  s_cfg.max_gps_age_s=5;
""" + "\n".join(assignments) + """
  assert(s_cfg.gps_interval_ms==EXPECTED_INTERVAL_MS && s_cfg.max_gps_age_s==5);
  gps_fix_t input={.valid=true,.time_valid=true,.lat_deg=22.5,.lon_deg=113.5,
    .year=2026,.month=9,.day=3,.hour=12,.last_fix_time_us=now_us};
  update_fix(&input,true);
  if (setjmp(stop)==0) location_publisher_task(&s_cfg);
  assert(sent==EXPECTED_SENDS && waits==4);
  for (int i=0; i<sent; i++) {
    assert(send_times[i]==6000000+(int64_t)i*EXPECTED_INTERVAL_MS*1000);
    assert(send_times[i]-6000000<=5000000);
  } // Samples at 6 seconds old or later are never sent in either profile.
  sent=waits=0;
  update_fix(&input,false);
  if (setjmp(stop)==0) location_publisher_task(&s_cfg);
  assert(sent==0 && waits==4); // No fake fallback while waiting for a fix.
  return 0;
}
""")

    def test_phase2_environment_and_uart_idle_polling(self):
        config=configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        flags=config["env:xiao-c6-gnss-debug"]["build_flags"]
        for flag in ("ALPHALOC_FAKE_GPS=0", "ALPHALOC_FAKE_GPS_USB_TIME=0",
                     "ALPHALOC_FAKE_GPS_STEPPING=0", "ALPHALOC_PHASE2=1",
                     "GPS_UART_TX_PIN=16", "GPS_UART_RX_PIN=17", "GPS_UART_BAUD=115200"):
            self.assertIn(flag,flags)
        self.assertNotIn("ALPHALOC_FAKE_GPS=1",flags)
        self.assertIn("ALPHALOC_FAKE_GPS=1",config["env:xiao-c6-debug"]["build_flags"])
        task=function_source("src/gps.c","gps_task")
        self.assertIn("#if !ALPHALOC_PHASE2\n      vTaskDelay",task)
        self.assertIn("#if !ALPHALOC_PHASE2\n              update_time_date",task)


if __name__ == "__main__":
    unittest.main(verbosity=2)
