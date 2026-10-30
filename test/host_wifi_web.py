#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Source-level Web lifecycle/security invariants; RF is covered on-device."""

import configparser
import unittest

from host_phase1 import ROOT, function_source, run_c


class WifiWebChecks(unittest.TestCase):
    def test_power_target_enables_only_usb_cable_gated_web(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        power = config["env:xiao-c6-gnss-power"]["build_flags"]
        self.assertIn("ALPHALOC_WIFI_WEB=1", power)
        self.assertIn("ALPHALOC_USB_HOST_WIFI=1", power)
        self.assertIn("ALPHALOC_BLE_CONFIG=0", power)
        for target in ("env:xiao-c6-debug", "env:xiao-c6-gnss-debug"):
            flags = config[target]["build_flags"]
            self.assertIn("ALPHALOC_WIFI_WEB=0", flags)
            self.assertNotIn("ALPHALOC_USB_HOST_WIFI=1", flags)

    def test_usb_task_uses_cable_signals_and_both_debounce_directions(self):
        task = function_source("src/main.c", "usb_host_wifi_task")
        self.assertIn("usb_serial_jtag_is_connected()", task)
        self.assertIn("USB_SERIAL_JTAG.test.val", task)
        self.assertIn("usb_config_source_allows_wifi", task)
        self.assertIn("usb_config_gate_update", task)
        self.assertIn("wifi_web_start(cfg)", task)
        self.assertIn("wifi_web_stop()", task)
        self.assertIn("wifi_web_has_clients()", task)
        self.assertIn("wifi_idle_timeout_s", task)
        self.assertIn("replug_required", task)
        self.assertNotIn("gpio_get_level", task)
        self.assertNotIn("VBUS", task)

    def test_web_managed_settings_survive_reboot_after_first_save(self):
        boot = function_source("src/main.c", "app_main")
        defaults = boot.split("if (!config_loaded)", 1)[1].split(
            "s_cfg.config_window_s", 1)[0]
        self.assertIn("s_cfg.camera_name_prefix", defaults)
        self.assertIn("s_cfg.camera_mac_prefix", defaults)
        self.assertIn("s_cfg.tz_offset_min", defaults)
        self.assertIn("s_cfg.dst_offset_min", defaults)
        self.assertIn("s_cfg.max_gps_age_s", defaults)
        tail = boot.split("s_cfg.config_window_s", 1)[1]
        self.assertNotIn("s_cfg.max_gps_age_s = 5", tail)

    def test_passwords_are_not_rendered_back_into_the_page(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        root = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1)[0]
        self.assertIn('type=\\"password\\"', root)
        self.assertIn("留空则保持当前密码", root)
        self.assertNotIn("cfg.wifi_pass", root)
        self.assertNotIn("cfg.ap_pass", root)

    def test_chinese_brand_contact_and_pairing_management(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        client = (ROOT / "src/ble_client.c").read_text()
        header = (ROOT / "include/ble_client.h").read_text()
        config = (ROOT / "src/config.c").read_text()
        cmake = (ROOT / "CMakeLists.txt").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        routes = source.split("static esp_err_t register_web_routes", 1)[1].split(
            "#if FRAMENAV_EMBEDDED_TLS", 1
        )[0]

        self.assertIn("帧迹", page)
        self.assertIn("相机配对", page)
        self.assertIn("sqkkyzx@outlook.com", source)
        self.assertIn("ble_client_get_bonds", page)
        self.assertIn("ble_client_pairing_seconds_remaining", page)
        self.assertIn("/pair/start", routes)
        self.assertIn("/bond/delete", routes)
        self.assertIn("ble_client_start_pairing", source)
        self.assertIn("ble_client_delete_bond", source)
        self.assertIn("ble_client_get_bonds", header)
        self.assertIn("pairing_mode_active", client)
        self.assertIn('"FrameNav"', config)
        self.assertIn('"framenav1234"', config)
        self.assertIn("project(framenav)", cmake)
        self.assertIn("github.com/finfinack/alphaloc", page)
        self.assertIn(">AlphaLoc</a>", page)
        self.assertNotIn("<title>AlphaLoc", page)
        self.assertNotIn('class=\\"brand\\">AlphaLoc', page)
        self.assertNotIn('ble_svc_gap_device_name_set("AlphaLoc")', client)
        self.assertIn("USB 插入后无设备接入自动关闭", page)
        self.assertIn(">5 分钟</option>", page)
        self.assertIn(">15 分钟</option>", page)

    def test_status_page_shows_version_track_usage_and_earliest_record(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn("固件版本：%s", page)
        self.assertIn("轨迹数据：%s", page)
        self.assertIn("最早记录：%s", page)
        self.assertIn("track_store_get_status", page)
        self.assertIn("esp_app_get_description", page)
        self.assertRegex(cmake, r'set\(PROJECT_VER "\d+\.\d+\.\d+"\)')

    def test_live_selected_location_not_raw_gps_or_stepped_time(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        handler = source.split("static esp_err_t handle_location", 1)[1].split("static esp_err_t handle_root", 1)[0]
        self.assertIn("location_service_details", handler)
        self.assertIn("format_track_time(observed", handler)
        self.assertIn("Cache-Control", handler)
        self.assertNotIn("location_service_set_phone", handler)
        self.assertIn("setInterval(liveLocation,3000)", source)
        self.assertIn("textContent=d[k]", source)
        self.assertIn("无有效定位（尚未获取或已过期）", handler)

    def test_idle_clients_distinguishes_ap_associations_from_sta_uplink(self):
        clients = function_source("src/wifi_web.c", "wifi_web_has_clients")
        self.assertIn("s_ap_active", clients)
        self.assertIn("wifi_web_last_activity_us()", clients)
        self.assertIn("ALPHALOC_WIFI_ACTIVITY_HOLD_US", clients)
        self.assertNotIn("if (s_sta_has_ip) return true", clients)

    def test_default_tls_is_embedded_and_updates_replace_the_pair(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        cmake = (ROOT / "src/CMakeLists.txt").read_text()
        self.assertIn("FRAMENAV_EMBEDDED_TLS", source)
        self.assertIn("start_https_server", source)
        self.assertIn("https://framenav.igp.wiki/download/certs.zip", source)
        self.assertIn("tls_credentials_from_zip", source)
        self.assertIn("tls_credentials_validate", source)
        self.assertIn("esp_http_client_is_complete_data_received", source)
        self.assertIn("config.prvtkey_pem = s_tls_key", source)
        self.assertIn("cert_store_write", source)
        self.assertIn("scripts/embed_tls_cert.py", cmake)
        self.assertIn("--key", cmake)

    def test_bond_form_token_parser_round_trips_display_order(self):
        run_c(
            """
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
"""
            + function_source("src/wifi_web.c", "hex_nibble")
            + function_source("src/wifi_web.c", "parse_bond_token")
            + """
int main(void) {
  uint8_t address[6] = {0};
  assert(parse_bond_token("020000123456", address));
  const uint8_t expected[6] = {0x56, 0x34, 0x12, 0x00, 0x00, 0x02};
  assert(memcmp(address, expected, sizeof(address)) == 0);
  assert(parse_bond_token("aabbccddeeff", address));
  assert(address[0] == 0xFF && address[5] == 0xAA);
  assert(!parse_bond_token("02:00:00:12:34:56", address));
  assert(!parse_bond_token("02000012345Z", address));
  assert(!parse_bond_token("", address));
  return 0;
}
"""
        )

    def test_bond_list_learns_and_renders_camera_identity(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        client = (ROOT / "src/ble_client.c").read_text()
        header = (ROOT / "include/ble_client.h").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]

        self.assertIn("BLE_CLIENT_CAMERA_NAME_MAX", header)
        self.assertIn("bonds[i].name", page)
        self.assertIn("未识别的 Sony 相机", page)
        self.assertIn("camera_name_save", client)
        self.assertIn("camera_name_load", client)
        self.assertIn("camera_name_delete", client)
        self.assertIn("fields.name", client)
        self.assertIn("0x2A24", client)
        self.assertIn("0x2A00", client)
        self.assertIn("ble_gattc_read_by_uuid", client)

        run_c(
            """
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CAMERA_NAME_NVS_KEY_SIZE 16
"""
            + function_source("src/ble_client.c", "camera_name_nvs_key")
            + function_source("src/ble_client.c", "camera_name_copy")
            + """
int main(void) {
  const uint8_t address[6] = {0x56, 0x34, 0x12, 0x00, 0x00, 0x02};
  char key[CAMERA_NAME_NVS_KEY_SIZE];
  camera_name_nvs_key(0, address, key);
  assert(strcmp(key, "n0020000123456") == 0);

  const uint8_t padded[] = "  ILCE-7CM2  ";
  char name[48];
  assert(camera_name_copy(name, sizeof(name), padded, sizeof(padded) - 1));
  assert(strcmp(name, "ILCE-7CM2") == 0);
  const uint8_t controls[] = {'A', '\\n', '7', 0x7F, 'C'};
  assert(camera_name_copy(name, sizeof(name), controls, sizeof(controls)));
  assert(strcmp(name, "A7C") == 0);
  assert(!camera_name_copy(name, sizeof(name), NULL, 0));
  return 0;
}
"""
        )

    def test_repeated_start_stop_keeps_process_global_network_runtime(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        cleanup = function_source("src/wifi_web.c", "wifi_cleanup_resources")
        start = function_source("src/wifi_web.c", "wifi_web_start_unlocked")
        self.assertNotIn("esp_event_loop_delete_default", cleanup)
        self.assertIn("s_netif_initialized", start)
        self.assertIn("s_default_event_loop_ready", start)
        self.assertIn("wifi_web_is_started", source)
        self.assertIn("esp_wifi_deinit", cleanup)

    def test_sta_ip_and_timed_ap_fallback_are_both_exposed(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        readme = (ROOT / "README.md").read_text()
        start = function_source("src/wifi_web.c", "wifi_web_start_unlocked")
        self.assertLess(start.index("if (!start_softap())"),
                        start.index("esp_wifi_start()"))
        self.assertIn("IP_EVENT_STA_GOT_IP", source)
        self.assertIn("wifi_fallback_cb", source)
        self.assertIn("ALPHALOC_WIFI_STA_FALLBACK_TIMEOUT_MS", source)
        self.assertIn('FRAMENAV_HOSTNAME "framenav.device.igp.wiki"', source)
        self.assertIn('ALPHALOC_AP_URL "https://" FRAMENAV_HOSTNAME "/"', source)
        self.assertIn("WIFI_MODE_APSTA", source)
        self.assertIn("同时保留设备热点", source)
        self.assertNotIn("loc.igp.wiki", source)
        self.assertNotIn("192.168.4.1", source)
        self.assertIn("https://framenav.device.igp.wiki/", readme)
        self.assertIn("http://10.254.254.1/", readme)
        self.assertNotIn("loc.igp.wiki", readme)
        self.assertNotIn("192.168.4.1", readme)

    def test_repeated_failed_sta_connections_keep_the_capped_backoff(self):
        handler = function_source("src/wifi_web.c", "wifi_event_handler")
        delay = handler.split(
            "if (s_reconnect_timer && s_sta_configured) {", 1
        )[1].split("esp_timer_stop", 1)[0]
        run_c(
            r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define ALPHALOC_WIFI_RECONNECT_BACKOFF_MS 1500
#define ESP_LOGI(...) ((void)0)
static uint8_t s_sta_retry_count;
static bool s_ap_active;
static uint64_t next_delay(void) {
'''
            + delay
            + r'''
  return delay_us;
}
int main(void) {
  for (int ap = 0; ap < 2; ++ap) {
    s_ap_active = ap != 0;
    s_sta_retry_count = 0;
    uint64_t previous = 0;
    uint64_t base = ap ? 30000000ULL : 1500000ULL;
    for (int attempt = 0; attempt < 30; ++attempt) {
      uint64_t delay = next_delay();
      assert(delay >= previous && delay >= base && delay <= base * 4);
      if (attempt >= 3) assert(delay == base * 4);
      previous = delay;
    }
  }
  return 0;
}
'''
        )

    def test_captive_portal_dns_dhcp_and_http_redirect_lifecycle(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        softap = function_source("src/wifi_web.c", "start_softap")
        configure = function_source(
            "src/wifi_web.c", "configure_softap_network"
        )
        cleanup = function_source("src/wifi_web.c", "wifi_cleanup_resources")
        redirect = source.split(
            "static esp_err_t handle_captive_redirect", 1
        )[1].split("bool wifi_web_start", 1)[0]
        routes = source.split("static esp_err_t register_web_routes", 1)[1].split(
            "#if FRAMENAV_EMBEDDED_TLS", 1
        )[0]
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        self.assertIn("ALPHALOC_AP_URL", softap)
        self.assertIn("captive_dns_start", softap)
        self.assertIn("ESP_NETIF_CAPTIVEPORTAL_URI", configure)
        self.assertLess(
            configure.index("ESP_NETIF_CAPTIVEPORTAL_URI"),
            configure.rindex("esp_netif_dhcps_start"),
        )
        self.assertIn("captive_dns_stop", cleanup)
        self.assertIn('httpd_resp_set_status(req, "303 See Other")', redirect)
        self.assertIn('httpd_resp_set_hdr(req, "Location", ALPHALOC_AP_URL)', redirect)
        self.assertIn("httpd_register_err_handler", routes)
        self.assertIn("ALPHALOC_AP_URL", page)

    def test_softap_uses_high_10_network_and_updates_dhcp_safely(self):
        configure = function_source(
            "src/wifi_web.c", "configure_softap_network"
        )
        self.assertIn("esp_netif_dhcps_get_status", configure)
        self.assertIn("esp_netif_dhcps_stop", configure)
        self.assertIn("dhcp_status != ESP_NETIF_DHCP_STOPPED", configure)
        self.assertIn("IP4_ADDR(&ip_info.ip, 10, 254, 254, 1)", configure)
        self.assertIn("IP4_ADDR(&ip_info.gw, 10, 254, 254, 1)", configure)
        self.assertIn(
            "IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0)", configure
        )
        self.assertIn("esp_netif_set_ip_info", configure)
        self.assertIn("esp_netif_dhcps_start", configure)
        self.assertLess(
            configure.index("esp_netif_dhcps_stop"),
            configure.index("esp_netif_set_ip_info"),
        )
        self.assertLess(
            configure.index("esp_netif_set_ip_info"),
            configure.rindex("esp_netif_dhcps_start"),
        )

    def test_empty_camera_filters_can_be_saved_but_blank_passwords_preserve(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        save = source.split("static esp_err_t handle_save", 1)[1].split(
            "bool wifi_web_start", 1)[0]
        self.assertIn('strstr(body, "cam_name=")', save)
        self.assertIn('strstr(body, "cam_mac=")', save)
        self.assertIn('strstr(body, "wifi_ssid=")', save)
        self.assertIn('strstr(body, "ap_ssid=")', save)
        wifi_pass = save.split('form_get(body, "wifi_pass"')[1].split(
            'form_get(body, "ap_ssid"')[0]
        ap_pass = save.split('form_get(body, "ap_pass"')[1].split(
            'form_get(body, "phone_ttl_h"')[0]
        self.assertIn("if (value[0] != '\\0')", wifi_pass)
        self.assertIn("if (value[0] != '\\0')", ap_pass)

    def test_settings_order_collapsible_power_profiles_and_live_apply(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        save = source.split("static esp_err_t handle_save", 1)[1].split(
            "static esp_err_t handle_pair_start", 1
        )[0]

        self.assertLess(page.index("连接设置"), page.index("定位设置"))
        self.assertLess(page.index("定位设置"), page.index("省电设置"))
        for text in (
            "蓝牙省电模式", "蓝牙省电高级配置", "开机快速搜索持续时间",
            "从手机获取一次定位", "手机位置有效期", "GPS 始终搜星",
            "断联后蓝牙搜索周期", "蓝牙搜索连续监听时长",
        ):
            self.assertIn(text, page)
        self.assertIn('<details id=\\"ble-advanced\\">', page)
        self.assertNotIn('name=\\"loc_mode\\"', page)
        self.assertIn("bi.forEach(e=>e.disabled=m!==2)", page)
        self.assertIn('form_get(body, "ble_mode"', save)
        self.assertIn('form_get(body, "phone_ttl_h"', save)
        self.assertIn("ble_client_config_changed", save)

    def test_large_settings_page_does_not_live_on_http_task_stack(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        start = function_source("src/wifi_web.c", "wifi_web_start_unlocked")
        self.assertNotIn("char bonds_html[4096]", page)
        self.assertIn("char *bonds_html = malloc(bonds_html_len)", page)
        self.assertIn("free(bonds_html)", page)
        self.assertIn("server_cfg.stack_size = 16384", start)


if __name__ == "__main__":
    unittest.main(verbosity=2)
