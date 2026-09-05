/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "track_store.h"

#include <limits.h>
#include <math.h>
#include <string.h>

bool track_epoch_from_fix(const gps_fix_t *fix, uint32_t *out_timestamp) {
  if (!fix || !out_timestamp || !fix->valid || !fix->time_valid ||
      fix->year < 2020 || fix->year > 2099 || fix->month < 1 ||
      fix->month > 12 || fix->day < 1 || fix->hour > 23 ||
      fix->minute > 59 || fix->second > 60) {
    return false;
  }

  static const uint8_t month_days[] = {31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31};
  unsigned max_day = month_days[fix->month - 1];
  const bool leap = (fix->year % 4U == 0U) &&
                    ((fix->year % 100U != 0U) || (fix->year % 400U == 0U));
  if (fix->month == 2 && leap) max_day = 29;
  if (fix->day > max_day) return false;

  // Howard Hinnant's civil-date conversion, with the Unix epoch offset.
  int year = (int)fix->year;
  unsigned month = fix->month;
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = (unsigned)(year - era * 400);
  const unsigned adjusted_month = month > 2 ? month - 3U : month + 9U;
  const unsigned day_of_year =
      (153U * adjusted_month + 2U) / 5U +
      (unsigned)fix->day - 1U;
  const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
                              year_of_era / 100U + day_of_year;
  const int64_t days = (int64_t)era * 146097LL + day_of_era - 719468LL;
  const int64_t epoch = days * 86400LL + (int64_t)fix->hour * 3600LL +
                        (int64_t)fix->minute * 60LL + fix->second;
  if (epoch < TRACK_TIMESTAMP_MIN || epoch > TRACK_TIMESTAMP_MAX) return false;
  *out_timestamp = (uint32_t)epoch;
  return true;
}

bool track_record_from_fix(const gps_fix_t *fix, track_record_t *out_record) {
  uint32_t timestamp;
  if (!fix || !out_record || !track_epoch_from_fix(fix, &timestamp) ||
      !isfinite(fix->lat_deg) || !isfinite(fix->lon_deg) ||
      fix->lat_deg < -90.0 || fix->lat_deg > 90.0 ||
      fix->lon_deg < -180.0 || fix->lon_deg > 180.0) {
    return false;
  }

  track_record_t record;
  memset(&record, 0, sizeof(record));
  record.latitude_e7 = (int32_t)llround(fix->lat_deg * 10000000.0);
  record.longitude_e7 = (int32_t)llround(fix->lon_deg * 10000000.0);
  if (fix->altitude_valid && isfinite(fix->altitude_m) &&
      fix->altitude_m >= -10000.0 && fix->altitude_m <= 1000000.0) {
    record.altitude_cm = (int32_t)llround(fix->altitude_m * 100.0);
  } else {
    record.altitude_cm = TRACK_ALTITUDE_UNKNOWN;
  }
  record.timestamp_utc = timestamp;
  *out_record = record;
  return true;
}

bool track_record_is_valid(const track_record_t *record) {
  if (!record || record->timestamp_utc < TRACK_TIMESTAMP_MIN ||
      record->timestamp_utc > TRACK_TIMESTAMP_MAX ||
      record->latitude_e7 < -900000000 ||
      record->latitude_e7 > 900000000 ||
      record->longitude_e7 < -1800000000 ||
      record->longitude_e7 > 1800000000) {
    return false;
  }
  return record->altitude_cm == TRACK_ALTITUDE_UNKNOWN ||
         (record->altitude_cm >= -1000000 &&
          record->altitude_cm <= 100000000);
}
