#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Source-level Web lifecycle/security invariants; RF is covered on-device."""

import configparser
import unittest

from host_phase1 import ROOT, function_source, run_c


class WifiWebChecks(unittest.TestCase):
    def test_power_target_enables_web_without_usb_gating(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        power = config["env:xiao-c6-gnss-power"]["build_flags"]
        self.assertIn("ALPHALOC_WIFI_WEB=1", power)
        self.assertNotIn("ALPHALOC_USB_HOST_WIFI=1", power)
        self.assertNotIn("ALPHALOC_USB_VBUS_GPIO=0", power)
        self.assertIn("ALPHALOC_BLE_CONFIG=1", power)
        self.assertIn("ALPHALOC_BLE_LOCATION=1", power)
        for target in ("env:xiao-c6-debug", "env:xiao-c6-gnss-debug"):
            flags = config[target]["build_flags"]
            self.assertIn("ALPHALOC_WIFI_WEB=0", flags)
            self.assertNotIn("ALPHALOC_USB_HOST_WIFI=1", flags)

    def test_wifi_starts_on_boot_and_usb_data_host_wakes_after_idle(self):
        task = function_source("src/main.c", "wifi_web_task")
        main_source = (ROOT / "src/main.c").read_text()
        self.assertIn("wifi_web_start(cfg)", task)
        self.assertIn("wifi_web_stop()", task)
        self.assertIn("wifi_web_has_clients()", task)
        self.assertIn("previous_wifi_client", task)
        self.assertIn("gps_wifi_connection_changed", task)
        self.assertIn("wifi_idle_timeout_s", task)
        self.assertIn("stopping until a wake source", task)
        self.assertIn("usb_data_host_connected()", task)
        self.assertIn("usb_wake", task)
        self.assertIn("wifi_web_take_wake_request()", task)
        self.assertIn("camera_wake", task)
        self.assertIn("idle_shutdown", task)
        self.assertIn("#define WIFI_IDLE_POLL_MS 2000U", main_source)
        self.assertIn("#define WIFI_STOPPED_POLL_MS 5000U", main_source)
        self.assertIn("WIFI_STOPPED_POLL_MS", task)
        self.assertIn("USB data-host wake", task)
        self.assertNotIn("USB_SERIAL_JTAG", task)
        ble = (ROOT / "src/ble_client.c").read_text()
        self.assertGreaterEqual(ble.count("wifi_web_request_wake()"), 2)
        host = function_source("src/main.c", "usb_data_host_connected")
        self.assertIn("usb_serial_jtag_is_connected()", host)

        boot = function_source("src/main.c", "app_main")
        self.assertIn('xTaskCreate(wifi_web_task, "wifi_web"', boot)
        self.assertIn("s_cfg.config_window_s > 0", boot)

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
        self.assertNotIn("cfg.wifi_pass", root)
        self.assertNotIn("cfg.ap_pass", root)
        self.assertNotIn('name=\\"wifi_ssid\\"', root)
        self.assertNotIn('name=\\"wifi_pass\\"', root)

    def test_chinese_brand_and_pairing_management(self):
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
        self.assertIn("添加设备", page)
        self.assertIn("已配对设备", page)
        self.assertIn("ble_client_get_bonds", page)
        self.assertIn("ble_client_pairing_seconds_remaining", page)
        self.assertIn("/pair/start", routes)
        self.assertIn("/bond/delete", routes)
        self.assertIn("ble_client_start_pairing", source)
        self.assertIn("ble_client_delete_bond", source)
        self.assertIn("ble_client_get_bonds", header)
        self.assertIn("pairing_mode_active", client)
        softap = function_source("src/wifi_web.c", "start_softap")
        self.assertIn('"FrameNav-%02X%02X%02X"', softap)
        self.assertIn("ap_mac[3], ap_mac[4], ap_mac[5]", softap)
        self.assertIn('"framenav"', softap)
        self.assertNotIn('"FrameNav"', config)
        self.assertNotIn('"framenav1234"', config)
        self.assertIn("project(framenav)", cmake)
        self.assertIn("<title>帧迹 FrameNav</title>", page)
        self.assertNotIn("github.com/finfinack/alphaloc", page)
        self.assertNotIn("AlphaLoc", page)
        self.assertNotIn("<title>AlphaLoc", page)
        self.assertNotIn('class=\\"brand\\">AlphaLoc', page)
        self.assertNotIn('ble_svc_gap_device_name_set("AlphaLoc")', client)
        self.assertIn("无人使用时关闭", page)
        self.assertIn(">5 分钟</option>", page)
        self.assertIn(">15 分钟</option>", page)

    def test_external_station_configuration_and_scanning_are_removed(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        config = (ROOT / "include/config.h").read_text()
        persistence = (ROOT / "src/config.c").read_text()
        ble = (ROOT / "src/ble_config_server.c").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        routes = source.split("static esp_err_t register_web_routes", 1)[1].split(
            "#if FRAMENAV_EMBEDDED_TLS", 1
        )[0]
        self.assertNotIn("wifi_ssid", config)
        self.assertNotIn("wifi_pass", config)
        self.assertNotIn('nvs_set_str(nvs, "wifi_ssid"', persistence)
        self.assertNotIn('nvs_set_str(nvs, "wifi_pass"', persistence)
        self.assertNotIn("FIELD_WIFI_SSID", ble)
        self.assertNotIn("FIELD_WIFI_PASS", ble)
        for marker in ("wifi_ssid", "wifi_pass", "wifi/scan", "wifi-scan",
                       "esp_wifi_scan_start", "WIFI_MODE_APSTA",
                       "esp_netif_create_default_wifi_sta", "esp_wifi_connect"):
            self.assertNotIn(marker, source)
        self.assertNotIn('name=\\"wifi_ssid\\"', page)
        self.assertNotIn('name=\\"wifi_pass\\"', page)
        self.assertNotIn("扫描网络", page)
        self.assertNotIn("/wifi/scan", routes)
        self.assertIn("WIFI_MODE_AP", function_source("src/wifi_web.c", "start_softap"))

    def test_ble_config_uuids_match_android_and_expose_compact_bonds(self):
        source = (ROOT / "src/ble_config_server.c").read_text()
        web = (ROOT / "src/wifi_web.c").read_text()
        self.assertIn("BLE_UUID128_INIT(0xA1, 0x7E, 0x00, 0x01", source)
        self.assertIn("BLE_UUID128_INIT(0xA1, 0x7E, 0x00, 0x13", source)
        self.assertIn("BLE_UUID128_INIT(0xA1, 0x7E, 0x00, 0x14", source)
        self.assertIn("FIELD_BONDS_JSON", source)
        self.assertIn("wifi_web_get_bonds_json()", source)
        self.assertIn("wifi_web_get_ble_config_json()", source)
        self.assertIn('cJSON_AddItemToObject(root, "c", camera)', web)
        self.assertIn('cJSON_AddItemToObject(root, "l", location)', web)
        self.assertIn("strlen(json) > 240U", web)
        for characteristic in ("settings_json", "telemetry_json", "bonds_json"):
            self.assertIn(f".uuid = &k_chr_{characteristic}_uuid.u,\n"
                          "             .access_cb = gatt_access_cb,\n"
                          "             .flags = BLE_GATT_CHR_F_READ,", source)
        self.assertNotIn("FIELD_WIFI_SSID", source)
        self.assertNotIn("FIELD_WIFI_PASS", source)
        self.assertIn("wifi_web_get_ble_telemetry_json()", source)

    def test_phone_ble_location_does_not_force_android_pairing(self):
        config = (ROOT / "src/ble_config_server.c").read_text()
        location = (ROOT / "src/ble_location_server.c").read_text()
        self.assertNotIn("BLE_GATT_CHR_F_READ_ENC", config)
        self.assertNotIn("BLE_GATT_CHR_F_WRITE_ENC", location)
        self.assertNotIn("ble_gap_security_initiate(s_conn_handle)", location)

    def test_ble_telemetry_has_a_mtu_sized_guard_and_keeps_verbose_http_shape(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        self.assertIn('cJSON_AddStringToObject(root, "a", active)', source)
        for key in ('"g"', '"p"', '"m"', '"s"'):
            self.assertIn(key, source)
        self.assertIn("strlen(json) > 240U", source)
        self.assertIn("ble_telemetry_add(m, cJSON_CreateBool(manual.clock_pending))", source)
        self.assertIn("telemetry_fail:", source)
        self.assertIn("build_location_json()", source)

    def test_wifi_start_limits_fixed_ap_transmit_power(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        self.assertIn("FRAMENAV_WIFI_MAX_TX_POWER_QDBM 52", source)
        power = function_source("src/wifi_web.c", "configure_wifi_tx_power")
        self.assertIn("esp_wifi_set_max_tx_power", power)
        self.assertIn("13 dBm", source)
        self.assertIn("FRAMENAV_WIFI_MAX_TX_POWER_QDBM 52", source)
        start = function_source("src/wifi_web.c", "wifi_web_start_unlocked")
        self.assertLess(start.index("esp_wifi_start()"),
                        start.index("configure_wifi_tx_power()"))

    def test_http_servers_accept_normal_mobile_browser_headers(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        self.assertIn("FRAMENAV_HTTP_MAX_REQ_HDR_LEN 2048U", source)
        http = function_source("src/wifi_web.c", "wifi_web_start_unlocked")
        self.assertIn("server_cfg.max_req_hdr_len = FRAMENAV_HTTP_MAX_REQ_HDR_LEN", http)
        self.assertNotIn("start_https_server", source)
        self.assertNotIn("FRAMENAV_EMBEDDED_TLS", source)
        self.assertNotIn("https://", source)
        self.assertNotIn("cert_store_", source)
        self.assertNotIn("tls_credentials_", source)

    def test_status_page_shows_track_count_and_internal_storage_usage(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        storage = source.split("static esp_err_t handle_storage", 1)[1].split(
            "static esp_err_t handle_location", 1
        )[0]
        self.assertIn("track_store_get_status", storage)
        self.assertIn('"record_count"', storage)
        self.assertIn('"used_bytes"', storage)
        self.assertIn('"capacity_bytes"', storage)
        self.assertIn("轨迹条数：", page)
        self.assertIn("存储占用：", page)
        self.assertIn('id=\\"storage-record-count\\"', page)
        self.assertIn('id=\\"storage-space\\"', page)
        self.assertIn("fetch('/location'", page)
        self.assertIn('"storage"', source)

    def test_live_selected_location_not_raw_gps_or_stepped_time(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        location = source.split("static cJSON *build_location_json", 1)[1].split("static esp_err_t handle_location", 1)[0]
        self.assertIn("location_service_details", location)
        self.assertIn("location_service_live_status", location)
        self.assertIn("location_service_manual_status", location)
        self.assertIn('"activeSource"', location)
        self.assertIn('"gnss"', location)
        self.assertIn('"phone"', location)
        self.assertIn('"manual"', location)
        self.assertIn('"storage"', location)
        self.assertNotIn("location_service_set_phone", location)
        self.assertIn("Cache-Control", source)
        self.assertIn("setInterval(liveLocation,10000)", source)
        self.assertIn("fetch('/location'", source)
        self.assertNotIn("fetch('/activity'", source)
        self.assertIn("q('#loc-gnss-visible').textContent=d.gnss.satellitesVisible", source)
        self.assertIn("q('#loc-gnss-used').textContent=d.gnss.satellitesUsed", source)
        self.assertIn('id=\\"loc-gnss-visible\\"', source)
        self.assertIn('id=\\"loc-gnss-used\\"', source)
        self.assertIn("暂不可用", source)
        self.assertNotIn("（UTC%c", source)

    def test_no_camera_tracking_controls_and_visible_satellite_count(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        location = source.split("static cJSON *build_location_json", 1)[1].split(
            "static esp_err_t handle_location", 1
        )[0]
        self.assertIn("satellitesVisible", location)
        self.assertIn("satellitesUsed", location)
        self.assertIn("GPS_SEARCH_PAUSED", location)
        self.assertIn("GPS_SEARCH_WAKING", location)
        self.assertIn("搜到卫星：", page)
        self.assertIn("loc-gnss-visible", page)
        self.assertIn("loc-gnss-search", page)
        self.assertIn("GNSS 定位", page)
        self.assertIn("手机定位", page)
        self.assertIn("手动设置经纬度", page)
        self.assertIn('name=\\\"track_idle\\\"', page)
        self.assertIn('name=\\\"track_int_s\\\"', page)
        self.assertIn('name=\\\"source_order\\\"', page)
        self.assertIn('name=\\\"phone_push_enabled\\\"', page)
        self.assertIn('name=\\\"phone_interval_s\\\"', page)
        save = source.split("static esp_err_t handle_save", 1)[1].split(
            "static cJSON *read_phone_json", 1
        )[0]
        self.assertIn('source_gnss_enabled', save)
        self.assertIn('source_phone_enabled', save)
        self.assertIn('source_manual_enabled', save)
        self.assertIn('form_get(body, "source_order"', save)
        self.assertIn('track_enabled', save)
        self.assertIn('phone_push_enabled', save)
        self.assertIn('form_get(body, "phone_interval_s"', save)

    def test_idle_clients_keeps_only_ap_associations_and_recent_http_activity(self):
        clients = function_source("src/wifi_web.c", "wifi_web_has_clients")
        self.assertIn("s_ap_active", clients)
        self.assertIn("wifi_web_last_activity_us()", clients)
        self.assertIn("ALPHALOC_WIFI_ACTIVITY_HOLD_US", clients)
        self.assertNotIn("if (s_sta_has_ip) return true", clients)

    def test_webui_uses_plain_http_without_certificate_management(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        self.assertIn('#define ALPHALOC_AP_URL "http://10.254.254.1/"', source)
        self.assertNotIn("FRAMENAV_EMBEDDED_TLS", source)
        self.assertNotIn("start_https_server", source)
        self.assertNotIn("https://", source)
        self.assertNotIn("/cert/update", source)
        self.assertNotIn("tls_credentials", source)
        self.assertNotIn("cert_store", source)

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
        self.assertIn("FrameNav 管理手机", page)
        self.assertIn("推送相机", page)
        self.assertIn("<small>%s · MAC %s</small>", page)
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

    def test_fixed_ap_is_started_without_sta_or_timed_fallback(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        readme = (ROOT / "README.md").read_text()
        start = function_source("src/wifi_web.c", "wifi_web_start_unlocked")
        self.assertLess(start.index("if (!start_softap())"),
                        start.index("esp_wifi_start()"))
        self.assertIn('#define ALPHALOC_AP_URL "http://10.254.254.1/"', source)
        self.assertNotIn("FRAMENAV_HOSTNAME", source)
        self.assertIn("WIFI_MODE_AP", source)
        self.assertNotIn("WIFI_MODE_APSTA", source)
        self.assertNotIn("IP_EVENT_STA_GOT_IP", source)
        self.assertNotIn("wifi_fallback_cb", source)
        self.assertNotIn("已连外部网络", source)
        self.assertNotIn("loc.igp.wiki", source)
        self.assertNotIn("192.168.4.1", source)
        self.assertIn("http://10.254.254.1/", readme)
        self.assertNotIn("loc.igp.wiki", readme)
        self.assertNotIn("192.168.4.1", readme)

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
        self.assertIn('"FrameNav-%02X%02X%02X"', softap)
        self.assertIn('"framenav"', softap)
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
        self.assertIn('"/config"', routes)
        self.assertIn('"/storage"', routes)
        registered_routes = routes.count("httpd_register_uri_handler(server,")
        self.assertGreaterEqual(registered_routes, 16)
        self.assertIn("server_cfg.max_uri_handlers = 24;", source)
        self.assertNotIn("ALPHALOC_AP_URL", page)
        self.assertNotIn("10.254.254.1", page)

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

    def test_camera_filters_can_be_saved_without_network_credentials(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        save = source.split("static esp_err_t handle_save", 1)[1].split(
            "bool wifi_web_start", 1)[0]
        self.assertIn('strstr(body, "cam_name=")', save)
        self.assertIn('strstr(body, "cam_mac=")', save)
        self.assertNotIn('form_get(body, "wifi_ssid"', save)
        self.assertNotIn('form_get(body, "wifi_pass"', save)
        self.assertNotIn("wifi_apply_sta_config", save)
        self.assertNotIn('form_get(body, "ap_ssid"', save)
        self.assertNotIn('form_get(body, "ap_pass"', save)

    def test_settings_order_collapsible_power_profiles_and_live_apply(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        save = source.split("static esp_err_t handle_save", 1)[1].split(
            "static esp_err_t handle_pair_start", 1
        )[0]

        self.assertLess(page.index("连接配置"), page.index("定位配置"))
        for text in (
            "已配对设备", "管理手机", "推送相机", "热点运行时间", "蓝牙省电",
            "定位来源和优先级", "GNSS 定位", "手机定位", "手动设置经纬度",
            "轨迹记录开启", "在未连接相机时也记录轨迹", "轨迹记录频率",
            "照片时区", "夏时令", "手机定位获取频率", "使用真实定位持续更新",
        ):
            self.assertIn(text, page)
        self.assertNotIn("设备 IP", page)
        self.assertNotIn("设备地址", page)
        self.assertNotIn("更新本地证书", page)
        self.assertIn('<details id=\\"ble-advanced\\">', page)
        self.assertNotIn('name=\\"loc_mode\\"', page)
        self.assertIn("bi.forEach(e=>e.disabled=m!==2)", page)
        manual_row = page.split('class=\\"manual-location\\"', 1)[1].split(
            'id=\\"manual-dirty\\"', 1
        )[0]
        self.assertIn('id=\\"manual-lat\\"', manual_row)
        self.assertIn('id=\\"manual-lon\\"', manual_row)
        self.assertIn('form_get(body, "ble_mode"', save)
        self.assertIn('source_order', save)
        self.assertIn('manual_dirty', save)
        self.assertIn('phone_interval_s', save)
        self.assertIn('"/location/clock"', source)
        self.assertIn("ble_client_config_changed", save)

    def test_large_settings_page_does_not_live_on_http_task_stack(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        page = source.split("static esp_err_t handle_root", 1)[1].split(
            "static esp_err_t handle_save", 1
        )[0]
        start = function_source("src/wifi_web.c", "wifi_web_start_unlocked")
        self.assertNotIn("char bonds_html[4096]", page)
        self.assertIn("char *phone_bonds_html = malloc(bonds_html_len)", page)
        self.assertIn("char *camera_bonds_html = malloc(bonds_html_len)", page)
        self.assertIn("free(phone_bonds_html)", page)
        self.assertIn("free(camera_bonds_html)", page)
        self.assertIn("server_cfg.stack_size = 16384", start)


if __name__ == "__main__":
    unittest.main(verbosity=2)
