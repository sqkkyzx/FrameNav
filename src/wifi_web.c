/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_web.h"
#include "location_service.h"
#include "cert_store.h"
#include "test_debug.h"

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
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "gps.h"
#include "lwip/ip4_addr.h"
#include "mbedtls/ssl.h"
#include "track_store.h"

#ifndef FRAMENAV_EMBEDDED_TLS
#define FRAMENAV_EMBEDDED_TLS 0
#endif

#define FRAMENAV_HTTP_MAX_CLIENTS 2
#define FRAMENAV_HTTPS_MAX_CLIENTS 6
// Each server owns listen, control and message sockets, plus captive DNS and
// an outbound TLS download. Browser HTTPS preconnections need six client slots.
#define FRAMENAV_WEB_RESERVED_SOCKETS 8

#if ALPHALOC_WIFI_WEB
_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= FRAMENAV_HTTP_MAX_CLIENTS +
                   FRAMENAV_HTTPS_MAX_CLIENTS + FRAMENAV_WEB_RESERVED_SOCKETS,
               "Increase CONFIG_LWIP_MAX_SOCKETS for concurrent HTTP/HTTPS");
#if FRAMENAV_EMBEDDED_TLS
#ifndef CONFIG_MBEDTLS_DYNAMIC_BUFFER
#error "Enable CONFIG_MBEDTLS_DYNAMIC_BUFFER for concurrent HTTPS on the XIAO"
#endif
// PEM is larger than its DER certificate message, so this also covers updates.
_Static_assert(MBEDTLS_SSL_OUT_CONTENT_LEN >= FRAMENAV_CERT_MAX_BYTES,
               "Increase CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN for full chains");
#endif
#endif

#ifndef ALPHALOC_BATTERY_MONITOR
#define ALPHALOC_BATTERY_MONITOR 0
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
static httpd_handle_t s_https_server;
static portMUX_TYPE s_activity_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_activity_us;
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
static bool s_sta_configured = false;
static bool s_cert_update_busy = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static bool start_softap(void);

#ifndef ALPHALOC_WIFI_STA_FALLBACK_TIMEOUT_MS
#define ALPHALOC_WIFI_STA_FALLBACK_TIMEOUT_MS 15000
#endif

#ifndef ALPHALOC_WIFI_RECONNECT_BACKOFF_MS
#define ALPHALOC_WIFI_RECONNECT_BACKOFF_MS 1500
#endif

#define FRAMENAV_HOSTNAME "framenav.device.igp.wiki"
#if FRAMENAV_EMBEDDED_TLS
#define ALPHALOC_AP_URL "https://" FRAMENAV_HOSTNAME "/"
#else
#define ALPHALOC_AP_URL "http://" FRAMENAV_HOSTNAME "/"
#endif
#define ALPHALOC_HTTP_URL "http://" FRAMENAV_HOSTNAME "/"
#define FRAMENAV_CERT_UPDATE_URL \
  "https://framenav.igp.wiki/download/certs.zip"
#define FRAMENAV_CONTACT_EMAIL "sqkkyzx@outlook.com"
#define FRAMENAV_PAIRING_WINDOW_S 90U
#define ALPHALOC_WIFI_ACTIVITY_HOLD_US 30000000LL

static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_fallback_timer = NULL;
static captive_dns_handle_t s_captive_dns = NULL;
static uint8_t s_sta_retry_count = 0;

#if FRAMENAV_EMBEDDED_TLS
extern const unsigned char framenav_server_cert[];
extern const unsigned int framenav_server_cert_len;
extern const unsigned char framenav_server_key[];
extern const unsigned int framenav_server_key_len;
#endif

static tls_credentials_t s_tls_owned;
static const uint8_t *s_tls_cert;
static size_t s_tls_cert_len;
static const uint8_t *s_tls_key;
static size_t s_tls_key_len;

static void tls_cert_release_owned(void) {
  tls_credentials_free(&s_tls_owned);
  s_tls_cert = NULL;
  s_tls_cert_len = 0;
  s_tls_key = NULL;
  s_tls_key_len = 0;
}

static bool tls_cert_load(void) {
  tls_cert_release_owned();
  tls_credentials_t stored = {
      .cert = malloc(FRAMENAV_CERT_MAX_BYTES + 1U),
      .key = malloc(FRAMENAV_KEY_MAX_BYTES + 1U),
  };
  if (stored.cert && stored.key &&
      cert_store_read(stored.cert, FRAMENAV_CERT_MAX_BYTES + 1U, &stored.cert_len,
                        stored.key, FRAMENAV_KEY_MAX_BYTES + 1U, &stored.key_len)) {
    const uint8_t *key = stored.key;
    size_t key_size = stored.key_len + 1U;
#if FRAMENAV_EMBEDDED_TLS
    if (!stored.key_len) {
      key = framenav_server_key; /* Migrate old certificate-only records. */
      key_size = framenav_server_key_len;
    }
#endif
    if (tls_credentials_validate(stored.cert, stored.cert_len + 1U,
                                   key, key_size)) {
      s_tls_owned = stored;
      s_tls_cert = stored.cert;
      s_tls_cert_len = stored.cert_len + 1U;
      s_tls_key = key;
      s_tls_key_len = key_size;
      ESP_LOGI(TAG, "Using downloaded local TLS credentials");
      return true;
    }
  }
  tls_credentials_free(&stored);
#if FRAMENAV_EMBEDDED_TLS
  s_tls_cert = framenav_server_cert;
  s_tls_cert_len = framenav_server_cert_len;
  s_tls_key = framenav_server_key;
  s_tls_key_len = framenav_server_key_len;
  ESP_LOGI(TAG, "Using embedded default TLS credentials");
  return s_tls_cert_len > 0 && s_tls_key_len > 0;
#else
  ESP_LOGW(TAG, "No TLS certificate is available");
  return false;
#endif
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
  if (s_https_server) {
    httpd_ssl_stop(s_https_server);
    s_https_server = NULL;
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
  s_sta_configured = false;
  s_sta_retry_count = 0;
  s_sta_start_us = 0;
  tls_cert_release_owned();
  portENTER_CRITICAL(&s_activity_lock);
  s_activity_us = 0;
  portEXIT_CRITICAL(&s_activity_lock);
}

static void wifi_reconnect_cb(void *arg) {
  (void)arg;
  if (s_started && s_sta_configured && !s_sta_has_ip) {
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
  wifi_cfg.ap.channel = 1;
  wifi_cfg.ap.ssid_hidden = 0;
  wifi_cfg.ap.beacon_interval = 100;

  // Keep the configured STA alive while the fallback AP is available. This
  // lets the device both offer local setup access and reach the Internet.
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
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
  s_sta_has_ip = false;
  if (s_reconnect_timer) (void)esp_timer_stop(s_reconnect_timer);
  if (s_fallback_timer) (void)esp_timer_stop(s_fallback_timer);
  if (s_started && s_sta_configured) {
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
      ESP_LOGW(TAG, "STA reconnect while AP fallback is active failed: %s",
               esp_err_to_name(err));
    }
  }
  ESP_LOGI(TAG, "AP configured: ssid=%s captive=%s", cfg.ap_ssid,
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
      ESP_LOGI(TAG, "WiFi STA address: http://" IPSTR "/; canonical=%s",
               IP2STR(&got_ip->ip_info.ip), ALPHALOC_AP_URL);
    }
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

    if (s_reconnect_timer && s_sta_configured) {
      uint64_t delay_us =
          s_ap_active ? 30000000ULL
                      : (uint64_t)ALPHALOC_WIFI_RECONNECT_BACKOFF_MS * 1000ULL;
      if (s_sta_retry_count < 4) s_sta_retry_count++;
      delay_us *= s_sta_retry_count;
      ESP_LOGI(TAG, "Next external WiFi attempt in %us",
               (unsigned)(delay_us / 1000000ULL));
      esp_timer_stop(s_reconnect_timer);
      esp_timer_start_once(s_reconnect_timer, delay_us);
    }
  }
}

static void wifi_apply_sta_config(const app_config_t *cfg) {
  if (!cfg || !s_started || !s_netif_sta) return;
  s_sta_configured = cfg->wifi_ssid[0] != '\0';
  if (s_reconnect_timer) (void)esp_timer_stop(s_reconnect_timer);
  wifi_config_t sta = {0};
  strncpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid) - 1);
  strncpy((char *)sta.sta.password, cfg->wifi_pass,
          sizeof(sta.sta.password) - 1);
  (void)esp_wifi_disconnect();
  s_sta_has_ip = false;
  if (!s_sta_configured) return;
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Updated STA credentials were rejected: %s",
             esp_err_to_name(err));
    return;
  }
  s_sta_has_ip = false;
  s_sta_start_us = esp_timer_get_time();
  if (cfg->wifi_ssid[0] != '\0') {
    err = esp_wifi_connect();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Updated STA connect start failed: %s",
               esp_err_to_name(err));
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

static esp_err_t handle_location(httpd_req_t *req) {
  web_activity();
  app_config_t cfg;
  if (!config_get_snapshot(s_cfg, &cfg)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "Configuration busy");
  }
  gps_fix_t fix = {0};
  location_source_t source = LOCATION_NONE;
  uint32_t observed = 0;
  bool valid = location_service_details(&fix, &source, &observed);
  char coordinates[96] = "—", altitude[48] = "—", time_text[96] = "—", age[48] = "—";
  const char *label = "无有效定位";
  const char *time_label = "定位时间";
  if (valid && isfinite(fix.lat_deg) && isfinite(fix.lon_deg)) {
    int64_t seconds = (esp_timer_get_time() - fix.last_fix_time_us) / 1000000;
    snprintf(coordinates, sizeof(coordinates), "经度 %.6f，纬度 %.6f（WGS-84）", fix.lon_deg, fix.lat_deg);
    if (fix.altitude_valid && isfinite(fix.altitude_m))
      snprintf(altitude, sizeof(altitude), "%.1f 米", fix.altitude_m);
    else snprintf(altitude, sizeof(altitude), "未提供");
    snprintf(age, sizeof(age), "%lld 秒前", (long long)seconds);
    if (observed) format_track_time(observed, cfg.tz_offset_min, cfg.dst_offset_min, time_text, sizeof(time_text));
    label = source == LOCATION_PHONE ? "手机位置（手动获取）" : "GNSS 卫星定位";
  } else valid = false;
  phone_location_status_t phone;
  location_service_phone_status(&phone);
  char phone_coordinates[96] = "—", phone_accuracy[48] = "—";
  char phone_time[96] = "—", phone_remaining[96] = "—";
  if (phone.present) {
    snprintf(phone_coordinates, sizeof(phone_coordinates), "经度 %.6f，纬度 %.6f（WGS-84）",
               phone.longitude, phone.latitude);
    snprintf(phone_accuracy, sizeof(phone_accuracy), "约 %.0f 米", phone.accuracy_m);
    format_track_time(phone.captured_utc, cfg.tz_offset_min, cfg.dst_offset_min,
                        phone_time, sizeof(phone_time));
  }
  if (phone.available)
    snprintf(phone_remaining, sizeof(phone_remaining), "%u 小时 %u 分 %u 秒",
               (unsigned)(phone.remaining_s / 3600),
               (unsigned)(phone.remaining_s / 60 % 60), (unsigned)(phone.remaining_s % 60));
  char json[2048];
  int len = snprintf(json, sizeof(json),
      "{\"status\":\"%s\",\"source\":\"%s\",\"coordinates\":\"%s\",\"altitude\":\"%s\","
      "\"time\":\"%s\",\"timeLabel\":\"%s\",\"age\":\"%s\","
      "\"phoneStatus\":\"%s\",\"phoneCoordinates\":\"%s\",\"phoneAccuracy\":\"%s\","
      "\"phoneTime\":\"%s\",\"phoneRemaining\":\"%s\"}",
      valid ? "有效" : "无有效定位（尚未获取或已过期）", label, coordinates, altitude,
      time_text, time_label, age,
      phone.available ? "有效" : phone.present ? "已过期，请重新获取" : "尚未获取",
      phone_coordinates, phone_accuracy, phone_time, phone_remaining);
  if (len < 0 || (size_t)len >= sizeof(json))
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status too large");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, len);
}

static void wifi_page_state(char *dot_class, size_t dot_len, char *state,
                            size_t state_len, char *address,
                            size_t address_len) {
  if (!dot_class || !state || !address || dot_len == 0 || state_len == 0 ||
      address_len == 0) {
    return;
  }
  const char *dot = s_sta_has_ip ? "dot-green" :
                    (s_ap_active ? "dot-blue" : "dot-yellow");
  const char *text = s_sta_has_ip
                         ? (s_ap_active ? "已连接外部 Wi‑Fi，同时保留设备热点"
                                        : "已连接外部 Wi‑Fi")
                         : (s_ap_active ? "设备热点已开启，正在等待外部 Wi‑Fi"
                                        : "正在连接外部 Wi‑Fi");
  snprintf(dot_class, dot_len, "%s", dot);
  snprintf(state, state_len, "%s", text);
  snprintf(address, address_len, "未分配");
  if (s_sta_has_ip && s_netif_sta) {
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_netif_sta, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
      ip4_addr_t lwip_ip = {.addr = ip_info.ip.addr};
      (void)ip4addr_ntoa_r(&lwip_ip, address, (int)address_len);
    }
  } else if (s_ap_active) {
    snprintf(address, address_len, "10.254.254.1（设备热点）");
  }
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
  char wifi_dot_class[16];
  char wifi_state[96];
  char wifi_address[64];
  wifi_page_state(wifi_dot_class, sizeof(wifi_dot_class), wifi_state,
                  sizeof(wifi_state), wifi_address, sizeof(wifi_address));
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
      ".brand{font-size:27px;font-weight:780}.brand span{color:#257149}.tagline,.muted,small,.hint{color:#66736b}"
      ".tagline{margin:4px 0 16px}.card{background:#fff;border:1px solid #d7dfd9;border-radius:15px;"
      "padding:18px;margin:13px 0;box-shadow:0 3px 14px rgba(20,50,30,.05)}"
      "h2{font-size:18px;margin:0 0 5px}h3{font-size:14px;margin:18px 0 8px}.caption{font-size:12px;color:#718078;margin:0 0 14px}"
      ".statusbar{display:flex;flex-wrap:wrap;gap:9px 15px;font-size:13px}.statusitem{display:flex;align-items:center;gap:6px}"
      ".dot{width:9px;height:9px;flex-shrink:0;border-radius:50%%}.dot-green{background:#2e9a55}.dot-red{background:#d9534f}"
      ".location-status .statusitem{min-width:0;max-width:100%%;line-height:1.5}"
      ".location-status .statusitem>span:last-child{min-width:0;overflow-wrap:anywhere}"
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
      "<div class=\"statusitem\"><span class=\"dot %s\"></span><span>Wi‑Fi：%s · 地址 %s</span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>固件版本：%s</span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>轨迹数据：%s</span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>最早记录：%s</span></div>",
      gps_dot_class, gps_lock_str, (unsigned)gps_status.satellites,
      gps_const_str, cam_dot_class, cam_conn_str, cam_bond_str,
      wifi_dot_class, wifi_state, wifi_address, firmware_version, track_usage,
      track_earliest);
#if ALPHALOC_BATTERY_MONITOR
  ok = ok && page_send(req, page, page_len,
                         "<div class=\"statusitem\"><span class=\"dot %s\"></span><span>%s</span></div>",
                         bat_dot_class, bat_text);
#endif
  ok = ok && page_send(
      req, page, page_len,
      "</div></section><section class=\"card\"><h2>当前定位</h2>"
      "<div class=\"statusbar location-status\">"
      "<div class=\"statusitem\"><span id=\"loc-dot\" class=\"dot dot-gray\"></span><span>状态：<span id=\"loc-status\">读取中…</span></span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>定位来源：<span id=\"loc-source\">—</span></span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>坐标：<span id=\"loc-coordinates\">—</span></span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>海拔：<span id=\"loc-altitude\">—</span></span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span><span id=\"loc-timeLabel\">定位时间</span>：<span id=\"loc-time\">—</span></span></div>"
      "<div class=\"statusitem\"><span class=\"dot dot-gray\"></span><span>观测距今：<span id=\"loc-age\">—</span></span></div></div>"
      "<details><summary>定位显示说明</summary><p class=\"hint\">每 3 秒刷新，不触发额外定位查询。显示设备当前选用的位置，不代表相机已写入 EXIF。"
      "时间按所选时区显示；手机位置的获取时间保持不变，相机时间会继续前进。</p></details></section>"
      "<section class=\"card\"><h2>连接设置</h2>"
      "<p class=\"caption\">管理相机配对和配置页网络。</p><h3>相机配对</h3>");
  ok = ok && page_send(req, page, page_len, "%s", bonds_html);
  ok = ok && page_send(
      req, page, page_len,
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
      "<label class=\"field\">USB 插入后无设备接入自动关闭</label><select class=\"text\" form=\"settings\" name=\"wifi_idle_s\">"
      "<option value=\"300\" %s>5 分钟</option><option value=\"900\" %s>15 分钟</option></select>"
      "<p class=\"hint\">Wi‑Fi 只在检测到 USB 线缆时开启；备用热点按关联设备保持，接入现有 Wi‑Fi 时按近期网页活动保持。无设备接入或活动达到所选时间后关闭，需拔出 USB 再重新插入才能再次开启。</p></details></section>",
      pairing_state, camera_name, camera_mac, wifi_ssid, ap_ssid,
      cfg.wifi_idle_timeout_s == WIFI_IDLE_TIMEOUT_DEFAULT_S ? "selected" : "",
      cfg.wifi_idle_timeout_s == WIFI_IDLE_TIMEOUT_LONG_S ? "selected" : "");
  free(bonds_html);

  ok = ok && page_send(
      req, page, page_len,
      "<section class=\"card\"><h2>安全连接</h2>"
      "<p class=\"hint\">配置页使用 <strong>" FRAMENAV_HOSTNAME
      "</strong>。设备同时提供热点并尝试连接已保存的外部 Wi‑Fi；更新证书需要外部 Wi‑Fi 能上网。</p>"
      "<form method=\"POST\" action=\"/cert/update\">"
      "<button class=\"secondary\" type=\"submit\">下载并更新证书和私钥</button></form>"
      "<p class=\"hint\">更新地址：" FRAMENAV_CERT_UPDATE_URL
      "。将一起下载证书和私钥，校验通过后保存；更新失败时保留现有凭据。"
      "保存后重新插拔 USB 才会加载新证书和私钥。</p></section>");

  ok = ok && page_send(
      req, page, page_len,
      "<section class=\"card\"><h2>定位设置</h2><p class=\"caption\">控制发送给相机的位置和时间信息。</p>"
      "<p class=\"hint\">GPS 始终搜星，有有效卫星定位时优先使用 GPS；没有 GPS 时，使用尚未过期的手机位置。</p>"
      "<h3>室内手机位置</h3>"
      "<button class=\"secondary\" id=\"phone-get\" type=\"button\">从手机获取一次定位</button> "
      "<button class=\"secondary\" id=\"phone-clear\" type=\"button\">清除手机位置</button>"
      "<p class=\"hint\" id=\"phone-message\" role=\"status\">请在系统浏览器打开 HTTPS 配置页，点击获取并允许手机定位。</p>"
      "<p class=\"hint\">先尝试普通定位 15 秒，可使用手机最近 10 分钟的位置；失败后再尝试高精度定位 60 秒。室内请保持移动数据可用。Chrome 定位失败时，请用手机自带浏览器获取；位置保存到设备后，切换浏览器仍可使用。</p>"
      "<h3>无法调用手机定位时手动输入</h3><div class=\"grid\">"
      "<div><label class=\"field\">纬度</label><input class=\"text\" id=\"manual-lat\" type=\"number\" min=\"-90\" max=\"90\" step=\"0.000001\" placeholder=\"例如 23.1291\"></div>"
      "<div><label class=\"field\">经度</label><input class=\"text\" id=\"manual-lon\" type=\"number\" min=\"-180\" max=\"180\" step=\"0.000001\" placeholder=\"例如 113.2644\"></div></div>"
      "<button class=\"secondary\" id=\"phone-manual\" type=\"button\">保存手动位置</button>"
      "<p class=\"hint\">手动位置按同一有效期保存；GPS 有效时仍自动优先使用 GPS。坐标使用 WGS‑84 十进制度。</p>"
      "<p>手机位置：<span id=\"loc-phoneStatus\">读取中</span></p>"
      "<p id=\"loc-phoneCoordinates\">—</p>"
      "<p>手机估算精度：<span id=\"loc-phoneAccuracy\">—</span></p>"
      "<p>获取时间：<span id=\"loc-phoneTime\">—</span></p>"
      "<p>剩余有效期：<span id=\"loc-phoneRemaining\">—</span></p>"
      "<label class=\"field\">手机位置有效期</label><div class=\"unit\">"
      "<input class=\"text\" form=\"settings\" type=\"number\" name=\"phone_ttl_h\" value=\"%u\" min=\"1\" max=\"720\" step=\"1\"><span>小时</span></div>"
      "<p class=\"hint\">默认 6 小时，从手机获取时间开始计算；修改后请保存设置。关闭 Wi-Fi 或拔插 USB 不清除位置，整机断电后需重新获取。</p>"
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
      (unsigned)cfg.phone_location_ttl_hours);

  ok = ok && page_send(
      req, page, page_len,
      "<section class=\"card\"><h2>省电设置</h2><p class=\"caption\">GPS 持续搜星；可单独配置蓝牙搜索策略。</p>"
      "<h3>蓝牙省电模式</h3><div class=\"modes\">"
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
      (unsigned)bluetooth_policy.boot_fast_search_s,
      (double)bluetooth_policy.scan_period_ms / 1000.0,
      (unsigned)bluetooth_policy.scan_window_ms);

  ok = ok && page_send(
      req, page, page_len,
      "<div class=\"savebar\"><p class=\"hint\">定位与省电设置保存后立即生效；Wi‑Fi 设置下次开启配置网络时生效。</p>"
      "<button form=\"settings\" type=\"submit\">保存设置</button></div>"
      "<footer>配置地址：<a href=\"" ALPHALOC_AP_URL "\">" ALPHALOC_AP_URL
      "</a><br>联系邮箱：<a href=\"mailto:" FRAMENAV_CONTACT_EMAIL "\">" FRAMENAV_CONTACT_EMAIL "</a>"
      "<br>FrameNav 是基于 <a href=\"https://github.com/finfinack/alphaloc\">AlphaLoc</a> "
      "开发的开源项目，采用 <a href=\"https://www.apache.org/licenses/LICENSE-2.0\">Apache-2.0</a> 许可证。</footer>"
      "<script>(()=>{const q=(s)=>document.querySelector(s),qa=(s)=>[...document.querySelectorAll(s)];"
      "let locating=false;async function liveLocation(){if(document.hidden||locating)return;locating=true;"
      "const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),5000);"
      "try{const r=await fetch('/location',{cache:'no-store',signal:controller.signal});if(!r.ok)throw Error();"
      "const d=await r.json();['status','source','coordinates','altitude','time','timeLabel','age',"
      "'phoneStatus','phoneCoordinates','phoneAccuracy','phoneTime','phoneRemaining'].forEach(k=>q('#loc-'+k).textContent=d[k]);"
      "q('#loc-dot').className='dot '+(d.status==='有效'?'dot-green':'dot-red');}"
      "catch(e){q('#loc-status').textContent='读取失败，无法确认当前定位';"
      "q('#loc-dot').className='dot dot-gray';"
      "['source','coordinates','altitude','time','age','phoneStatus','phoneCoordinates','phoneAccuracy','phoneTime','phoneRemaining'].forEach(k=>q('#loc-'+k).textContent='—');}"
      "finally{clearTimeout(timer);locating=false;}}setInterval(liveLocation,3000);"
      "document.addEventListener('visibilitychange',liveLocation);liveLocation();"
      "const pg=q('#phone-get'),pc=q('#phone-clear'),pm=q('#phone-message'),manualButton=q('#phone-manual'),ml=q('#manual-lat'),mn=q('#manual-lon');"
      "function phoneBusy(b){pg.disabled=pc.disabled=manualButton.disabled=b;}"
      "async function phonePost(path,data){const c=new AbortController(),t=setTimeout(()=>c.abort(),8000);"
      "try{const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify(data),signal:c.signal});const text=await r.text();if(!r.ok)throw Error(text);pm.textContent=text;}"
      "finally{clearTimeout(t);}}"
      "pg.addEventListener('click',()=>{if(!window.isSecureContext){pm.textContent='请使用 https://" FRAMENAV_HOSTNAME "/ 打开后获取定位';return;}"
      "if(!navigator.geolocation){pm.textContent='当前浏览器不支持定位，请用手机系统浏览器打开';return;}"
      "phoneBusy(true);pm.textContent='正在请求手机定位，请允许定位权限…';"
      "const save=async p=>{try{pm.textContent='正在保存手机位置…';"
      "await phonePost('/location/phone',{latitude:p.coords.latitude,longitude:p.coords.longitude,"
      "accuracy:p.coords.accuracy,timestamp_ms:Math.trunc(p.timestamp),now_ms:Date.now()});liveLocation();}"
      "catch(e){pm.textContent='保存失败：'+(e.message||'请重试');}finally{phoneBusy(false);}};"
      "const failed=e=>{pm.textContent=({1:'定位权限被拒绝，请在浏览器设置中允许定位',2:'手机暂时无法定位，请先在地图 App 确认定位正常',3:'手机定位超时，请先在地图 App 完成定位，并尝试手机自带浏览器'})[e.code]||'手机定位失败';phoneBusy(false);};"
      "navigator.geolocation.getCurrentPosition(save,e=>{if(e.code!==2&&e.code!==3){failed(e);return;}"
      "pm.textContent='普通定位暂不可用，正在尝试高精度定位，最多等待 60 秒…';"
      "navigator.geolocation.getCurrentPosition(save,failed,{enableHighAccuracy:true,maximumAge:60000,timeout:60000});},"
      "{enableHighAccuracy:false,maximumAge:600000,timeout:15000});});"
      "manualButton.addEventListener('click',()=>{const latitude=Number(ml.value),longitude=Number(mn.value);"
      "if(!Number.isFinite(latitude)||!Number.isFinite(longitude)||latitude<-90||latitude>90||longitude<-180||longitude>180){pm.textContent='请输入有效的纬度（−90 至 90）和经度（−180 至 180）';return;}"
      "phoneBusy(true);pm.textContent='正在保存手动位置…';phonePost('/location/phone',{latitude,longitude,accuracy:0,timestamp_ms:Date.now(),now_ms:Date.now()}).then(liveLocation).catch(e=>{pm.textContent='保存失败：'+(e.message||'请重试');}).finally(()=>phoneBusy(false));});"
      "pc.addEventListener('click',async()=>{phoneBusy(true);try{await phonePost('/location/phone/clear',{});liveLocation();}"
      "catch(e){pm.textContent='清除失败：'+(e.message||'请重试');}finally{phoneBusy(false);}});"
      "function pulse(){if(!document.hidden)fetch('/activity',{cache:'no-store'}).catch(()=>{})}"
      "setInterval(pulse,10000);document.addEventListener('visibilitychange',pulse);pulse();"
      "const bi=qa('.ble-custom'),bs=q('#ble-state');"
      "const bp={0:[15,10,100],1:[30,3,100]};"
      "function ble(m,change){if(bp[m]&&change)bi.forEach((e,i)=>e.value=bp[m][i]);"
      "bi.forEach(e=>e.disabled=m!==2);bs.textContent=m===2?'可编辑':'只读'}"
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
  app_config_t previous = next;

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

  uint32_t parsed = 0;
  form_get(body, "phone_ttl_h", value, sizeof(value));
  if (strstr(body, "phone_ttl_h=") != NULL) {
    if (!parse_u32_range(value, PHONE_LOCATION_TTL_MAX_HOURS, &parsed) || parsed < 1)
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Phone location TTL must be 1-720 hours");
    next.phone_location_ttl_hours = parsed;
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
                               "Save failed");
  }
  app_config_t applied;
  if (!config_get_snapshot(s_cfg, &applied)) {
    applied = next;
    config_validate(&applied);
  }
  if (strcmp(previous.wifi_ssid, applied.wifi_ssid) != 0 ||
      strcmp(previous.wifi_pass, applied.wifi_pass) != 0) {
    wifi_apply_sta_config(&applied);
  }
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
  if (!valid || !location_service_set_phone(latitude, longitude, accuracy,
                                               (uint64_t)captured_ms, (uint64_t)now_ms)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "手机定位数据无效、已陈旧或手机时间不正确，请重新获取。\n");
  }
  return httpd_resp_sendstr(req, "手机位置已保存；无 GPS 时将在有效期内使用，GPS 恢复后自动优先使用 GPS。\n");
}

static esp_err_t handle_phone_clear(httpd_req_t *req) {
  web_activity();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  cJSON *json = read_phone_json(req);
  if (!cJSON_IsObject(json)) {
    cJSON_Delete(json);
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "Invalid request");
  }
  cJSON_Delete(json);
  location_service_clear_phone();
  return httpd_resp_sendstr(req, "手机位置已清除。\n");
}

static esp_err_t handle_pair_start(httpd_req_t *req) {
  web_activity();
  if (!ble_client_start_pairing(FRAMENAV_PAIRING_WINDOW_S)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "蓝牙尚未就绪，暂时无法开始配对。\n");
  }
  return redirect_to_root(req);
}

static esp_err_t handle_bond_delete(httpd_req_t *req) {
  web_activity();
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

#if FRAMENAV_EMBEDDED_TLS
typedef struct {
  uint8_t *buffer;
  size_t length, capacity;
  bool failed;
} cert_download_ctx_t;

static void cert_download_free(cert_download_ctx_t *ctx) {
  volatile uint8_t *bytes = ctx->buffer;
  for (size_t i = 0; i < ctx->capacity; ++i) bytes[i] = 0;
  free(ctx->buffer);
  *ctx = (cert_download_ctx_t){0};
}

static bool cert_download_append(cert_download_ctx_t *ctx, const uint8_t *data,
                                  size_t incoming) {
  if (incoming > FRAMENAV_TLS_ZIP_MAX_BYTES - ctx->length) {
    ctx->failed = true;
    return false;
  }
  size_t needed = ctx->length + incoming;
  if (needed > ctx->capacity) {
    size_t capacity = ctx->capacity ? ctx->capacity : 4096U;
    while (capacity < needed) capacity *= 2U;
    /* Wipe the previous allocation before freeing: it may contain a key. */
    uint8_t *buffer = malloc(capacity);
    if (!buffer) {
      ctx->failed = true;
      return false;
    }
    if (ctx->length) memcpy(buffer, ctx->buffer, ctx->length);
    size_t length = ctx->length;
    cert_download_free(ctx);
    *ctx = (cert_download_ctx_t){.buffer = buffer, .capacity = capacity,
                                .length = length};
  }
  memcpy(ctx->buffer + ctx->length, data, incoming);
  ctx->length += incoming;
  return true;
}

static bool download_updated_tls_credentials(void) {
  cert_download_ctx_t ctx = {0};
  esp_http_client_config_t config = {
      .url = FRAMENAV_CERT_UPDATE_URL,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 15000,
      .buffer_size = 1024,
      .buffer_size_tx = 512,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .disable_auto_redirect = true,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return false;
  bool ok = esp_http_client_open(client, 0) == ESP_OK;
  int64_t content_length = ok ? esp_http_client_fetch_headers(client) : -1;
  ok = ok && content_length >= 0 &&
       content_length <= FRAMENAV_TLS_ZIP_MAX_BYTES &&
       esp_http_client_get_status_code(client) == 200;
  uint8_t chunk[1024];
  int64_t deadline = esp_timer_get_time() + 30000000LL;
  /* fetch_headers() may already have cached the entire body. Drain read()
   * even when the network receive counter reports completion. */
  while (ok) {
    if (esp_timer_get_time() >= deadline) { ok = false; break; }
    int received = esp_http_client_read(client, (char *)chunk, sizeof(chunk));
    if (received <= 0) {
      ok = received == 0 && esp_http_client_is_complete_data_received(client);
      break;
    }
    ok = cert_download_append(&ctx, chunk, (size_t)received);
  }
  ok = ok && esp_http_client_is_complete_data_received(client);
  /* Free the outbound TLS session before allocating/parsing the new pair. */
  esp_http_client_cleanup(client);
  volatile uint8_t *chunk_bytes = chunk;
  for (size_t i = 0; i < sizeof(chunk); ++i) chunk_bytes[i] = 0;
  tls_credentials_t credentials = {0};
  ok = ok && !ctx.failed &&
       tls_credentials_from_zip(ctx.buffer, ctx.length, &credentials);
  cert_download_free(&ctx);
  if (ok) ok = cert_store_write(credentials.cert, credentials.cert_len,
                                  credentials.key, credentials.key_len);
  tls_credentials_free(&credentials);
  return ok;
}
#endif

static esp_err_t handle_cert_update(httpd_req_t *req) {
  web_activity();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
#if !FRAMENAV_EMBEDDED_TLS
  httpd_resp_set_status(req, "503 Service Unavailable");
  return httpd_resp_sendstr(req, "No embedded TLS key");
#else
  if (!s_sta_has_ip) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(
        req, "请先连接可上网的 Wi‑Fi，设备热点本身不能下载证书");
  }
  portENTER_CRITICAL(&s_radio_lock);
  bool claimed = !s_cert_update_busy && !s_radio_owned;
  if (claimed) s_cert_update_busy = true;
  portEXIT_CRITICAL(&s_radio_lock);
  if (!claimed) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "设备正在更新证书或使用 Wi‑Fi，请稍后重试");
  }
  bool ok = download_updated_tls_credentials();
  portENTER_CRITICAL(&s_radio_lock);
  s_cert_update_busy = false;
  portEXIT_CRITICAL(&s_radio_lock);
  if (!ok) {
    httpd_resp_set_status(req, "502 Bad Gateway");
    return httpd_resp_sendstr(
        req, "下载、解包或证书与私钥校验失败，或保存失败；现有 HTTPS 服务继续使用原凭据。\n");
  }
  return httpd_resp_sendstr(
      req, "证书和私钥已校验并保存；重新插拔 USB 让 HTTPS 服务加载新凭据。\n");
#endif
}

static esp_err_t register_web_routes(httpd_handle_t server) {
  esp_err_t err;
  httpd_uri_t root = {
      .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL};
  httpd_uri_t location = {
      .uri = "/location", .method = HTTP_GET, .handler = handle_location};
  httpd_uri_t phone_location = {
      .uri = "/location/phone", .method = HTTP_POST, .handler = handle_phone_location};
  httpd_uri_t phone_clear = {
      .uri = "/location/phone/clear", .method = HTTP_POST, .handler = handle_phone_clear};
  httpd_uri_t activity = {
      .uri = "/activity", .method = HTTP_GET, .handler = handle_activity};
  httpd_uri_t favicon = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = handle_favicon};
  httpd_uri_t save = {
      .uri = "/save", .method = HTTP_POST, .handler = handle_save};
  httpd_uri_t cert_update = {.uri = "/cert/update",
                             .method = HTTP_POST,
                             .handler = handle_cert_update};
  httpd_uri_t pair_start = {
      .uri = "/pair/start", .method = HTTP_POST, .handler = handle_pair_start};
  httpd_uri_t bond_delete = {.uri = "/bond/delete",
                             .method = HTTP_POST,
                             .handler = handle_bond_delete};
  err = httpd_register_uri_handler(server, &root);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &location);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &phone_location);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &phone_clear);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &activity);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &favicon);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &save);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &cert_update);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &pair_start);
  if (err == ESP_OK) err = httpd_register_uri_handler(server, &bond_delete);
  if (err == ESP_OK) {
    err = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND,
                                     handle_captive_redirect);
  }
  return err;
}

#if FRAMENAV_EMBEDDED_TLS
static void https_session_event(esp_https_server_user_cb_arg_t *session) {
  if (session && session->user_cb_state == HTTPD_SSL_USER_CB_SESS_CREATE) {
    ESP_LOGI(TAG, "HTTPS client handshake succeeded; heap free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
}

static bool start_https_server(void) {
  if (!s_tls_cert && !tls_cert_load()) return false;
  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.httpd.uri_match_fn = httpd_uri_match_wildcard;
  config.httpd.stack_size = 16384;
  config.httpd.max_uri_handlers = 12;
  config.httpd.max_open_sockets = FRAMENAV_HTTPS_MAX_CLIENTS;
  config.httpd.lru_purge_enable = true;
  // Session tickets triggered read/cleanup crashes with this IDF dynamic-TLS
  // configuration. Keep established HTTPS connections and use full handshakes.
  config.session_tickets = false;
  config.user_cb = https_session_event;
  config.servercert = s_tls_cert;
  config.servercert_len = s_tls_cert_len;
  config.prvtkey_pem = s_tls_key;
  config.prvtkey_len = s_tls_key_len;
  esp_err_t err = httpd_ssl_start(&s_https_server, &config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "HTTPS server start failed: %s", esp_err_to_name(err));
    s_https_server = NULL;
    return false;
  }
  err = register_web_routes(s_https_server);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "HTTPS route registration failed: %s", esp_err_to_name(err));
    httpd_ssl_stop(s_https_server);
    s_https_server = NULL;
    return false;
  }
  ESP_LOGI(TAG, "HTTPS web ready: %s; TLS output=%u bytes, clients=%u",
           ALPHALOC_AP_URL, (unsigned)MBEDTLS_SSL_OUT_CONTENT_LEN,
           (unsigned)FRAMENAV_HTTPS_MAX_CLIENTS);
  return true;
}
#endif

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
  s_sta_configured = cfg_snapshot.wifi_ssid[0] != '\0';
  wifi_config_t wifi_cfg = {0};
  strncpy((char *)wifi_cfg.sta.ssid, cfg_snapshot.wifi_ssid,
          sizeof(wifi_cfg.sta.ssid) - 1);
  strncpy((char *)wifi_cfg.sta.password, cfg_snapshot.wifi_pass,
          sizeof(wifi_cfg.sta.password) - 1);
  err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set WiFi AP+STA mode: %s", esp_err_to_name(err));
    wifi_cleanup_resources();
    return false;
  }
  err = s_sta_configured ? esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg) : ESP_OK;
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure WiFi STA: %s", esp_err_to_name(err));
    s_sta_configured = false;
  }
  // Configure the AP while the radio is stopped. USB always exposes local
  // setup immediately; a failed upstream association must not hide the AP.
  if (!start_softap()) {
    wifi_cleanup_resources();
    return false;
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
  if (s_sta_configured) {
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
  server_cfg.max_uri_handlers = 12;
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

#if FRAMENAV_EMBEDDED_TLS
  (void)cert_store_init();
  (void)tls_cert_load();
  (void)start_https_server();
#endif

  if (!s_ap_active && !s_sta_has_ip && s_fallback_timer) {
    (void)esp_timer_start_once(
        s_fallback_timer,
        (uint64_t)ALPHALOC_WIFI_STA_FALLBACK_TIMEOUT_MS * 1000ULL);
  }
  ESP_LOGI(TAG, "WiFi web started: AP available immediately; external WiFi=%s",
           s_sta_configured ? "configured" : "not configured");
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
  // A STA having an upstream IP only proves that the device joined the
  // configured router. Treat recent HTTP activity as the user connection;
  // otherwise the router itself would keep Wi-Fi on forever. This also covers
  // a phone using the external Wi-Fi path while the fallback AP is active.
  if (ap_client) return true;
  int64_t last = wifi_web_last_activity_us();
  int64_t now = esp_timer_get_time();
  return last > 0 && now >= last && now - last <= ALPHALOC_WIFI_ACTIVITY_HOLD_US;
}
