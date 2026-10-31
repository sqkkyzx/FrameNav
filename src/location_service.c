/* SPDX-License-Identifier: Apache-2.0 */
#include "location_service.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "track_store.h"

#ifndef ALPHALOC_LOCATION_NVS
#define ALPHALOC_LOCATION_NVS 0
#endif

#if ALPHALOC_LOCATION_NVS
#include "esp_log.h"
#include "nvs.h"
#endif

#define GNSS_FRESH_US 5000000LL
#define PHONE_MAX_CAPTURE_AGE_MS 60000ULL
#define PHONE_CLOCK_STORE_INTERVAL_MS 300000ULL
#define MANUAL_AUTO_STORE_INTERVAL_MS 60000ULL
#define UTC_MIN_MS 1577836800000ULL /* 2020-01-01 */
#define UTC_MAX_MS 4102444799999ULL /* 2099-12-31 */

typedef struct {
  bool present;
  bool clock_confirmed;
  double latitude, longitude, accuracy_m;
  uint64_t captured_ms, clock_ms;
  int64_t received_us;
} phone_fix_t;

#if ALPHALOC_LOCATION_NVS
#define PHONE_STORE_NAMESPACE "framenav_loc"
#define PHONE_STORE_KEY "fixed"
#define PHONE_STORE_MAGIC 0x46584C31U
#define PHONE_STORE_VERSION 1U

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  double latitude;
  double longitude;
  double accuracy_m;
  uint64_t captured_ms;
  uint64_t clock_ms;
} phone_store_record_t;

static bool phone_store_valid(const phone_store_record_t *record) {
  return record && record->magic == PHONE_STORE_MAGIC &&
         record->version == PHONE_STORE_VERSION &&
         isfinite(record->latitude) && isfinite(record->longitude) &&
         isfinite(record->accuracy_m) && record->latitude >= -90.0 &&
         record->latitude <= 90.0 && record->longitude >= -180.0 &&
         record->longitude <= 180.0 && record->accuracy_m >= 0.0 &&
         record->accuracy_m <= 20000000.0 &&
         record->captured_ms >= UTC_MIN_MS && record->captured_ms <= UTC_MAX_MS &&
         record->clock_ms >= record->captured_ms &&
         record->clock_ms <= UTC_MAX_MS;
}

static bool phone_store_load(phone_fix_t *out) {
  if (!out) return false;
  nvs_handle_t nvs = 0;
  if (nvs_open(PHONE_STORE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
    return false;
  }
  phone_store_record_t record = {0};
  size_t size = sizeof(record);
  esp_err_t err = nvs_get_blob(nvs, PHONE_STORE_KEY, &record, &size);
  nvs_close(nvs);
  if (err != ESP_OK || size != sizeof(record) || !phone_store_valid(&record)) {
    return false;
  }
  *out = (phone_fix_t){
      .present = true,
      .latitude = record.latitude,
      .longitude = record.longitude,
      .accuracy_m = record.accuracy_m,
      .captured_ms = record.captured_ms,
      .clock_ms = record.clock_ms,
      .received_us = esp_timer_get_time(),
  };
  return true;
}

static bool phone_store_save(const phone_fix_t *phone) {
  if (!phone || !phone->present) return false;
  phone_store_record_t record = {
      .magic = PHONE_STORE_MAGIC,
      .version = PHONE_STORE_VERSION,
      .latitude = phone->latitude,
      .longitude = phone->longitude,
      .accuracy_m = phone->accuracy_m,
      .captured_ms = phone->captured_ms,
      .clock_ms = phone->clock_ms,
  };
  nvs_handle_t nvs = 0;
  esp_err_t err = nvs_open(PHONE_STORE_NAMESPACE, NVS_READWRITE, &nvs);
  bool opened = err == ESP_OK;
  if (err == ESP_OK) err = nvs_set_blob(nvs, PHONE_STORE_KEY, &record, sizeof(record));
  if (err == ESP_OK) err = nvs_commit(nvs);
  if (opened) nvs_close(nvs);
  if (err != ESP_OK) {
    ESP_LOGW("location", "Fixed location save failed: %s", esp_err_to_name(err));
  }
  return err == ESP_OK;
}

static void phone_store_clear(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(PHONE_STORE_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return;
  err = nvs_erase_key(nvs, PHONE_STORE_KEY);
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(nvs);
  nvs_close(nvs);
}
#else
static bool phone_store_load(phone_fix_t *out) {
  (void)out;
  return false;
}
static bool phone_store_save(const phone_fix_t *phone) { (void)phone; return true; }
static void phone_store_clear(void) {}
#endif

static app_config_t *s_cfg;
static SemaphoreHandle_t s_lock;
static phone_fix_t s_phone;
static phone_fix_t s_live_phone;
static int64_t s_manual_last_store_us;
static bool s_initialized;
static uint64_t s_phone_persisted_clock_ms;

void location_service_init(app_config_t *cfg) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
  s_cfg = cfg;
  phone_fix_t restored = {0};
  bool loaded = phone_store_load(&restored);
  if (loaded) {
    s_phone = restored;
    memset(&s_live_phone, 0, sizeof(s_live_phone));
    s_manual_last_store_us = esp_timer_get_time();
    s_phone_persisted_clock_ms = restored.clock_ms;
  } else if (!s_initialized) {
    memset(&s_phone, 0, sizeof(s_phone));
    memset(&s_live_phone, 0, sizeof(s_live_phone));
    s_manual_last_store_us = 0;
    s_phone_persisted_clock_ms = 0;
  }
  s_initialized = true;
  xSemaphoreGive(s_lock);
}

static bool phone_age(const phone_fix_t *phone, int64_t now_us,
                        uint32_t ttl_hours, uint64_t *age_us) {
  if (!phone->present || !phone->clock_confirmed ||
      now_us < phone->received_us || !ttl_hours ||
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
      .present = true, .clock_confirmed = true,
      .latitude = latitude, .longitude = longitude,
      .accuracy_m = accuracy_m, .captured_ms = captured_ms,
      .clock_ms = phone_now_ms, .received_us = esp_timer_get_time(),
  };
  if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
  if (!phone_store_save(&phone)) {
    xSemaphoreGive(s_lock);
    return false;
  }
  s_phone = phone;
  s_manual_last_store_us = phone.received_us;
  s_phone_persisted_clock_ms = phone_now_ms;
  xSemaphoreGive(s_lock);
  return true;
}

bool location_service_set_live_phone(double latitude, double longitude,
                                     double accuracy_m, uint64_t captured_ms,
                                     uint64_t phone_now_ms) {
  if (!s_cfg || !isfinite(latitude) || !isfinite(longitude) ||
      latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180 ||
      !isfinite(accuracy_m) || accuracy_m < 0 || accuracy_m > 20000000 ||
      captured_ms < UTC_MIN_MS || captured_ms > UTC_MAX_MS ||
      phone_now_ms < captured_ms || phone_now_ms > UTC_MAX_MS ||
      phone_now_ms - captured_ms > PHONE_MAX_CAPTURE_AGE_MS) return false;
  phone_fix_t phone = {
      .present = true, .clock_confirmed = true,
      .latitude = latitude, .longitude = longitude,
      .accuracy_m = accuracy_m, .captured_ms = captured_ms,
      .clock_ms = phone_now_ms, .received_us = esp_timer_get_time(),
  };
  if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
  s_live_phone = phone;
  app_config_t cfg;
  if (config_get_snapshot(s_cfg, &cfg) && cfg.manual_auto_update) {
    phone_fix_t manual = phone;
    manual.accuracy_m = 0.0;
    manual.captured_ms = phone_now_ms;
    manual.clock_ms = phone_now_ms;
    bool need_store = !s_phone.present ||
        esp_timer_get_time() - s_manual_last_store_us >=
            (int64_t)MANUAL_AUTO_STORE_INTERVAL_MS * 1000LL;
    if (need_store && phone_store_save(&manual)) {
      s_manual_last_store_us = manual.received_us;
    }
    s_phone = manual;
  }
  xSemaphoreGive(s_lock);
  return true;
}

bool location_service_set_manual(double latitude, double longitude,
                                 uint64_t phone_now_ms) {
  return location_service_set_phone(latitude, longitude, 0.0,
                                    phone_now_ms, phone_now_ms);
}

/* Called with s_lock held. Repeated/stale clock samples cannot extend TTL. */
static void phone_sync_clock(uint64_t utc_ms, int64_t now_us) {
  if (!s_phone.present || utc_ms < s_phone.clock_ms || utc_ms > UTC_MAX_MS)
    return;
  uint64_t estimated_ms = s_phone.clock_ms;
  if (s_phone.clock_confirmed && now_us >= s_phone.received_us) {
    estimated_ms += (uint64_t)(now_us - s_phone.received_us) / 1000ULL;
  }
  if (s_phone.clock_confirmed && utc_ms <= estimated_ms) return;
  s_phone.clock_ms = utc_ms;
  s_phone.received_us = now_us;
  s_phone.clock_confirmed = true;
  if (utc_ms - s_phone_persisted_clock_ms >= PHONE_CLOCK_STORE_INTERVAL_MS &&
      phone_store_save(&s_phone)) {
    s_phone_persisted_clock_ms = utc_ms;
  }
}

bool location_service_sync_phone_clock(uint64_t phone_now_ms) {
  if (phone_now_ms < UTC_MIN_MS || phone_now_ms > UTC_MAX_MS || !s_lock ||
      xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
  phone_sync_clock(phone_now_ms, esp_timer_get_time());
  xSemaphoreGive(s_lock);
  return true;
}

static void phone_clear(void) {
  memset(&s_phone, 0, sizeof(s_phone));
  s_phone_persisted_clock_ms = 0;
  phone_store_clear();
}

void location_service_clear_phone(void) {
  if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
  phone_clear();
  xSemaphoreGive(s_lock);
}

static void phone_status_for(const phone_fix_t *phone,
                             phone_location_status_t *out,
                             const app_config_t *cfg) {
  if (!out || !phone || !cfg) return;
  *out = (phone_location_status_t){0};
  out->present = phone->present;
  out->clock_pending = phone->present && !phone->clock_confirmed;
  out->latitude = phone->latitude;
  out->longitude = phone->longitude;
  out->accuracy_m = phone->accuracy_m;
  out->captured_utc = (uint32_t)(phone->captured_ms / 1000ULL);
  uint64_t age = 0;
  int64_t now_us = esp_timer_get_time();
  out->available = phone_age(phone, now_us, cfg->phone_location_ttl_hours, &age);
  if (out->available) {
    uint64_t remaining = (uint64_t)cfg->phone_location_ttl_hours * 3600000000ULL - age;
    out->remaining_s = (uint32_t)((remaining + 999999ULL) / 1000000ULL);
  }
}

static void location_status_copy(bool manual, phone_location_status_t *out) {
  if (!out) return;
  *out = (phone_location_status_t){0};
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg)) return;
  if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
  phone_fix_t phone = manual ? s_phone : s_live_phone;
  xSemaphoreGive(s_lock);
  phone_status_for(&phone, out, &cfg);
}

void location_service_manual_status(phone_location_status_t *out) {
  location_status_copy(true, out);
}

void location_service_phone_status(phone_location_status_t *out) {
  location_status_copy(false, out);
}

void location_service_live_status(live_location_status_t *out) {
  if (!out) return;
  *out = (live_location_status_t){0};
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg) || !s_lock ||
      xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
  phone_fix_t phone = s_live_phone;
  xSemaphoreGive(s_lock);
  out->present = phone.present;
  out->latitude = phone.latitude;
  out->longitude = phone.longitude;
  out->accuracy_m = phone.accuracy_m;
  out->captured_ms = phone.captured_ms;
  if (!phone.present || esp_timer_get_time() < phone.received_us) return;
  uint64_t monotonic_age_ms =
      (uint64_t)(esp_timer_get_time() - phone.received_us) / 1000ULL;
  uint64_t initial_age_ms = phone.clock_ms - phone.captured_ms;
  uint64_t age_ms = initial_age_ms + monotonic_age_ms;
  uint64_t ttl_ms = (uint64_t)cfg.phone_location_ttl_hours * 3600000ULL;
  out->available = cfg.phone_location_ttl_hours > 0 && age_ms < ttl_ms;
  if (out->available)
    out->remaining_s = (uint32_t)((ttl_ms - age_ms + 999ULL) / 1000ULL);
}

bool location_service_clear_manual(void) {
  location_service_clear_phone();
  return true;
}

static bool location_from_manual(const phone_fix_t *phone, int64_t now_us,
                                 gps_fix_t *out, uint32_t *observed_utc) {
  uint64_t age = 0;
  app_config_t cfg;
  if (!phone || !out || !observed_utc || !s_cfg ||
      !config_get_snapshot(s_cfg, &cfg) ||
      !phone_age(phone, now_us, cfg.phone_location_ttl_hours, &age)) return false;
  uint64_t clock_ms = phone->clock_ms + (uint64_t)(now_us - phone->received_us) / 1000ULL;
  if (clock_ms > UTC_MAX_MS) return false;
  time_t epoch = (time_t)(clock_ms / 1000ULL);
  struct tm civil;
  if (!gmtime_r(&epoch, &civil)) return false;
  *out = (gps_fix_t){
      .lat_deg = phone->latitude, .lon_deg = phone->longitude,
      .valid = true, .time_valid = true, .altitude_valid = false,
      .year = civil.tm_year + 1900, .month = civil.tm_mon + 1, .day = civil.tm_mday,
      .hour = civil.tm_hour, .minute = civil.tm_min, .second = civil.tm_sec,
      .last_fix_time_us = now_us - (int64_t)age,
      .last_update_time_us = now_us,
  };
  *observed_utc = (uint32_t)(phone->captured_ms / 1000ULL);
  return true;
}

static bool source_enabled(const app_config_t *cfg, const char *name) {
  if (strcmp(name, "gnss") == 0) return cfg->source_gnss_enabled != 0;
  if (strcmp(name, "phone") == 0) return cfg->source_phone_enabled != 0;
  if (strcmp(name, "manual") == 0) return cfg->source_manual_enabled != 0;
  return false;
}

static bool choose_source(const app_config_t *cfg, const char *name,
                          bool gnss_available, bool phone_available,
                          bool manual_available) {
  if (!source_enabled(cfg, name)) return false;
  if (strcmp(name, "gnss") == 0) return gnss_available;
  if (strcmp(name, "phone") == 0) return phone_available;
  if (strcmp(name, "manual") == 0) return manual_available;
  return false;
}

bool location_service_details(gps_fix_t *fix, location_source_t *source,
                               uint32_t *observed_utc) {
  if (!fix || !source || !observed_utc) return false;
  *fix = (gps_fix_t){0};
  *source = LOCATION_NONE;
  *observed_utc = 0;
  app_config_t cfg;
  if (!s_cfg || !config_get_snapshot(s_cfg, &cfg)) return false;
  gps_fix_t gps = {0};
  uint32_t gps_utc = 0;
  bool have_gps = gps_get_latest(&gps);
  int64_t now_us = esp_timer_get_time();
  bool gps_available = have_gps && gps.valid && gps.time_valid &&
      isfinite(gps.lat_deg) && isfinite(gps.lon_deg) &&
      fabs(gps.lat_deg) <= 90 && fabs(gps.lon_deg) <= 180 &&
      gps.last_fix_time_us >= 0 && now_us >= gps.last_fix_time_us &&
      now_us - gps.last_fix_time_us <= GNSS_FRESH_US &&
      track_epoch_from_fix(&gps, &gps_utc);
  if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
  if (gps_available) phone_sync_clock((uint64_t)gps_utc * 1000ULL +
                                      (uint64_t)(now_us - gps.last_fix_time_us) / 1000ULL, now_us);
  phone_fix_t phone = s_phone;
  phone_fix_t live_phone = s_live_phone;
  uint64_t age = 0;
  bool manual_available = phone_age(&phone, now_us,
                                     cfg.phone_location_ttl_hours, &age);
  uint64_t live_age = 0;
  bool phone_available = phone_age(&live_phone, now_us,
                                    cfg.phone_location_ttl_hours, &live_age);
  xSemaphoreGive(s_lock);
  const char *cursor = cfg.source_order;
  while (cursor && *cursor) {
    while (*cursor == ',' || *cursor == ' ') ++cursor;
    char name[8] = {0};
    size_t used = 0;
    while (*cursor && *cursor != ',' && *cursor != ' ' && used + 1 < sizeof(name))
      name[used++] = *cursor++;
    while (*cursor && *cursor != ',') ++cursor;
    if (*cursor == ',') ++cursor;
    if (!used) continue;
    if (!choose_source(&cfg, name, gps_available, phone_available,
                       manual_available)) continue;
    if (strcmp(name, "gnss") == 0) {
      *fix = gps; *source = LOCATION_GNSS; *observed_utc = gps_utc;
      return true;
    }
    if (strcmp(name, "phone") == 0) {
      if (location_from_manual(&live_phone, now_us, fix, observed_utc)) {
        *source = LOCATION_PHONE; return true;
      }
    }
    if (strcmp(name, "manual") == 0) {
      if (location_from_manual(&phone, now_us, fix, observed_utc)) {
        *source = LOCATION_MANUAL; return true;
      }
    }
  }
  return false;
}

bool location_service_get(gps_fix_t *fix) {
  location_source_t source;
  uint32_t observed;
  return location_service_details(fix, &source, &observed);
}
