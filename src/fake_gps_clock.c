/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gps.h"

#if ALPHALOC_FAKE_GPS && ALPHALOC_FAKE_GPS_STEPPING
#include <time.h>

#if ALPHALOC_FAKE_GPS_USB_TIME
#include "fake_gps_usb_time.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_clock_ready;
static int64_t s_clock_epoch_us;
static int64_t s_clock_uptime_us;

bool gps_fake_set_usb_time(int64_t epoch_ms, int64_t uptime_us) {
  if (epoch_ms < ALPHALOC_USB_TIME_MIN_MS || epoch_ms >= ALPHALOC_USB_TIME_MAX_MS ||
      uptime_us < 0) {
    return false;
  }
  portENTER_CRITICAL(&s_clock_mux);
  bool accepted = !s_clock_ready;
  if (accepted) {
    s_clock_epoch_us = epoch_ms * 1000;
    s_clock_uptime_us = uptime_us;
    s_clock_ready = true;
  }
  portEXIT_CRITICAL(&s_clock_mux);
  return accepted;
}
#else
#include "fake_clock_seed.generated.h"
#endif

bool gps_fake_apply_clock(gps_fix_t *fix, int64_t uptime_us) {
  if (!fix || uptime_us < 0) {
    return false;
  }
#if ALPHALOC_FAKE_GPS_USB_TIME
  portENTER_CRITICAL(&s_clock_mux);
  bool ready = s_clock_ready;
  int64_t anchor_epoch_us = s_clock_epoch_us;
  int64_t anchor_uptime_us = s_clock_uptime_us;
  portEXIT_CRITICAL(&s_clock_mux);
  if (!ready || uptime_us < anchor_uptime_us ||
      uptime_us - anchor_uptime_us > INT64_MAX - anchor_epoch_us) {
    fix->time_valid = false;
    return false;
  }
  const time_t epoch =
      (time_t)((anchor_epoch_us + uptime_us - anchor_uptime_us) / 1000000LL);
#else
  // Advance by elapsed wall time, not by number of packets or function calls.
  // esp_timer uptime includes time spent disconnected and in task delays.
  const time_t epoch =
      (time_t)(ALPHALOC_FAKE_GPS_EPOCH + uptime_us / 1000000LL);
#endif
  struct tm utc;
  if (gmtime_r(&epoch, &utc) == NULL || utc.tm_year < 70 ||
      utc.tm_year > 8099) {
    return false;
  }
  fix->year = (uint16_t)(utc.tm_year + 1900);
  fix->month = (uint8_t)(utc.tm_mon + 1);
  fix->day = (uint8_t)utc.tm_mday;
  fix->hour = (uint8_t)utc.tm_hour;
  fix->minute = (uint8_t)utc.tm_min;
  fix->second = (uint8_t)utc.tm_sec;
  fix->time_valid = true;
  return true;
}
#endif
