#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Host checks for the 16-byte internal-flash track format and integration."""

import configparser
import unittest
from datetime import datetime, timezone

from host_phase1 import PRELUDE, ROOT, function_source, run_c


FORMAT_SOURCE = (
    function_source("src/track_format.c", "track_epoch_from_fix")
    + function_source("src/track_format.c", "track_record_from_fix")
    + function_source("src/track_format.c", "track_record_is_valid")
)


class TrackStoreChecks(unittest.TestCase):
    def test_four_value_record_is_exactly_16_bytes(self):
        expected = int(datetime(2026, 9, 5, 12, 34, 56,
                                tzinfo=timezone.utc).timestamp())
        run_c(PRELUDE + '\n#include "track_store.h"\n' + FORMAT_SOURCE + f"""
int main(void) {{
  assert(sizeof(track_record_t)==16);
  gps_fix_t fix={{.valid=true,.time_valid=true,.altitude_valid=true,
    .lat_deg=48.137154,.lon_deg=11.576124,.altitude_m=123.45,
    .year=2026,.month=9,.day=5,.hour=12,.minute=34,.second=56}};
  track_record_t record;
  assert(track_record_from_fix(&fix,&record));
  assert(record.timestamp_utc=={expected}U);
  assert(record.latitude_e7==481371540);
  assert(record.longitude_e7==115761240);
  assert(record.altitude_cm==12345);
  assert(track_record_is_valid(&record));
  fix.altitude_valid=false;
  assert(track_record_from_fix(&fix,&record));
  assert(record.altitude_cm==TRACK_ALTITUDE_UNKNOWN);
  assert(track_record_is_valid(&record));
  return 0;
}}
""")

    def test_invalid_date_and_coordinates_are_rejected(self):
        run_c(PRELUDE + '\n#include "track_store.h"\n' + FORMAT_SOURCE + """
int main(void) {
  gps_fix_t fix={.valid=true,.time_valid=true,.lat_deg=0,.lon_deg=0,
    .year=2026,.month=2,.day=29,.hour=0};
  track_record_t record;
  assert(!track_record_from_fix(&fix,&record));
  fix.year=2028; assert(track_record_from_fix(&fix,&record));
  fix.lat_deg=91; assert(!track_record_from_fix(&fix,&record));
  fix.lat_deg=0; fix.lon_deg=-181;
  assert(!track_record_from_fix(&fix,&record));
  return 0;
}
""")

    def test_production_partition_and_three_second_task_are_enabled(self):
        partitions = (ROOT / "partitions.csv").read_text()
        self.assertIn("track,    data, 0x40,    0x200000,0x177000,", partitions)
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        flags = config["env:xiao-c6-gnss-power"]["build_flags"]
        self.assertIn("ALPHALOC_TRACK_STORE=1", flags)
        main = (ROOT / "src/main.c").read_text()
        store = (ROOT / "src/track_store.c").read_text()
        self.assertIn("track_store_init()", main)
        self.assertIn("track_store_task", main)
        self.assertIn("TRACK_STORE_INTERVAL_S", store)
        task = function_source("src/track_store.c", "track_store_task")
        self.assertIn("snapshot.track_interval_s * 1000U", task)
        self.assertIn("snapshot.track_enabled", task)
        self.assertIn("poll_ms > 5000U", task)
        self.assertIn("offsetof(track_record_t, timestamp_utc)", store)


if __name__ == "__main__":
    unittest.main(verbosity=2)
