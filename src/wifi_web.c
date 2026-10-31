/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_web.h"
#include "location_service.h"
#include "track_store.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ble_client.h"
#include "cJSON.h"
#include "captive_dns.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "gps.h"

#define FRAMENAV_HTTP_MAX_CLIENTS 2
#define FRAMENAV_WEB_RESERVED_SOCKETS 8
#define FRAMENAV_HTTP_MAX_REQ_HDR_LEN 2048U
// ESP-IDF uses quarter-dBm units. 40 maps to 10 dBm (10 mW), which is
// approximately half the linear power of the previous 13 dBm / 20 mW cap.
#define FRAMENAV_WIFI_MAX_TX_POWER_QDBM 40

#if ALPHALOC_WIFI_WEB
_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= FRAMENAV_HTTP_MAX_CLIENTS +
                   FRAMENAV_WEB_RESERVED_SOCKETS,
               "Increase CONFIG_LWIP_MAX_SOCKETS for the HTTP server");
#endif

#ifndef ALPHALOC_BATTERY_MONITOR
#define ALPHALOC_BATTERY_MONITOR 0
#endif

#ifndef ALPHALOC_GNSS_STANDBY
#define ALPHALOC_GNSS_STANDBY 0
#endif

#if ALPHALOC_BATTERY_MONITOR
#include "battery.h"
#endif

static const char *TAG = "wifi_web";
static portMUX_TYPE s_radio_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_radio_owned;
static bool radio_claim(void) {
  portENTER_CRITICAL(&s_radio_lock);
  bool ok = !s_radio_owned;
  if (ok) s_radio_owned = true;
  portEXIT_CRITICAL(&s_radio_lock);
  return ok;
}
static void radio_release(void) {
  portENTER_CRITICAL(&s_radio_lock);
  s_radio_owned = false;
  portEXIT_CRITICAL(&s_radio_lock);
}

static httpd_handle_t s_server;
static portMUX_TYPE s_activity_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_activity_us;
static portMUX_TYPE s_wake_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_wake_requested;
static void web_activity(void) {
  portENTER_CRITICAL(&s_activity_lock);
  s_activity_us = esp_timer_get_time();
  portEXIT_CRITICAL(&s_activity_lock);
}
int64_t wifi_web_last_activity_us(void) {
  portENTER_CRITICAL(&s_activity_lock);
  int64_t value = s_activity_us;
  portEXIT_CRITICAL(&s_activity_lock);
  return value;
}
void wifi_web_request_wake(void) {
  portENTER_CRITICAL(&s_wake_lock);
  s_wake_requested = true;
  portEXIT_CRITICAL(&s_wake_lock);
}
bool wifi_web_take_wake_request(void) {
  portENTER_CRITICAL(&s_wake_lock);
  bool requested = s_wake_requested;
  s_wake_requested = false;
  portEXIT_CRITICAL(&s_wake_lock);
  return requested;
}
static esp_err_t handle_activity(httpd_req_t *req) {
  web_activity();
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, "OK");
}
static esp_err_t handle_favicon(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
  return httpd_resp_send(req, NULL, 0);
}

static app_config_t *s_cfg;
static esp_netif_t *s_netif_ap = NULL;
static volatile bool s_started;
static bool s_wifi_handlers_registered = false;
static bool s_netif_initialized = false;
static bool s_default_event_loop_ready = false;
static bool s_ap_active = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static bool start_softap(void);

#define ALPHALOC_AP_URL "http://10.254.254.1/"
#define FRAMENAV_PAIRING_WINDOW_S 90U
#define ALPHALOC_WIFI_ACTIVITY_HOLD_US 30000000LL

static captive_dns_handle_t s_captive_dns = NULL;

static void configure_wifi_tx_power(void) {
  int8_t before = 0;
  int8_t after = 0;
  bool have_before = esp_wifi_get_max_tx_power(&before) == ESP_OK;
  esp_err_t err = esp_wifi_set_max_tx_power(FRAMENAV_WIFI_MAX_TX_POWER_QDBM);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to limit WiFi TX power: %s", esp_err_to_name(err));
    return;
  }
  if (have_before && esp_wifi_get_max_tx_power(&after) == ESP_OK) {
    ESP_LOGI(TAG, "WiFi max TX power: %d -> %d quarter-dBm (%.1f dBm)",
             (int)before, (int)after, (double)after / 4.0);
  } else {
    ESP_LOGI(TAG, "WiFi max TX power limited to %.1f dBm",
             (double)FRAMENAV_WIFI_MAX_TX_POWER_QDBM / 4.0);
  }
}

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

  // RFC 8910 DHCP option 114 gives newer clients the local HTTP address.
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
  if (s_wifi_handlers_registered) {
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                 &wifi_event_handler);
    s_wifi_handlers_registered = false;
  }
  (void)esp_wifi_stop();
  (void)esp_wifi_deinit();
  if (s_netif_ap) {
    esp_netif_destroy(s_netif_ap);
    s_netif_ap = NULL;
  }
  // esp-netif is process-global and cannot be deinitialized. Keep the default
  // event loop too, so a later USB reconnect can start WiFi again safely.
  s_ap_active = false;
  portENTER_CRITICAL(&s_activity_lock);
  s_activity_us = 0;
  portEXIT_CRITICAL(&s_activity_lock);
}

static bool start_softap(void) {
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
  uint8_t ap_mac[6] = {0};
  esp_err_t err = esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read AP MAC: %s", esp_err_to_name(err));
    return false;
  }
  snprintf((char *)wifi_cfg.ap.ssid, sizeof(wifi_cfg.ap.ssid),
           "FrameNav-%02X%02X%02X", ap_mac[3], ap_mac[4], ap_mac[5]);
  wifi_cfg.ap.ssid_len = strlen((char *)wifi_cfg.ap.ssid);
  strncpy((char *)wifi_cfg.ap.password, "framenav",
          sizeof(wifi_cfg.ap.password) - 1);
  wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_cfg.ap.max_connection = 4;
  wifi_cfg.ap.channel = 1;
  wifi_cfg.ap.ssid_hidden = 0;
  wifi_cfg.ap.beacon_interval = 100;

  err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to switch to AP mode: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure AP: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set AP 20MHz bandwidth: %s", esp_err_to_name(err));
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
  ESP_LOGI(TAG, "AP configured: ssid=%s captive=%s", wifi_cfg.ap.ssid,
           ALPHALOC_AP_URL);
  return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (!s_started) {
    return;
  }
  if (event_base != WIFI_EVENT) {
    return;
  }

  if (event_id == WIFI_EVENT_AP_START) {
    wifi_config_t ap = {0};
    uint8_t channel = 0;
    wifi_second_chan_t secondary;
    (void)esp_wifi_get_channel(&channel, &secondary);
    if (esp_wifi_get_config(WIFI_IF_AP, &ap) == ESP_OK) {
      ESP_LOGI(TAG, "AP radio started: ssid=%.*s channel=%u hidden=%u beacon=%u",
               (int)sizeof(ap.ap.ssid), (const char *)ap.ap.ssid,
               (unsigned)channel, (unsigned)ap.ap.ssid_hidden,
               (unsigned)ap.ap.beacon_interval);
    }
    return;
  }
  if (event_id == WIFI_EVENT_AP_STACONNECTED ||
      event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_sta_list_t clients = {0};
    if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK) {
      ESP_LOGI(TAG, "AP associated clients=%u", (unsigned)clients.num);
    }
    return;
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

static bool page_send(httpd_req_t *req, char *page, size_t page_len,
                        const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static bool page_send(httpd_req_t *req, char *page, size_t page_len,
                        const char *format, ...) {
  if (!req || !page || !page_len) {
    return false;
  }
  va_list args;
  va_start(args, format);
  int written = vsnprintf(page, page_len, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= page_len) {
    return false;
  }
  return httpd_resp_send_chunk(req, page, written) == ESP_OK;
}

static bool __attribute__((unused)) format_track_time(uint32_t timestamp_utc,
                              int16_t tz_offset_min, int16_t dst_offset_min,
                              char *out, size_t out_len) {
  if (!timestamp_utc || !out || out_len == 0) return false;
  const int offset_min = (int)tz_offset_min + (int)dst_offset_min;
  const int64_t local_seconds =
      (int64_t)timestamp_utc + (int64_t)offset_min * 60LL;
  time_t local_time = (time_t)local_seconds;
  struct tm civil;
  if (!gmtime_r(&local_time, &civil)) return false;
  int written = snprintf(out, out_len,
                         "%04d-%02d-%02d %02d:%02d:%02d",
                         civil.tm_year + 1900, civil.tm_mon + 1, civil.tm_mday,
                         civil.tm_hour, civil.tm_min, civil.tm_sec);
  return written > 0 && (size_t)written < out_len;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
  if (!root) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "暂时无法读取设备数据");
  }
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "暂时无法读取设备数据");
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t response = httpd_resp_sendstr(req, json);
  free(json);
  return response;
}

static cJSON *build_config_json(void) {
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg)) return NULL;
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
#define ADD_CONFIG_STRING(name, value) \
  do { if (!cJSON_AddStringToObject(root, name, value)) goto no_memory; } while (0)
#define ADD_CONFIG_NUMBER(name, value) \
  do { if (!cJSON_AddNumberToObject(root, name, value)) goto no_memory; } while (0)
  ADD_CONFIG_STRING("cam_name", cfg.camera_name_prefix);
  ADD_CONFIG_STRING("cam_mac", cfg.camera_mac_prefix);
  ADD_CONFIG_NUMBER("tz", cfg.tz_offset_min);
  ADD_CONFIG_NUMBER("dst", cfg.dst_offset_min);
  ADD_CONFIG_NUMBER("source_gnss", cfg.source_gnss_enabled);
  ADD_CONFIG_NUMBER("source_phone", cfg.source_phone_enabled);
  ADD_CONFIG_NUMBER("source_manual", cfg.source_manual_enabled);
  ADD_CONFIG_STRING("source_order", cfg.source_order);
  ADD_CONFIG_NUMBER("track_enabled", cfg.track_enabled);
  ADD_CONFIG_NUMBER("gnss_idle", cfg.gnss_without_camera);
  ADD_CONFIG_NUMBER("track_idle", cfg.record_without_camera);
  ADD_CONFIG_NUMBER("track_int_s", cfg.track_interval_s);
  ADD_CONFIG_NUMBER("phone_push_enabled", cfg.phone_push_enabled);
  ADD_CONFIG_NUMBER("phone_interval_s", cfg.phone_interval_s);
  ADD_CONFIG_NUMBER("manual_auto_update", cfg.manual_auto_update);
  ADD_CONFIG_NUMBER("wifi_idle_s", cfg.wifi_idle_timeout_s);
  ADD_CONFIG_NUMBER("ble_mode", cfg.bluetooth_power_mode);
  ADD_CONFIG_NUMBER("ble_boot", cfg.custom_ble_boot_fast_search_s);
  ADD_CONFIG_NUMBER("ble_period", (double)cfg.custom_ble_scan_period_ms / 1000.0);
  ADD_CONFIG_NUMBER("ble_window", cfg.custom_ble_scan_window_ms);
  phone_location_status_t manual = {0};
  location_service_manual_status(&manual);
  ADD_CONFIG_NUMBER("manual_lat", manual.present ? manual.latitude : 0);
  ADD_CONFIG_NUMBER("manual_lon", manual.present ? manual.longitude : 0);
  ADD_CONFIG_NUMBER("manual_enabled", cfg.source_manual_enabled);
#undef ADD_CONFIG_STRING
#undef ADD_CONFIG_NUMBER
  return root;

no_memory:
#undef ADD_CONFIG_STRING
#undef ADD_CONFIG_NUMBER
  cJSON_Delete(root);
  return NULL;
}

char *wifi_web_get_config_json(void) {
  cJSON *root = build_config_json();
  if (!root) return NULL;
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

static double compact_coord(double value);

static cJSON *build_ble_config_json(void) {
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg)) return NULL;
  phone_location_status_t manual = {0};
  location_service_manual_status(&manual);
  cJSON *root = cJSON_CreateObject();
  cJSON *camera = cJSON_CreateArray();
  cJSON *location = cJSON_CreateArray();
  if (!root || !camera || !location) {
    cJSON_Delete(root); cJSON_Delete(camera); cJSON_Delete(location);
    return NULL;
  }
  cJSON_AddItemToObject(root, "c", camera);
  cJSON_AddItemToArray(camera, cJSON_CreateString(cfg.camera_name_prefix));
  cJSON_AddItemToArray(camera, cJSON_CreateString(cfg.camera_mac_prefix));
  cJSON_AddItemToArray(camera, cJSON_CreateNumber(cfg.tz_offset_min));
  cJSON_AddItemToArray(camera, cJSON_CreateNumber(cfg.dst_offset_min));
  cJSON_AddItemToArray(camera, cJSON_CreateNumber(cfg.wifi_idle_timeout_s));
  cJSON_AddItemToArray(camera, cJSON_CreateNumber(cfg.bluetooth_power_mode));
  cJSON_AddItemToArray(camera, cJSON_CreateNumber(cfg.custom_ble_boot_fast_search_s));
  cJSON_AddItemToArray(camera, cJSON_CreateNumber(
      (double)cfg.custom_ble_scan_period_ms / 1000.0));
  cJSON_AddItemToArray(camera, cJSON_CreateNumber(cfg.custom_ble_scan_window_ms));

  cJSON_AddItemToArray(location, cJSON_CreateString(cfg.source_order));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.source_gnss_enabled));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.source_phone_enabled));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.source_manual_enabled));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.track_enabled));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.gnss_without_camera));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.record_without_camera));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.track_interval_s));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.phone_push_enabled));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.phone_interval_s));
  cJSON_AddItemToArray(location, cJSON_CreateNumber(cfg.manual_auto_update));
  if (manual.present) {
    cJSON_AddItemToArray(location, cJSON_CreateNumber(compact_coord(manual.latitude)));
    cJSON_AddItemToArray(location, cJSON_CreateNumber(compact_coord(manual.longitude)));
  } else {
    cJSON_AddItemToArray(location, cJSON_CreateNull());
    cJSON_AddItemToArray(location, cJSON_CreateNull());
  }
  cJSON_AddItemToObject(root, "l", location);
  return root;
}

char *wifi_web_get_ble_config_json(void) {
  cJSON *root = build_ble_config_json();
  if (!root) return NULL;
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  /* The phone negotiates MTU 247, leaving 246 bytes for one ATT read. Keep
   * the compact payload below that boundary so Android receives it in one
   * read even when all configuration fields are populated. */
  if (!json || strlen(json) > 240U) {
    if (json) {
      ESP_LOGW(TAG, "BLE config payload too large: %u bytes",
               (unsigned)strlen(json));
    }
    free(json);
    return NULL;
  }
  return json;
}

static esp_err_t handle_config(httpd_req_t *req) {
  web_activity();
  return send_json(req, build_config_json());
}

static esp_err_t handle_storage(httpd_req_t *req) {
  web_activity();
  track_store_status_t status = {0};
  (void)track_store_get_status(&status);
  cJSON *root = cJSON_CreateObject();
  if (!root) return send_json(req, NULL);
  if (!cJSON_AddBoolToObject(root, "available", status.available) ||
      !cJSON_AddNumberToObject(root, "capacity_bytes", status.capacity_bytes) ||
      !cJSON_AddNumberToObject(root, "used_bytes", status.used_bytes) ||
      !cJSON_AddNumberToObject(root, "record_count", status.record_count)) {
    cJSON_Delete(root);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "暂时无法读取存储信息");
  }
  return send_json(req, root);
}

static cJSON *build_bonds_json(void) {
  ble_client_bond_t bonds[BLE_CLIENT_MAX_BONDS] = {0};
  int count = ble_client_get_bonds(bonds, BLE_CLIENT_MAX_BONDS);
  if (count < 0) return NULL;
  cJSON *root = cJSON_CreateObject();
  cJSON *items = root ? cJSON_AddArrayToObject(root, "bonds") : NULL;
  if (!root || !items) { cJSON_Delete(root); return NULL; }
  for (int i = 0; i < count && i < BLE_CLIENT_MAX_BONDS; ++i) {
    char mac[18];
    char token[13];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             bonds[i].addr[5], bonds[i].addr[4], bonds[i].addr[3],
             bonds[i].addr[2], bonds[i].addr[1], bonds[i].addr[0]);
    snprintf(token, sizeof(token), "%02X%02X%02X%02X%02X%02X",
             bonds[i].addr[5], bonds[i].addr[4], bonds[i].addr[3],
             bonds[i].addr[2], bonds[i].addr[1], bonds[i].addr[0]);
    cJSON *item = cJSON_CreateObject();
    if (!item ||
        !cJSON_AddStringToObject(item, "name",
                                 bonds[i].name[0] ? bonds[i].name : "未命名设备") ||
        !cJSON_AddStringToObject(item, "mac", mac) ||
        !cJSON_AddStringToObject(item, "token", token) ||
        !cJSON_AddNumberToObject(item, "type", bonds[i].addr_type) ||
        !cJSON_AddBoolToObject(item, "connected", bonds[i].connected)) {
      cJSON_Delete(item); cJSON_Delete(root); return NULL;
    }
    cJSON_AddItemToArray(items, item);
  }
  return root;
}

char *wifi_web_get_bonds_json(void) {
  cJSON *root = build_bonds_json();
  if (!root) return NULL;
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

static esp_err_t handle_bonds(httpd_req_t *req) {
  web_activity();
  cJSON *root = build_bonds_json();
  if (!root) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "暂时无法读取已配对设备。");
  }
  return send_json(req, root);
}

static cJSON *build_location_json(void) {
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg)) return NULL;
  gps_fix_t gnss = {0}, selected = {0};
  gps_status_t gps_status = {0};
  (void)gps_get_latest(&gnss);
  (void)gps_get_status(&gps_status);
  location_source_t source = LOCATION_NONE;
  uint32_t selected_observed = 0;
  (void)location_service_details(&selected, &source, &selected_observed);
  int64_t now_us = esp_timer_get_time();
  bool gnss_present = gnss.valid && isfinite(gnss.lat_deg) &&
      isfinite(gnss.lon_deg) && fabs(gnss.lat_deg) <= 90.0 &&
      fabs(gnss.lon_deg) <= 180.0;
  bool gnss_valid = gnss_present && gnss.time_valid &&
      isfinite(gnss.lon_deg) && now_us >= gnss.last_fix_time_us &&
      now_us - gnss.last_fix_time_us <= 5000000LL;
  live_location_status_t phone = {0};
  phone_location_status_t manual = {0};
  location_service_live_status(&phone);
  location_service_manual_status(&manual);
  track_store_status_t storage = {0};
  (void)track_store_get_status(&storage);
  const char *active = source == LOCATION_GNSS ? "gnss" :
                       source == LOCATION_PHONE ? "phone" :
                       source == LOCATION_MANUAL ? "manual" : "none";
  const char *search = gps_status.search_state == GPS_SEARCH_ACTIVE ? "searching" :
                       gps_status.search_state == GPS_SEARCH_PAUSED ? "paused" :
                       gps_status.search_state == GPS_SEARCH_WAKING ? "waking" :
                       gps_status.has_lock ? "locked" : "unknown";
  cJSON *root = cJSON_CreateObject();
  cJSON *gps = cJSON_CreateObject();
  cJSON *mobile = cJSON_CreateObject();
  cJSON *fixed = cJSON_CreateObject();
  cJSON *storage_json = cJSON_CreateObject();
  if (!root || !gps || !mobile || !fixed || !storage_json ||
      !cJSON_AddStringToObject(root, "activeSource", active) ||
      !cJSON_AddBoolToObject(gps, "present", gnss_present) ||
      !cJSON_AddBoolToObject(gps, "valid", gnss_valid) ||
      !cJSON_AddNumberToObject(gps, "latitude", gnss_present ? gnss.lat_deg : 0) ||
      !cJSON_AddNumberToObject(gps, "longitude", gnss_present ? gnss.lon_deg : 0) ||
      !cJSON_AddNumberToObject(gps, "satellitesVisible", gps_status.satellites_visible) ||
      !cJSON_AddNumberToObject(gps, "satellitesUsed", gps_status.satellites) ||
      !cJSON_AddStringToObject(gps, "searchState", search) ||
      !cJSON_AddBoolToObject(mobile, "present", phone.present) ||
      !cJSON_AddBoolToObject(mobile, "valid", phone.available) ||
      !cJSON_AddNumberToObject(mobile, "latitude", phone.present ? phone.latitude : 0) ||
      !cJSON_AddNumberToObject(mobile, "longitude", phone.present ? phone.longitude : 0) ||
      !cJSON_AddNumberToObject(mobile, "accuracy", phone.present ? phone.accuracy_m : 0) ||
      !cJSON_AddBoolToObject(mobile, "enabled", cfg.phone_push_enabled != 0) ||
      !cJSON_AddNumberToObject(mobile, "updatedAt", phone.captured_ms) ||
      !cJSON_AddNumberToObject(mobile, "remainingSeconds", phone.remaining_s) ||
      !cJSON_AddBoolToObject(fixed, "present", manual.present) ||
      !cJSON_AddBoolToObject(fixed, "valid", manual.available) ||
      !cJSON_AddBoolToObject(fixed, "clockPending", manual.clock_pending) ||
      !cJSON_AddNumberToObject(fixed, "latitude", manual.present ? manual.latitude : 0) ||
      !cJSON_AddNumberToObject(fixed, "longitude", manual.present ? manual.longitude : 0) ||
      !cJSON_AddNumberToObject(fixed, "updatedAt", (double)manual.captured_utc * 1000.0) ||
      !cJSON_AddNumberToObject(fixed, "remainingSeconds", manual.remaining_s) ||
      !cJSON_AddBoolToObject(storage_json, "available", storage.available) ||
      !cJSON_AddNumberToObject(storage_json, "capacity_bytes", storage.capacity_bytes) ||
      !cJSON_AddNumberToObject(storage_json, "used_bytes", storage.used_bytes) ||
      !cJSON_AddNumberToObject(storage_json, "record_count", storage.record_count)) {
    cJSON_Delete(gps); cJSON_Delete(mobile); cJSON_Delete(fixed);
    cJSON_Delete(storage_json); cJSON_Delete(root);
    return NULL;
  }
  cJSON_AddItemToObject(root, "gnss", gps);
  cJSON_AddItemToObject(root, "phone", mobile);
  cJSON_AddItemToObject(root, "manual", fixed);
  cJSON_AddItemToObject(root, "storage", storage_json);
  (void)selected_observed;
  return root;
}

char *wifi_web_get_location_json(void) {
  cJSON *root = build_location_json();
  if (!root) return NULL;
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

static double compact_coord(double value) {
  return round(value * 1000000.0) / 1000000.0;
}

static bool ble_telemetry_add(cJSON *array, cJSON *item) {
  if (!array || !item) {
    cJSON_Delete(item);
    return false;
  }
  if (!cJSON_AddItemToArray(array, item)) {
    cJSON_Delete(item);
    return false;
  }
  return true;
}

static cJSON *build_ble_telemetry_json(void) {
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg)) return NULL;
  gps_fix_t gnss = {0};
  gps_status_t gps_status = {0};
  (void)gps_get_latest(&gnss);
  (void)gps_get_status(&gps_status);
  live_location_status_t phone = {0};
  phone_location_status_t manual = {0};
  location_service_live_status(&phone);
  location_service_manual_status(&manual);
  track_store_status_t storage = {0};
  (void)track_store_get_status(&storage);

  bool gnss_present = gnss.valid && isfinite(gnss.lat_deg) &&
      isfinite(gnss.lon_deg) && fabs(gnss.lat_deg) <= 90.0 &&
      fabs(gnss.lon_deg) <= 180.0;
  int64_t now_us = esp_timer_get_time();
  bool gnss_valid = gnss_present && gnss.time_valid &&
      now_us >= gnss.last_fix_time_us &&
      now_us - gnss.last_fix_time_us <= 5000000LL;
  location_source_t source = LOCATION_NONE;
  gps_fix_t selected = {0};
  uint32_t selected_observed = 0;
  (void)location_service_details(&selected, &source, &selected_observed);
  (void)selected;
  (void)selected_observed;

  const char *active = source == LOCATION_GNSS ? "g" :
                       source == LOCATION_PHONE ? "p" :
                       source == LOCATION_MANUAL ? "m" : "n";
  int search_code = gps_status.search_state == GPS_SEARCH_ACTIVE ? 1 :
                    gps_status.search_state == GPS_SEARCH_PAUSED ? 2 :
                    gps_status.search_state == GPS_SEARCH_WAKING ? 3 :
                    gps_status.has_lock ? 4 : 0;

  cJSON *root = cJSON_CreateObject();
  cJSON *g = cJSON_CreateArray();
  cJSON *p = cJSON_CreateArray();
  cJSON *m = cJSON_CreateArray();
  cJSON *s = cJSON_CreateArray();
  if (!root || !g || !p || !m || !s ||
      !cJSON_AddStringToObject(root, "a", active)) {
    cJSON_Delete(root); cJSON_Delete(g); cJSON_Delete(p);
    cJSON_Delete(m); cJSON_Delete(s); return NULL;
  }
  bool ok = ble_telemetry_add(g, cJSON_CreateBool(gnss_present)) &&
            ble_telemetry_add(g, cJSON_CreateBool(gnss_valid));
  if (gnss_present) {
    ok = ok && ble_telemetry_add(
                   g, cJSON_CreateNumber(compact_coord(gnss.lat_deg))) &&
         ble_telemetry_add(g, cJSON_CreateNumber(compact_coord(gnss.lon_deg)));
  } else {
    ok = ok && ble_telemetry_add(g, cJSON_CreateNull()) &&
         ble_telemetry_add(g, cJSON_CreateNull());
  }
  ok = ok && ble_telemetry_add(
                   g, cJSON_CreateNumber(gps_status.satellites_visible)) &&
       ble_telemetry_add(g, cJSON_CreateNumber(gps_status.satellites)) &&
       ble_telemetry_add(g, cJSON_CreateNumber(search_code));

  ok = ok && ble_telemetry_add(p, cJSON_CreateBool(phone.present)) &&
       ble_telemetry_add(p, cJSON_CreateBool(phone.available));
  if (phone.present) {
    ok = ok && ble_telemetry_add(
                   p, cJSON_CreateNumber(compact_coord(phone.latitude))) &&
         ble_telemetry_add(p, cJSON_CreateNumber(compact_coord(phone.longitude)));
  } else {
    ok = ok && ble_telemetry_add(p, cJSON_CreateNull()) &&
         ble_telemetry_add(p, cJSON_CreateNull());
  }
  ok = ok && ble_telemetry_add(
                   p, cJSON_CreateNumber(phone.present ? round(phone.accuracy_m) : 0)) &&
       ble_telemetry_add(p, cJSON_CreateBool(cfg.phone_push_enabled != 0)) &&
       ble_telemetry_add(p, cJSON_CreateNumber(phone.captured_ms)) &&
       ble_telemetry_add(p, cJSON_CreateNumber(phone.remaining_s));

  ok = ok && ble_telemetry_add(m, cJSON_CreateBool(manual.present)) &&
       ble_telemetry_add(m, cJSON_CreateBool(manual.available)) &&
       ble_telemetry_add(m, cJSON_CreateBool(manual.clock_pending));
  if (manual.present) {
    ok = ok && ble_telemetry_add(
                   m, cJSON_CreateNumber(compact_coord(manual.latitude))) &&
         ble_telemetry_add(m, cJSON_CreateNumber(compact_coord(manual.longitude)));
  } else {
    ok = ok && ble_telemetry_add(m, cJSON_CreateNull()) &&
         ble_telemetry_add(m, cJSON_CreateNull());
  }
  ok = ok && ble_telemetry_add(
                   m, cJSON_CreateNumber((double)manual.captured_utc * 1000.0)) &&
       ble_telemetry_add(m, cJSON_CreateNumber(manual.remaining_s));

  ok = ok && ble_telemetry_add(s, cJSON_CreateBool(storage.available)) &&
       ble_telemetry_add(s, cJSON_CreateNumber(storage.record_count)) &&
       ble_telemetry_add(s, cJSON_CreateNumber(storage.used_bytes)) &&
       ble_telemetry_add(s, cJSON_CreateNumber(storage.capacity_bytes));
  if (!ok || !cJSON_AddItemToObject(root, "g", g)) goto telemetry_fail;
  g = NULL;
  if (!cJSON_AddItemToObject(root, "p", p)) goto telemetry_fail;
  p = NULL;
  if (!cJSON_AddItemToObject(root, "m", m)) goto telemetry_fail;
  m = NULL;
  if (!cJSON_AddItemToObject(root, "s", s)) goto telemetry_fail;
  s = NULL;
  return root;

telemetry_fail:
  cJSON_Delete(root);
  cJSON_Delete(g);
  cJSON_Delete(p);
  cJSON_Delete(m);
  cJSON_Delete(s);
  return NULL;
}

char *wifi_web_get_ble_telemetry_json(void) {
  cJSON *root = build_ble_telemetry_json();
  if (!root) return NULL;
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  /* Keep the compact payload below the 246-byte ATT payload available after
   * the app requests MTU 247. A larger payload would be truncated by some
   * Android GATT stacks instead of being delivered as valid JSON. */
  if (!json || strlen(json) > 240U) {
    if (json) {
      ESP_LOGW(TAG, "BLE telemetry payload too large: %u bytes",
               (unsigned)strlen(json));
    }
    free(json);
    return NULL;
  }
  return json;
}

static esp_err_t handle_location(httpd_req_t *req) {
  web_activity();
  return send_json(req, build_location_json());
}

static void wifi_page_state(char *dot_class, size_t dot_len, char *state,
                            size_t state_len) {
  if (!dot_class || !state || dot_len == 0 || state_len == 0) {
    return;
  }
  const char *dot = s_ap_active ? "dot-blue" : "dot-yellow";
  const char *text = s_ap_active ? "热点已开启" : "正在启动";
  snprintf(dot_class, dot_len, "%s", dot);
  snprintf(state, state_len, "%s", text);
}

static esp_err_t handle_root(httpd_req_t *req) {
  web_activity();
  app_config_t cfg;
  if (!config_get_snapshot(s_cfg, &cfg)) {
    cfg = *s_cfg;
    config_validate(&cfg);
  }
  const size_t page_len = 8192;
  char *page = malloc(page_len);
  if (!page) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "页面暂时无法打开，请稍后重试");
  }
  gps_status_t gps_status = {0};
  (void)gps_get_status(&gps_status);
  bool cam_connected = ble_client_is_connected();
  bool cam_bonded = ble_client_is_bonded();
  const char *cam_dot_class =
      cam_connected ? (cam_bonded ? "dot-green" : "dot-blue") : "dot-red";
  const char *cam_conn_str = cam_connected ? "已连接" : "未连接";
  const char *gps_dot_class __attribute__((unused)) = gps_status.has_lock ? "dot-green" :
      gps_status.search_state == GPS_SEARCH_PAUSED ? "dot-gray" :
      gps_status.search_state == GPS_SEARCH_WAKING ? "dot-blue" : "dot-yellow";
  const char *gps_lock_str __attribute__((unused)) = gps_status.has_lock ? "已定位" :
      gps_status.search_state == GPS_SEARCH_PAUSED ? "已暂停搜星" :
      gps_status.search_state == GPS_SEARCH_WAKING ? "正在恢复" : "搜索中";
  char wifi_dot_class[16];
  char wifi_state[96];
  wifi_page_state(wifi_dot_class, sizeof(wifi_dot_class), wifi_state,
                  sizeof(wifi_state));
#if ALPHALOC_BATTERY_MONITOR
  battery_status_t bat = {0};
  bool bat_ok = battery_get_status(&bat) && bat.valid;
  const char *bat_dot_class = "dot-gray";
  char bat_text[64] = "电量：—";
  if (bat_ok) {
    if (bat.percent > 50.0f) {
      bat_dot_class = "dot-green";
    } else if (bat.percent > 30.0f) {
      bat_dot_class = "dot-yellow";
    } else {
      bat_dot_class = "dot-red";
    }
    snprintf(bat_text, sizeof(bat_text), "电量：%.0f%%",
             (double)bat.percent);
  }
#endif
  phone_location_status_t manual = {0};
  location_service_manual_status(&manual);
  track_store_status_t storage = {0};
  bool storage_available = track_store_get_status(&storage);
  char storage_count[24] = "不可用";
  char storage_used[64] = "不可用";
  if (storage_available) {
    snprintf(storage_count, sizeof(storage_count), "%u",
             (unsigned)storage.record_count);
    snprintf(storage_used, sizeof(storage_used), "%.1f KB / %.1f KB",
             (double)storage.used_bytes / 1024.0,
             (double)storage.capacity_bytes / 1024.0);
  }

  ble_client_bond_t bonds[BLE_CLIENT_MAX_BONDS] = {0};
  int bond_count = ble_client_get_bonds(bonds, BLE_CLIENT_MAX_BONDS);
  const size_t bonds_html_len = 4096;
  char *phone_bonds_html = malloc(bonds_html_len);
  char *camera_bonds_html = malloc(bonds_html_len);
  if (!phone_bonds_html || !camera_bonds_html) {
    free(phone_bonds_html);
    free(camera_bonds_html);
    free(page);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "页面暂时无法打开，请稍后重试");
  }
  size_t phone_bonds_used = 0, camera_bonds_used = 0;
  phone_bonds_html[0] = '\0';
  camera_bonds_html[0] = '\0';
  if (bond_count < 0) {
    phone_bonds_used = (size_t)snprintf(
        phone_bonds_html, bonds_html_len,
        "<p class=\"muted\">暂时无法读取设备列表，请稍后刷新。</p>");
    camera_bonds_used = (size_t)snprintf(
        camera_bonds_html, bonds_html_len,
        "<p class=\"muted\">暂时无法读取设备列表，请稍后刷新。</p>");
  } else if (bond_count == 0) {
    phone_bonds_used = (size_t)snprintf(
        phone_bonds_html, bonds_html_len,
        "<p class=\"muted\">还没有已配对的手机。</p>");
    camera_bonds_used = (size_t)snprintf(
        camera_bonds_html, bonds_html_len,
        "<p class=\"muted\">还没有添加设备。</p>");
  } else {
    for (int i = 0; i < bond_count; ++i) {
      char token[13];
      char mac[18];
      snprintf(token, sizeof(token), "%02X%02X%02X%02X%02X%02X",
               bonds[i].addr[5], bonds[i].addr[4], bonds[i].addr[3],
               bonds[i].addr[2], bonds[i].addr[1], bonds[i].addr[0]);
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               bonds[i].addr[5], bonds[i].addr[4], bonds[i].addr[3],
               bonds[i].addr[2], bonds[i].addr[1], bonds[i].addr[0]);
      bool camera = bonds[i].name[0] != '\0';
      const char *raw_name = camera ? bonds[i].name : "FrameNav 管理手机";
      char display_name[BLE_CLIENT_CAMERA_NAME_MAX * 6];
      html_escape_attr(display_name, sizeof(display_name), raw_name);
      char *target = camera ? camera_bonds_html : phone_bonds_html;
      size_t *used = camera ? &camera_bonds_used : &phone_bonds_used;
      if (*used >= bonds_html_len) continue;
      int n = snprintf(
          target + *used, bonds_html_len - *used,
          "<div class=\"bond\"><div><strong>%s</strong>"
          "<small>%s · MAC %s</small></div>"
          "<form class=\"inline\" method=\"POST\" action=\"/bond/delete\">"
          "<input type=\"hidden\" name=\"bond\" value=\"%s\">"
          "<input type=\"hidden\" name=\"type\" value=\"%u\">"
          "<button class=\"danger\" type=\"submit\" "
          "onclick=\"return confirm('确定移除该设备？')\">"
          "移除</button></form></div>",
          display_name,
          bonds[i].connected ? "正在使用" : "已添加", mac, token,
          (unsigned)bonds[i].addr_type);
      if (n < 0 || (size_t)n >= bonds_html_len - *used) {
        target[bonds_html_len - 1] = '\0';
        *used = bonds_html_len - 1;
        continue;
      }
      *used += (size_t)n;
    }
  }

  char pairing_notice[96] = "";
  if (ble_client_pairing_seconds_remaining() > 0) {
    snprintf(pairing_notice, sizeof(pairing_notice),
             "<p class=\"hint\">请在设备上确认配对</p>");
  }

  bluetooth_power_policy_t bluetooth_policy;
  config_resolve_bluetooth_power(&cfg, &bluetooth_policy);

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  bool ok = page_send(
      req, page, page_len,
      "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<link rel=\"icon\" href=\"data:,\">"
      "<title>帧迹 FrameNav</title><style>"
      "*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
      "margin:0 auto;padding:20px;max-width:720px;color:#17231c;background:#eef3ef}"
      ".brand{font-size:27px;font-weight:780}.brand span{color:#257149}.muted,small,.hint{color:#66736b}.phone-location{font-size:13px}"
      ".card{background:#fff;border:1px solid #d7dfd9;border-radius:8px;"
      "padding:18px;margin:13px 0;box-shadow:0 3px 14px rgba(20,50,30,.05)}"
      "h2{font-size:18px;margin:0 0 5px}h3{font-size:14px;margin:18px 0 8px}"
      ".statusbar{display:flex;flex-wrap:wrap;gap:9px 15px;font-size:13px}.statusitem{display:flex;align-items:center;gap:6px}"
      ".dot{width:9px;height:9px;flex-shrink:0;border-radius:50%%}.dot-green{background:#2e9a55}.dot-red{background:#d9534f}"
      ".location-status .statusitem{min-width:0;max-width:100%%;line-height:1.5}"
      ".location-status .statusitem>span:last-child{min-width:0;overflow-wrap:anywhere}"
      ".source-status{border-top:1px solid #e2e8e4;padding:11px 0}.source-status:first-of-type{border-top:0}"
      ".source-status p{font-size:13px;margin:5px 0;overflow-wrap:anywhere}.source-active{display:inline-block;margin-left:8px;"
      "padding:3px 8px;border-radius:20px;background:#e2f4e7;color:#1b7137;font-size:11px;font-weight:700}"
      ".source-list{list-style:none;margin:8px 0;padding:0}.source-row{display:flex;align-items:center;gap:9px;"
      "padding:9px;border:1px solid #d7dfd9;border-radius:6px;margin:6px 0;background:#fff;cursor:grab}"
      ".source-row.dragging{opacity:.45}.source-row label{flex:1;font-weight:650}.source-row input[type=checkbox]{width:18px;height:18px}"
      ".source-grip{touch-action:none;user-select:none;cursor:grab}"
      ".source-row button{padding:4px 8px;background:#34463b}.setting-line{border-top:1px solid #e2e8e4;padding:10px 0}"
      ".switch-line{display:flex;align-items:center;gap:10px}.switch-line input{width:18px;height:18px}"
      ".dot-blue{background:#2f6fdb}.dot-yellow{background:#f0b429}.dot-gray{background:#9aa3af}"
      "label.field{display:block;margin:13px 0 5px;font-weight:650;font-size:13px}"
      "input.text,select{width:100%%;padding:10px;border:1px solid #cbd6ce;border-radius:8px;background:#fff;color:#17231c}"
      "input:disabled{opacity:.62}button{padding:10px 14px;border:0;border-radius:8px;background:#257149;color:#fff;font-weight:700;white-space:nowrap;flex-shrink:0}"
      ".danger{background:#bd3e3e}.secondary{background:#34463b}.bond{display:flex;justify-content:space-between;gap:10px;"
      "align-items:center;padding:11px 0;border-top:1px solid #e6ece8}.bond small{display:block;margin-top:3px}"
      ".inline{margin:0;flex-shrink:0}.bond>div{min-width:0;overflow-wrap:anywhere}.inline input{display:none}.hint{font-size:12px;line-height:1.55}.grid{display:grid;grid-template-columns:1fr 1fr;gap:11px}.no-camera-settings{grid-template-columns:repeat(3,minmax(0,1fr))}"
      ".manual-location{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr) auto;gap:8px;align-items:end}.manual-location>div{min-width:0}.manual-location .field{margin:0 0 5px}.manual-location button{padding:10px 4px;font-size:12px}"
      ".modes{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}.mode{display:flex;gap:8px;border:1px solid #d4ddd7;"
      "border-radius:8px;padding:10px;font-size:12px}.mode input{margin-top:2px}.mode strong{display:block;font-size:13px;margin-bottom:3px}"
      "details{margin-top:12px;border-top:1px solid #e2e8e4;padding-top:12px}"
      ".unit{display:flex;align-items:center;gap:7px}.unit input{flex:1}.unit span{font-size:12px;color:#68756d;white-space:nowrap}"
      ".savebar{position:sticky;bottom:0;z-index:5;display:flex;justify-content:flex-end;align-items:center;gap:12px;margin:15px -4px 0;padding:9px 4px;background:rgba(238,243,239,.96);border-top:1px solid #d7dfd9}"
      "@media(max-width:520px){body{padding:12px}.grid,.no-camera-settings{grid-template-columns:1fr}.bond{align-items:flex-start}}"
      "</style></head><body><div class=\"brand\">帧迹 <span>FrameNav</span></div>"
      "<form id=\"settings\" method=\"POST\" action=\"/save\"></form>"
      "<section class=\"card\"><h2>连接信息</h2><div class=\"statusbar\">"
      "<div class=\"statusitem\"><span class=\"dot %s\"></span><span>设备：%s</span></div>"
      "<div class=\"statusitem\"><span class=\"dot %s\"></span><span>Wi‑Fi：%s</span></div>",
      cam_dot_class, cam_conn_str,
      wifi_dot_class, wifi_state);
#if ALPHALOC_BATTERY_MONITOR
  ok = ok && page_send(req, page, page_len,
                         "<div class=\"statusitem\"><span class=\"dot %s\"></span><span>%s</span></div>",
                         bat_dot_class, bat_text);
#endif
  ok = ok && page_send(
      req, page, page_len,
      "</div></section><section class=\"card\"><h2>定位信息</h2>"
      "<div class=\"source-status\"><div><strong>GNSS 定位</strong><span id=\"loc-active-gnss\" class=\"source-active\" hidden>正在使用</span></div>"
      "<p>坐标：<span id=\"loc-gnss-coordinates\">—</span></p><p>搜到卫星：<span id=\"loc-gnss-visible\">—</span> · 使用：<span id=\"loc-gnss-used\">—</span></p><p>搜索状态：<span id=\"loc-gnss-search\">—</span></p></div>"
      "<div class=\"source-status\"><div><strong>手机定位</strong><span id=\"loc-active-phone\" class=\"source-active\" hidden>正在使用</span></div>"
      "<p>坐标：<span id=\"loc-phone-coordinates\">—</span> · 精度：<span id=\"loc-phone-accuracy\">—</span></p><p>持续推送：<span id=\"loc-phone-enabled\">—</span> · 获取时间：<span id=\"loc-phone-updated\">—</span></p><p>剩余有效期：<span id=\"loc-phone-remaining\">—</span></p></div>"
      "<div class=\"source-status\"><div><strong>手动设置</strong><span id=\"loc-active-manual\" class=\"source-active\" hidden>正在使用</span></div>"
      "<p>坐标：<span id=\"loc-manual-coordinates\">—</span></p><p>设置时间：<span id=\"loc-manual-updated\">—</span> · 剩余有效期：<span id=\"loc-manual-remaining\">—</span></p></div></section>");
  ok = ok && page_send(
      req, page, page_len,
      "<section class=\"card\"><h2>存储轨迹</h2>"
      "<div class=\"statusbar\"><div class=\"statusitem\"><span>轨迹条数：<span id=\"storage-record-count\">%s</span></span></div>"
      "<div class=\"statusitem\"><span>存储占用：<span id=\"storage-space\">%s</span></span></div></div></section>"
      "<section class=\"card\"><h2>连接配置</h2><h3>已配对设备</h3><h3>管理手机</h3>",
      storage_count, storage_used);
  ok = ok && page_send(req, page, page_len, "%s", phone_bonds_html);
  ok = ok && page_send(req, page, page_len, "<h3>推送相机</h3>%s", camera_bonds_html);
  ok = ok && page_send(
      req, page, page_len,
      "%s<form method=\"POST\" action=\"/pair/start\">"
      "<button class=\"secondary\" type=\"submit\">添加设备</button></form>"
      "<details><summary>热点运行时间</summary><label class=\"field\">无人使用时关闭热点</label><select class=\"text\" form=\"settings\" name=\"wifi_idle_s\">"
      "<option value=\"300\" %s>5 分钟</option><option value=\"900\" %s>15 分钟</option></select>"
      "</details><details><summary>蓝牙省电</summary><div class=\"modes\">"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"ble_mode\" value=\"0\"><span><strong>省电</strong></span></label>"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"ble_mode\" value=\"1\"><span><strong>平衡</strong></span></label>"
      "<label class=\"mode\"><input form=\"settings\" type=\"radio\" name=\"ble_mode\" value=\"2\"><span><strong>自定义</strong></span></label></div>"
      "<details id=\"ble-advanced\"><summary>高级设置</summary><label class=\"field\">开机后查找设备</label><div class=\"unit\"><input class=\"text ble-custom\" form=\"settings\" type=\"number\" name=\"ble_boot\" value=\"%u\" min=\"0\" max=\"300\"><span>秒</span></div>"
      "<div class=\"grid\"><div><label class=\"field\">再次查找间隔</label><div class=\"unit\"><input class=\"text ble-custom\" form=\"settings\" type=\"number\" name=\"ble_period\" value=\"%.2f\" min=\"1\" max=\"10.24\" step=\"0.01\"><span>秒</span></div></div>"
      "<div><label class=\"field\">每次查找时长</label><div class=\"unit\"><input class=\"text ble-custom\" form=\"settings\" type=\"number\" name=\"ble_window\" value=\"%u\" min=\"20\" max=\"1000\"><span>毫秒</span></div></div></div></details></details></section>",
      pairing_notice,
      cfg.wifi_idle_timeout_s == WIFI_IDLE_TIMEOUT_DEFAULT_S ? "selected" : "",
      cfg.wifi_idle_timeout_s == WIFI_IDLE_TIMEOUT_LONG_S ? "selected" : "",
      (unsigned)bluetooth_policy.boot_fast_search_s,
      (double)bluetooth_policy.scan_period_ms / 1000.0,
      (unsigned)bluetooth_policy.scan_window_ms);
  free(phone_bonds_html);
  free(camera_bonds_html);

  ok = ok && page_send(
      req, page, page_len,
      "<section class=\"card\"><h2>定位配置</h2><h3>通用</h3>"
      "<label class=\"field\">定位来源和优先级</label><p class=\"hint\">按顺序使用第一个有效且启用的来源。拖动可调整优先级。</p>"
      "<input id=\"source-order\" form=\"settings\" type=\"hidden\" name=\"source_order\" value=\"%s\"><ul id=\"source-list\" class=\"source-list\">"
      "<li class=\"source-row\" draggable=\"true\" data-source=\"gnss\"><span class=\"source-grip\" aria-label=\"拖动排序\">☰</span><label for=\"source-gnss\">GNSS 定位</label><input id=\"source-gnss\" form=\"settings\" type=\"checkbox\" name=\"source_gnss\" value=\"1\" %s></li>"
      "<li class=\"source-row\" draggable=\"true\" data-source=\"phone\"><span class=\"source-grip\" aria-label=\"拖动排序\">☰</span><label for=\"source-phone\">手机定位</label><input id=\"source-phone\" form=\"settings\" type=\"checkbox\" name=\"source_phone\" value=\"1\" %s></li>"
      "<li class=\"source-row\" draggable=\"true\" data-source=\"manual\"><span class=\"source-grip\" aria-label=\"拖动排序\">☰</span><label for=\"source-manual\">手动设置经纬度</label><input id=\"source-manual\" form=\"settings\" type=\"checkbox\" name=\"source_manual\" value=\"1\" %s></li></ul>"
      "<div class=\"setting-line switch-line\"><input form=\"settings\" type=\"checkbox\" name=\"track_enabled\" value=\"1\" %s><label>轨迹记录开启</label></div>"
      "<div class=\"setting-line switch-line\"><input form=\"settings\" type=\"checkbox\" name=\"track_idle\" value=\"1\" %s><label>在未连接相机时也记录轨迹</label></div>"
      "<label class=\"field\">轨迹记录频率</label><div class=\"unit\"><input class=\"text\" form=\"settings\" type=\"number\" name=\"track_int_s\" value=\"%u\" min=\"1\" max=\"3600\"><span>秒</span></div>"
      "<label class=\"field\">照片时区</label><select class=\"text\" form=\"settings\" id=\"timezone\" name=\"tz\">"
      "<option value=\"-720\">UTC−12:00</option><option value=\"-600\">UTC−10:00</option><option value=\"-480\">UTC−08:00</option>"
      "<option value=\"-420\">UTC−07:00</option><option value=\"-360\">UTC−06:00</option><option value=\"-300\">UTC−05:00</option>"
      "<option value=\"-240\">UTC−04:00</option><option value=\"-180\">UTC−03:00</option><option value=\"-60\">UTC−01:00</option>"
      "<option value=\"0\">UTC±00:00</option><option value=\"60\">UTC+01:00</option><option value=\"120\">UTC+02:00</option>"
      "<option value=\"180\">UTC+03:00</option><option value=\"210\">UTC+03:30</option><option value=\"240\">UTC+04:00</option>"
      "<option value=\"300\">UTC+05:00</option><option value=\"330\">UTC+05:30</option><option value=\"345\">UTC+05:45</option>"
      "<option value=\"360\">UTC+06:00</option><option value=\"420\">UTC+07:00</option><option value=\"480\">UTC+08:00</option>"
      "<option value=\"540\">UTC+09:00</option><option value=\"570\">UTC+09:30</option><option value=\"600\">UTC+10:00</option>"
      "<option value=\"660\">UTC+11:00</option><option value=\"720\">UTC+12:00</option><option value=\"840\">UTC+14:00</option></select>"
      "<label class=\"field\">夏时令</label><select class=\"text\" form=\"settings\" id=\"dst\" name=\"dst\"><option value=\"0\">关闭</option>"
      "<option value=\"30\">增加 30 分钟</option><option value=\"60\">增加 1 小时</option><option value=\"120\">增加 2 小时</option></select>"
      "<h3>GNSS 定位</h3><div class=\"setting-line switch-line\"><input form=\"settings\" type=\"checkbox\" name=\"gnss_idle\" value=\"1\" %s><label>在未连接任何设备时也持续搜星</label></div>"
      "<h3>手机定位</h3><div class=\"setting-line switch-line\"><input form=\"settings\" type=\"checkbox\" name=\"phone_push_enabled\" value=\"1\" %s><label>通过蓝牙持续推送手机定位</label></div>"
      "<label class=\"field\">手机定位获取频率</label><div class=\"unit\"><input class=\"text\" form=\"settings\" type=\"number\" name=\"phone_interval_s\" value=\"%u\" min=\"1\" max=\"3600\"><span>秒</span></div>"
      "<h3>手动设置经纬度（兜底位置）</h3><div class=\"manual-location\">"
      "<div><label class=\"field\" for=\"manual-lon\">经度</label><input class=\"text\" id=\"manual-lon\" form=\"settings\" name=\"manual_lon\" type=\"number\" min=\"-180\" max=\"180\" step=\"0.000001\" value=\"%.6f\"></div>"
      "<div><label class=\"field\" for=\"manual-lat\">纬度</label><input class=\"text\" id=\"manual-lat\" form=\"settings\" name=\"manual_lat\" type=\"number\" min=\"-90\" max=\"90\" step=\"0.000001\" value=\"%.6f\"></div>"
      "<button class=\"danger\" id=\"manual-clear\" type=\"button\">清除兜底位置</button></div>"
      "<input form=\"settings\" type=\"hidden\" name=\"manual_dirty\" id=\"manual-dirty\" value=\"0\"><input form=\"settings\" type=\"hidden\" name=\"manual_now_ms\" id=\"manual-now-ms\" value=\"0\">"
      "<div class=\"setting-line switch-line\"><input form=\"settings\" type=\"checkbox\" name=\"manual_auto_update\" value=\"1\" %s><label>使用真实定位持续更新</label></div>"
      "<p class=\"hint\" id=\"manual-message\" role=\"status\"></p></section>",
      cfg.source_order,
      cfg.source_gnss_enabled ? "checked" : "",
      cfg.source_phone_enabled ? "checked" : "",
      cfg.source_manual_enabled ? "checked" : "",
      cfg.track_enabled ? "checked" : "",
      cfg.record_without_camera ? "checked" : "",
      (unsigned)cfg.track_interval_s,
      cfg.gnss_without_camera ? "checked" : "",
      cfg.phone_push_enabled ? "checked" : "",
      (unsigned)cfg.phone_interval_s,
      manual.present ? manual.longitude : 0.0,
      manual.present ? manual.latitude : 0.0,
      cfg.manual_auto_update ? "checked" : "");

  ok = ok && page_send(
      req, page, page_len,
      "<div class=\"savebar\"><button form=\"settings\" type=\"submit\">保存设置</button></div>"
      "<script>(()=>{const q=(s)=>document.querySelector(s),qa=(s)=>[...document.querySelectorAll(s)];"
      "let locating=false;const ml=q('#manual-lat'),mn=q('#manual-lon'),manualMessage=q('#manual-message');"
      "const timeText=t=>t?new Date(Number(t)).toLocaleString():'—';"
      "const remainingText=s=>!s?'已过期':s<60?'不足 1 分钟':s<3600?Math.ceil(s/60)+' 分钟':Math.floor(s/3600)+' 小时 '+Math.floor(s%%3600/60)+' 分';"
      "const coords=(x,y,valid)=>valid&&Number.isFinite(+x)&&Number.isFinite(+y)?Number(x).toFixed(6)+', '+Number(y).toFixed(6):'—';"
      "async function liveLocation(){if(document.hidden||locating)return;locating=true;try{"
      "const r=await fetch('/location',{cache:'no-store'});if(!r.ok)throw Error();const d=await r.json();"
      "['gnss','phone','manual'].forEach(s=>{const n=q('#loc-active-'+s);n.hidden=d.activeSource!==s;});"
      "q('#loc-gnss-coordinates').textContent=coords(d.gnss.latitude,d.gnss.longitude,d.gnss.present);"
      "q('#loc-gnss-visible').textContent=d.gnss.satellitesVisible;q('#loc-gnss-used').textContent=d.gnss.satellitesUsed;"
      "q('#loc-gnss-search').textContent=({searching:'搜索中',paused:'已暂停搜星',waking:'正在恢复',locked:'已定位',unknown:'状态未知'})[d.gnss.searchState]||'状态未知';"
      "q('#loc-phone-enabled').textContent=d.phone.enabled?'已开启':'已关闭';q('#loc-phone-updated').textContent=timeText(d.phone.updatedAt);"
      "q('#loc-phone-remaining').textContent=d.phone.present?remainingText(d.phone.remainingSeconds):'未获取';"
      "q('#loc-phone-coordinates').textContent=coords(d.phone.latitude,d.phone.longitude,d.phone.present);"
      "q('#loc-phone-accuracy').textContent=d.phone.present?Math.round(d.phone.accuracy)+' m':'—';"
      "q('#loc-manual-coordinates').textContent=coords(d.manual.latitude,d.manual.longitude,d.manual.present);"
      "q('#loc-manual-updated').textContent=timeText(d.manual.updatedAt);q('#loc-manual-remaining').textContent=d.manual.clockPending?'等待校时':d.manual.present?remainingText(d.manual.remainingSeconds):'未设置';"
      "q('#storage-record-count').textContent=d.storage.available?d.storage.record_count:'不可用';"
      "q('#storage-space').textContent=d.storage.available?`${(d.storage.used_bytes/1024).toFixed(1)} KB / ${(d.storage.capacity_bytes/1024).toFixed(1)} KB`:'不可用';"
      "}catch(e){['gnss-coordinates','gnss-visible','gnss-used','gnss-search','phone-coordinates','phone-accuracy','phone-enabled','phone-updated','phone-remaining','manual-coordinates','manual-updated','manual-remaining'].forEach(x=>q('#loc-'+x).textContent='暂不可用');['storage-record-count','storage-space'].forEach(x=>q('#'+x).textContent='暂不可用');}finally{locating=false;}}"
      "setInterval(liveLocation,10000);document.addEventListener('visibilitychange',liveLocation);liveLocation();"
      "ml.addEventListener('input',()=>q('#manual-dirty').value='1');mn.addEventListener('input',()=>q('#manual-dirty').value='1');"
      "q('#settings').addEventListener('submit',()=>{q('#manual-now-ms').value=Date.now();syncSourceOrder();});"
      "const sourceList=q('#source-list'),orderInput=q('#source-order');function syncSourceOrder(){orderInput.value=[...sourceList.children].map(e=>e.dataset.source).join(',');}"
      "const sourceOrder=orderInput.value.split(',');sourceOrder.forEach(s=>{const row=sourceList.querySelector('[data-source=\"'+s+'\"]');if(row)sourceList.appendChild(row);});"
      "let dragging=null;sourceList.addEventListener('dragstart',e=>{dragging=e.target.closest('.source-row');if(dragging)dragging.classList.add('dragging');});"
      "sourceList.addEventListener('dragend',()=>{if(dragging)dragging.classList.remove('dragging');dragging=null;syncSourceOrder();});"
      "sourceList.addEventListener('dragover',e=>{e.preventDefault();const target=e.target.closest('.source-row');if(dragging&&target&&target!==dragging){const r=target.getBoundingClientRect();sourceList.insertBefore(dragging,e.clientY<r.top+r.height/2?target:target.nextSibling);}});"
      "let touchRow=null;sourceList.addEventListener('pointerdown',e=>{if(!e.target.closest('.source-grip'))return;touchRow=e.target.closest('.source-row');touchRow.classList.add('dragging');});"
      "sourceList.addEventListener('pointermove',e=>{if(!touchRow)return;const rows=[...sourceList.querySelectorAll('.source-row')].filter(x=>x!==touchRow);const target=rows.find(x=>e.clientY<x.getBoundingClientRect().top+x.getBoundingClientRect().height/2);sourceList.insertBefore(touchRow,target||null);});"
      "function finishTouchDrag(){if(touchRow)touchRow.classList.remove('dragging');touchRow=null;syncSourceOrder();}sourceList.addEventListener('pointerup',finishTouchDrag);sourceList.addEventListener('pointercancel',finishTouchDrag);"
      "q('#manual-clear').addEventListener('click',async()=>{if(!confirm('清除手动兜底位置？'))return;manualMessage.textContent='正在清除…';"
      "try{const r=await fetch('/location/manual/clear',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});if(!r.ok)throw Error();ml.value='';mn.value='';q('#manual-dirty').value='0';manualMessage.textContent='兜底位置已清除。';liveLocation();}catch(e){manualMessage.textContent='清除失败，请重试。';}});"
      "const bi=qa('.ble-custom');"
      "const bp={0:[15,10,100],1:[30,3,100]};"
      "function ble(m,change){if(bp[m]&&change)bi.forEach((e,i)=>e.value=bp[m][i]);"
      "bi.forEach(e=>e.disabled=m!==2)}"
      "qa('[name=ble_mode]').forEach(e=>e.addEventListener('change',()=>ble(+e.value,true)));"
      "q('[name=ble_mode][value=\"%u\"]').checked=true;"
      "q('#timezone').value=\"%d\";q('#dst').value=\"%d\";ble(%u,false)})();</script>"
      "</body></html>",
      (unsigned)cfg.bluetooth_power_mode,
      (int)cfg.tz_offset_min, (int)cfg.dst_offset_min,
      (unsigned)cfg.bluetooth_power_mode);
  free(page);
  if (!ok) return ESP_FAIL;
  esp_err_t res = httpd_resp_send_chunk(req, NULL, 0);
  if (res == ESP_OK) ESP_LOGI(TAG, "Configuration page streamed successfully");
  return res;
}

static esp_err_t handle_save(httpd_req_t *req) {
  web_activity();
  char body[2048];
  if (req->content_len >= sizeof(body)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "提交内容过长，请减少设置项后重试");
  }
  size_t total = 0;
  while (total < req->content_len) {
    int recv = httpd_req_recv(req, body + total, req->content_len - total);
    if (recv <= 0) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "没有收到设置内容");
    }
    total += (size_t)recv;
  }
  if (total == 0) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "没有收到设置内容");
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
    if (!config_set_camera_name_prefix(&next, value, strlen(value))) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "相机名称前缀编码无效");
    }
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

  uint32_t parsed = 0;
  next.source_gnss_enabled = strstr(body, "source_gnss=1") != NULL;
  next.source_phone_enabled = strstr(body, "source_phone=1") != NULL;
  next.source_manual_enabled = strstr(body, "source_manual=1") != NULL;
  next.track_enabled = strstr(body, "track_enabled=1") != NULL;
  next.phone_push_enabled = strstr(body, "phone_push_enabled=1") != NULL;
  next.manual_auto_update = strstr(body, "manual_auto_update=1") != NULL;
  form_get(body, "source_order", value, sizeof(value));
  if (value[0] != '\0') {
    char source_order[sizeof(next.source_order)];
    strncpy(source_order, value, sizeof(source_order) - 1);
    source_order[sizeof(source_order) - 1] = '\0';
    bool seen_gnss = false, seen_phone = false, seen_manual = false;
    char *cursor = source_order;
    unsigned count = 0;
    bool order_valid = true;
    while (*cursor) {
      char *end = strchr(cursor, ',');
      if (end) *end = '\0';
      if (strcmp(cursor, "gnss") == 0 && !seen_gnss) seen_gnss = true;
      else if (strcmp(cursor, "phone") == 0 && !seen_phone) seen_phone = true;
      else if (strcmp(cursor, "manual") == 0 && !seen_manual) seen_manual = true;
      else order_valid = false;
      ++count;
      if (!end) break;
      cursor = end + 1;
    }
    if (!order_valid || count != 3 || !seen_gnss || !seen_phone || !seen_manual)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "定位来源顺序无效");
    strncpy(next.source_order, value, sizeof(next.source_order) - 1);
    next.source_order[sizeof(next.source_order) - 1] = '\0';
  }
  form_get(body, "phone_interval_s", value, sizeof(value));
  if (!parse_u32_range(value, 3600, &parsed) || parsed < 1)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "手机定位获取频率需为 1 至 3600 秒");
  next.phone_interval_s = parsed;

  double manual_latitude = 0, manual_longitude = 0;
  uint64_t manual_now_ms = 0;
  bool manual_dirty = strstr(body, "manual_dirty=1") != NULL;
  if (manual_dirty) {
    char *end = NULL;
    form_get(body, "manual_lat", value, sizeof(value));
    manual_latitude = strtod(value, &end);
    if (!value[0] || end == value || *end || !isfinite(manual_latitude) ||
        manual_latitude < -90 || manual_latitude > 90)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请输入有效的纬度");
    form_get(body, "manual_lon", value, sizeof(value));
    end = NULL;
    manual_longitude = strtod(value, &end);
    if (!value[0] || end == value || *end || !isfinite(manual_longitude) ||
        manual_longitude < -180 || manual_longitude > 180)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请输入有效的经度");
    form_get(body, "manual_now_ms", value, sizeof(value));
    end = NULL;
    unsigned long long parsed_time = strtoull(value, &end, 10);
    if (!value[0] || end == value || *end || parsed_time < 1577836800000ULL ||
        parsed_time > 4102444799999ULL)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "设备时间同步失败，请刷新页面后重试");
    manual_now_ms = (uint64_t)parsed_time;
  }
  form_get(body, "max_gps_age", value, sizeof(value));
  if (strstr(body, "max_gps_age=") != NULL) {
    if (!parse_u32_range(value, 86400, &parsed)) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "卫星位置最大年龄请设置为 0 至 86400 秒");
    }
    next.max_gps_age_s = parsed;
  }

  form_get(body, "phone_ttl_h", value, sizeof(value));
  if (strstr(body, "phone_ttl_h=") != NULL) {
    if (!parse_u32_range(value, PHONE_LOCATION_TTL_MAX_HOURS, &parsed) || parsed < 1)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "位置有效期请设置为 1 至 720 小时");
    next.phone_location_ttl_hours = parsed;
  }
  next.gnss_without_camera = strstr(body, "gnss_idle=1") != NULL;
  next.record_without_camera = strstr(body, "track_idle=1") != NULL;
  form_get(body, "track_int_s", value, sizeof(value));
  if (parse_u32_range(value, LOCATION_RECORD_INTERVAL_MAX_S, &parsed) &&
      parsed >= LOCATION_RECORD_INTERVAL_MIN_S) {
    next.track_interval_s = parsed;
  }
  form_get(body, "ble_mode", value, sizeof(value));
  if (parse_u32_range(value, BLUETOOTH_POWER_CUSTOM, &parsed)) {
    next.bluetooth_power_mode = (uint8_t)parsed;
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
  form_get(body, "wifi_idle_s", value, sizeof(value));
  if (parse_u32_range(value, WIFI_IDLE_TIMEOUT_LONG_S, &parsed) &&
      (parsed == WIFI_IDLE_TIMEOUT_DEFAULT_S || parsed == WIFI_IDLE_TIMEOUT_LONG_S)) {
    next.wifi_idle_timeout_s = parsed;
  }

  if (!config_apply(s_cfg, &next)) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "设置未能保存，请重试");
  }
  if (manual_dirty && !location_service_set_manual(manual_latitude,
                                                    manual_longitude,
                                                    manual_now_ms)) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "设置已保存，但兜底位置未能保存");
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
  ESP_LOGI(TAG, "Settings applied: bluetooth_mode=%u",
           (unsigned)applied.bluetooth_power_mode);
  return redirect_to_root(req);
}

static cJSON *read_phone_json(httpd_req_t *req) {
  char content_type[64];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                   sizeof(content_type)) != ESP_OK ||
      strncmp(content_type, "application/json", 16) != 0) return NULL;
  char body[512];
  if (!req->content_len || req->content_len >= sizeof(body)) return NULL;
  size_t total = 0;
  while (total < req->content_len) {
    int received = httpd_req_recv(req, body + total, req->content_len - total);
    if (received <= 0) return NULL;
    total += (size_t)received;
  }
  if (memchr(body, 0, total)) return NULL;
  body[total] = 0;
  return cJSON_ParseWithOpts(body, NULL, true);
}

static bool json_number(const cJSON *object, const char *name, double *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return false;
  *out = item->valuedouble;
  return true;
}

static esp_err_t handle_phone_location(httpd_req_t *req) {
  web_activity();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  cJSON *json = read_phone_json(req);
  double latitude = 0, longitude = 0, accuracy = 0, captured_ms = 0, now_ms = 0;
  bool valid = json && json_number(json, "latitude", &latitude) &&
      json_number(json, "longitude", &longitude) &&
      json_number(json, "accuracy", &accuracy) &&
      json_number(json, "timestamp_ms", &captured_ms) &&
      json_number(json, "now_ms", &now_ms) &&
      captured_ms >= 0 && now_ms >= 0 &&
      captured_ms <= 4102444799999.0 && now_ms <= 4102444799999.0 &&
      floor(captured_ms) == captured_ms && floor(now_ms) == now_ms;
  cJSON_Delete(json);
  if (!valid || !location_service_set_live_phone(latitude, longitude, accuracy,
                                                  (uint64_t)captured_ms,
                                                  (uint64_t)now_ms)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "位置无效或已过期，请重新获取。\n");
  }
  return httpd_resp_sendstr(req, "手机实时定位已接收。\n");
}

static esp_err_t handle_manual_location(httpd_req_t *req) {
  web_activity();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  cJSON *json = read_phone_json(req);
  double latitude = 0, longitude = 0, now_ms = 0;
  bool valid = json && json_number(json, "latitude", &latitude) &&
      json_number(json, "longitude", &longitude) &&
      json_number(json, "now_ms", &now_ms) && now_ms >= 1577836800000.0 &&
      now_ms <= 4102444799999.0 && floor(now_ms) == now_ms;
  cJSON_Delete(json);
  if (!valid || !location_service_set_manual(latitude, longitude,
                                              (uint64_t)now_ms)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "手动坐标无效，未能保存。\n");
  }
  return httpd_resp_sendstr(req, "手动兜底位置已保存。\n");
}

static esp_err_t handle_phone_clock(httpd_req_t *req) {
  web_activity();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  cJSON *json = read_phone_json(req);
  double now_ms = 0;
  bool valid = json && json_number(json, "now_ms", &now_ms) &&
      now_ms >= 1577836800000.0 && now_ms <= 4102444799999.0 &&
      floor(now_ms) == now_ms;
  cJSON_Delete(json);
  if (!valid) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "时间无效，请刷新页面后重试。\n");
  }
  (void)location_service_sync_phone_clock((uint64_t)now_ms);
  return httpd_resp_sendstr(req, "OK\n");
}

static esp_err_t handle_phone_clear(httpd_req_t *req) {
  web_activity();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  cJSON *json = read_phone_json(req);
  if (!cJSON_IsObject(json)) {
    cJSON_Delete(json);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "请求无效，请刷新页面后重试。");
  }
  cJSON_Delete(json);
  (void)location_service_clear_manual();
  return httpd_resp_sendstr(req, "手动兜底位置已清除。\n");
}

static esp_err_t handle_pair_start(httpd_req_t *req) {
  web_activity();
  if (!ble_client_start_pairing(FRAMENAV_PAIRING_WINDOW_S)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "暂时无法添加设备，请稍后重试。\n");
  }
  return redirect_to_root(req);
}

static esp_err_t handle_bond_delete(httpd_req_t *req) {
  web_activity();
  char body[128];
  if (req->content_len == 0 || req->content_len >= sizeof(body)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "请求无效，请刷新页面后重试");
  }
  size_t total = 0;
  while (total < req->content_len) {
    int received = httpd_req_recv(req, body + total,
                                  req->content_len - total);
    if (received <= 0) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "没有收到设备信息");
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
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "设备信息无效");
  }
  bond.addr_type = (uint8_t)addr_type;
  if (!ble_client_delete_bond(&bond)) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
      return httpd_resp_sendstr(req, "未能移除设备，请刷新页面后重试。\n");
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

static esp_err_t register_web_routes(httpd_handle_t server) {
  esp_err_t err;
  httpd_uri_t root = {
      .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL};
  httpd_uri_t location = {
      .uri = "/location", .method = HTTP_GET, .handler = handle_location};
  httpd_uri_t config = {
      .uri = "/config", .method = HTTP_GET, .handler = handle_config};
  httpd_uri_t storage = {
      .uri = "/storage", .method = HTTP_GET, .handler = handle_storage};
  httpd_uri_t bonds = {
      .uri = "/bonds", .method = HTTP_GET, .handler = handle_bonds};
  httpd_uri_t phone_location = {
      .uri = "/location/phone", .method = HTTP_POST, .handler = handle_phone_location};
  httpd_uri_t live_location = {
      .uri = "/location/live", .method = HTTP_POST, .handler = handle_phone_location};
  httpd_uri_t manual_location = {
      .uri = "/location/manual", .method = HTTP_POST, .handler = handle_manual_location};
  httpd_uri_t phone_clock = {
      .uri = "/location/clock", .method = HTTP_POST, .handler = handle_phone_clock};
  httpd_uri_t phone_clear = {
      .uri = "/location/phone/clear", .method = HTTP_POST, .handler = handle_phone_clear};
  httpd_uri_t manual_clear = {
      .uri = "/location/manual/clear", .method = HTTP_POST, .handler = handle_phone_clear};
  httpd_uri_t activity = {
      .uri = "/activity", .method = HTTP_GET, .handler = handle_activity};
  httpd_uri_t favicon = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = handle_favicon};
  httpd_uri_t save = {
      .uri = "/save", .method = HTTP_POST, .handler = handle_save};
  httpd_uri_t pair_start = {
      .uri = "/pair/start", .method = HTTP_POST, .handler = handle_pair_start};
  httpd_uri_t bond_delete = {.uri = "/bond/delete",
                             .method = HTTP_POST,
                             .handler = handle_bond_delete};
  err = httpd_register_uri_handler(server, &root);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &config);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &storage);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &bonds);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &location);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &phone_location);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &live_location);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &manual_location);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &phone_clock);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &phone_clear);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &manual_clear);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &activity);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &favicon);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &save);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &pair_start);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &bond_delete);
  if (err == ESP_OK) {
    err = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND,
                                     handle_captive_redirect);
  }
  return err;
}

static bool wifi_web_start_unlocked(app_config_t *cfg) {
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
  wifi_init_config_t cfg_init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg_init);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }

  // Configure the fixed device AP while the radio is stopped.
  if (!start_softap()) {
    wifi_cleanup_resources();
    return false;
  }

  // Mark the lifecycle active before starting the radio so early AP events
  // cannot be lost. Any later error goes through cleanup and clears it.
  s_started = true;
  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }
  configure_wifi_tx_power();
  err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to enable WiFi power save: %s", esp_err_to_name(err));
  }
  httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
  server_cfg.uri_match_fn = httpd_uri_match_wildcard;
  server_cfg.max_req_hdr_len = FRAMENAV_HTTP_MAX_REQ_HDR_LEN;
  // The Chinese settings page performs several large snprintf operations.
  // Keep its generated HTML on the heap and retain enough HTTP-task stack for
  // libc formatting plus captive-portal redirects on memory-constrained C6.
  server_cfg.stack_size = 16384;
  /* Keep headroom for the live/manual location endpoints and future API
   * additions; ESP-IDF's default of 8 is too small for the full WebUI. */
  server_cfg.max_uri_handlers = 24;
  server_cfg.max_open_sockets = FRAMENAV_HTTP_MAX_CLIENTS;
  server_cfg.lru_purge_enable = true;
  err = httpd_start(&s_server, &server_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }
  err = register_web_routes(s_server);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP route registration failed: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }

  ESP_LOGI(TAG, "WiFi web started: fixed device AP available");
  return true;
}

void wifi_web_stop(void) {
  if (!radio_claim()) return;
  if (!s_started) {
    radio_release();
    return;
  }
  wifi_cleanup_resources();
  ESP_LOGI(TAG, "WiFi web stopped");
  radio_release();
}

bool wifi_web_start(app_config_t *cfg) {
  if (!radio_claim()) return false;
  bool ok = wifi_web_start_unlocked(cfg);
  radio_release();
  return ok;
}

bool wifi_web_is_started(void) { return s_started; }
bool wifi_web_has_clients(void) {
  if (!s_started) return false;
  bool ap_client = false;
  if (s_ap_active) {
    wifi_sta_list_t clients = {0};
    // Keep the radio alive if the driver cannot report the station list.
    if (esp_wifi_ap_get_sta_list(&clients) != ESP_OK) return true;
    ap_client = clients.num > 0;
  }
  // Recent HTTP activity keeps the AP alive while a phone uses the page.
  if (ap_client) return true;
  int64_t last = wifi_web_last_activity_us();
  int64_t now = esp_timer_get_time();
  return last > 0 && now >= last && now - last <= ALPHALOC_WIFI_ACTIVITY_HOLD_US;
}
