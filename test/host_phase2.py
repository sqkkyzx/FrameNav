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
#define GPS_GSV_TALKER_COUNT 7U
#define GPS_GSV_FRESH_US 15000000LL
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
static uint8_t s_gsv_satellites[GPS_GSV_TALKER_COUNT];
static int64_t s_gsv_seen_us[GPS_GSV_TALKER_COUNT];
static int s_fix_mutex=1;
static int64_t s_last_no_fix_log_us;
""" + source("update_fix", "gps_get_latest", "visible_satellites_now", "gps_get_status")


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

    def test_invalid_gga_and_missing_utc_invalidate_the_raw_fix(self):
        run_c(BASE + STATE + source("split_csv", "parse_int_range", "update_status_gga") + """
int main(void) {
  gps_fix_t out={0}, input={.valid=true,.time_valid=true,.lat_deg=22.5,.lon_deg=113.5,
    .year=2026,.month=9,.day=3,.hour=12,.last_fix_time_us=now_us};
  gps_status_t status;
  assert(gps_get_latest(&out) && !out.valid); // boot with no GNSS
  update_fix(&input,true); assert(gps_get_latest(&out) && out.valid && out.time_valid);
  assert(out.lat_deg==22.5 && out.year==2026);
  input.time_valid=false; s_latest_fix.time_valid=false;
  assert(gps_get_latest(&out) && !out.time_valid);
  input.time_valid=true; update_fix(&input,true);
  update_status_gga("$GNGGA,120000.00,2234.0000,N,11345.0000,E,0,00,99.99,0.0,M,0.0,M,,");
  assert(gps_get_latest(&out) && !out.valid);
  assert(gps_get_status(&status)); assert(!status.has_lock && status.satellites==0);
  update_fix(&input,true); assert(gps_get_latest(&out) && out.valid && out.time_valid);
  update_fix(&input,false); assert(gps_get_latest(&out) && !out.valid);
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

    def test_gsv_visible_satellites_sums_constellations_and_expires(self):
        run_c(BASE + STATE + source(
            "parse_int_range", "split_csv", "gsv_talker_index",
            "update_status_gsv") + """
int main(void) {
  (void)update_fix;
  gps_status_t status;
  update_status_gsv("$GPGSV,2,1,08,01,40,100,35");
  update_status_gsv("$GLGSV,1,1,07,65,35,120,32");
  update_status_gsv("$GBGSV,1,1,04,201,25,090,28");
  update_status_gsv("$BDGSV,1,1,06,211,20,080,30");
  assert(gps_get_status(&status));
  assert(status.satellites_visible == 21); // BeiDou aliases share one stream.

  update_status_gsv("$GNGSV,1,1,17,01,40,100,35");
  assert(gps_get_status(&status));
  assert(status.satellites_visible == 17); // Prefer the combined stream.

  now_us += GPS_GSV_FRESH_US + 1;
  assert(gps_get_status(&status));
  assert(status.satellites_visible == 0);
  assert(gsv_talker_index("$GPGGA,anything") == -1);
  update_status_gsv("$GPGSV,0,0,99");
  assert(gps_get_status(&status) && status.satellites_visible == 0);
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
        self.assertIn("update_status_gsv(line_buf)", task)
        self.assertIn("#if !ALPHALOC_PHASE2\n      vTaskDelay",task)
        self.assertIn("#if !ALPHALOC_PHASE2\n              update_time_date",task)


if __name__ == "__main__":
    unittest.main(verbosity=2)
