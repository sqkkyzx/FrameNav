#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Compile actual v7 clock/reader functions; test host protocol without USB."""

import importlib.util
from types import SimpleNamespace
import unittest

from host_phase1 import ROOT, PRELUDE, function_source, run_c

CORE = PRELUDE + """
#define ALPHALOC_FAKE_GPS_USB_TIME 1
#define ALPHALOC_FAKE_GPS_STEPPING 1
#define ALPHALOC_FAKE_GPS 1
#include "fake_gps_usb_time.h"
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int s_clock_mux;
static bool s_clock_ready;
static int64_t s_clock_epoch_us;
static int64_t s_clock_uptime_us;
""" + function_source("src/fake_gps_clock.c", "gps_fake_set_usb_time") + \
    function_source("src/fake_gps_clock.c", "gps_fake_apply_clock")


class UsbTimeChecks(unittest.TestCase):
    def test_strict_nonce_date_and_message_validation(self):
        run_c(PRELUDE + '#include "fake_gps_usb_time.h"\n' +
              function_source("src/fake_gps_usb_time.c", "gps_fake_parse_usb_time") + """
int main(void) {
    int64_t epoch=99;
    const uint64_t nonce=0x0123456789abcdefULL;
    assert(gps_fake_parse_usb_time("ALPHALOC_TIME_SET_V1 0123456789abcdef 1788437492123",nonce,&epoch));
    assert(epoch==1788437492123LL);
    assert(gps_fake_parse_usb_time("ALPHALOC_TIME_SET_V1 0123456789ABCDEF 1788437492123",nonce,&epoch));
    const char *bad[]={
      "ALPHALOC_TIME_SET_V1 0123456789abcdee 1788437492123",
      "ALPHALOC_TIME_SET_V1 0123456789abcdef 1788437492",
      "ALPHALOC_TIME_SET_V1 0123456789abcdef 17884374921234",
      "ALPHALOC_TIME_SET_V1 0123456789abcdef -788437492123",
      "ALPHALOC_TIME_SET_V1 0123456789abcdef 178843749212x",
      "ALPHALOC_TIME_SET_V1 0123456789abcdef 1577836799999",
      "ALPHALOC_TIME_SET_V1 0123456789abcdef 4102444800000",
      "ALPHALOC_TIME_SET_V1 0123456789abcdef 1788437492123 extra",
      "ALPHALOC_TIME_SET_V1 0123456789abcdeg 1788437492123",
      "OTHER_TIME_SET_V1 0123456789abcdef 1788437492123",
    };
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
      epoch=99;
      assert(!gps_fake_parse_usb_time(bad[i],nonce,&epoch)); assert(epoch==99);
    }
    assert(!gps_fake_parse_usb_time(NULL,nonce,&epoch));
    assert(!gps_fake_parse_usb_time(bad[0],nonce,NULL));
    assert(!gps_fake_parse_usb_time("ALPHALOC_TIME_SET_V1 0000000000000000 1788437492123",0,&epoch));
    return 0;
}
""")

    def test_unsynced_never_publishes_and_usb_anchor_survives_host_exit_not_reset(self):
        run_c(CORE + """
int main(void) {
    gps_fix_t fix={.time_valid=true};
    assert(!gps_fake_apply_clock(&fix,601000000)); assert(!fix.time_valid);
    assert(!gps_fake_set_usb_time(ALPHALOC_USB_TIME_MIN_MS-1,0));
    assert(!gps_fake_set_usb_time(ALPHALOC_USB_TIME_MAX_MS,0));
    assert(!gps_fake_set_usb_time(1788437492123LL,-1));
    assert(gps_fake_set_usb_time(1788437492123LL,5000000));
    assert(!gps_fake_set_usb_time(1788437492123LL-600000,6000000)); // once per boot
    assert(!gps_fake_apply_clock(&fix,4999999));
    assert(gps_fake_apply_clock(&fix,5000000));
    assert(fix.year==2026 && fix.month==9 && fix.day==3);
    assert(fix.hour==12 && fix.minute==11 && fix.second==32);
    assert(gps_fake_apply_clock(&fix,5876999)); assert(fix.second==32);
    assert(gps_fake_apply_clock(&fix,5877000)); assert(fix.second==33);
    assert(gps_fake_apply_clock(&fix,65000000)); assert(fix.minute==12 && fix.second==32);
    assert(!gps_fake_apply_clock(&fix,INT64_MAX));
    s_clock_ready=false; // new boot's zero-initialized validity, not an RTC
    assert(!gps_fake_apply_clock(&fix,1000000));
    assert(gps_fake_set_usb_time(1788438092123LL,1000000)); // fresh host time +10min
    assert(gps_fake_apply_clock(&fix,1000000)); assert(fix.minute==21 && fix.second==32);
    return 0;
}
""")

    def test_actual_latest_and_fallback_paths_both_wait_for_host(self):
        run_c(CORE + """
#define pdTRUE 1
#define pdMS_TO_TICKS(value) (value)
#define xSemaphoreTake(mutex, ticks) (1)
#define xSemaphoreGive(mutex) ((void)0)
#define FAKE_LAT_DEG 48.137154
#define FAKE_LON_DEG 11.576124
#define FAKE_YEAR 2024
#define FAKE_MONTH 1
#define FAKE_DAY 1
#define FAKE_HOUR 12
#define FAKE_MINUTE 0
#define FAKE_SECOND 0
static int64_t now_us=9000000;
#define esp_timer_get_time() (now_us)
static int s_fix_mutex=1;
static gps_fix_t s_latest_fix;
static gps_status_t s_status;
""" + function_source("src/gps.c", "gps_get_latest") +
              function_source("src/main.c", "get_location_for_send") + """
int main(void) {
    gps_fix_t fix={0};
    s_latest_fix.lat_deg=FAKE_LAT_DEG;
    s_latest_fix.lon_deg=FAKE_LON_DEG;
    assert(!gps_get_latest(&fix));
    assert(!get_location_for_send(&fix));
    s_fix_mutex=0; assert(!get_location_for_send(&fix)); // fallback cannot send 2024
    assert(gps_fake_set_usb_time(1788437492000LL,now_us));
    assert(get_location_for_send(&fix)); // fallback after sync
    assert(fix.valid && fix.time_valid && fix.year==2026 && fix.minute==11);
    s_fix_mutex=1; now_us+=60000000;
    assert(gps_get_latest(&fix)); // fake init invalid -> valid without reboot
    assert(fix.valid && fix.time_valid && s_status.has_lock);
    assert(fix.minute==12 && fix.second==32 && fix.lat_deg==FAKE_LAT_DEG);
    s_clock_ready=false; assert(!get_location_for_send(&fix));
    return 0;
}
""")

    def test_host_only_targets_expected_board_and_emits_matching_wire_format(self):
        spec=importlib.util.spec_from_file_location("usb_time_monitor", ROOT / "scripts/usb_time_monitor.py")
        monitor=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(monitor)
        self.assertEqual(monitor.time_command("0123456789abcdef",1788437492123),
                         b"ALPHALOC_TIME_SET_V1 0123456789abcdef 1788437492123\n")
        for nonce,epoch in [("0000000000000000",1788437492123),("xyz",1788437492123),
                            ("0123456789abcdef",1788437492),("0123456789abcdef",4102444800000)]:
            with self.assertRaises(ValueError): monitor.time_command(nonce,epoch)
        board=SimpleNamespace(device="/dev/cu.usbmodem3101",vid=0x303A,pid=0x1001,
                              serial_number="02:00:00:12:34:56")
        self.assertTrue(monitor.matching_device(board.device,board.serial_number,[board]))
        self.assertFalse(monitor.matching_device("/dev/cu.other",board.serial_number,[board]))
        self.assertFalse(monitor.matching_device(board.device,"OTHER",[board]))
        board.vid=0x1234
        self.assertFalse(monitor.matching_device(board.device,board.serial_number,[board]))

    def test_xiao_build_does_not_seed_clock_from_compile_time(self):
        config=(ROOT / "platformio.ini").read_text().split("[env:xiao-c6-debug]",1)[1]
        self.assertIn("ALPHALOC_FAKE_GPS_USB_TIME=1",config)
        self.assertNotIn("extra_scripts",config)

    def test_usb_console_installs_rx_backend_before_nonblocking_read_task(self):
        run_c(PRELUDE + """
#include <fcntl.h>
#include <unistd.h>
typedef int esp_err_t;
#define ESP_OK 0
#define pdPASS 1
#define ESP_LOGE(...) ((void)0)
typedef struct { unsigned tx_buffer_size; unsigned rx_buffer_size; } usb_serial_jtag_driver_config_t;
static bool installed;
static int install_calls, use_calls, task_calls, flags, install_result;
static bool usb_serial_jtag_is_driver_installed(void) { return installed; }
static int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *c) {
  assert(c->rx_buffer_size>=64 && c->tx_buffer_size>=256);
  install_calls++;
  if (install_result) return install_result;
  installed=true; return ESP_OK;
}
static void usb_serial_jtag_vfs_use_driver(void) { assert(installed); use_calls++; }
static int mock_fcntl(int fd,int op,...) {
  assert(fd==STDIN_FILENO && use_calls);
  if (op==F_GETFL) return flags;
  assert(op==F_SETFL); return 0;
}
#define fcntl mock_fcntl
static void usb_time_task(void *arg) { (void)arg; }
static int xTaskCreate(void (*fn)(void *),const char *name,int stack,void *arg,int priority,void *handle) {
  (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
  assert(fn==usb_time_task && installed && use_calls); task_calls++; return pdPASS;
}
""" + function_source("src/fake_gps_usb_time.c", "gps_fake_usb_time_start") + """
int main(void) {
  assert(gps_fake_usb_time_start());
  assert(install_calls==1 && use_calls==1 && task_calls==1);
  // Separate startup scenario: existing driver is reused, not installed twice.
  assert(gps_fake_usb_time_start()); assert(install_calls==1);
  flags=-1; assert(!gps_fake_usb_time_start()); assert(task_calls==2);
  installed=false; install_result=-1;
  assert(!gps_fake_usb_time_start()); assert(task_calls==2);
  return 0;
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
