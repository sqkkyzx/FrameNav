/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_web.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ble_client.h"
#include "captive_dns.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "gps.h"
#include "lwip/ip4_addr.h"
#include "track_store.h"

#ifndef ALPHALOC_BATTERY_MONITOR
#define ALPHALOC_BATTERY_MONITOR 0
#endif

#if ALPHALOC_BATTERY_MONITOR
#include "battery.h"
#endif

static const char *TAG = "wifi_web";

static httpd_handle_t s_server;
static app_config_t *s_cfg;
static esp_netif_t *s_netif = NULL;
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;
static const char *constellation_to_str(gps_constellation_t mask) {
  if (mask == (GPS_CONSTELLATION_GPS | GPS_CONSTELLATION_GLONASS)) {
    return "gps+glonass";
  }
  if (mask == GPS_CONSTELLATION_GPS) {
    return "gps";
  }
  if (mask == GPS_CONSTELLATION_GLONASS) {
    return "glonass";
  }
  return "none";
}
static volatile bool s_started;
static bool s_wifi_handlers_registered = false;
static bool s_ip_handlers_registered = false;
static bool s_netif_initialized = false;
static bool s_default_event_loop_ready = false;
static int64_t s_sta_start_us = 0;
static bool s_ap_active = false;
static bool s_sta_has_ip = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static bool start_softap(void);

#ifndef ALPHALOC_WIFI_STA_FALLBACK_TIMEOUT_MS
#define ALPHALOC_WIFI_STA_FALLBACK_TIMEOUT_MS 15000
#endif

#ifndef ALPHALOC_WIFI_RECONNECT_BACKOFF_MS
#define ALPHALOC_WIFI_RECONNECT_BACKOFF_MS 1500
#endif

#define ALPHALOC_AP_URL "http://10.254.254.1/"
#define FRAMENAV_CONTACT_EMAIL "sqkkyzx@outlook.com"
#define FRAMENAV_PAIRING_WINDOW_S 90U

static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_fallback_timer = NULL;
static captive_dns_handle_t s_captive_dns = NULL;
static uint8_t s_sta_retry_count = 0;

static bool configure_softap_network(esp_netif_t *netif) {
  esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
  esp_err_t err = esp_netif_dhcps_get_status(netif, &dhcp_status);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read AP DHCP status: %s", esp_err_to_name(err));
    return false;
  }

  if (dhcp_status != ESP_NETIF_DHCP_STOPPED) {
    err = esp_netif_dhcps_stop(netif);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to stop AP DHCP server: %s",
               esp_err_to_name(err));
      return false;
    }
  }

  esp_netif_ip_info_t ip_info = {0};
  IP4_ADDR(&ip_info.ip, 10, 254, 254, 1);
  IP4_ADDR(&ip_info.gw, 10, 254, 254, 1);
  IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
  err = esp_netif_set_ip_info(netif, &ip_info);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set AP network to 10.254.254.0/24: %s",
             esp_err_to_name(err));
    (void)esp_netif_dhcps_start(netif);
    return false;
  }

  // RFC 8910 DHCP option 114 gives newer clients the portal URI directly.
  // The URI string is static because ESP-IDF retains its pointer.
  err = esp_netif_dhcps_option(
      netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
      (void *)ALPHALOC_AP_URL, strlen(ALPHALOC_AP_URL));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set captive portal DHCP option: %s",
             esp_err_to_name(err));
    (void)esp_netif_dhcps_start(netif);
    return false;
  }

  err = esp_netif_dhcps_start(netif);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start AP DHCP server: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

static void wifi_cleanup_resources(void) {
  // Make timer/event callbacks no-ops before tearing down their resources.
  s_started = false;
  if (s_captive_dns) {
    captive_dns_stop(s_captive_dns);
    s_captive_dns = NULL;
  }
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
  }
  if (s_reconnect_timer) {
    esp_timer_stop(s_reconnect_timer);
    esp_timer_delete(s_reconnect_timer);
    s_reconnect_timer = NULL;
  }
  if (s_fallback_timer) {
    esp_timer_stop(s_fallback_timer);
    esp_timer_delete(s_fallback_timer);
    s_fallback_timer = NULL;
  }
  if (s_wifi_handlers_registered) {
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                 &wifi_event_handler);
    s_wifi_handlers_registered = false;
  }
  if (s_ip_handlers_registered) {
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 &wifi_event_handler);
    s_ip_handlers_registered = false;
  }
  (void)esp_wifi_stop();
  (void)esp_wifi_deinit();
  if (s_netif_sta) {
    esp_netif_destroy(s_netif_sta);
    s_netif_sta = NULL;
  }
  if (s_netif_ap) {
    esp_netif_destroy(s_netif_ap);
    s_netif_ap = NULL;
  }
  s_netif = NULL;
  // esp-netif is process-global and cannot be deinitialized. Keep the default
  // event loop too, so a later USB reconnect can start WiFi again safely.
  s_ap_active = false;
  s_sta_has_ip = false;
  s_sta_retry_count = 0;
}

static void wifi_reconnect_cb(void *arg) {
  (void)arg;
  if (s_started && !s_ap_active) {
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "WiFi reconnect start failed: %s", esp_err_to_name(err));
    }
  }
}

static void wifi_fallback_cb(void *arg) {
  (void)arg;
  if (s_started && !s_ap_active && !s_sta_has_ip) {
    ESP_LOGW(TAG, "STA unavailable after 15s; starting AP fallback");
    if (!start_softap()) {
      ESP_LOGE(TAG, "AP fallback failed; web remains unavailable until retry");
    }
  }
}

static bool start_softap(void) {
  app_config_t cfg;
  if (!config_get_snapshot(s_cfg, &cfg)) {
    cfg = *s_cfg;
    config_validate(&cfg);
  }
  if (!s_netif_ap) {
    s_netif_ap = esp_netif_create_default_wifi_ap();
    if (!s_netif_ap) {
      ESP_LOGE(TAG, "Failed to create AP network interface");
      return false;
    }
  }
  if (!configure_softap_network(s_netif_ap)) {
    return false;
  }
  wifi_config_t wifi_cfg = {0};
  strncpy((char *)wifi_cfg.ap.ssid, cfg.ap_ssid,
          sizeof(wifi_cfg.ap.ssid) - 1);
  wifi_cfg.ap.ssid_len = strlen((char *)wifi_cfg.ap.ssid);
  strncpy((char *)wifi_cfg.ap.password, cfg.ap_pass,
          sizeof(wifi_cfg.ap.password) - 1);
  wifi_cfg.ap.authmode = strlen((char *)wifi_cfg.ap.password) == 0
                             ? WIFI_AUTH_OPEN
                             : WIFI_AUTH_WPA2_PSK;
  wifi_cfg.ap.max_connection = 4;

  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to switch to AP mode: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure AP: %s", esp_err_to_name(err));
    return false;
  }
  if (!s_captive_dns) {
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_netif_ap, &ip_info) == ESP_OK) {
      s_captive_dns = captive_dns_start(ip_info.ip.addr);
    }
    if (!s_captive_dns) {
      ESP_LOGW(TAG, "Captive DNS unavailable; direct URL still works");
    }
  }
  s_ap_active = true;
  s_sta_has_ip = false;
  if (s_reconnect_timer) (void)esp_timer_stop(s_reconnect_timer);
  if (s_fallback_timer) (void)esp_timer_stop(s_fallback_timer);
  ESP_LOGI(TAG, "Started AP fallback: ssid=%s captive=%s", cfg.ap_ssid,
           ALPHALOC_AP_URL);
  return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (!s_started) {
    return;
  }
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
    s_sta_has_ip = true;
    if (s_fallback_timer) (void)esp_timer_stop(s_fallback_timer);
    if (got_ip) {
      ESP_LOGI(TAG, "WiFi web ready: http://" IPSTR "/",
               IP2STR(&got_ip->ip_info.ip));
    }
    return;
  }
  if (event_base != WIFI_EVENT) {
    return;
  }

  if (event_id == WIFI_EVENT_STA_CONNECTED) {
    s_sta_retry_count = 0;
    ESP_LOGI(TAG, "WiFi STA connected");
    return;
  }

  if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_sta_has_ip = false;
    const wifi_event_sta_disconnected_t *disc =
        (const wifi_event_sta_disconnected_t *)event_data;
    unsigned reason = disc ? (unsigned)disc->reason : 0;
    int64_t elapsed_ms = 0;
    if (s_sta_start_us > 0) {
      elapsed_ms = (esp_timer_get_time() - s_sta_start_us) / 1000;
    }

    ESP_LOGW(TAG, "WiFi disconnected, reason=%u", reason);

    if (elapsed_ms >= ALPHALOC_WIFI_STA_FALLBACK_TIMEOUT_MS && !s_ap_active) {
      ESP_LOGW(TAG, "STA connect timed out, falling back to AP");
      start_softap();
      return;
    }

    if (s_reconnect_timer) {
      uint64_t delay_us =
          (uint64_t)ALPHALOC_WIFI_RECONNECT_BACKOFF_MS * 1000ULL;
      if (s_sta_retry_count < 5) {
        delay_us *= (uint64_t)(s_sta_retry_count + 1);
        s_sta_retry_count++;
      }
      esp_timer_stop(s_reconnect_timer);
      esp_timer_start_once(s_reconnect_timer, delay_us);
    }
  }
}

static bool parse_i16_range(const char *value, int16_t min, int16_t max,
                            int16_t *out) {
  if (!value || value[0] == '\0') {
    return false;
  }
  char *end = NULL;
  long v = strtol(value, &end, 10);
  if (end == value || *end != '\0' || v < min || v > max) {
    return false;
  }
  *out = (int16_t)v;
  return true;
}

static bool parse_u32_range(const char *value, uint32_t max, uint32_t *out) {
  if (!value || value[0] == '\0') {
    return false;
  }
  char *end = NULL;
  unsigned long v = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || v > max) {
    return false;
  }
  *out = (uint32_t)v;
  return true;
}

static bool parse_decimal_scaled(const char *value, double scale,
                                 uint32_t min, uint32_t max, uint32_t *out) {
  if (!value || value[0] == '\0' || !out) {
    return false;
  }
  char *end = NULL;
  double parsed = strtod(value, &end);
  if (end == value || *end != '\0' || !isfinite(parsed) || parsed < 0.0) {
    return false;
  }
  double scaled_value = parsed * scale;
  if (scaled_value < (double)min || scaled_value > (double)max) {
    return false;
  }
  *out = (uint32_t)(scaled_value + 0.5);
  return true;
}

static void url_decode(char *dst, const char *src, size_t dst_len) {
  size_t di = 0;
  for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
    if (src[si] == '+') {
      dst[di++] = ' ';
    } else if (src[si] == '%' && src[si + 1] && src[si + 2]) {
      if (isxdigit((unsigned char)src[si + 1]) &&
          isxdigit((unsigned char)src[si + 2])) {
        char hex[3] = {src[si + 1], src[si + 2], '\0'};
        dst[di++] = (char)strtoul(hex, NULL, 16);
        si += 2;
      }
    } else {
      dst[di++] = src[si];
    }
  }
  dst[di] = '\0';
}

static void form_get(const char *body, const char *key, char *out,
                     size_t out_len) {
  const char *p = body;
  size_t key_len = strlen(key);
  while (p && *p) {
    const char *eq = strchr(p, '=');
    const char *amp = strchr(p, '&');
    if (!eq) {
      break;
    }
    size_t klen = (size_t)(eq - p);
    if (klen == key_len && strncmp(p, key, key_len) == 0) {
      size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
      char tmp[CONFIG_STR_MAX_64];
      if (vlen >= sizeof(tmp)) {
        vlen = sizeof(tmp) - 1;
      }
      memcpy(tmp, eq + 1, vlen);
      tmp[vlen] = '\0';
      url_decode(out, tmp, out_len);
      return;
    }
    if (!amp) {
      break;
    }
    p = amp + 1;
  }
  out[0] = '\0';
}

static void html_escape_attr(char *dst, size_t dst_len, const char *src) {
  size_t di = 0;
  if (!dst || dst_len == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
    const char *rep = NULL;
    switch (src[si]) {
    case '&':
      rep = "&amp;";
      break;
    case '"':
      rep = "&quot;";
      break;
    case '<':
      rep = "&lt;";
      break;
    case '>':
      rep = "&gt;";
      break;
    default:
      dst[di++] = src[si];
      continue;
    }
    size_t rep_len = strlen(rep);
    if (di + rep_len >= dst_len) {
      break;
    }
    memcpy(dst + di, rep, rep_len);
    di += rep_len;
  }
  dst[di] = '\0';
}

static int hex_nibble(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static bool parse_bond_token(const char *token, uint8_t out[6]) {
  if (!token || !out || strlen(token) != 12) {
    return false;
  }
  for (size_t display_index = 0; display_index < 6; ++display_index) {
    int high = hex_nibble(token[display_index * 2]);
    int low = hex_nibble(token[display_index * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    // Human-readable BLE addresses are the reverse of NimBLE's byte array.
    out[5 - display_index] = (uint8_t)((high << 4) | low);
  }
  return true;
}

static esp_err_t redirect_to_root(httpd_req_t *req) {
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, "返回 FrameNav 配置页");
}

static bool page_append(char *page, size_t page_len, size_t *used,
                        const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static bool page_append(char *page, size_t page_len, size_t *used,
                        const char *format, ...) {
  if (!page || !used || *used >= page_len) {
    return false;
  }
  va_list args;
  va_start(args, format);
  int written = vsnprintf(page + *used, page_len - *used, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= page_len - *used) {
    return false;
  }
  *used += (size_t)written;
  return true;
}

static bool format_track_time(uint32_t timestamp_utc, int16_t tz_offset_min,
                              int16_t dst_offset_min, char *out,
                              size_t out_len) {
  if (!timestamp_utc || !out || out_len == 0) return false;
  const int offset_min = (int)tz_offset_min + (int)dst_offset_min;
  const int64_t local_seconds =
      (int64_t)timestamp_utc + (int64_t)offset_min * 60LL;
  time_t local_time = (time_t)local_seconds;
  struct tm civil;
  if (!gmtime_r(&local_time, &civil)) return false;
  const char sign = offset_min < 0 ? '-' : '+';
  const int abs_offset = offset_min < 0 ? -offset_min : offset_min;
  int written = snprintf(out, out_len,
                         "%04d-%02d-%02d %02d:%02d:%02d（UTC%c%02d:%02d）",
                         civil.tm_year + 1900, civil.tm_mon + 1, civil.tm_mday,
                         civil.tm_hour, civil.tm_min, civil.tm_sec, sign,
                         abs_offset / 60, abs_offset % 60);
  return written > 0 && (size_t)written < out_len;
}

static esp_err_t handle_root(httpd_req_t *req) {
  app_config_t cfg;
  if (!config_get_snapshot(s_cfg, &cfg)) {
    cfg = *s_cfg;
    config_validate(&cfg);
  }
  const size_t page_len = 32768;
  char *page = malloc(page_len);
  if (!page) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "No memory");
  }
  gps_status_t gps_status = {0};
  const char *gps_const_str = "未知星座";
  if (gps_get_status(&gps_status)) {
    gps_const_str = constellation_to_str(gps_status.constellations);
  }
  bool cam_connected = ble_client_is_connected();
  bool cam_bonded = ble_client_is_bonded();
  const char *cam_dot_class =
      cam_connected ? (cam_bonded ? "dot-green" : "dot-blue") : "dot-red";
  const char *cam_conn_str = cam_connected ? "已连接" : "未连接";
  const char *cam_bond_str = cam_bonded ? "已配对" : "未完成配对";
  const char *gps_dot_class = gps_status.has_lock ? "dot-green" : "dot-red";
  const char *gps_lock_str = gps_status.has_lock ? "定位有效" : "等待定位";
  const esp_app_desc_t *app_desc = esp_app_get_description();
  const char *firmware_version =
      app_desc && app_desc->version[0] ? app_desc->version : "未知";
  track_store_status_t track_status = {0};
  char track_usage[96] = "不可用";
  char track_earliest[96] = "暂无记录";
  if (track_store_get_status(&track_status)) {
    const double used_kib = (double)track_status.used_bytes / 1024.0;
    const double capacity_kib = (double)track_status.capacity_bytes / 1024.0;
    const double percent = track_status.capacity_bytes
                               ? (double)track_status.used_bytes * 100.0 /
                                     (double)track_status.capacity_bytes
                               : 0.0;
    snprintf(track_usage, sizeof(track_usage),
             "%.1f / %.0f KiB（%.1f%%）· %u 条", used_kib, capacity_kib,
             percent, (unsigned)track_status.record_count);
    if (track_status.earliest_timestamp_utc != 0) {
      (void)format_track_time(track_status.earliest_timestamp_utc,
                              cfg.tz_offset_min, cfg.dst_offset_min,
                              track_earliest, sizeof(track_earliest));
    }
  }
#if ALPHALOC_BATTERY_MONITOR
  battery_status_t bat = {0};
  bool bat_ok = battery_get_status(&bat) && bat.valid;
  const char *bat_dot_class = "dot-gray";
  char bat_text[64] = "电池：未知";
  if (bat_ok) {
    if (bat.percent > 50.0f) {
      bat_dot_class = "dot-green";
    } else if (bat.percent > 30.0f) {
      bat_dot_class = "dot-yellow";
    } else {
      bat_dot_class = "dot-red";
    }
    snprintf(bat_text, sizeof(bat_text), "电池：%.0f%%（%.2fV）",
             (double)bat.percent, (double)bat.voltage_v);
  }
#endif
  char camera_name[CONFIG_STR_MAX_32 * 6];
  char camera_mac[CONFIG_STR_MAX_18 * 6];
  char wifi_ssid[CONFIG_STR_MAX_32 * 6];
  char ap_ssid[CONFIG_STR_MAX_32 * 6];
  html_escape_attr(camera_name, sizeof(camera_name), cfg.camera_name_prefix);
  html_escape_attr(camera_mac, sizeof(camera_mac), cfg.camera_mac_prefix);
  html_escape_attr(wifi_ssid, sizeof(wifi_ssid), cfg.wifi_ssid);
  html_escape_attr(ap_ssid, sizeof(ap_ssid), cfg.ap_ssid);

  ble_client_bond_t bonds[BLE_CLIENT_MAX_BONDS] = {0};
  int bond_count = ble_client_get_bonds(bonds, BLE_CLIENT_MAX_BONDS);
  const size_t bonds_html_len = 4096;
  char *bonds_html = malloc(bonds_html_len);
  if (!bonds_html) {
    free(page);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "No memory");
  }
  size_t bonds_used = 0;
  bonds_html[0] = '\0';
  if (bond_count < 0) {
    bonds_used = (size_t)snprintf(
        bonds_html, bonds_html_len,
        "<p class=\"muted\">暂时无法读取配对记录，请稍后刷新。</p>");
  } else if (bond_count == 0) {
    bonds_used = (size_t)snprintf(
        bonds_html, bonds_html_len,
        "<p class=\"muted\">尚未保存任何相机配对记录。</p>");
  } else {
    for (int i = 0; i < bond_count && bonds_used < bonds_html_len; ++i) {
      char mac[18];
      char token[13];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               bonds[i].addr[5], bonds[i].addr[4], bonds[i].addr[3],
               bonds[i].addr[2], bonds[i].addr[1], bonds[i].addr[0]);
      snprintf(token, sizeof(token), "%02X%02X%02X%02X%02X%02X",
               bonds[i].addr[5], bonds[i].addr[4], bonds[i].addr[3],
               bonds[i].addr[2], bonds[i].addr[1], bonds[i].addr[0]);
      const char *raw_name = bonds[i].name[0] != '\0'
                                 ? bonds[i].name
                                 : "未识别的 Sony 相机";
      char display_name[BLE_CLIENT_CAMERA_NAME_MAX * 6];
      html_escape_attr(display_name, sizeof(display_name), raw_name);
      int n = snprintf(
          bonds_html + bonds_used, bonds_html_len - bonds_used,
          "<div class=\"bond\"><div><strong>%s</strong>"
          "<small>%s · 地址类型 %u%s</small></div>"
          "<form class=\"inline\" method=\"POST\" action=\"/bond/delete\">"
          "<input type=\"hidden\" name=\"bond\" value=\"%s\">"
          "<input type=\"hidden\" name=\"type\" value=\"%u\">"
          "<button class=\"danger\" type=\"submit\" "
          "onclick=\"return confirm('确定解除这台相机的本机配对记录？')\">"
          "解除配对</button></form></div>",
          display_name, mac, (unsigned)bonds[i].addr_type,
          bonds[i].connected ? " · 当前连接" : "", token,
          (unsigned)bonds[i].addr_type);
      if (n < 0 || (size_t)n >= bonds_html_len - bonds_used) {
        bonds_html[bonds_html_len - 1] = '\0';
        break;
      }
      bonds_used += (size_t)n;
    }
  }

  uint32_t pairing_left = ble_client_pairing_seconds_remaining();
  char pairing_state[128];
  if (pairing_left > 0) {
    snprintf(pairing_state, sizeof(pairing_state),
             "新相机配对模式已开启，剩余约 %u 秒。请同时在相机菜单中进入配对。",
             (unsigned)pairing_left);
  } else {
    snprintf(pairing_state, sizeof(pairing_state),
             "配对模式未开启。已有相机会继续按省电策略自动重连。");
  }

  location_power_policy_t location_policy;
  bluetooth_power_policy_t bluetooth_policy;
  config_resolve_location_power(&cfg, &location_policy);
  config_resolve_bluetooth_power(&cfg, &bluetooth_policy);

  size_t used = 0;
  bool ok = page_append(
      page, page_len, &used,
      "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>帧迹 FrameNav</title><style>"
      "*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
      "margin:0 auto;padding:20px;max-width:720px;color:#17231c;background:#eef3ef}"
      ".brand{font-size:27px;font-weight:780}.brand span{color:#257149}.tagline,.muted,small,.hint{color:#66736b}"
      ".tagline{margin:4px 0 16px}.card{background:#fff;border:1px solid #d7dfd9;border-radius:15px;"
      "padding:18px;margin:13px 0;box-shadow:0 3px 14px rgba(20,50,30,.05)}"
      "h2{font-size:18px;margin:0 0 5px}h3{font-size:14px;margin:18px 0 8px}.caption{font-size:12px;color:#718078;margin:0 0 14px}"
      ".statusbar{display:flex;flex-wrap:wrap;gap:9px 15px;font-size:13px}.statusitem{display:flex;align-items:center;gap:6px}"
      ".dot{width:9px;height:9px;border-radius:50%%}.dot-green{background:#2e9a55}.dot-red{background:#d9534f}"
      ".dot-blue{background:#2f6fdb}.dot-yellow{background:#f0b429}.dot-gray{background:#9aa3af}"
      "label.field{display:block;margin:13px 0 5px;font-weight:650;font-size:13px}"
      "input.text,select{width:100%%;padding:10px;border:1px solid #cbd6ce;border-radius:9px;background:#fff;color:#17231c}"
      "input:disabled{opacity:.62}button{padding:10px 14px;border:0;border-radius:9px;background:#257149;color:#fff;font-weight:700;white-space:nowrap;flex-shrink:0}"
      ".danger{background:#bd3e3e}.secondary{background:#34463b}.bond{display:flex;justify-content:space-between;gap:10px;"
      "align-items:center;padding:11px 0;border-top:1px solid #e6ece8}.bond small{display:block;margin-top:3px}"
      ".inline{margin:0;flex-shrink:0}.bond>div{min-width:0;overflow-wrap:anywhere}.inline input{display:none}.hint{font-size:12px;line-height:1.55}.grid{display:grid;grid-template-columns:1fr 1fr;gap:11px}"
      ".modes{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.mode{display:flex;gap:8px;border:1px solid #d4ddd7;"
      "border-radius:10px;padding:10px;font-size:12px}.mode input{margin-top:2px}.mode strong{display:block;font-size:13px;margin-bottom:3px}"
      ".mode span span{display:block;color:#6c7971;line-height:1.4}details{margin-top:12px;border-top:1px solid #e2e8e4;padding-top:12px}"
      "summary{font-weight:700;font-size:13px;cursor:pointer}.summary-state{float:right;color:#77847c;font-weight:500}"
      ".rule{font-size:12px;line-height:1.55;background:#f0f6f2;border-radius:9px;padding:10px;margin:12px 0}"
      ".unit{display:flex;align-items:center;gap:7px}.unit input{flex:1}.unit span{font-size:12px;color:#68756d;white-space:nowrap}"
      ".divider{height:1px;background:#e2e8e4;margin:18px 0}.savebar{display:flex;justify-content:space-between;align-items:center;gap:12px;"
      "margin:15px 0}.savebar .hint{margin:0}footer{font-size:12px;color:#66736b;margin:18px 2px}a{color:#257149}"
      "@media(max-width:520px){body{padding:12px}.grid,.modes{grid-template-columns:1fr}.bond{align-items:flex-start}}"
      "</style></head><body><div class=\"brand\">帧迹 <span>FrameNav</span></div>"
      "<p class=\"tagline\">独立相机定位设备</p><form id=\"settings\" method=\"POST\" action=\"/save\"></form>"
      "<section class=\"card\"><h2>运行状态</h2><div class=\"statusbar\">"
      "<div class=\"statusitem\"><span class=\"dot %s\"></span><span>GNSS：%s · %u 颗卫星 · %s</span></div>"
      "<div class=\"statusitem\"><span class=\"dot %s\"></span><span>相机：%s · %s</span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>固件版本：%s</span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>轨迹数据：%s</span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>最早记录：%s</span></div>",
      gps_dot_class, gps_lock_str, (unsigned)gps_status.satellites,
      gps_const_str, cam_dot_class, cam_conn_str, cam_bond_str,
      firmware_version, track_usage, track_earliest);
#if ALPHALOC_BATTERY_MONITOR
  ok = ok && page_append(page, page_len, &used,
                         "<div class=\"statusitem\"><span class=\"dot %s\"></span><span>%s</span></div>",
                         bat_dot_class, bat_text);
#endif
  ok = ok && page_append(
      page, page_len, &used,
      "</div></section><section class=\"card\"><h2>连接设置</h2>"
      "<p class=\"caption\">管理相机配对和配置页网络。</p><h3>相机配对</h3>%s"
      "<p class=\"hint\">%s</p><form method=\"POST\" action=\"/pair/start\">"
      "<button class=\"secondary\" type=\"submit\">配对新相机（90 秒）</button></form>"
      "<details><summary>新配对筛选规则</summary><div class=\"rule\">只在手动开始新配对时生效；一般保持为空。</div>"
      "<label class=\"field\">相机名称前缀</label><input class=\"text\" form=\"settings\" name=\"cam_name\" value=\"%s\" "
      "placeholder=\"留空：接受所有受支持的 Sony 相机\">"
      "<label class=\"field\">相机 MAC 前缀</label><input class=\"text\" form=\"settings\" name=\"cam_mac\" value=\"%s\" placeholder=\"通常留空\"></details>"
      "<details><summary>Wi‑Fi 高级配置</summary><h3>接入现有 Wi‑Fi（仅 2.4 GHz）</h3>"
      "<label class=\"field\">Wi‑Fi 名称</label><input class=\"text\" form=\"settings\" name=\"wifi_ssid\" value=\"%s\">"
      "<label class=\"field\">Wi‑Fi 密码</label><input class=\"text\" form=\"settings\" type=\"password\" name=\"wifi_pass\" placeholder=\"留空则保持当前密码\">"
      "<h3>设备备用热点</h3><label class=\"field\">热点名称</label>"
      "<input class=\"text\" form=\"settings\" name=\"ap_ssid\" value=\"%s\">"
      "<label class=\"field\">热点密码</label><input class=\"text\" form=\"settings\" type=\"password\" name=\"ap_pass\" placeholder=\"留空则保持当前密码\">"
      "<p class=\"hint\">仅检测到 USB 数据主机时开启配置网络；固定配置地址为 10.254.254.1。</p></details></section>",
      bonds_html, pairing_state, camera_name, camera_mac, wifi_ssid, ap_ssid);
  free(bonds_html);

  ok = ok && page_append(
      page, page_len, &used,
      "<section class=\"card\"><h2>定位设置</h2><p class=\"caption\">控制发送给相机的位置和时间信息。</p>"
      "<label class=\"field\">失去 GPS 信号时保留最近定位</label><div class=\"unit\">"
      "<input class=\"text\" form=\"settings\" type=\"number\" name=\"max_age_s\" value=\"%u\" min=\"0\" max=\"86400\"><span>秒</span></div>"
      "<p class=\"hint\">超过此时间仍未恢复信号，就停止向相机发送旧坐标；0 表示立即停止。</p>"
      "<label class=\"field\">照片时区</label><select form=\"settings\" id=\"timezone\" name=\"tz\">"
      "<option value=\"-720\">(UTC−12:00) 国际日期变更线西</option><option value=\"-660\">(UTC−11:00) 萨摩亚</option>"
      "<option value=\"-600\">(UTC−10:00) 夏威夷</option><option value=\"-570\">(UTC−09:30) 马克萨斯群岛</option>"
      "<option value=\"-540\">(UTC−09:00) 阿拉斯加</option><option value=\"-480\">(UTC−08:00) 太平洋时间</option>"
      "<option value=\"-420\">(UTC−07:00) 山地时间</option><option value=\"-360\">(UTC−06:00) 中部时间</option>"
      "<option value=\"-300\">(UTC−05:00) 东部时间</option><option value=\"-240\">(UTC−04:00) 大西洋时间</option>"
      "<option value=\"-210\">(UTC−03:30) 纽芬兰</option><option value=\"-180\">(UTC−03:00) 巴西利亚</option>"
      "<option value=\"-120\">(UTC−02:00) 南乔治亚</option><option value=\"-60\">(UTC−01:00) 亚速尔群岛</option>"
      "<option value=\"0\">(UTC±00:00) 伦敦 / 协调世界时</option><option value=\"60\">(UTC+01:00) 柏林 / 巴黎</option>"
      "<option value=\"120\">(UTC+02:00) 雅典 / 开罗</option><option value=\"180\">(UTC+03:00) 莫斯科 / 利雅得</option>"
      "<option value=\"210\">(UTC+03:30) 德黑兰</option><option value=\"240\">(UTC+04:00) 迪拜</option>"
      "<option value=\"270\">(UTC+04:30) 喀布尔</option><option value=\"300\">(UTC+05:00) 伊斯兰堡</option>"
      "<option value=\"330\">(UTC+05:30) 印度</option><option value=\"345\">(UTC+05:45) 尼泊尔</option>"
      "<option value=\"360\">(UTC+06:00) 达卡</option><option value=\"390\">(UTC+06:30) 仰光</option>"
      "<option value=\"420\">(UTC+07:00) 曼谷 / 雅加达</option><option value=\"480\">(UTC+08:00) 北京 / 上海 / 新加坡</option>"
      "<option value=\"525\">(UTC+08:45) 尤克拉</option><option value=\"540\">(UTC+09:00) 东京 / 首尔</option>"
      "<option value=\"570\">(UTC+09:30) 阿德莱德</option><option value=\"600\">(UTC+10:00) 悉尼</option>"
      "<option value=\"630\">(UTC+10:30) 豪勋爵岛</option><option value=\"660\">(UTC+11:00) 所罗门群岛</option>"
      "<option value=\"720\">(UTC+12:00) 奥克兰 / 斐济</option><option value=\"765\">(UTC+12:45) 查塔姆群岛</option>"
      "<option value=\"780\">(UTC+13:00) 汤加</option><option value=\"840\">(UTC+14:00) 莱恩群岛</option></select>"
      "<label class=\"field\">夏时令偏移</label><select form=\"settings\" id=\"dst\" name=\"dst\">"
      "<option value=\"0\">不使用夏时令</option><option value=\"30\">增加 30 分钟</option>"
      "<option value=\"60\">增加 1 小时</option><option value=\"120\">增加 2 小时</option></select>"
      "<p class=\"hint\">设备不会自动判断夏时令；切换时需要手动修改。</p></section>",
      (unsigned)cfg.max_gps_age_s);

  ok = ok && page_append(
      page, page_len, &used,
      "<section class=\"card\"><h2>省电设置</h2><p class=\"caption\">定位和蓝牙可以分别选择策略。</p>"
      "<h3>定位省电模式</h3><div class=\"modes\">"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"loc_mode\" value=\"0\"><span><strong>省电</strong><span>断连后恢复定位需要更长时间。</span></span></label>"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"loc_mode\" value=\"1\" checked><span><strong>均衡</strong><span>兼顾功耗与定位恢复速度。</span></span></label>"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"loc_mode\" value=\"2\"><span><strong>轨迹</strong><span>相机断开后仍持续定位。</span></span></label>"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"loc_mode\" value=\"3\"><span><strong>自定义</strong><span>自行配置以下参数。</span></span></label></div>"
      "<details id=\"loc-advanced\"><summary>定位省电高级配置 <span class=\"summary-state\" id=\"loc-state\">只读</span></summary>"
      "<div class=\"rule\">相机连接期间，GNSS 始终实时工作。以下参数只影响相机未连接时的行为。</div>"
      "<div class=\"rule\" id=\"track-note\" hidden>轨迹模式下 GNSS 持续工作，以下待机参数不生效；加入 Flash 前不会保存 GPX。</div>"
      "<h3>启动与断线</h3><div class=\"grid\"><div><label class=\"field\">开机后连续定位</label><div class=\"unit\">"
      "<input class=\"text loc-custom\" form=\"settings\" type=\"number\" name=\"loc_boot\" value=\"%u\" min=\"0\" max=\"3600\"><span>秒</span></div></div>"
      "<div><label class=\"field\">相机断开后继续定位</label><div class=\"unit\"><input class=\"text loc-custom\" form=\"settings\" type=\"number\" "
      "name=\"loc_grace\" value=\"%u\" min=\"0\" max=\"3600\"><span>秒</span></div></div></div>"
      "<h3>定位待机巡检周期</h3><p class=\"hint\">几乎静止固定为 ≤1.8 km/h；介于该值与高速阈值之间为慢速。</p>"
      "<div class=\"grid\"><div><label class=\"field\">几乎静止时</label><div class=\"unit\"><input class=\"text loc-custom\" form=\"settings\" "
      "type=\"number\" name=\"loc_still\" value=\"%u\" min=\"5\" max=\"3600\"><span>秒</span></div></div>"
      "<div><label class=\"field\">慢速移动时</label><div class=\"unit\"><input class=\"text loc-custom\" form=\"settings\" type=\"number\" "
      "name=\"loc_move\" value=\"%u\" min=\"5\" max=\"3600\"><span>秒</span></div></div>"
      "<div><label class=\"field\">高速移动时</label><div class=\"unit\"><input class=\"text loc-custom\" form=\"settings\" type=\"number\" "
      "name=\"loc_fast\" value=\"%u\" min=\"5\" max=\"3600\"><span>秒</span></div></div>"
      "<div><label class=\"field\">高速移动判定</label><div class=\"unit\"><input class=\"text loc-custom\" form=\"settings\" type=\"number\" "
      "name=\"loc_speed\" value=\"%.1f\" min=\"1.9\" max=\"300\" step=\"0.1\"><span>km/h</span></div></div>"
      "<div><label class=\"field\">定位超时</label><div class=\"unit\"><input class=\"text loc-custom\" form=\"settings\" type=\"number\" "
      "name=\"loc_timeout\" value=\"%u\" min=\"5\" max=\"300\"><span>秒</span></div></div></div></details>"
      "<div class=\"divider\"></div><h3>蓝牙省电模式</h3><div class=\"modes\">"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"ble_mode\" value=\"0\"><span><strong>省电</strong><span>连接相机可能需要等待更久。</span></span></label>"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"ble_mode\" value=\"1\" checked><span><strong>正常</strong><span>兼顾待机和重连速度。</span></span></label>"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"ble_mode\" value=\"2\"><span><strong>自定义</strong><span>自行配置搜索参数。</span></span></label></div>"
      "<details id=\"ble-advanced\"><summary>蓝牙省电高级配置 <span class=\"summary-state\" id=\"ble-state\">只读</span></summary>"
      "<label class=\"field\">开机快速搜索持续时间</label><div class=\"unit\"><input class=\"text ble-custom\" form=\"settings\" type=\"number\" "
      "name=\"ble_boot\" value=\"%u\" min=\"0\" max=\"300\"><span>秒</span></div>"
      "<div class=\"grid\"><div><label class=\"field\">断联后蓝牙搜索周期</label><div class=\"unit\"><input class=\"text ble-custom\" form=\"settings\" "
      "type=\"number\" name=\"ble_period\" value=\"%.2f\" min=\"1\" max=\"10.24\" step=\"0.01\"><span>秒</span></div></div>"
      "<div><label class=\"field\">蓝牙搜索连续监听时长</label><div class=\"unit\"><input class=\"text ble-custom\" form=\"settings\" type=\"number\" "
      "name=\"ble_window\" value=\"%u\" min=\"20\" max=\"1000\"><span>毫秒</span></div></div></div>"
      "<p class=\"hint\">无配对记录时连续扫描；已有配对时，快速搜索阶段每秒监听 300 毫秒。</p></details></section>",
      (unsigned)location_policy.boot_work_s,
      (unsigned)location_policy.disconnect_grace_s,
      (unsigned)location_policy.stationary_sleep_s,
      (unsigned)location_policy.moving_sleep_s,
      (unsigned)location_policy.fast_sleep_s,
      (double)location_policy.fast_speed_kmh_x10 / 10.0,
      (unsigned)location_policy.fix_timeout_s,
      (unsigned)bluetooth_policy.boot_fast_search_s,
      (double)bluetooth_policy.scan_period_ms / 1000.0,
      (unsigned)bluetooth_policy.scan_window_ms);

  ok = ok && page_append(
      page, page_len, &used,
      "<div class=\"savebar\"><p class=\"hint\">定位与省电设置保存后立即生效；Wi‑Fi 设置下次开启配置网络时生效。</p>"
      "<button form=\"settings\" type=\"submit\">保存设置</button></div>"
      "<footer>配置地址：<a href=\"" ALPHALOC_AP_URL "\">" ALPHALOC_AP_URL
      "</a><br>联系邮箱：<a href=\"mailto:" FRAMENAV_CONTACT_EMAIL "\">" FRAMENAV_CONTACT_EMAIL "</a>"
      "<br>FrameNav 是基于 <a href=\"https://github.com/finfinack/alphaloc\">AlphaLoc</a> "
      "开发的开源项目，采用 <a href=\"https://www.apache.org/licenses/LICENSE-2.0\">Apache-2.0</a> 许可证。</footer>"
      "<script>(()=>{const q=(s)=>document.querySelector(s),qa=(s)=>[...document.querySelectorAll(s)];"
      "const li=qa('.loc-custom'),bi=qa('.ble-custom'),ln=q('#track-note'),ls=q('#loc-state'),bs=q('#ble-state');"
      "const lp={0:[45,10,300,120,30,28.8,30],1:[45,20,90,30,15,28.8,45]};"
      "const bp={0:[15,10,100],1:[30,3,100]};"
      "function loc(m,change){if(lp[m]&&change)li.forEach((e,i)=>e.value=lp[m][i]);"
      "li.forEach(e=>e.disabled=m!==3);ln.hidden=m!==2;ls.textContent=m===3?'可编辑':m===2?'不适用':'只读'}"
      "function ble(m,change){if(bp[m]&&change)bi.forEach((e,i)=>e.value=bp[m][i]);"
      "bi.forEach(e=>e.disabled=m!==2);bs.textContent=m===2?'可编辑':'只读'}"
      "qa('[name=loc_mode]').forEach(e=>e.addEventListener('change',()=>loc(+e.value,true)));"
      "qa('[name=ble_mode]').forEach(e=>e.addEventListener('change',()=>ble(+e.value,true)));"
      "q('[name=loc_mode][value=\"%u\"]').checked=true;q('[name=ble_mode][value=\"%u\"]').checked=true;"
      "q('#timezone').value=\"%d\";q('#dst').value=\"%d\";loc(%u,false);ble(%u,false)})();</script>"
      "</body></html>",
      (unsigned)cfg.location_power_mode, (unsigned)cfg.bluetooth_power_mode,
      (int)cfg.tz_offset_min, (int)cfg.dst_offset_min,
      (unsigned)cfg.location_power_mode, (unsigned)cfg.bluetooth_power_mode);
  if (!ok) {
    free(page);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Page too large");
  }

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  esp_err_t res = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
  free(page);
  return res;
}

static esp_err_t handle_save(httpd_req_t *req) {
  char body[1536];
  if (req->content_len >= sizeof(body)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
  }
  size_t total = 0;
  while (total < req->content_len) {
    int recv = httpd_req_recv(req, body + total, req->content_len - total);
    if (recv <= 0) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
    }
    total += (size_t)recv;
  }
  if (total == 0) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
  }
  body[total] = '\0';

  app_config_t next;
  if (!config_get_snapshot(s_cfg, &next)) {
    next = *s_cfg;
    config_validate(&next);
  }

  char value[CONFIG_STR_MAX_64];
  form_get(body, "cam_name", value, sizeof(value));
  if (strstr(body, "cam_name=") != NULL) {
    strncpy(next.camera_name_prefix, value, sizeof(next.camera_name_prefix) - 1);
    next.camera_name_prefix[sizeof(next.camera_name_prefix) - 1] = '\0';
  }

  form_get(body, "cam_mac", value, sizeof(value));
  if (strstr(body, "cam_mac=") != NULL) {
    strncpy(next.camera_mac_prefix, value, sizeof(next.camera_mac_prefix) - 1);
    next.camera_mac_prefix[sizeof(next.camera_mac_prefix) - 1] = '\0';
  }

  form_get(body, "tz", value, sizeof(value));
  int16_t tz = 0;
  if (parse_i16_range(value, -1440, 1440, &tz)) {
    next.tz_offset_min = tz;
  }

  form_get(body, "dst", value, sizeof(value));
  int16_t dst = 0;
  if (parse_i16_range(value, -1440, 1440, &dst)) {
    next.dst_offset_min = dst;
  }

  form_get(body, "wifi_ssid", value, sizeof(value));
  if (strstr(body, "wifi_ssid=") != NULL) {
    strncpy(next.wifi_ssid, value, sizeof(next.wifi_ssid) - 1);
    next.wifi_ssid[sizeof(next.wifi_ssid) - 1] = '\0';
  }

  form_get(body, "wifi_pass", value, sizeof(value));
  if (value[0] != '\0') {
    strncpy(next.wifi_pass, value, sizeof(next.wifi_pass) - 1);
    next.wifi_pass[sizeof(next.wifi_pass) - 1] = '\0';
  }

  form_get(body, "ap_ssid", value, sizeof(value));
  if (strstr(body, "ap_ssid=") != NULL) {
    strncpy(next.ap_ssid, value, sizeof(next.ap_ssid) - 1);
    next.ap_ssid[sizeof(next.ap_ssid) - 1] = '\0';
  }

  form_get(body, "ap_pass", value, sizeof(value));
  if (value[0] != '\0') {
    strncpy(next.ap_pass, value, sizeof(next.ap_pass) - 1);
    next.ap_pass[sizeof(next.ap_pass) - 1] = '\0';
  }

  form_get(body, "max_age_s", value, sizeof(value));
  uint32_t max_age = 0;
  if (parse_u32_range(value, 86400, &max_age)) {
    next.max_gps_age_s = max_age;
  }

  uint32_t parsed = 0;
  form_get(body, "loc_mode", value, sizeof(value));
  if (parse_u32_range(value, LOCATION_POWER_CUSTOM, &parsed)) {
    next.location_power_mode = (uint8_t)parsed;
  }
  form_get(body, "ble_mode", value, sizeof(value));
  if (parse_u32_range(value, BLUETOOTH_POWER_CUSTOM, &parsed)) {
    next.bluetooth_power_mode = (uint8_t)parsed;
  }

  form_get(body, "loc_boot", value, sizeof(value));
  if (parse_u32_range(value, 3600, &parsed)) {
    next.custom_boot_work_s = parsed;
  }
  form_get(body, "loc_grace", value, sizeof(value));
  if (parse_u32_range(value, 3600, &parsed)) {
    next.custom_disconnect_grace_s = parsed;
  }
  form_get(body, "loc_still", value, sizeof(value));
  if (parse_u32_range(value, 3600, &parsed) && parsed >= 5) {
    next.custom_stationary_sleep_s = parsed;
  }
  form_get(body, "loc_move", value, sizeof(value));
  if (parse_u32_range(value, 3600, &parsed) && parsed >= 5) {
    next.custom_moving_sleep_s = parsed;
  }
  form_get(body, "loc_fast", value, sizeof(value));
  if (parse_u32_range(value, 3600, &parsed) && parsed >= 5) {
    next.custom_fast_sleep_s = parsed;
  }
  form_get(body, "loc_speed", value, sizeof(value));
  if (parse_decimal_scaled(value, 10.0, 19, 3000, &parsed)) {
    next.custom_fast_speed_kmh_x10 = parsed;
  }
  form_get(body, "loc_timeout", value, sizeof(value));
  if (parse_u32_range(value, 300, &parsed) && parsed >= 5) {
    next.custom_fix_timeout_s = parsed;
  }
  form_get(body, "ble_boot", value, sizeof(value));
  if (parse_u32_range(value, 300, &parsed)) {
    next.custom_ble_boot_fast_search_s = parsed;
  }
  form_get(body, "ble_period", value, sizeof(value));
  if (parse_decimal_scaled(value, 1000.0, 1000, 10240, &parsed)) {
    next.custom_ble_scan_period_ms = parsed;
  }
  form_get(body, "ble_window", value, sizeof(value));
  if (parse_u32_range(value, 1000, &parsed) && parsed >= 20) {
    next.custom_ble_scan_window_ms = parsed;
  }

  if (!config_apply(s_cfg, &next)) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Save failed");
  }
  app_config_t applied;
  if (!config_get_snapshot(s_cfg, &applied)) {
    applied = next;
    config_validate(&applied);
  }
#if ALPHALOC_GNSS_STANDBY
  location_power_policy_t location_policy;
  config_resolve_location_power(&applied, &location_policy);
  gps_apply_power_policy(&location_policy);
#endif
  ble_client_config_changed();
  ESP_LOGI(TAG, "Settings applied: location_mode=%u bluetooth_mode=%u",
           (unsigned)applied.location_power_mode,
           (unsigned)applied.bluetooth_power_mode);
  return redirect_to_root(req);
}

static esp_err_t handle_pair_start(httpd_req_t *req) {
  if (!ble_client_start_pairing(FRAMENAV_PAIRING_WINDOW_S)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "蓝牙尚未就绪，暂时无法开始配对。\n");
  }
  return redirect_to_root(req);
}

static esp_err_t handle_bond_delete(httpd_req_t *req) {
  char body[128];
  if (req->content_len == 0 || req->content_len >= sizeof(body)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
  }
  size_t total = 0;
  while (total < req->content_len) {
    int received = httpd_req_recv(req, body + total,
                                  req->content_len - total);
    if (received <= 0) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
    }
    total += (size_t)received;
  }
  body[total] = '\0';

  char token[32];
  char type_text[16];
  form_get(body, "bond", token, sizeof(token));
  form_get(body, "type", type_text, sizeof(type_text));
  uint32_t addr_type = 0;
  ble_client_bond_t bond = {0};
  if (!parse_bond_token(token, bond.addr) ||
      !parse_u32_range(type_text, UINT8_MAX, &addr_type)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid bond");
  }
  bond.addr_type = (uint8_t)addr_type;
  if (!ble_client_delete_bond(&bond)) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "配对记录不存在或删除失败，请刷新后重试。\n");
  }
  return redirect_to_root(req);
}

static esp_err_t handle_captive_redirect(httpd_req_t *req,
                                         httpd_err_code_t error) {
  (void)error;
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", ALPHALOC_AP_URL);
  // Apple captive-network detection requires a non-empty response body.
  return httpd_resp_sendstr(req, "打开 FrameNav 配置页");
}

bool wifi_web_start(app_config_t *cfg) {
  if (s_started) {
    return true;
  }
  if (!cfg) return false;
  s_cfg = cfg;

  esp_err_t err;
  if (!s_netif_initialized) {
    err = esp_netif_init();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp-netif init failed: %s", esp_err_to_name(err));
      return false;
    }
    s_netif_initialized = true;
  }
  if (!s_default_event_loop_ready) {
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Default event loop init failed: %s", esp_err_to_name(err));
      return false;
    }
    s_default_event_loop_ready = true;
  }
  err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   &wifi_event_handler, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi event handler registration failed: %s",
             esp_err_to_name(err));
    return false;
  }
  s_wifi_handlers_registered = true;
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   &wifi_event_handler, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "IP event handler registration failed: %s",
             esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }
  s_ip_handlers_registered = true;
  s_netif_sta = esp_netif_create_default_wifi_sta();
  if (!s_netif_sta) {
    ESP_LOGE(TAG, "Failed to create STA network interface");
    wifi_cleanup_resources();
    return false;
  }
  s_netif = s_netif_sta;

  wifi_init_config_t cfg_init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg_init);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }

  const esp_timer_create_args_t reconnect_timer_args = {
      .callback = wifi_reconnect_cb,
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_reconnect",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&reconnect_timer_args, &s_reconnect_timer) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to create WiFi reconnect timer");
  }
  const esp_timer_create_args_t fallback_timer_args = {
      .callback = wifi_fallback_cb,
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_fallback",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&fallback_timer_args, &s_fallback_timer) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to create AP fallback timer");
  }

  app_config_t cfg_snapshot;
  if (!config_get_snapshot(s_cfg, &cfg_snapshot)) {
    cfg_snapshot = *s_cfg;
    config_validate(&cfg_snapshot);
  }
  wifi_config_t wifi_cfg = {0};
  strncpy((char *)wifi_cfg.sta.ssid, cfg_snapshot.wifi_ssid,
          sizeof(wifi_cfg.sta.ssid) - 1);
  strncpy((char *)wifi_cfg.sta.password, cfg_snapshot.wifi_pass,
          sizeof(wifi_cfg.sta.password) - 1);
  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set WiFi STA mode: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }
  err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure WiFi STA: %s", esp_err_to_name(err));
    if (!start_softap()) {
      wifi_cleanup_resources();
      return false;
    }
  }

  // Mark the lifecycle active before starting the radio so early connect/IP
  // events cannot be lost. Any later error goes through cleanup and clears it.
  s_started = true;
  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }
  err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to enable WiFi power save: %s", esp_err_to_name(err));
  }
  s_sta_start_us = esp_timer_get_time();
  if (!s_ap_active) {
    err = esp_wifi_connect();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "WiFi connect start failed: %s", esp_err_to_name(err));
    }
  }

  httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
  server_cfg.uri_match_fn = httpd_uri_match_wildcard;
  // The Chinese settings page performs several large snprintf operations.
  // Keep its generated HTML on the heap and retain enough HTTP-task stack for
  // libc formatting plus captive-portal redirects on memory-constrained C6.
  server_cfg.stack_size = 16384;
  err = httpd_start(&s_server, &server_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }

  httpd_uri_t root = {
      .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL};
  httpd_uri_t save = {.uri = "/save",
                      .method = HTTP_POST,
                      .handler = handle_save,
                      .user_ctx = NULL};
  httpd_uri_t pair_start = {.uri = "/pair/start",
                            .method = HTTP_POST,
                            .handler = handle_pair_start,
                            .user_ctx = NULL};
  httpd_uri_t bond_delete = {.uri = "/bond/delete",
                             .method = HTTP_POST,
                             .handler = handle_bond_delete,
                             .user_ctx = NULL};
  err = httpd_register_uri_handler(s_server, &root);
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &save);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &pair_start);
  }
  if (err == ESP_OK) {
    err = httpd_register_uri_handler(s_server, &bond_delete);
  }
  if (err == ESP_OK) {
    err = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND,
                                     handle_captive_redirect);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP route registration failed: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }

  if (!s_ap_active && !s_sta_has_ip && s_fallback_timer) {
    (void)esp_timer_start_once(
        s_fallback_timer,
        (uint64_t)ALPHALOC_WIFI_STA_FALLBACK_TIMEOUT_MS * 1000ULL);
  }
  ESP_LOGI(TAG, "WiFi web started; trying STA for 15s before AP fallback");
  return true;
}

void wifi_web_stop(void) {
  if (!s_started) {
    return;
  }
  wifi_cleanup_resources();
  ESP_LOGI(TAG, "WiFi web stopped");
}

bool wifi_web_is_started(void) { return s_started; }
