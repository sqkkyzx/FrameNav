#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Render the actual configuration handler with a fragmented-heap limit."""

import unittest

from host_phase1 import ROOT, function_source, run_c


class WebStreamChecks(unittest.TestCase):
    def test_complete_page_under_allocation_limit_and_failed_connection_cleanup(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        root = "static esp_err_t handle_root" + source.split(
            "static esp_err_t handle_root", 1
        )[1].split("static esp_err_t handle_save", 1)[0]
        run_c(
            r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ble_client.h"

#define ALPHALOC_BATTERY_MONITOR 0
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef struct { int unused; } httpd_req_t;
typedef struct {
  bool available;
  uint32_t capacity_bytes, used_bytes, record_count;
} track_store_status_t;

static app_config_t cfg;
static app_config_t *s_cfg = &cfg;
static char response[65536];
static size_t response_used, live_bytes, peak_bytes;
static size_t allocation_limit = 8192;
static int chunks, fail_chunk, errors, rejected, completed;

static void *limited_malloc(size_t size) {
  if (size > allocation_limit) { ++rejected; return NULL; }
  size_t *block = malloc(sizeof(*block) + size);
  if (!block) return NULL;
  *block = size;
  live_bytes += size;
  if (live_bytes > peak_bytes) peak_bytes = live_bytes;
  return block + 1;
}
static void limited_free(void *ptr) {
  if (!ptr) return;
  size_t *block = (size_t *)ptr - 1;
  live_bytes -= *block;
  free(block);
}
static void web_activity(void) {}
bool config_get_snapshot(const app_config_t *in, app_config_t *out) {
  *out = *in; return true;
}
void config_validate(app_config_t *in) { (void)in; }
void config_resolve_bluetooth_power(const app_config_t *in,
                                   bluetooth_power_policy_t *out) {
  (void)in;
  *out = (bluetooth_power_policy_t){30, 3000, 100};
}
bool gps_get_status(gps_status_t *out) {
  *out = (gps_status_t){.has_lock=true, .satellites=12,
                        .constellations=GPS_CONSTELLATION_GPS,
                        .satellites_visible=12}; return true;
}
bool ble_client_is_connected(void) { return true; }
bool ble_client_is_bonded(void) { return true; }
uint32_t ble_client_pairing_seconds_remaining(void) { return 90; }
int ble_client_get_bonds(ble_client_bond_t *out, size_t capacity) {
  assert(capacity >= 3);
  for (int i = 0; i < 3; ++i) {
    out[i].addr[5] = (uint8_t)(i + 1);
    out[i].addr_type = 1;
    out[i].connected = i == 0;
    memset(out[i].name, '"', sizeof(out[i].name) - 1);
  }
  return 3;
}
bool track_store_get_status(track_store_status_t *out) {
  *out = (track_store_status_t){true, 1536000, 65536, 4096};
  return true;
}
static void wifi_page_state(char *dot, size_t dot_len, char *state,
                            size_t state_len) {
  snprintf(dot, dot_len, "dot-green");
  snprintf(state, state_len, "connected");
}
static int httpd_resp_set_type(httpd_req_t *req, const char *type) {
  (void)req; assert(!strcmp(type, "text/html; charset=utf-8")); return ESP_OK;
}
static int httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value) {
  (void)req; assert(!strcmp(name, "Cache-Control"));
  assert(!strcmp(value, "no-store")); return ESP_OK;
}
static int httpd_resp_send_err(httpd_req_t *req, int status, const char *message) {
  (void)req; (void)message; assert(status == 500); ++errors; return ESP_FAIL;
}
static int httpd_resp_send_chunk(httpd_req_t *req, const char *data, int size) {
  (void)req;
  if (++chunks == fail_chunk) return ESP_FAIL;
  if (!size) { ++completed; return ESP_OK; }
  assert(size > 0 && response_used + (size_t)size < sizeof(response));
  memcpy(response + response_used, data, (size_t)size);
  response_used += (size_t)size;
  response[response_used] = '\0';
  return ESP_OK;
}
#define malloc limited_malloc
#define free limited_free
'''
            + function_source("src/wifi_web.c", "html_escape_attr")
            + function_source("src/wifi_web.c", "page_send")
            + root
            + r'''
static void reset_response(void) {
  assert(live_bytes == 0);
  response_used = peak_bytes = 0;
  chunks = fail_chunk = errors = rejected = completed = 0;
  response[0] = '\0';
}
int main(void) {
  httpd_req_t req = {0};
  memset(cfg.camera_name_prefix, '"', sizeof(cfg.camera_name_prefix) - 1);
  memset(cfg.camera_mac_prefix, '&', sizeof(cfg.camera_mac_prefix) - 1);
  memset(cfg.wifi_ssid, '"', sizeof(cfg.wifi_ssid) - 1);
  cfg.bluetooth_power_mode = BLUETOOTH_POWER_CUSTOM;
  cfg.wifi_idle_timeout_s = WIFI_IDLE_TIMEOUT_LONG_S;
  cfg.phone_location_ttl_hours = PHONE_LOCATION_TTL_MAX_HOURS;
  cfg.tz_offset_min = -720;
  cfg.dst_offset_min = 60;
  for (int i = 0; i < 20; ++i) {
    reset_response();
    assert(handle_root(&req) == ESP_OK);
    assert(!errors && !rejected && completed == 1 && chunks > 1);
    assert(live_bytes == 0 && peak_bytes <= 12288);
    assert(!strstr(response, "固件版本"));
    assert(!strstr(response, "轨迹数据"));
    assert(!strstr(response, "10.254.254.1"));
    assert(strstr(response, "&quot;&quot;"));
    assert(!strstr(response, "navigator.geolocation"));
    assert(!strstr(response, "action=\"/cert/update\""));
    assert(strstr(response, "轨迹条数："));
    assert(strstr(response, "存储占用："));
    assert(!strstr(response, "ap_ssid"));
    assert(strstr(response, "搜到卫星："));
    assert(strstr(response, "loc-running-gnss-dot"));
    assert(strstr(response, "loc-running-gnss\">"));
    assert(strstr(response, "相机和手机都未连接时搜星"));
    assert(strstr(response, "name=\"track_int_s\""));
    assert(strstr(response, "border-radius:50%"));
    assert(response_used > 8000);
    assert(!strcmp(response + response_used - 14, "</body></html>"));
  }
  reset_response(); fail_chunk = 3;
  assert(handle_root(&req) == ESP_FAIL);
  assert(!errors && !completed && live_bytes == 0);
  reset_response(); allocation_limit = 4096;
  assert(handle_root(&req) == ESP_FAIL);
  assert(errors == 1 && rejected == 1 && !chunks && live_bytes == 0);
  return 0;
}
'''
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
