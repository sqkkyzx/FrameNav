/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gps.h"
#include "gnss_rate.h"
#include "gnss_power.h"
#include "power_policy.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define GPS_UART_BUF_SIZE 2048
#define GPS_LINE_MAX 128

static const char *TAG = "gps";

#ifndef ALPHALOC_FAKE_GPS
#define ALPHALOC_FAKE_GPS 0
#endif

#define FAKE_LAT_DEG 48.137154
#define FAKE_LON_DEG 11.576124
#define FAKE_YEAR 2024
#define FAKE_MONTH 1
#define FAKE_DAY 1
#define FAKE_HOUR 12
#define FAKE_MINUTE 0
#define FAKE_SECOND 0

#ifndef ALPHALOC_VERBOSE
#define ALPHALOC_VERBOSE 0
#endif

#ifndef ALPHALOC_LOG_NMEA
#define ALPHALOC_LOG_NMEA 0
#endif

#if ALPHALOC_VERBOSE
#define VLOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define VLOGI(...) ((void)0)
#endif

#if ALPHALOC_LOG_NMEA
#define NMEALOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define NMEALOGI(...) ((void)0)
#endif

static gps_fix_t s_latest_fix;
static SemaphoreHandle_t s_fix_mutex;
static gps_config_t s_cfg;
static gps_status_t s_status;
static int64_t s_last_no_fix_log_us;

#if ALPHALOC_GNSS_STANDBY
#if !ALPHALOC_GNSS_RATE_1HZ || !ALPHALOC_PHASE2 || ALPHALOC_FAKE_GPS
#error "GNSS standby requires real Phase 2 and the shared UBX parser"
#endif
static gnss_power_t s_power;
static portMUX_TYPE s_camera_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_camera_connected;
static uint32_t s_camera_generation, s_camera_seen_generation;
static int64_t s_camera_changed_us;
static location_power_policy_t s_pending_power_policy;
static uint32_t s_power_policy_generation, s_power_policy_seen_generation;
static bool s_system_sleep_requested, s_system_sleep_ready;
void gps_request_system_sleep(bool requested) {
  portENTER_CRITICAL(&s_camera_lock);
  s_system_sleep_requested=requested;
  if(!requested)s_system_sleep_ready=false;
  portEXIT_CRITICAL(&s_camera_lock);
}
bool gps_system_sleep_ready(void) {
  portENTER_CRITICAL(&s_camera_lock);
  bool ready=s_system_sleep_ready&&!s_camera_connected&&s_system_sleep_requested;
  portEXIT_CRITICAL(&s_camera_lock);return ready;
}

static void power_policy_to_duty(const location_power_policy_t *policy,
                                 gnss_duty_params_t *out) {
  if (!policy || !out) return;
  *out = (gnss_duty_params_t){
      .continuous = policy->continuous,
      .boot_work_s = policy->boot_work_s,
      .disconnect_grace_s = policy->disconnect_grace_s,
      .stationary_sleep_s = policy->stationary_sleep_s,
      .moving_sleep_s = policy->moving_sleep_s,
      .fast_sleep_s = policy->fast_sleep_s,
      .fix_timeout_s = policy->fix_timeout_s,
      .fast_speed_mps = (double)policy->fast_speed_kmh_x10 / 36.0,
  };
}

void gps_apply_power_policy(const location_power_policy_t *policy) {
  if (!policy) return;
  portENTER_CRITICAL(&s_camera_lock);
  s_pending_power_policy = *policy;
  ++s_power_policy_generation;
  portEXIT_CRITICAL(&s_camera_lock);
}

void gps_camera_connection_changed(bool connected) {
  // BLE callbacks never block on UART or mutate the GPS-task-owned policy.
  portENTER_CRITICAL(&s_camera_lock);
  s_camera_connected = connected;
  s_camera_changed_us = esp_timer_get_time();
  ++s_camera_generation;
  portEXIT_CRITICAL(&s_camera_lock);
}
#endif

#if ALPHALOC_GNSS_RATE_1HZ
#if !ALPHALOC_PHASE2 || ALPHALOC_FAKE_GPS
#error "GNSS receiver configuration requires real Phase 2 UART, never Fake GPS"
#endif
static gnss_rate_t s_rate;
static uint32_t s_rmc_count, s_rmc_sample_count;
static int64_t s_rmc_sample_us;

static bool rate_uart_send(const uint8_t *data, size_t len, void *arg) {
  (void)arg;
  return uart_write_bytes(s_cfg.uart_num, data, len) == (int)len;
}

static void rate_log(gnss_rate_event_t event, void *arg) {
  (void)arg;
  switch (event) {
  case GNSS_RATE_SENT:
    ESP_LOGI(TAG, "GNSS 1Hz request: RAM only, attempt %u/3; baud unchanged",
             s_rate.attempts);
    break;
  case GNSS_RATE_ACK:
    ESP_LOGI(TAG, "GNSS CFG-VALSET acknowledged; RAM readback still required");
    break;
  case GNSS_RATE_CONFIRMED:
    ESP_LOGI(TAG, "GNSS 1Hz RAM verified: meas=%ums nav=%u RMC=%u GGA=%u ack=%d; "
                 "output cadence and fix quality are checked separately",
             s_rate.meas_ms, s_rate.nav_ratio, s_rate.rmc_rate, s_rate.gga_rate,
             s_rate.acknowledged);
    s_rmc_sample_us = esp_timer_get_time();
    s_rmc_sample_count = s_rmc_count;
    break;
  case GNSS_RATE_ACK_TIMEOUT:
    ESP_LOGW(TAG, "GNSS rate ACK timeout; checking RAM before retrying");
    break;
  case GNSS_RATE_MISMATCH:
    ESP_LOGW(TAG, "GNSS rate RAM mismatch: meas=%ums nav=%u RMC=%u GGA=%u",
             s_rate.meas_ms, s_rate.nav_ratio, s_rate.rmc_rate, s_rate.gga_rate);
    break;
  case GNSS_RATE_NAK:
    ESP_LOGW(TAG, "GNSS rate command rejected; 1Hz UNCONFIRMED, continuing NMEA");
    break;
  case GNSS_RATE_READ_TIMEOUT:
    ESP_LOGW(TAG, "GNSS rate RAM readback timeout; continuing NMEA");
    break;
  case GNSS_RATE_TX_ERROR:
    ESP_LOGW(TAG, "GNSS rate UART write failed; continuing NMEA");
    break;
  case GNSS_RATE_GIVE_UP:
    ESP_LOGW(TAG, "GNSS 1Hz UNCONFIRMED after 3 attempts; stop commands, keep NMEA");
    break;
  }
}
#endif

#if ALPHALOC_GNSS_STANDBY
static bool power_sleep_allowed(void *arg) {
  (void)arg;
  portENTER_CRITICAL(&s_camera_lock);
  bool allowed = !s_camera_connected &&
                 s_camera_generation == s_camera_seen_generation;
  portEXIT_CRITICAL(&s_camera_lock);
  return allowed;
}

static int64_t power_clock(void *arg) {
  (void)arg;
  return esp_timer_get_time();
}

static bool power_uart_send(const uint8_t *data, size_t len, void *arg) {
  (void)arg;
  if (len >= 4 && data[2] == 0x02 && data[3] == 0x41 &&
      !power_sleep_allowed(NULL)) {
    return false;
  }
  return uart_write_bytes(s_cfg.uart_num, data, len) == (int)len &&
         uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(100)) == ESP_OK;
}

static void power_log(gnss_power_event_t event, void *arg) {
  (void)arg;
  switch (event) {
  case GNSS_POWER_ID_QUERY:
    ESP_LOGI(TAG, "GNSS power: polling MON-VER, attempt %u/3", s_power.identity_attempts);
    break;
  case GNSS_POWER_ID_OK:
  case GNSS_POWER_ID_UNSUPPORTED:
    ESP_LOGI(TAG, "GNSS identity: FW=%s PROT=%s MOD=%s HW=%s; standby=%s",
             s_power.identity.fwversion, s_power.identity.protversion,
             s_power.identity.mod, s_power.identity.hw,
             s_power.identity.supported ? "supported profile" : "DISABLED: unknown profile");
    break;
  case GNSS_POWER_ID_TIMEOUT:
    ESP_LOGW(TAG, "GNSS power: MON-VER unconfirmed; standby disabled, keep working");
    break;
  case GNSS_POWER_SLEEP_SENT:
    ESP_LOGI(TAG, "GNSS standby requested: sleep=%us speed=%.3fm/s comparable=%d; "
                 "UART wake + finite receiver timer; uptime_ms=%lld",
             (unsigned)s_power.duty.next_sleep_s, s_power.duty.last_speed_mps,
             s_power.duty.have_speed, (long long)(esp_timer_get_time() / 1000));
    break;
  case GNSS_POWER_SLEEP_QUIET:
    ESP_LOGI(TAG, "GNSS standby: UART quiet; actual low current requires measurement");
    break;
  case GNSS_POWER_WAKE_SENT:
    ESP_LOGI(TAG, "GNSS wake requested: camera=%d; waiting for new NMEA; uptime_ms=%lld",
             s_power.duty.camera_connected, (long long)(esp_timer_get_time() / 1000));
    break;
  case GNSS_POWER_WAKE_OK:
    ESP_LOGI(TAG, "GNSS awake: fresh NMEA resumed; 1Hz RAM restore pending while working; mode=%s",
             s_power.disable_after_wake ? "standby-disabled" :
             s_power.duty.camera_connected ? "camera-work" : "timed-work");
    break;
  case GNSS_POWER_WAKE_RETRY:
    ESP_LOGW(TAG, "GNSS wake: retry %u/3", s_power.wake_attempts);
    break;
  default:
    ESP_LOGW(TAG, "GNSS power fault=%d; disable standby, recover continuous reception", event);
    break;
  }
}

static void power_frame(uint8_t cls, uint8_t id, const uint8_t *payload,
                         size_t len, int64_t now_us, void *arg) {
  (void)arg;
  gnss_power_frame(&s_power, cls, id, payload, len, now_us);
}

static void power_step(size_t *line_len) {
  // A connect arriving during UART TX must trigger a wake before another
  // blocking read. Bound the loop against continual external event churn.
  for (unsigned pass = 0; pass < 4; ++pass) {
    bool connected;
    uint32_t generation;
    uint32_t policy_generation;
    int64_t changed_us;
    location_power_policy_t policy;
    portENTER_CRITICAL(&s_camera_lock);
    connected = s_camera_connected;
    generation = s_camera_generation;
    policy_generation = s_power_policy_generation;
    policy = s_pending_power_policy;
    changed_us = s_camera_changed_us;
    s_power.deep_requested=s_system_sleep_requested;
    portEXIT_CRITICAL(&s_camera_lock);
    if (policy_generation != s_power_policy_seen_generation) {
      s_power_policy_seen_generation = policy_generation;
      gnss_duty_params_t params;
      power_policy_to_duty(&policy, &params);
      gnss_power_apply_params(&s_power, &params, esp_timer_get_time());
      ESP_LOGI(TAG, "GNSS policy applied: continuous=%d boot=%us grace=%us "
                    "sleep=%u/%u/%us fast=%.1fkm/h timeout=%us",
               policy.continuous, (unsigned)policy.boot_work_s,
               (unsigned)policy.disconnect_grace_s,
               (unsigned)policy.stationary_sleep_s,
               (unsigned)policy.moving_sleep_s,
               (unsigned)policy.fast_sleep_s,
               (double)policy.fast_speed_kmh_x10 / 10.0,
               (unsigned)policy.fix_timeout_s);
    }
    if (generation != s_camera_seen_generation) {
      s_camera_seen_generation = generation;
      gnss_power_camera_event(&s_power, connected, changed_us);
      ESP_LOGI(TAG, "GNSS camera policy: connected=%d", connected);
    }
    gnss_power_poll(&s_power, esp_timer_get_time());
    portENTER_CRITICAL(&s_camera_lock);
    s_system_sleep_ready=s_power.phase==GNSS_POWER_SLEEP&&s_power.deep_committed;
    portEXIT_CRITICAL(&s_camera_lock);
    unsigned actions = gnss_power_take_actions(&s_power);
    if (actions & GNSS_POWER_INVALIDATE_FIX) {
      if (xSemaphoreTake(s_fix_mutex, portMAX_DELAY) == pdTRUE) {
        s_latest_fix.valid = false;
        s_latest_fix.time_valid = false;
        s_status.has_lock = false;
        s_status.satellites = 0;
        xSemaphoreGive(s_fix_mutex);
      }
    }
    if (actions & GNSS_POWER_RESET_RX) {
      // Discard pre-wake bytes and parser fragments. Wake loses receiver RAM;
      // reapply 1Hz only after fresh NMEA, preserving the identity capability.
      uart_flush_input(s_cfg.uart_num);
      *line_len = 0;
      gnss_rate_init(&s_rate, esp_timer_get_time(), rate_uart_send, rate_log, NULL);
      gnss_rate_set_frame_observer(&s_rate, power_frame, NULL);
      s_rmc_count = s_rmc_sample_count = 0;
      s_rmc_sample_us = esp_timer_get_time();
    }
    portENTER_CRITICAL(&s_camera_lock);
    bool changed = s_camera_generation != s_camera_seen_generation;
    portEXIT_CRITICAL(&s_camera_lock);
    if (!changed) break;
  }
}
#endif

#if ALPHALOC_PHASE2
static uint32_t s_uart_bytes;
static uint32_t s_nmea_ok;
static uint32_t s_nmea_bad;
static int64_t s_last_uart_log_us;

static void log_uart_status(void) {
  int64_t now = esp_timer_get_time();
  if (now - s_last_uart_log_us < ALPHALOC_GNSS_STATUS_PERIOD_US) {
    return;
  }
  s_last_uart_log_us = now;
  gps_fix_t fix = {0};
  gps_status_t status = {0};
  gps_get_latest(&fix);
  gps_get_status(&status);
  int64_t age_ms = fix.last_fix_time_us > 0 ?
      (now - fix.last_fix_time_us) / 1000 : -1;
  ESP_LOGI(TAG, "GNSS UART: bytes=%" PRIu32 " nmea_ok=%" PRIu32
               " nmea_bad=%" PRIu32 " fix=%d utc_valid=%d sats=%u age_ms=%" PRId64,
           s_uart_bytes, s_nmea_ok, s_nmea_bad, fix.valid, fix.time_valid,
           status.satellites, age_ms);
#if ALPHALOC_GNSS_RATE_1HZ
  int64_t elapsed = now - s_rmc_sample_us;
  if (elapsed >= 10000000LL) {
    double hz = (double)(s_rmc_count - s_rmc_sample_count) * 1000000.0 / elapsed;
    ESP_LOGI(TAG, "GNSS RMC received=%.2fHz over %.1fs; 1Hz_RAM=%s ubx_ok=%" PRIu32
                 " ubx_bad=%" PRIu32 " (cadence is NOT fix validity)",
             hz, elapsed / 1000000.0,
             s_rate.stage == GNSS_RATE_VERIFIED ? "verified" : "unconfirmed",
             s_rate.ubx_ok, s_rate.ubx_bad);
    s_rmc_sample_us = now;
    s_rmc_sample_count = s_rmc_count;
  }
#endif
}
#endif

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

static bool nmea_checksum_valid(const char *line) {
  if (!line || line[0] != '$') {
    return false;
  }
  const char *star = strchr(line, '*');
  if (!star || !star[1] || !star[2] || star[3] != '\0') {
    return false;
  }
  int high = hex_value(star[1]);
  int low = hex_value(star[2]);
  if (high < 0 || low < 0) {
    return false;
  }
  uint8_t checksum = 0;
  for (const char *p = line + 1; p < star; ++p) {
    checksum ^= (uint8_t)*p;
  }
  return checksum == (uint8_t)((high << 4) | low);
}

static bool parse_int_range(const char *value, int min, int max, int *out) {
  if (!value || value[0] == '\0') {
    return false;
  }
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < min || parsed > max) {
    return false;
  }
  *out = (int)parsed;
  return true;
}

static bool parse_time_hms(const char *value, uint8_t *hour, uint8_t *minute,
                           uint8_t *second) {
  if (!value || strlen(value) < 6) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    if (!isdigit((unsigned char)value[i])) {
      return false;
    }
  }
  if (value[6] != '\0' && value[6] != '.') {
    return false;
  }
  if (value[6] == '.') {
    if (value[7] == '\0') {
      return false;
    }
    for (size_t i = 7; value[i]; ++i) {
      if (!isdigit((unsigned char)value[i])) {
        return false;
      }
    }
  }
  int hh = (value[0] - '0') * 10 + (value[1] - '0');
  int mm = (value[2] - '0') * 10 + (value[3] - '0');
  int ss = (value[4] - '0') * 10 + (value[5] - '0');
  if (hh > 23 || mm > 59 || ss > 60) {
    return false;
  }
  *hour = (uint8_t)hh;
  *minute = (uint8_t)mm;
  *second = (uint8_t)ss;
  return true;
}

static bool date_valid(int year, int month, int day) {
  static const uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
  if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1) {
    return false;
  }
  int max_day = days_in_month[month - 1];
  bool leap = ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0));
  if (month == 2 && leap) {
    max_day = 29;
  }
  return day <= max_day;
}

static int split_csv(char *buf, const char *fields[], int max_fields) {
  int count = 0;
  char *p = buf;
  while (count < max_fields) {
    fields[count++] = p;
    char *comma = strchr(p, ',');
    if (!comma) {
      break;
    }
    *comma = '\0';
    p = comma + 1;
  }
  return count;
}

static void update_status_gga(const char *line) {
  // GGA fields: 6=fix quality, 7=satellites, 8=HDOP, 9/10=MSL altitude/unit.
  char buf[GPS_LINE_MAX];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  const char *fields[15] = {0};
  int field_count = split_csv(buf, fields, 15);
  if (field_count < 11) {
    return;
  }

  int fix_quality = 0;
  int sats = 0;
  if (!parse_int_range(fields[6], 0, 8, &fix_quality) ||
      !parse_int_range(fields[7], 0, 64, &sats)) {
    return;
  }
  double altitude_m = 0.0;
  bool altitude_valid = false;
  if (fix_quality > 0 && strcmp(fields[10], "M") == 0 &&
      fields[9][0] != '\0') {
    char *end = NULL;
    altitude_m = strtod(fields[9], &end);
    altitude_valid = end != fields[9] && *end == '\0' &&
                     isfinite(altitude_m) && altitude_m >= -10000.0 &&
                     altitude_m <= 1000000.0;
  }
  if (xSemaphoreTake(s_fix_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_status.has_lock = (fix_quality > 0);
    s_status.satellites = (uint8_t)sats;
#if ALPHALOC_PHASE2
    if (fix_quality == 0) {
      s_latest_fix.valid = false;
      s_latest_fix.time_valid = false;
      s_latest_fix.altitude_valid = false;
    } else if (altitude_valid) {
      s_latest_fix.altitude_m = altitude_m;
      s_latest_fix.altitude_valid = true;
    }
#endif
    if (strncmp(line, "$GPGGA", 6) == 0) {
      s_status.constellations = GPS_CONSTELLATION_GPS;
    } else if (strncmp(line, "$GNGGA", 6) == 0) {
      s_status.constellations =
          (GPS_CONSTELLATION_GPS | GPS_CONSTELLATION_GLONASS);
    }
    xSemaphoreGive(s_fix_mutex);
  }
}

static bool parse_deg_min(const char *value, const char *hemisphere,
                          bool is_latitude, double *out) {
  if (value == NULL || value[0] == '\0') {
    return false;
  }
  char *end = NULL;
  double v = strtod(value, &end);
  if (end == value || *end != '\0' || !isfinite(v)) {
    return false;
  }
  double deg = floor(v / 100.0);
  double min = v - (deg * 100.0);
  if (min < 0.0 || min >= 60.0) {
    return false;
  }
  double max_deg = is_latitude ? 90.0 : 180.0;
  if (deg < 0.0 || deg > max_deg) {
    return false;
  }
  double parsed = deg + (min / 60.0);
  if (parsed > max_deg) {
    return false;
  }
  if (hemisphere == NULL || hemisphere[0] == '\0' || hemisphere[1] != '\0') {
    return false;
  }
  if (is_latitude) {
    if (hemisphere[0] == 'S') {
      parsed = -parsed;
    } else if (hemisphere[0] != 'N') {
      return false;
    }
  } else {
    if (hemisphere[0] == 'W') {
      parsed = -parsed;
    } else if (hemisphere[0] != 'E') {
      return false;
    }
  }
  *out = parsed;
  return true;
}

static bool parse_rmc(const char *line, gps_fix_t *out) {
  char buf[GPS_LINE_MAX];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  const char *fields[20] = {0};
  int field_count = split_csv(buf, fields, 20);

  if (field_count < 10) {
    return false;
  }

  if (strcmp(fields[2], "A") != 0) {
    return false;
  }

  out->time_valid = false;
  out->year = 0;
  out->month = 0;
  out->day = 0;
  out->hour = 0;
  out->minute = 0;
  out->second = 0;
  bool time_ok = false;
  if (fields[1][0] != '\0') {
    if (parse_time_hms(fields[1], &out->hour, &out->minute, &out->second)) {
      time_ok = true;
    }
  }

  if (strlen(fields[9]) == 6 && strspn(fields[9], "0123456789") == 6) {
    int dd = 0, mm = 0, yy = 0;
    char extra = '\0';
    if (sscanf(fields[9], "%2d%2d%2d%c", &dd, &mm, &yy, &extra) == 3 &&
        date_valid(2000 + yy, mm, dd)) {
      out->day = (uint8_t)dd;
      out->month = (uint8_t)mm;
      out->year = (uint16_t)(2000 + yy);
    }
  }
  out->time_valid = time_ok && out->year != 0 && out->month != 0 && out->day != 0;
#if ALPHALOC_PHASE2
  // Keep position and UTC from one complete RMC epoch, not stale cached time.
  if (!out->time_valid) {
    return false;
  }
#endif

  double lat = 0.0;
  double lon = 0.0;
  if (!parse_deg_min(fields[3], fields[4], true, &lat) ||
      !parse_deg_min(fields[5], fields[6], false, &lon)) {
    return false;
  }

  out->lat_deg = lat;
  out->lon_deg = lon;
  out->valid = true;
  out->last_fix_time_us = esp_timer_get_time();
  return true;
}

static void update_fix(const gps_fix_t *fix, bool has_fix) {
  if (xSemaphoreTake(s_fix_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_latest_fix.last_update_time_us = esp_timer_get_time();
    if (has_fix) {
      const bool date_present =
          (fix->year != 0 && fix->month != 0 && fix->day != 0);
      s_latest_fix.lat_deg = fix->lat_deg;
      s_latest_fix.lon_deg = fix->lon_deg;
      if (fix->altitude_valid) {
        s_latest_fix.altitude_m = fix->altitude_m;
        s_latest_fix.altitude_valid = true;
      }
      s_latest_fix.valid = true;
      s_latest_fix.last_fix_time_us = fix->last_fix_time_us;
      if (fix->time_valid) {
        s_latest_fix.hour = fix->hour;
        s_latest_fix.minute = fix->minute;
        s_latest_fix.second = fix->second;
        s_latest_fix.time_valid = true;
      }
      if (date_present) {
        s_latest_fix.year = fix->year;
        s_latest_fix.month = fix->month;
        s_latest_fix.day = fix->day;
      }
      VLOGI("Fix lat=%.7f lon=%.7f time=%04u-%02u-%02u %02u:%02u:%02u",
            fix->lat_deg, fix->lon_deg, (unsigned)fix->year,
            (unsigned)fix->month, (unsigned)fix->day, (unsigned)fix->hour,
            (unsigned)fix->minute, (unsigned)fix->second);
    } else {
      s_latest_fix.valid = false;
      s_latest_fix.altitude_valid = false;
#if ALPHALOC_PHASE2
      s_latest_fix.time_valid = false;
#endif
      int64_t now = esp_timer_get_time();
      if (now - s_last_no_fix_log_us > 5000000) {
        s_last_no_fix_log_us = now;
        VLOGI("No valid fix");
      }
    }
    xSemaphoreGive(s_fix_mutex);
  }
}

#if !ALPHALOC_PHASE2
static void update_time_date(const gps_fix_t *fix, bool time_present,
                             bool date_present) {
  if (xSemaphoreTake(s_fix_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_latest_fix.last_update_time_us = esp_timer_get_time();
    if (time_present) {
      s_latest_fix.hour = fix->hour;
      s_latest_fix.minute = fix->minute;
      s_latest_fix.second = fix->second;
      s_latest_fix.time_valid = true;
    }
    if (date_present) {
      s_latest_fix.year = fix->year;
      s_latest_fix.month = fix->month;
      s_latest_fix.day = fix->day;
    }
    xSemaphoreGive(s_fix_mutex);
  }
}
#endif

static bool parse_zda(const char *line, gps_fix_t *out, bool *time_ok,
                      bool *date_ok) {
  char buf[GPS_LINE_MAX];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  const char *fields[8] = {0};
  int field_count = split_csv(buf, fields, 8);

  if (field_count < 5) {
    return false;
  }

  *time_ok = false;
  *date_ok = false;
  if (fields[1][0] != '\0') {
    if (parse_time_hms(fields[1], &out->hour, &out->minute, &out->second)) {
      *time_ok = true;
    }
  }

  if (fields[2][0] != '\0' && fields[3][0] != '\0' && fields[4][0] != '\0') {
    int dd = 0, mm = 0, yyyy = 0;
    if (parse_int_range(fields[2], 1, 31, &dd) &&
        parse_int_range(fields[3], 1, 12, &mm) &&
        parse_int_range(fields[4], 2000, 2099, &yyyy) &&
        date_valid(yyyy, mm, dd)) {
      out->day = (uint8_t)dd;
      out->month = (uint8_t)mm;
      out->year = (uint16_t)yyyy;
      *date_ok = true;
    }
  }

  return *time_ok || *date_ok;
}

static void gps_task(void *arg) {
  (void)arg;
  uint8_t rx_buf[GPS_UART_BUF_SIZE];
  char line_buf[GPS_LINE_MAX];
  size_t line_len = 0;

#if ALPHALOC_GNSS_RATE_1HZ
  gnss_rate_init(&s_rate, esp_timer_get_time(), rate_uart_send, rate_log, NULL);
  s_rmc_sample_us = esp_timer_get_time();
#endif
#if ALPHALOC_GNSS_STANDBY
  // Initial window is MCU uptime, irrespective of whether the receiver kept BBR.
  location_power_policy_t initial_policy;
  uint32_t initial_policy_generation;
  portENTER_CRITICAL(&s_camera_lock);
  initial_policy = s_pending_power_policy;
  initial_policy_generation = s_power_policy_generation;
  portEXIT_CRITICAL(&s_camera_lock);
  if (initial_policy_generation > 0) {
    gnss_duty_params_t initial_params;
    power_policy_to_duty(&initial_policy, &initial_params);
    gnss_power_init_with_params(&s_power, 0, power_uart_send, power_log, NULL,
                                &initial_params);
    s_power_policy_seen_generation = initial_policy_generation;
  } else {
    gnss_power_init(&s_power, 0, power_uart_send, power_log, NULL);
  }
  s_power.clock = power_clock;
  s_power.sleep_allowed = power_sleep_allowed;
  // MCU may have woken while GNSS remains in indefinite backup. Always send
  // an RX wake edge before identity/config queries (also safe on ordinary boot).
  static const uint8_t boot_wake=0xff;
  (void)power_uart_send(&boot_wake,1,NULL);
  gnss_rate_set_frame_observer(&s_rate, power_frame, NULL);
#endif
  while (true) {
#if ALPHALOC_GNSS_STANDBY
    power_step(&line_len);
#endif
    int len = uart_read_bytes(s_cfg.uart_num, rx_buf, sizeof(rx_buf),
                              pdMS_TO_TICKS(200));
#if ALPHALOC_PHASE2
    if (len > 0) {
      s_uart_bytes += (uint32_t)len;
    }
    log_uart_status();
#endif

#if ALPHALOC_GNSS_RATE_1HZ
    // With data, consume buffered replies before evaluating their deadlines.
    if (len <= 0
#if ALPHALOC_GNSS_STANDBY
        && gnss_power_receiving(&s_power)
#endif
    ) gnss_rate_poll(&s_rate, esp_timer_get_time());
#endif

    // Add error handling for UART read failures
    if (len < 0) {
      ESP_LOGE(TAG, "UART read error: %d", len);
      vTaskDelay(pdMS_TO_TICKS(100));
      line_len = 0; // Reset buffer on error
      continue;
    }

    if (len == 0) {
#if !ALPHALOC_PHASE2
      vTaskDelay(pdMS_TO_TICKS(s_cfg.update_interval_ms));
#endif
      continue;
    }

#if ALPHALOC_LOG_NMEA && !ALPHALOC_PHASE2
    ESP_LOG_BUFFER_CHAR(TAG, rx_buf, len);
#endif

    for (int i = 0; i < len; ++i) {
      char c = (char)rx_buf[i];
#if ALPHALOC_GNSS_RATE_1HZ
      if (gnss_rate_consume(&s_rate, rx_buf[i], esp_timer_get_time())) {
        line_len = 0; // Binary frames must never become NMEA text.
        continue;
      }
      if (c == '$') line_len = 0; // Resync after noise/truncated frames.
      if (line_len == 0 && c != '$') continue;
#endif
      if (c == '\n' || c == '\r') {
        if (line_len == 0) {
          continue;
        }
        line_buf[line_len] = '\0';

        // Validate minimum NMEA sentence length before parsing
        if (line_len >= 10 && nmea_checksum_valid(line_buf)) {
#if ALPHALOC_PHASE2
          ++s_nmea_ok;
#endif
#if ALPHALOC_GNSS_RATE_1HZ
          gnss_rate_note_nmea(&s_rate);
#endif
#if ALPHALOC_GNSS_STANDBY
          gnss_power_note_nmea(&s_power, esp_timer_get_time());
          if (!gnss_power_receiving(&s_power)) {
            line_len = 0;
            continue;
          }
#endif
          NMEALOGI("NMEA: %s", line_buf);
          if (strncmp(line_buf, "$GPGGA", 6) == 0 ||
              strncmp(line_buf, "$GNGGA", 6) == 0) {
            update_status_gga(line_buf);
          }
          gps_fix_t fix = {0};
          if (strncmp(line_buf, "$GPRMC", 6) == 0 ||
              strncmp(line_buf, "$GNRMC", 6) == 0) {
#if ALPHALOC_GNSS_RATE_1HZ
            ++s_rmc_count;
#endif
            if (parse_rmc(line_buf, &fix)) {
#if ALPHALOC_GNSS_STANDBY
              if (gnss_power_note_fix(&s_power, &fix, esp_timer_get_time()))
#endif
              update_fix(&fix, true);
            } else {
              update_fix(&fix, false);
            }
          } else if (strncmp(line_buf, "$GPZDA", 6) == 0 ||
                     strncmp(line_buf, "$GNZDA", 6) == 0) {
            bool time_ok = false;
            bool date_ok = false;
            if (parse_zda(line_buf, &fix, &time_ok, &date_ok)) {
#if !ALPHALOC_PHASE2
              update_time_date(&fix, time_ok, date_ok);
#endif
            }
          }
#if ALPHALOC_PHASE2
        } else {
          ++s_nmea_bad;
#endif
        }
        line_len = 0;
        continue;
      }

      // Add bounds check to prevent buffer overflow
      if (isprint((unsigned char)c) && line_len < sizeof(line_buf) - 1) {
        line_buf[line_len++] = c;
      } else if (line_len >= sizeof(line_buf) - 1) {
        // Buffer full, discard this sentence
        line_len = 0;
      }
    }
#if ALPHALOC_GNSS_RATE_1HZ
#if ALPHALOC_GNSS_STANDBY
    power_step(&line_len);
    if (gnss_power_receiving(&s_power))
#endif
    gnss_rate_poll(&s_rate, esp_timer_get_time());
#endif
  }
}

void gps_init(const gps_config_t *cfg) {
  if (!cfg) {
    ESP_LOGE(TAG, "GPS config is null");
    return;
  }
  s_cfg = *cfg;
  s_fix_mutex = xSemaphoreCreateMutex();
  if (s_fix_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create GPS mutex");
    return;
  }
  memset(&s_latest_fix, 0, sizeof(s_latest_fix));
  memset(&s_status, 0, sizeof(s_status));
  s_last_no_fix_log_us = 0;

#if ALPHALOC_FAKE_GPS
  if (xSemaphoreTake(s_fix_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    const int64_t now = esp_timer_get_time();
    s_latest_fix.lat_deg = FAKE_LAT_DEG;
    s_latest_fix.lon_deg = FAKE_LON_DEG;
    s_latest_fix.valid = true;
    s_latest_fix.time_valid = true;
    s_latest_fix.year = FAKE_YEAR;
    s_latest_fix.month = FAKE_MONTH;
    s_latest_fix.day = FAKE_DAY;
    s_latest_fix.hour = FAKE_HOUR;
    s_latest_fix.minute = FAKE_MINUTE;
    s_latest_fix.second = FAKE_SECOND;
    s_latest_fix.last_fix_time_us = now;
    s_latest_fix.last_update_time_us = now;
#if ALPHALOC_FAKE_GPS_STEPPING
    if (!gps_fake_apply_clock(&s_latest_fix, now)) {
      s_latest_fix.valid = false;
      s_latest_fix.time_valid = false;
#if ALPHALOC_FAKE_GPS_USB_TIME
      ESP_LOGI(TAG, "Fake GPS waiting for USB host time; publishing blocked");
#else
      ESP_LOGE(TAG, "Fake stepping clock initialization failed");
#endif
    }
#endif
    s_status.has_lock = s_latest_fix.valid;
    s_status.satellites = 8;
    s_status.constellations = GPS_CONSTELLATION_GPS;
    xSemaphoreGive(s_fix_mutex);
  }
  ESP_LOGI(TAG, "Fake GPS enabled");
#if ALPHALOC_FAKE_GPS_USB_TIME
  ESP_LOGI(TAG, "Fake coordinates configured; no GPS timestamp until USB time sync");
#elif ALPHALOC_FAKE_GPS_STEPPING
  ESP_LOGI(TAG, "Fake UTC stepping from build time: %04u-%02u-%02uT%02u:%02u:%02uZ; "
               "not a persistent RTC",
           s_latest_fix.year, s_latest_fix.month, s_latest_fix.day,
           s_latest_fix.hour, s_latest_fix.minute, s_latest_fix.second);
#endif
  return;
#endif

  uart_config_t uart_cfg = {
      .baud_rate = s_cfg.baud_rate,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ALPHALOC_POWER_SAVE
      .source_clk = UART_SCLK_XTAL, // Fixed 40 MHz; baud unaffected by DFS.
#endif
  };

  ESP_ERROR_CHECK(
      uart_driver_install(s_cfg.uart_num, GPS_UART_BUF_SIZE, 0, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(s_cfg.uart_num, &uart_cfg));
  ESP_ERROR_CHECK(uart_set_pin(s_cfg.uart_num, s_cfg.tx_pin, s_cfg.rx_pin,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_LOGI(TAG, "GNSS UART%d configured: TX=GPIO%d RX=GPIO%d baud=%d 8N1; "
#if ALPHALOC_GNSS_RATE_1HZ
               "requesting 1Hz RAM config after valid NMEA; waiting for verification",
#else
               "receiving NMEA, no receiver configuration commands sent",
#endif
           s_cfg.uart_num, s_cfg.tx_pin, s_cfg.rx_pin, s_cfg.baud_rate);

  // Bump stack to avoid overflow when parsing/logging NMEA sentences.
  BaseType_t ret = xTaskCreate(gps_task, "gps_task", 6144, NULL, 5, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create GPS task");
    return;
  }
  ESP_LOGI(TAG, "GPS task started");
}

bool gps_get_latest(gps_fix_t *out_fix) {
  if (!out_fix || !s_fix_mutex) {
    return false;
  }
  if (xSemaphoreTake(s_fix_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
#if ALPHALOC_FAKE_GPS
  // A simulated stationary fix is always fresh. Otherwise Phase 1 silently
  // stops transmitting after max_gps_age_s (300 seconds by default).
  s_latest_fix.last_fix_time_us = esp_timer_get_time();
  s_latest_fix.last_update_time_us = s_latest_fix.last_fix_time_us;
#if ALPHALOC_FAKE_GPS_STEPPING
  if (!gps_fake_apply_clock(&s_latest_fix, s_latest_fix.last_fix_time_us)) {
    xSemaphoreGive(s_fix_mutex);
    return false;
  }
#if ALPHALOC_FAKE_GPS_USB_TIME
  s_latest_fix.valid = true;
  s_status.has_lock = true;
#endif
#endif
#endif
  *out_fix = s_latest_fix;
  xSemaphoreGive(s_fix_mutex);
  return true;
}

bool gps_get_status(gps_status_t *out_status) {
  if (!out_status || !s_fix_mutex) {
    return false;
  }
  if (xSemaphoreTake(s_fix_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  *out_status = s_status;
  xSemaphoreGive(s_fix_mutex);
  return true;
}
