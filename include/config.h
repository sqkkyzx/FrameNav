/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_CONFIG_H
#define ALPHALOC_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define CONFIG_STR_MAX_32 32
#define CONFIG_STR_MAX_64 64
#define CONFIG_STR_MAX_18 18
#define WIFI_IDLE_TIMEOUT_DEFAULT_S 300U
#define WIFI_IDLE_TIMEOUT_LONG_S 900U
#define PHONE_LOCATION_TTL_DEFAULT_HOURS 6U
#define PHONE_LOCATION_TTL_MAX_HOURS 720U
#define LOCATION_RECORD_INTERVAL_DEFAULT_S 15U
#define LOCATION_RECORD_INTERVAL_MIN_S 1U
#define LOCATION_RECORD_INTERVAL_MAX_S 3600U

typedef enum {
  FIXED_LOCATION_GPS_PRIORITY = 0,
  FIXED_LOCATION_PRIORITY = 1,
} fixed_location_priority_t;

typedef enum {
  FIXED_LOCATION_DELETE_AFTER_1_MIN = 0,
  FIXED_LOCATION_DELETE_AFTER_5_MIN = 1,
  FIXED_LOCATION_DELETE_WHEN_FAR = 2,
  FIXED_LOCATION_DELETE_AFTER_1_MIN_WHEN_FAR = 3,
  FIXED_LOCATION_DELETE_AFTER_5_MIN_WHEN_FAR = 4,
  FIXED_LOCATION_KEEP_UNTIL_EXPIRY = 5,
} fixed_location_delete_policy_t;

typedef enum
{
  LOCATION_POWER_SAVING = 0,
  LOCATION_POWER_BALANCED = 1,
  LOCATION_POWER_TRACK = 2,
  LOCATION_POWER_CUSTOM = 3,
} location_power_mode_t;

typedef enum
{
  BLUETOOTH_POWER_SAVING = 0,
  BLUETOOTH_POWER_NORMAL = 1,
  BLUETOOTH_POWER_CUSTOM = 2,
} bluetooth_power_mode_t;

typedef struct
{
  bool continuous;
  bool search_without_camera;
  uint32_t boot_work_s;
  uint32_t disconnect_grace_s;
  uint32_t stationary_sleep_s;
  uint32_t moving_sleep_s;
  uint32_t fast_sleep_s;
  uint32_t fast_speed_kmh_x10;
  uint32_t fix_timeout_s;
} location_power_policy_t;

typedef struct
{
  uint32_t boot_fast_search_s;
  uint32_t scan_period_ms;
  uint32_t scan_window_ms;
} bluetooth_power_policy_t;

typedef struct
{
  uint32_t gps_interval_ms;
  uint32_t max_gps_age_s;
  uint32_t config_window_s;
  char camera_name_prefix[CONFIG_STR_MAX_32];
  char camera_mac_prefix[CONFIG_STR_MAX_18];
  uint32_t ble_passkey;
  int16_t tz_offset_min;
  int16_t dst_offset_min;
  uint8_t location_power_mode;
  uint8_t bluetooth_power_mode;
  uint32_t custom_boot_work_s;
  uint32_t custom_disconnect_grace_s;
  uint32_t custom_stationary_sleep_s;
  uint32_t custom_moving_sleep_s;
  uint32_t custom_fast_sleep_s;
  uint32_t custom_fast_speed_kmh_x10;
  uint32_t custom_fix_timeout_s;
  uint32_t custom_ble_boot_fast_search_s;
  uint32_t custom_ble_scan_period_ms;
  uint32_t custom_ble_scan_window_ms;
  uint32_t wifi_idle_timeout_s;
  uint32_t phone_location_ttl_hours;
  uint8_t fixed_location_priority;
  uint8_t fixed_location_delete_policy;
  uint8_t gnss_without_camera;
  uint8_t record_without_camera;
  uint32_t track_interval_s;
  uint8_t source_gnss_enabled;
  uint8_t source_phone_enabled;
  uint8_t source_manual_enabled;
  char source_order[24];
  uint8_t track_enabled;
  uint8_t phone_push_enabled;
  uint32_t phone_interval_s;
  uint8_t manual_auto_update;
} app_config_t;

void config_set_defaults(app_config_t *cfg);
void config_validate(app_config_t *cfg);
bool config_load(app_config_t *cfg);
bool config_save(const app_config_t *cfg);
bool config_apply(app_config_t *cfg, const app_config_t *new_cfg);
bool config_get_snapshot(const app_config_t *cfg, app_config_t *out);
void config_resolve_location_power(const app_config_t *cfg,
                                   location_power_policy_t *out);
void config_resolve_bluetooth_power(const app_config_t *cfg,
                                    bluetooth_power_policy_t *out);

#endif
