#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Verify Web power-mode presets resolve to the intended runtime policy."""

import unittest

from host_phase1 import ROOT, function_source, run_c


SOURCE = (
    '#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n'
    '#include "config.h"\n'
    + function_source("src/config.c", "config_resolve_location_power")
    + "\n"
    + function_source("src/config.c", "config_resolve_bluetooth_power")
)


class ConfigProfileChecks(unittest.TestCase):
    def test_location_presets_and_custom_values(self):
        run_c(SOURCE + r"""
int main(void) {
  app_config_t cfg={0};
  location_power_policy_t p={0};

  cfg.location_power_mode=LOCATION_POWER_SAVING;
  config_resolve_location_power(&cfg,&p);
  assert(!p.continuous && p.boot_work_s==45 && p.disconnect_grace_s==10);
  assert(p.stationary_sleep_s==300 && p.moving_sleep_s==120);
  assert(p.fast_sleep_s==30 && p.fast_speed_kmh_x10==288);
  assert(p.fix_timeout_s==30);

  cfg.location_power_mode=LOCATION_POWER_BALANCED;
  config_resolve_location_power(&cfg,&p);
  assert(!p.continuous && p.boot_work_s==45 && p.disconnect_grace_s==20);
  assert(p.stationary_sleep_s==90 && p.moving_sleep_s==30);
  assert(p.fast_sleep_s==15 && p.fast_speed_kmh_x10==288);
  assert(p.fix_timeout_s==45);

  cfg.location_power_mode=LOCATION_POWER_TRACK;
  config_resolve_location_power(&cfg,&p);
  assert(p.continuous);

  cfg.location_power_mode=LOCATION_POWER_CUSTOM;
  cfg.custom_boot_work_s=11; cfg.custom_disconnect_grace_s=12;
  cfg.custom_stationary_sleep_s=101; cfg.custom_moving_sleep_s=41;
  cfg.custom_fast_sleep_s=13; cfg.custom_fast_speed_kmh_x10=543;
  cfg.custom_fix_timeout_s=27;
  config_resolve_location_power(&cfg,&p);
  assert(!p.continuous && p.boot_work_s==11 && p.disconnect_grace_s==12);
  assert(p.stationary_sleep_s==101 && p.moving_sleep_s==41);
  assert(p.fast_sleep_s==13 && p.fast_speed_kmh_x10==543);
  assert(p.fix_timeout_s==27);
  return 0;
}
""")

    def test_bluetooth_presets_and_custom_values(self):
        run_c(SOURCE + r"""
int main(void) {
  app_config_t cfg={0};
  bluetooth_power_policy_t p={0};

  cfg.bluetooth_power_mode=BLUETOOTH_POWER_SAVING;
  config_resolve_bluetooth_power(&cfg,&p);
  assert(p.boot_fast_search_s==15 && p.scan_period_ms==10000);
  assert(p.scan_window_ms==100);

  cfg.bluetooth_power_mode=BLUETOOTH_POWER_NORMAL;
  config_resolve_bluetooth_power(&cfg,&p);
  assert(p.boot_fast_search_s==30 && p.scan_period_ms==3000);
  assert(p.scan_window_ms==100);

  cfg.bluetooth_power_mode=BLUETOOTH_POWER_CUSTOM;
  cfg.custom_ble_boot_fast_search_s=17;
  cfg.custom_ble_scan_period_ms=2250;
  cfg.custom_ble_scan_window_ms=75;
  config_resolve_bluetooth_power(&cfg,&p);
  assert(p.boot_fast_search_s==17 && p.scan_period_ms==2250);
  assert(p.scan_window_ms==75);
  return 0;
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
