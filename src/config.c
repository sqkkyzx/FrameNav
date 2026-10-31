/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "framenav"
#define GPS_INTERVAL_MIN_MS 1000U
#define GPS_INTERVAL_MAX_MS 60000U
#define MAX_GPS_AGE_MAX_S 86400U
#define CONFIG_WINDOW_MAX_S 3600U
#define TZ_OFFSET_MIN_MIN -1440
#define TZ_OFFSET_MAX_MIN 1440
#define BLE_PASSKEY_MAX 999999U
#define LOCATION_DURATION_MAX_S 3600U
#define LOCATION_SLEEP_MIN_S 5U
#define LOCATION_FAST_SPEED_MIN_X10 19U
#define LOCATION_FAST_SPEED_MAX_X10 3000U
#define LOCATION_TIMEOUT_MAX_S 300U
#define BLE_FAST_SEARCH_MAX_S 300U
#define BLE_SCAN_PERIOD_MIN_MS 1000U
#define BLE_SCAN_PERIOD_MAX_MS 10240U
#define BLE_SCAN_WINDOW_MIN_MS 20U
#define BLE_SCAN_WINDOW_MAX_MS 1000U
static const char *TAG = "config";
static SemaphoreHandle_t s_config_mutex = NULL;

static void config_ensure_mutex(void)
{
  if (s_config_mutex == NULL)
  {
    s_config_mutex = xSemaphoreCreateMutex();
    if (s_config_mutex == NULL)
    {
      ESP_LOGE(TAG, "Failed to create config mutex");
    }
  }
}

static void config_set_str_default(char *dst, size_t dst_len, const char *src)
{
  if (!dst || dst_len == 0)
  {
    return;
  }
  if (!src)
  {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = '\0';
}

static void config_terminate_str(char *dst, size_t dst_len)
{
  if (dst && dst_len > 0)
  {
    dst[dst_len - 1] = '\0';
  }
}

static bool source_order_is_valid(const char *order)
{
  if (!order || !order[0]) return false;
  char copy[sizeof(((app_config_t *)0)->source_order)];
  config_set_str_default(copy, sizeof(copy), order);
  bool seen_gnss = false, seen_phone = false, seen_manual = false;
  char *cursor = copy;
  unsigned count = 0;
  while (*cursor)
  {
    char *end = strchr(cursor, ',');
    if (end) *end = '\0';
    if (strcmp(cursor, "gnss") == 0 && !seen_gnss) seen_gnss = true;
    else if (strcmp(cursor, "phone") == 0 && !seen_phone) seen_phone = true;
    else if (strcmp(cursor, "manual") == 0 && !seen_manual) seen_manual = true;
    else return false;
    ++count;
    if (!end) break;
    cursor = end + 1;
  }
  return count == 3 && seen_gnss && seen_phone && seen_manual;
}

void config_set_defaults(app_config_t *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->gps_interval_ms = 5000;
  cfg->max_gps_age_s = 300;
  cfg->config_window_s = 300;
  cfg->ble_passkey = 123456;
  cfg->tz_offset_min = 60;
  cfg->dst_offset_min = 60;
  cfg->location_power_mode = LOCATION_POWER_BALANCED;
  cfg->bluetooth_power_mode = BLUETOOTH_POWER_NORMAL;
  cfg->custom_boot_work_s = 45;
  cfg->custom_disconnect_grace_s = 20;
  cfg->custom_stationary_sleep_s = 90;
  cfg->custom_moving_sleep_s = 30;
  cfg->custom_fast_sleep_s = 15;
  cfg->custom_fast_speed_kmh_x10 = 288;
  cfg->custom_fix_timeout_s = 45;
  cfg->custom_ble_boot_fast_search_s = 30;
  cfg->custom_ble_scan_period_ms = 3000;
  cfg->custom_ble_scan_window_ms = 100;
  cfg->wifi_idle_timeout_s = WIFI_IDLE_TIMEOUT_DEFAULT_S;
  cfg->phone_location_ttl_hours = PHONE_LOCATION_TTL_DEFAULT_HOURS;
  cfg->fixed_location_priority = FIXED_LOCATION_GPS_PRIORITY;
  cfg->fixed_location_delete_policy = FIXED_LOCATION_KEEP_UNTIL_EXPIRY;
  cfg->gnss_without_camera = 0;
  cfg->record_without_camera = 0;
  cfg->track_interval_s = LOCATION_RECORD_INTERVAL_DEFAULT_S;
  cfg->source_gnss_enabled = 1;
  cfg->source_phone_enabled = 1;
  cfg->source_manual_enabled = 1;
  config_set_str_default(cfg->source_order, sizeof(cfg->source_order),
                         "gnss,phone,manual");
  cfg->track_enabled = 1;
  cfg->phone_push_enabled = 1;
  cfg->phone_interval_s = 5;
  cfg->manual_auto_update = 1;
#ifdef ALPHALOC_DEFAULT_CAMERA_NAME_PREFIX
  config_set_str_default(cfg->camera_name_prefix,
                         sizeof(cfg->camera_name_prefix),
                         ALPHALOC_DEFAULT_CAMERA_NAME_PREFIX);
#else
  config_set_str_default(cfg->camera_name_prefix,
                         sizeof(cfg->camera_name_prefix), "SonyA7");
#endif

#ifdef ALPHALOC_DEFAULT_CAMERA_MAC_PREFIX
  config_set_str_default(cfg->camera_mac_prefix,
                         sizeof(cfg->camera_mac_prefix),
                         ALPHALOC_DEFAULT_CAMERA_MAC_PREFIX);
#else
  cfg->camera_mac_prefix[0] = '\0';
#endif

#ifdef ALPHALOC_DEFAULT_WIFI_SSID
  config_set_str_default(cfg->wifi_ssid, sizeof(cfg->wifi_ssid),
                         ALPHALOC_DEFAULT_WIFI_SSID);
#else
  config_set_str_default(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "");
#endif

#ifdef ALPHALOC_DEFAULT_WIFI_PASS
  config_set_str_default(cfg->wifi_pass, sizeof(cfg->wifi_pass),
                         ALPHALOC_DEFAULT_WIFI_PASS);
#else
  config_set_str_default(cfg->wifi_pass, sizeof(cfg->wifi_pass), "");
#endif
}

void config_validate(app_config_t *cfg)
{
  if (!cfg)
  {
    return;
  }

  app_config_t defaults;
  config_set_defaults(&defaults);

  if (cfg->gps_interval_ms < GPS_INTERVAL_MIN_MS ||
      cfg->gps_interval_ms > GPS_INTERVAL_MAX_MS)
  {
    cfg->gps_interval_ms = defaults.gps_interval_ms;
  }
  if (cfg->max_gps_age_s > MAX_GPS_AGE_MAX_S)
  {
    cfg->max_gps_age_s = defaults.max_gps_age_s;
  }
  if (cfg->config_window_s > CONFIG_WINDOW_MAX_S)
  {
    cfg->config_window_s = defaults.config_window_s;
  }
  if (cfg->ble_passkey > BLE_PASSKEY_MAX)
  {
    cfg->ble_passkey = defaults.ble_passkey;
  }
  if (cfg->tz_offset_min < TZ_OFFSET_MIN_MIN ||
      cfg->tz_offset_min > TZ_OFFSET_MAX_MIN)
  {
    cfg->tz_offset_min = defaults.tz_offset_min;
  }
  if (cfg->dst_offset_min < TZ_OFFSET_MIN_MIN ||
      cfg->dst_offset_min > TZ_OFFSET_MAX_MIN)
  {
    cfg->dst_offset_min = defaults.dst_offset_min;
  }
  if (cfg->location_power_mode > LOCATION_POWER_CUSTOM)
  {
    cfg->location_power_mode = defaults.location_power_mode;
  }
  if (cfg->bluetooth_power_mode > BLUETOOTH_POWER_CUSTOM)
  {
    cfg->bluetooth_power_mode = defaults.bluetooth_power_mode;
  }
  if (cfg->custom_boot_work_s > LOCATION_DURATION_MAX_S)
  {
    cfg->custom_boot_work_s = defaults.custom_boot_work_s;
  }
  if (cfg->custom_disconnect_grace_s > LOCATION_DURATION_MAX_S)
  {
    cfg->custom_disconnect_grace_s = defaults.custom_disconnect_grace_s;
  }
  if (cfg->custom_stationary_sleep_s < LOCATION_SLEEP_MIN_S ||
      cfg->custom_stationary_sleep_s > LOCATION_DURATION_MAX_S)
  {
    cfg->custom_stationary_sleep_s = defaults.custom_stationary_sleep_s;
  }
  if (cfg->custom_moving_sleep_s < LOCATION_SLEEP_MIN_S ||
      cfg->custom_moving_sleep_s > LOCATION_DURATION_MAX_S)
  {
    cfg->custom_moving_sleep_s = defaults.custom_moving_sleep_s;
  }
  if (cfg->custom_fast_sleep_s < LOCATION_SLEEP_MIN_S ||
      cfg->custom_fast_sleep_s > LOCATION_DURATION_MAX_S)
  {
    cfg->custom_fast_sleep_s = defaults.custom_fast_sleep_s;
  }
  if (cfg->custom_fast_speed_kmh_x10 < LOCATION_FAST_SPEED_MIN_X10 ||
      cfg->custom_fast_speed_kmh_x10 > LOCATION_FAST_SPEED_MAX_X10)
  {
    cfg->custom_fast_speed_kmh_x10 = defaults.custom_fast_speed_kmh_x10;
  }
  if (cfg->custom_fix_timeout_s < LOCATION_SLEEP_MIN_S ||
      cfg->custom_fix_timeout_s > LOCATION_TIMEOUT_MAX_S)
  {
    cfg->custom_fix_timeout_s = defaults.custom_fix_timeout_s;
  }
  if (cfg->custom_ble_boot_fast_search_s > BLE_FAST_SEARCH_MAX_S)
  {
    cfg->custom_ble_boot_fast_search_s =
        defaults.custom_ble_boot_fast_search_s;
  }
  if (cfg->custom_ble_scan_period_ms < BLE_SCAN_PERIOD_MIN_MS ||
      cfg->custom_ble_scan_period_ms > BLE_SCAN_PERIOD_MAX_MS)
  {
    cfg->custom_ble_scan_period_ms = defaults.custom_ble_scan_period_ms;
  }
  if (cfg->custom_ble_scan_window_ms < BLE_SCAN_WINDOW_MIN_MS ||
      cfg->custom_ble_scan_window_ms > BLE_SCAN_WINDOW_MAX_MS ||
      cfg->custom_ble_scan_window_ms > cfg->custom_ble_scan_period_ms)
  {
    cfg->custom_ble_scan_window_ms = defaults.custom_ble_scan_window_ms;
  }
  if (cfg->wifi_idle_timeout_s != WIFI_IDLE_TIMEOUT_DEFAULT_S &&
      cfg->wifi_idle_timeout_s != WIFI_IDLE_TIMEOUT_LONG_S)
  {
    cfg->wifi_idle_timeout_s = defaults.wifi_idle_timeout_s;
  }

  config_terminate_str(cfg->camera_name_prefix,
                       sizeof(cfg->camera_name_prefix));
  config_terminate_str(cfg->camera_mac_prefix, sizeof(cfg->camera_mac_prefix));
  config_terminate_str(cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
  config_terminate_str(cfg->wifi_pass, sizeof(cfg->wifi_pass));
  if (cfg->phone_location_ttl_hours < 1 ||
      cfg->phone_location_ttl_hours > PHONE_LOCATION_TTL_MAX_HOURS)
  {
    cfg->phone_location_ttl_hours = defaults.phone_location_ttl_hours;
  }
  if (cfg->fixed_location_priority > FIXED_LOCATION_PRIORITY) {
    cfg->fixed_location_priority = defaults.fixed_location_priority;
  }
  if (cfg->fixed_location_delete_policy > FIXED_LOCATION_KEEP_UNTIL_EXPIRY) {
    cfg->fixed_location_delete_policy = defaults.fixed_location_delete_policy;
  }
  if (cfg->gnss_without_camera > 1) {
    cfg->gnss_without_camera = defaults.gnss_without_camera;
  }
  if (cfg->record_without_camera > 1) {
    cfg->record_without_camera = defaults.record_without_camera;
  }
  if (cfg->track_interval_s < LOCATION_RECORD_INTERVAL_MIN_S ||
      cfg->track_interval_s > LOCATION_RECORD_INTERVAL_MAX_S) {
    cfg->track_interval_s = defaults.track_interval_s;
  }
  if (cfg->source_gnss_enabled > 1) cfg->source_gnss_enabled = defaults.source_gnss_enabled;
  if (cfg->source_phone_enabled > 1) cfg->source_phone_enabled = defaults.source_phone_enabled;
  if (cfg->source_manual_enabled > 1) cfg->source_manual_enabled = defaults.source_manual_enabled;
  if (cfg->track_enabled > 1) cfg->track_enabled = defaults.track_enabled;
  if (cfg->phone_push_enabled > 1) cfg->phone_push_enabled = defaults.phone_push_enabled;
  if (cfg->manual_auto_update > 1) cfg->manual_auto_update = defaults.manual_auto_update;
  if (cfg->phone_interval_s < 1 || cfg->phone_interval_s > 3600)
    cfg->phone_interval_s = defaults.phone_interval_s;
  config_terminate_str(cfg->source_order, sizeof(cfg->source_order));
  if (!source_order_is_valid(cfg->source_order))
    config_set_str_default(cfg->source_order, sizeof(cfg->source_order), defaults.source_order);

}

static void config_read_str(nvs_handle_t nvs, const char *key, char *out,
                            size_t out_len)
{
  size_t len = out_len;
  esp_err_t err = nvs_get_str(nvs, key, out, &len);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
  {
    ESP_LOGW(TAG, "Ignoring invalid NVS string %s: %s", key,
             esp_err_to_name(err));
  }
}

bool config_load(app_config_t *cfg)
{
  config_set_defaults(cfg);
  config_ensure_mutex();

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "NVS open failed, using defaults: %s", esp_err_to_name(err));
    config_validate(cfg);
    return false;
  }

  nvs_get_u32(nvs, "gps_int_ms", &cfg->gps_interval_ms);
  nvs_get_u32(nvs, "max_age_s", &cfg->max_gps_age_s);
  nvs_get_u32(nvs, "cfg_win_s", &cfg->config_window_s);
  nvs_get_u32(nvs, "ble_pass", &cfg->ble_passkey);
  nvs_get_u8(nvs, "loc_mode", &cfg->location_power_mode);
  nvs_get_u8(nvs, "ble_mode", &cfg->bluetooth_power_mode);
  nvs_get_u32(nvs, "loc_boot", &cfg->custom_boot_work_s);
  nvs_get_u32(nvs, "loc_grace", &cfg->custom_disconnect_grace_s);
  nvs_get_u32(nvs, "loc_still", &cfg->custom_stationary_sleep_s);
  nvs_get_u32(nvs, "loc_move", &cfg->custom_moving_sleep_s);
  nvs_get_u32(nvs, "loc_fast", &cfg->custom_fast_sleep_s);
  nvs_get_u32(nvs, "loc_spd10", &cfg->custom_fast_speed_kmh_x10);
  nvs_get_u32(nvs, "loc_timeout", &cfg->custom_fix_timeout_s);
  nvs_get_u32(nvs, "ble_boot", &cfg->custom_ble_boot_fast_search_s);
  nvs_get_u32(nvs, "ble_period", &cfg->custom_ble_scan_period_ms);
  nvs_get_u32(nvs, "ble_window", &cfg->custom_ble_scan_window_ms);
  nvs_get_u32(nvs, "wifi_idle_s", &cfg->wifi_idle_timeout_s);
  nvs_get_u32(nvs, "phone_ttl_h", &cfg->phone_location_ttl_hours);
  nvs_get_u8(nvs, "fixed_prio", &cfg->fixed_location_priority);
  nvs_get_u8(nvs, "fixed_delete", &cfg->fixed_location_delete_policy);
  nvs_get_u8(nvs, "gnss_idle", &cfg->gnss_without_camera);
  nvs_get_u8(nvs, "track_idle", &cfg->record_without_camera);
  nvs_get_u32(nvs, "track_int_s", &cfg->track_interval_s);
  nvs_get_u8(nvs, "src_gnss", &cfg->source_gnss_enabled);
  nvs_get_u8(nvs, "src_phone", &cfg->source_phone_enabled);
  nvs_get_u8(nvs, "src_manual", &cfg->source_manual_enabled);
  nvs_get_u8(nvs, "track_on", &cfg->track_enabled);
  nvs_get_u8(nvs, "phone_push", &cfg->phone_push_enabled);
  nvs_get_u32(nvs, "phone_int", &cfg->phone_interval_s);
  nvs_get_u8(nvs, "manual_auto", &cfg->manual_auto_update);
  config_read_str(nvs, "src_order", cfg->source_order, sizeof(cfg->source_order));
  int16_t tz = cfg->tz_offset_min;
  int16_t dst = cfg->dst_offset_min;
  uint16_t old_tz = 0;
  uint16_t old_dst = 0;
  if (nvs_get_i16(nvs, "tz_off", &tz) == ESP_OK)
  {
    cfg->tz_offset_min = tz;
  }
  else if (nvs_get_u16(nvs, "tz_off", &old_tz) == ESP_OK)
  {
    cfg->tz_offset_min = (int16_t)old_tz;
  }
  if (nvs_get_i16(nvs, "dst_off", &dst) == ESP_OK)
  {
    cfg->dst_offset_min = dst;
  }
  else if (nvs_get_u16(nvs, "dst_off", &old_dst) == ESP_OK)
  {
    cfg->dst_offset_min = (int16_t)old_dst;
  }

  config_read_str(nvs, "cam_name", cfg->camera_name_prefix,
                  sizeof(cfg->camera_name_prefix));
  config_read_str(nvs, "cam_mac", cfg->camera_mac_prefix,
                  sizeof(cfg->camera_mac_prefix));
  config_read_str(nvs, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
  config_read_str(nvs, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));

  nvs_close(nvs);
  config_validate(cfg);
  return true;
}

static bool config_save_unlocked(const app_config_t *cfg)
{
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
    return false;
  }

  err = nvs_set_u32(nvs, "gps_int_ms", cfg->gps_interval_ms);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "max_age_s", cfg->max_gps_age_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "cfg_win_s", cfg->config_window_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "ble_pass", cfg->ble_passkey);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "loc_mode", cfg->location_power_mode);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "ble_mode", cfg->bluetooth_power_mode);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "loc_boot", cfg->custom_boot_work_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "loc_grace", cfg->custom_disconnect_grace_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "loc_still", cfg->custom_stationary_sleep_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "loc_move", cfg->custom_moving_sleep_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "loc_fast", cfg->custom_fast_sleep_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "loc_spd10", cfg->custom_fast_speed_kmh_x10);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "loc_timeout", cfg->custom_fix_timeout_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "ble_boot", cfg->custom_ble_boot_fast_search_s);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "ble_period", cfg->custom_ble_scan_period_ms);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "ble_window", cfg->custom_ble_scan_window_ms);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "wifi_idle_s", cfg->wifi_idle_timeout_s);
  if (err == ESP_OK) err = nvs_set_i16(nvs, "tz_off", cfg->tz_offset_min);
  if (err == ESP_OK) err = nvs_set_i16(nvs, "dst_off", cfg->dst_offset_min);
  if (err == ESP_OK) err = nvs_set_str(nvs, "cam_name", cfg->camera_name_prefix);
  if (err == ESP_OK) err = nvs_set_str(nvs, "cam_mac", cfg->camera_mac_prefix);
  if (err == ESP_OK) err = nvs_set_str(nvs, "wifi_ssid", cfg->wifi_ssid);
  if (err == ESP_OK) err = nvs_set_str(nvs, "wifi_pass", cfg->wifi_pass);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "phone_ttl_h", cfg->phone_location_ttl_hours);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "fixed_prio", cfg->fixed_location_priority);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "fixed_delete", cfg->fixed_location_delete_policy);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "gnss_idle", cfg->gnss_without_camera);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "track_idle", cfg->record_without_camera);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "track_int_s", cfg->track_interval_s);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "src_gnss", cfg->source_gnss_enabled);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "src_phone", cfg->source_phone_enabled);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "src_manual", cfg->source_manual_enabled);
  if (err == ESP_OK) err = nvs_set_str(nvs, "src_order", cfg->source_order);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "track_on", cfg->track_enabled);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "phone_push", cfg->phone_push_enabled);
  if (err == ESP_OK) err = nvs_set_u32(nvs, "phone_int", cfg->phone_interval_s);
  if (err == ESP_OK) err = nvs_set_u8(nvs, "manual_auto", cfg->manual_auto_update);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool config_save(const app_config_t *cfg)
{
  if (!cfg)
  {
    return false;
  }
  app_config_t validated = *cfg;
  config_validate(&validated);
  config_ensure_mutex();
  if (s_config_mutex &&
      xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
  {
    ESP_LOGW(TAG, "Failed to acquire config mutex");
    return false;
  }
  bool ok = config_save_unlocked(&validated);
  if (s_config_mutex)
  {
    xSemaphoreGive(s_config_mutex);
  }
  return ok;
}

bool config_apply(app_config_t *cfg, const app_config_t *new_cfg)
{
  if (!cfg || !new_cfg)
  {
    return false;
  }

  app_config_t validated = *new_cfg;
  config_validate(&validated);
  config_ensure_mutex();
  if (s_config_mutex &&
      xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
  {
    ESP_LOGW(TAG, "Failed to acquire config mutex");
    return false;
  }
  bool ok = config_save_unlocked(&validated);
  if (ok)
  {
    *cfg = validated;
  }
  if (s_config_mutex)
  {
    xSemaphoreGive(s_config_mutex);
  }
  return ok;
}

bool config_get_snapshot(const app_config_t *cfg, app_config_t *out)
{
  if (!cfg || !out)
  {
    return false;
  }

  config_ensure_mutex();
  if (s_config_mutex &&
      xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
  {
    return false;
  }
  *out = *cfg;
  if (s_config_mutex)
  {
    xSemaphoreGive(s_config_mutex);
  }
  return true;
}

void config_resolve_location_power(const app_config_t *cfg,
                                   location_power_policy_t *out)
{
  if (!cfg || !out)
  {
    return;
  }
  switch ((location_power_mode_t)cfg->location_power_mode)
  {
  case LOCATION_POWER_SAVING:
    *out = (location_power_policy_t){
        .search_without_camera = cfg->gnss_without_camera,
        .boot_work_s = 45,
        .disconnect_grace_s = 10,
        .stationary_sleep_s = 300,
        .moving_sleep_s = 120,
        .fast_sleep_s = 30,
        .fast_speed_kmh_x10 = 288,
        .fix_timeout_s = 30,
    };
    break;
  case LOCATION_POWER_TRACK:
    *out = (location_power_policy_t){
        .search_without_camera = cfg->gnss_without_camera,
        .continuous = true,
        .boot_work_s = 45,
        .disconnect_grace_s = 20,
        .stationary_sleep_s = 90,
        .moving_sleep_s = 30,
        .fast_sleep_s = 15,
        .fast_speed_kmh_x10 = 288,
        .fix_timeout_s = 45,
    };
    break;
  case LOCATION_POWER_CUSTOM:
    *out = (location_power_policy_t){
        .search_without_camera = cfg->gnss_without_camera,
        .boot_work_s = cfg->custom_boot_work_s,
        .disconnect_grace_s = cfg->custom_disconnect_grace_s,
        .stationary_sleep_s = cfg->custom_stationary_sleep_s,
        .moving_sleep_s = cfg->custom_moving_sleep_s,
        .fast_sleep_s = cfg->custom_fast_sleep_s,
        .fast_speed_kmh_x10 = cfg->custom_fast_speed_kmh_x10,
        .fix_timeout_s = cfg->custom_fix_timeout_s,
    };
    break;
  case LOCATION_POWER_BALANCED:
  default:
    *out = (location_power_policy_t){
        .search_without_camera = cfg->gnss_without_camera,
        .boot_work_s = 45,
        .disconnect_grace_s = 20,
        .stationary_sleep_s = 90,
        .moving_sleep_s = 30,
        .fast_sleep_s = 15,
        .fast_speed_kmh_x10 = 288,
        .fix_timeout_s = 45,
    };
    break;
  }
}

void config_resolve_bluetooth_power(const app_config_t *cfg,
                                    bluetooth_power_policy_t *out)
{
  if (!cfg || !out)
  {
    return;
  }
  switch ((bluetooth_power_mode_t)cfg->bluetooth_power_mode)
  {
  case BLUETOOTH_POWER_SAVING:
    *out = (bluetooth_power_policy_t){
        .boot_fast_search_s = 15,
        .scan_period_ms = 10000,
        .scan_window_ms = 100,
    };
    break;
  case BLUETOOTH_POWER_CUSTOM:
    *out = (bluetooth_power_policy_t){
        .boot_fast_search_s = cfg->custom_ble_boot_fast_search_s,
        .scan_period_ms = cfg->custom_ble_scan_period_ms,
        .scan_window_ms = cfg->custom_ble_scan_window_ms,
    };
    break;
  case BLUETOOTH_POWER_NORMAL:
  default:
    *out = (bluetooth_power_policy_t){
        .boot_fast_search_s = 30,
        .scan_period_ms = 3000,
        .scan_window_ms = 100,
    };
    break;
  }
}
