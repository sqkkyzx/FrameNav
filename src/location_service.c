/* SPDX-License-Identifier: Apache-2.0 */
#include "location_service.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "track_store.h"

#define GNSS_FRESH_US 5000000LL
#define PHONE_MAX_CAPTURE_AGE_MS 60000ULL
#define UTC_MIN_MS 1577836800000ULL /* 2020-01-01 */
#define UTC_MAX_MS 4102444799999ULL /* 2099-12-31 */

typedef struct {
  bool present;
  double latitude, longitude, accuracy_m;
  uint64_t captured_ms, clock_ms;
  int64_t received_us;
} phone_fix_t;

static app_config_t *s_cfg;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static phone_fix_t s_phone;

void location_service_init(app_config_t *cfg) {
  s_cfg = cfg;
}

static phone_fix_t phone_snapshot(void) {
  portENTER_CRITICAL(&s_lock);
  phone_fix_t phone = s_phone;
  portEXIT_CRITICAL(&s_lock);
  return phone;
}

static bool phone_age(const phone_fix_t *phone, int64_t now_us,
                        uint32_t ttl_hours, uint64_t *age_us) {
  if (!phone->present || now_us < phone->received_us || !ttl_hours ||
      ttl_hours > PHONE_LOCATION_TTL_MAX_HOURS) return false;
  *age_us = (uint64_t)(now_us - phone->received_us) +
            (phone->clock_ms - phone->captured_ms) * 1000ULL;
  return *age_us < (uint64_t)ttl_hours * 3600000000ULL;
}

bool location_service_set_phone(double latitude, double longitude,
                                 double accuracy_m, uint64_t captured_ms,
                                 uint64_t phone_now_ms) {
  if (!s_cfg || !isfinite(latitude) || !isfinite(longitude) ||
      latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180 ||
      !isfinite(accuracy_m) || accuracy_m < 0 || accuracy_m > 20000000 ||
      captured_ms < UTC_MIN_MS || captured_ms > UTC_MAX_MS ||
      phone_now_ms < captured_ms || phone_now_ms > UTC_MAX_MS ||
      phone_now_ms - captured_ms > PHONE_MAX_CAPTURE_AGE_MS) return false;
  phone_fix_t phone = {
      .present = true, .latitude = latitude, .longitude = longitude,
      .accuracy_m = accuracy_m, .captured_ms = captured_ms,
      .clock_ms = phone_now_ms, .received_us = esp_timer_get_time(),
  };
  portENTER_CRITICAL(&s_lock);
  s_phone = phone;
  portEXIT_CRITICAL(&s_lock);
  return true;
}

void location_service_clear_phone(void) {
  portENTER_CRITICAL(&s_lock);
  memset(&s_phone, 0, sizeof(s_phone));
  portEXIT_CRITICAL(&s_lock);
}

void location_service_phone_status(phone_location_status_t *out) {
  if (!out) return;
  *out = (phone_location_status_t){0};
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg)) return;
  phone_fix_t phone = phone_snapshot();
  out->present = phone.present;
  out->latitude = phone.latitude;
  out->longitude = phone.longitude;
  out->accuracy_m = phone.accuracy_m;
  out->captured_utc = (uint32_t)(phone.captured_ms / 1000ULL);
  uint64_t age = 0;
  out->available = phone_age(&phone, esp_timer_get_time(),
                              cfg.phone_location_ttl_hours, &age);
  if (out->available) {
    uint64_t remaining = (uint64_t)cfg.phone_location_ttl_hours * 3600000000ULL - age;
    out->remaining_s = (uint32_t)((remaining + 999999ULL) / 1000000ULL);
  }
}

bool location_service_details(gps_fix_t *fix, location_source_t *source,
                               uint32_t *observed_utc) {
  if (!fix || !source || !observed_utc) return false;
  *fix = (gps_fix_t){0};
  *source = LOCATION_NONE;
  *observed_utc = 0;
  int64_t now_us = esp_timer_get_time();
  gps_fix_t gps = {0};
  if (gps_get_latest(&gps) && gps.valid && gps.time_valid &&
      isfinite(gps.lat_deg) && isfinite(gps.lon_deg) &&
      fabs(gps.lat_deg) <= 90 && fabs(gps.lon_deg) <= 180 &&
      gps.last_fix_time_us >= 0 && now_us >= gps.last_fix_time_us &&
      now_us - gps.last_fix_time_us <= GNSS_FRESH_US &&
      track_epoch_from_fix(&gps, observed_utc)) {
    *fix = gps;
    *source = LOCATION_GNSS;
    return true;
  }
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg)) return false;
  phone_fix_t phone = phone_snapshot();
  uint64_t age = 0;
  if (!phone_age(&phone, now_us, cfg.phone_location_ttl_hours, &age)) return false;
  uint64_t clock_ms = phone.clock_ms + (uint64_t)(now_us - phone.received_us) / 1000ULL;
  if (clock_ms > UTC_MAX_MS) return false;
  time_t epoch = (time_t)(clock_ms / 1000ULL);
  struct tm civil;
  if (!gmtime_r(&epoch, &civil)) return false;
  *fix = (gps_fix_t){
      .lat_deg = phone.latitude, .lon_deg = phone.longitude,
      .valid = true, .time_valid = true, .altitude_valid = false,
      .year = civil.tm_year + 1900, .month = civil.tm_mon + 1, .day = civil.tm_mday,
      .hour = civil.tm_hour, .minute = civil.tm_min, .second = civil.tm_sec,
      .last_fix_time_us = now_us - (int64_t)age,
      .last_update_time_us = now_us,
  };
  *source = LOCATION_PHONE;
  *observed_utc = (uint32_t)(phone.captured_ms / 1000ULL);
  return true;
}

bool location_service_get(gps_fix_t *fix) {
  location_source_t source;
  uint32_t observed;
  return location_service_details(fix, &source, &observed);
}
