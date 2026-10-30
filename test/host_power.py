#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise actual opt-in power/scan policy in C; no serial or RF access."""

import configparser
import re
import unittest

from host_phase1 import PRELUDE, ROOT, function_source, run_c


def policy(enabled=True):
    return PRELUDE + f"\n#define ALPHALOC_POWER_SAVE {int(enabled)}\n" + \
        '#include "power_policy.h"\n'


def scan_code(enabled=True):
    code = policy(enabled) + """
#define BLE_HS_FOREVER INT32_MAX
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
struct ble_gap_disc_params { uint8_t passive, filter_duplicates; uint16_t itvl, window; };
static bool s_connecting_camera, connected, scan_active;
static uint8_t s_own_addr_type;
static int64_t now_us, pairing_until_us;
static app_config_t config_storage;
static const app_config_t *s_cfg=&config_storage;
static bluetooth_power_policy_t scan_policy={30,3000,100};
bool config_get_snapshot(const app_config_t *cfg, app_config_t *out) {
    assert(cfg==s_cfg && out); *out=*cfg; return true;
}
void config_set_defaults(app_config_t *cfg) { memset(cfg,0,sizeof(*cfg)); }
void config_resolve_bluetooth_power(const app_config_t *cfg,
                                    bluetooth_power_policy_t *out) {
    (void)cfg; *out=scan_policy;
}
#define esp_timer_get_time() (now_us)
static int64_t pairing_deadline_get(void) { return pairing_until_us; }
#if ALPHALOC_POWER_SAVE
#define MYNEWT_VAL(name) 4
typedef struct { uint8_t type, val[6]; } ble_addr_t;
static int bond_count=1, bond_rc, bond_lookups;
static int ble_store_util_bonded_peers(ble_addr_t *peers, int *num, int max_peers) {
    assert(peers && num && max_peers==4);
    bond_lookups++;
    if (bond_rc) return bond_rc;
    *num=bond_count;
    return 0;
}
#endif
static int starts, scan_rc;
static int32_t last_duration;
static struct ble_gap_disc_params last_params;
static bool ble_client_is_connected(void) { return connected; }
static bool ble_gap_disc_active(void) { return scan_active; }
static int ble_client_gap_event_cb(void *a, void *b) { (void)a; (void)b; return 0; }
static int ble_gap_disc(uint8_t own, int32_t duration, struct ble_gap_disc_params *params,
                       int (*cb)(void *, void *), void *arg) {
    (void)own; (void)arg; assert(cb==ble_client_gap_event_cb);
    assert(duration>0 && params->window<=params->itvl);
    assert(params->itvl>=4 && params->itvl<=0x4000);
    assert(params->window>=4 && params->window<=0x4000);
    starts++; last_duration=duration; last_params=*params;
    if (scan_rc==0) scan_active=true;
    return scan_rc;
}
"""
    names = ["scan_has_saved_bonds", "scan_ms_to_units"] if enabled else []
    names += ["configure_scan_params", "ble_start_scan"]
    if enabled:
        names.append("ble_scan_complete")
    return code + "\n".join(function_source("src/ble_client.c", n) for n in names)


class PowerChecks(unittest.TestCase):
    def test_web_pairing_mode_temporarily_forces_continuous_scan(self):
        run_c(scan_code() + """
int main(void) {
    bond_count=1; now_us=60000000; pairing_until_us=150000000;
    ble_start_scan();
    assert(starts==1 && last_duration==90000);
    assert(last_params.itvl==16 && last_params.window==16);
    scan_active=false; now_us=149999999; ble_start_scan();
    assert(last_duration==1 && last_params.itvl==16 && last_params.window==16);
    scan_active=false; now_us=150000000; ble_start_scan();
    assert(last_duration==BLE_HS_FOREVER);
    assert(last_params.itvl==4800 && last_params.window==160);
    ble_scan_complete(123);
    return 0;
}
""")

    def test_bonded_boot_is_300ms_per_second_until_mcu_uptime_30_seconds(self):
        run_c(scan_code() + """
int main(void) {
    now_us=2000000; ble_start_scan();
    assert(starts==1 && last_duration==28000 && bond_lookups==1);
    assert(last_params.itvl==1600 && last_params.window==480);
    assert(!last_params.passive && last_params.filter_duplicates);
    // A failed connection uses only the REMAINING MCU boot period.
    scan_active=false; now_us=10000000; ble_start_scan();
    assert(starts==2 && last_duration==20000 && last_params.itvl==1600);
    scan_active=false; now_us=29999999; ble_start_scan();
    assert(last_duration==1); // ceil fractional ms, never a zero/forever boot scan
    assert(last_params.window==480 && last_params.itvl==1600);
    scan_active=false; now_us=30000000; ble_scan_complete(0);
    assert(starts==4 && last_duration==BLE_HS_FOREVER);
    assert(last_params.itvl==4800 && last_params.window==160);
    assert(!last_params.passive && last_params.filter_duplicates);
    return 0;
}
""")

    def test_scan_completion_cannot_interrupt_connection_or_duplicate_scan(self):
        run_c(scan_code() + """
int main(void) {
    now_us=1000000; ble_start_scan(); assert(starts==1);
    ble_scan_complete(0); assert(starts==1); // scan still active
    scan_active=false; s_connecting_camera=true;
    ble_scan_complete(0); assert(starts==1);
    s_connecting_camera=false; connected=true;
    ble_scan_complete(0); assert(starts==1);
    connected=false; ble_scan_complete(123); assert(starts==1); // preempted
    assert(bond_lookups==1); // guards also prevent unnecessary store reads
    // A late disconnect or host resync must NOT get a new boot grace period.
    now_us=120000000; ble_start_scan();
    assert(starts==2 && last_duration==BLE_HS_FOREVER);
    assert(last_params.itvl==4800 && last_params.window==160);
    scan_active=false; now_us=121000000; ble_start_scan();
    assert(starts==3 && last_duration==BLE_HS_FOREVER && last_params.itvl==4800);
    assert(last_params.window==160);
    return 0;
}
""")

    def test_failed_scan_start_does_not_mark_scan_active_or_renew_budget(self):
        run_c(scan_code() + """
int main(void) {
    now_us=0; scan_rc=7; ble_start_scan();
    assert(starts==1 && !scan_active && last_duration==30000);
    assert(last_params.itvl==1600 && last_params.window==480);
    now_us=50000000; scan_rc=0; ble_scan_complete(0);
    assert(starts==2 && scan_active && last_params.itvl==4800);
    assert(last_duration==BLE_HS_FOREVER && last_params.window==160);
    return 0;
}
""")

    def test_debug_profile_keeps_forever_full_duty_scan(self):
        run_c(scan_code(False) + """
int main(void) {
    now_us=3600000000LL; (void)now_us;
    ble_start_scan();
    assert(starts==1 && last_duration==BLE_HS_FOREVER);
    assert(last_params.itvl==16 && last_params.window==16);
    assert(!last_params.passive && last_params.filter_duplicates);
    return 0;
}
""")

    def test_no_bonds_means_continuous_scanning_even_long_after_boot(self):
        run_c(scan_code() + """
int main(void) {
    bond_count=0;
    const int64_t times[]={0, 2000000, 29999999, 30000000, 120000000, 3600000000LL};
    for (unsigned i=0; i<sizeof(times)/sizeof(times[0]); i++) {
        scan_active=false; now_us=times[i]; ble_start_scan();
        assert(last_duration==BLE_HS_FOREVER);
        assert(last_params.itvl==16 && last_params.window==16);
        assert(!last_params.passive && last_params.filter_duplicates);
    }
    ble_scan_complete(0); // A stale completion must not duplicate the active scan.
    assert(starts==6 && bond_lookups==6);
    return 0;
}
""")

    def test_any_saved_bond_never_uses_continuous_scan_at_any_boot_age(self):
        run_c(scan_code() + """
int main(void) {
    for (bond_count=1; bond_count<=4; bond_count++) {
        for (now_us=0; now_us<=60000000; now_us+=1000000) {
            scan_active=false; ble_start_scan();
            assert(last_params.window==(now_us<30000000 ? 480 : 160));
            assert(last_params.window<last_params.itvl);
            assert(last_params.itvl==(now_us<30000000 ? 1600 : 4800));
            assert(last_duration==(now_us<30000000 ?
                                   (30000000-now_us)/1000 : BLE_HS_FOREVER));
        }
    }
    ble_scan_complete(0);
    return 0;
}
""")

    def test_first_pairing_then_disconnect_uses_saved_bonds_without_reboot(self):
        run_c(scan_code() + """
int main(void) {
    bond_count=0; now_us=2000000; ble_start_scan();
    assert(last_params.itvl==16 && last_duration==BLE_HS_FOREVER);
    // Discovery cancels scanning; pairing saves a bond while connected.
    scan_active=false; connected=true; bond_count=1; now_us=8000000;
    ble_start_scan(); assert(starts==1);
    connected=false; now_us=10000000; ble_start_scan();
    assert(starts==2 && last_params.itvl==1600 && last_params.window==480);
    assert(last_duration==20000);
    // A later disconnect stays at 1/30 duty, despite the link no longer existing.
    scan_active=false; now_us=90000000; ble_start_scan();
    ble_scan_complete(0);
    assert(starts==3 && last_params.itvl==4800 && last_params.window==160);
    assert(last_duration==BLE_HS_FOREVER);
    return 0;
}
""")

    def test_clearing_last_bond_restores_continuous_pairing_discovery(self):
        run_c(scan_code() + """
int main(void) {
    now_us=60000000; ble_start_scan();
    assert(last_params.itvl==4800 && last_params.window==160);
    // Existing pairing-recovery code may remove an invalid bond.
    scan_active=false; bond_count=0; ble_start_scan();
    ble_scan_complete(0);
    assert(starts==2 && last_params.itvl==16 && last_params.window==16);
    assert(last_duration==BLE_HS_FOREVER);
    return 0;
}
""")

    def test_bond_lookup_failure_never_falls_back_to_continuous_scan(self):
        run_c(scan_code() + """
int main(void) {
    bond_rc=7; bond_count=0; now_us=1000000; ble_start_scan();
    assert(last_params.itvl==1600 && last_params.window==480 && last_duration==29000);
    scan_active=false; now_us=90000000; ble_start_scan();
    assert(last_params.itvl==4800 && last_params.window==160);
    assert(last_duration==BLE_HS_FOREVER);
    // Only a successful empty-store lookup permits continuous discovery.
    scan_active=false; bond_rc=0; ble_start_scan();
    ble_scan_complete(0);
    assert(last_params.itvl==16 && last_params.window==16);
    return 0;
}
""")

    def test_runtime_bluetooth_profile_changes_scan_duration_and_duty(self):
        run_c(scan_code() + """
int main(void) {
    scan_policy=(bluetooth_power_policy_t){15,10000,100};
    now_us=14999999; ble_start_scan();
    assert(last_duration==1 && last_params.itvl==1600 && last_params.window==480);
    scan_active=false; now_us=15000000; ble_start_scan();
    assert(last_duration==BLE_HS_FOREVER);
    assert(last_params.itvl==16000 && last_params.window==160);

    scan_active=false;
    scan_policy=(bluetooth_power_policy_t){0,2250,75};
    ble_start_scan();
    assert(last_duration==BLE_HS_FOREVER);
    assert(last_params.itvl==3600 && last_params.window==120);
    ble_scan_complete(0); // stale completion is ignored while scan is active
    return 0;
}
""")

    def test_power_management_enables_dfs_without_cpu_light_sleep(self):
        run_c(policy() + """
#define CONFIG_PM_ENABLE 1
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 0
#define ALPHALOC_LIGHT_SLEEP 0
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
static esp_pm_config_t seen;
static int calls;
static int esp_pm_configure(const esp_pm_config_t *cfg) { seen=*cfg; calls++; return 0; }
""" + function_source("src/main.c", "configure_power_management") + """
int main(void) {
    configure_power_management();
    assert(calls==1 && seen.max_freq_mhz==80 && seen.min_freq_mhz==40);
    assert(!seen.light_sleep_enable);
    return 0;
}
""")

    def test_uart_and_led_use_frequency_scaling_independent_clocks(self):
        uart = re.search(r"uart_config_t uart_cfg = \{.*?\n  \};",
                         function_source("src/gps.c", "gps_init"), re.S).group()
        led = re.search(r"const ledc_timer_config_t timer = \{.*?\n  \};",
                        function_source("src/main.c", "xiao_status_led_task"), re.S).group()
        run_c(policy() + """
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_XTAL 40
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_8_BIT 8
#define LEDC_TIMER_0 0
#define LEDC_USE_XTAL_CLK 40
typedef struct { int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk; } uart_config_t;
typedef struct { int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg; } ledc_timer_config_t;
static gps_config_t s_cfg={.baud_rate=115200};
int main(void) {
""" + uart + "\n" + led + """
    assert(uart_cfg.source_clk==UART_SCLK_XTAL && uart_cfg.baud_rate==115200);
    assert(timer.clk_cfg==LEDC_USE_XTAL_CLK && timer.freq_hz==2000);
    assert(timer.duty_resolution==8);
    return 0;
}
""")

    def test_power_profile_rate_limits_gnss_summary_not_gnss_updates(self):
        run_c(policy() + """
#include <inttypes.h>
static int64_t now_us, s_last_uart_log_us;
static uint32_t s_uart_bytes=100, s_nmea_ok=2, s_nmea_bad=0;
static int logs;
#define esp_timer_get_time() (now_us)
#define ESP_LOGI(tag, fmt, ...) do { printf(fmt, __VA_ARGS__); logs++; } while(0)
bool gps_get_latest(gps_fix_t *fix) { fix->last_fix_time_us=1; return true; }
bool gps_get_status(gps_status_t *status) { status->satellites=8; return true; }
""" + function_source("src/gps.c", "log_uart_status") + """
int main(void) {
    now_us=5000000; log_uart_status(); assert(logs==0);
    now_us=29999999; log_uart_status(); assert(logs==0);
    now_us=30000000; log_uart_status(); assert(logs==1);
    now_us=59000000; log_uart_status(); assert(logs==1);
    now_us=60000000; log_uart_status(); assert(logs==2);
    return 0;
}
""")

    def test_opt_in_build_keeps_real_gnss_observer_and_debug_rollback(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        env = config["env:xiao-c6-gnss-power"]
        flags = env["build_flags"]
        for flag in ("ALPHALOC_POWER_SAVE=1", "ALPHALOC_PHASE2=1", "ALPHALOC_PHASE1=0",
                     "ALPHALOC_FAKE_GPS=0", "ALPHALOC_FAKE_GPS_USB_TIME=0",
                     "ALPHALOC_FAKE_GPS_STEPPING=0", "ALPHALOC_LOG_NMEA=0",
                     "ALPHALOC_VERBOSE=0", "ALPHALOC_LIGHT_SLEEP=0",
                     "ALPHALOC_WIFI_WEB=1",
                     "ALPHALOC_LOCATION_OBSERVER=1",
                     "GPS_UART_TX_PIN=19", "GPS_UART_RX_PIN=17", "GPS_UART_BAUD=115200"):
            self.assertIn(flag, flags)
        self.assertNotIn("ALPHALOC_USB_HOST_WIFI=1", flags)
        self.assertNotIn("ALPHALOC_USB_VBUS_GPIO=0", flags)
        self.assertEqual(env["build_type"], "release")
        self.assertIn("sdkconfig.xiao-c6.defaults;sdkconfig.xiao-c6-power.defaults",
                      env["board_build.cmake_extra_args"])
        defaults = (ROOT / "sdkconfig.xiao-c6-power.defaults").read_text()
        for setting in ("CONFIG_PM_ENABLE=y", "CONFIG_BT_LE_SLEEP_ENABLE=y",
                        "CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL=y",
                        "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80=y",
                        "CONFIG_FREERTOS_USE_TICKLESS_IDLE=n"):
            self.assertIn(setting, defaults)
        debug_flags = config["env:xiao-c6-gnss-debug"]["build_flags"]
        self.assertNotIn("ALPHALOC_POWER_SAVE=1", debug_flags)
        self.assertIn("ALPHALOC_LOG_NMEA=1", debug_flags)
        boot = function_source("src/main.c", "app_main")
        self.assertIn("s_cfg.gps_interval_ms = ALPHALOC_LOCATION_INTERVAL_MS;", boot)
        self.assertIn("s_cfg.max_gps_age_s = 5;", boot)
        self.assertIn("ble_start_scan();", function_source("src/ble_client.c", "ble_on_sync"))
        callback = function_source("src/ble_client.c", "ble_client_gap_event_cb")
        self.assertIn("ble_scan_complete(event->disc_complete.reason);", callback)
        disconnect = callback.split("case BLE_GAP_EVENT_DISCONNECT:")[1].split("case BLE_GAP_EVENT_DISC_COMPLETE:")[0]
        self.assertIn("ble_start_scan();", disconnect)
        ble_source = (ROOT / "src/ble_client.c").read_text()
        self.assertNotIn("ble_restart_fast_scan", ble_source)
        self.assertNotIn("s_scan_fast_until_us", ble_source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
