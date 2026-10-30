/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_duty.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define DUTY_US_PER_SECOND 1000000LL

static int64_t duty_deadline(int64_t now_us, uint32_t seconds) {
  const int64_t duration_us = seconds * DUTY_US_PER_SECOND;
  return now_us > INT64_MAX - duration_us ? INT64_MAX : now_us + duration_us;
}

static bool duty_priority_work(const gnss_duty_t *state, int64_t now_us) {
  if (state->params.require_camera && !state->camera_connected) return false;
  return state->params.continuous || state->camera_connected ||
         now_us < state->boot_deadline_us ||
         now_us < state->grace_deadline_us;
}

static gnss_duty_params_t duty_default_params(void) {
  return (gnss_duty_params_t){
      .boot_work_s = GNSS_DUTY_WORK_S,
      .disconnect_grace_s = GNSS_DUTY_DISCONNECT_GRACE_S,
      .stationary_sleep_s = GNSS_DUTY_SLEEP_UNKNOWN_S,
      .moving_sleep_s = GNSS_DUTY_SLEEP_NORMAL_S,
      .fast_sleep_s = GNSS_DUTY_SLEEP_FAST_S,
      .fix_timeout_s = GNSS_DUTY_WORK_S,
      .fast_speed_mps = GNSS_DUTY_FAST_SPEED_MPS,
  };
}

static gnss_duty_params_t duty_sanitize_params(
    const gnss_duty_params_t *params) {
  gnss_duty_params_t result = params ? *params : duty_default_params();
  const gnss_duty_params_t defaults = duty_default_params();
  if (result.stationary_sleep_s == 0) {
    result.stationary_sleep_s = defaults.stationary_sleep_s;
  }
  if (result.moving_sleep_s == 0) {
    result.moving_sleep_s = defaults.moving_sleep_s;
  }
  if (result.fast_sleep_s == 0) {
    result.fast_sleep_s = defaults.fast_sleep_s;
  }
  if (result.fix_timeout_s == 0) {
    result.fix_timeout_s = defaults.fix_timeout_s;
  }
  if (!isfinite(result.fast_speed_mps) ||
      result.fast_speed_mps <= GNSS_DUTY_STATIONARY_SPEED_MPS) {
    result.fast_speed_mps = defaults.fast_speed_mps;
  }
  return result;
}

static bool duty_leap_year(unsigned year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int64_t duty_days_before_year(unsigned year) {
  const int64_t previous = (int64_t)year - 1;
  return previous * 365 + previous / 4 - previous / 100 + previous / 400;
}

static bool duty_fix_epoch(const gps_fix_t *fix, int64_t *epoch_s) {
  static const uint8_t month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (!fix || !fix->valid || !fix->time_valid ||
      !isfinite(fix->lat_deg) || !isfinite(fix->lon_deg) ||
      fix->lat_deg < -90.0 || fix->lat_deg > 90.0 ||
      fix->lon_deg < -180.0 || fix->lon_deg > 180.0 ||
      fix->year < 1970 || fix->year > 9999 ||
      fix->month < 1 || fix->month > 12 || fix->day < 1 ||
      fix->hour > 23 || fix->minute > 59 || fix->second > 59) {
    return false;
  }
  const bool leap = duty_leap_year(fix->year);
  const unsigned days_in_month = month_days[fix->month - 1] +
                                 (leap && fix->month == 2);
  if (fix->day > days_in_month) {
    return false;
  }
  int64_t days = duty_days_before_year(fix->year) - duty_days_before_year(1970);
  for (unsigned month = 1; month < fix->month; ++month) {
    days += month_days[month - 1] + (leap && month == 2);
  }
  days += fix->day - 1;
  *epoch_s = days * 86400 + fix->hour * 3600 + fix->minute * 60 + fix->second;
  return *epoch_s > 0;
}

static double duty_distance_m(double lat1, double lon1,
                               double lat2, double lon2) {
  const double radians = 0.017453292519943295769;
  const double sin_lat = sin((lat2 - lat1) * radians / 2.0);
  const double sin_lon = sin((lon2 - lon1) * radians / 2.0);
  double haversine = sin_lat * sin_lat +
                     cos(lat1 * radians) * cos(lat2 * radians) * sin_lon * sin_lon;
  // Rounding at identical/antipodal positions must not produce NaN.
  if (haversine < 0.0) haversine = 0.0;
  if (haversine > 1.0) haversine = 1.0;
  return 2.0 * 6371000.0 * atan2(sqrt(haversine), sqrt(1.0 - haversine));
}

static uint32_t duty_sleep_for_speed_with_params(const gnss_duty_t *state,
                                                 double speed_mps) {
  if (speed_mps <= GNSS_DUTY_STATIONARY_SPEED_MPS) {
    return state->params.stationary_sleep_s;
  }
  return speed_mps > state->params.fast_speed_mps ?
      state->params.fast_sleep_s : state->params.moving_sleep_s;
}

// Kept as a small default-policy helper for host policy tests.
static uint32_t __attribute__((unused)) duty_sleep_for_speed(double speed_mps) {
  gnss_duty_t state = {.params = duty_default_params()};
  return duty_sleep_for_speed_with_params(&state, speed_mps);
}

void gnss_duty_init(gnss_duty_t *state, int64_t now_us) {
  gnss_duty_init_with_params(state, now_us, NULL);
}

void gnss_duty_init_with_params(gnss_duty_t *state, int64_t now_us,
                                const gnss_duty_params_t *params) {
  if (!state) return;
  memset(state, 0, sizeof(*state));
  state->params = duty_sanitize_params(params);
  state->phase = GNSS_DUTY_WORKING;
  state->boot_deadline_us = duty_deadline(now_us, state->params.boot_work_s);
  state->next_sleep_s = GNSS_DUTY_SLEEP_UNKNOWN_S;
}

void gnss_duty_apply_params(gnss_duty_t *state,
                            const gnss_duty_params_t *params,
                            int64_t now_us) {
  if (!state || !params) return;
  const bool was_continuous = state->params.continuous;
  state->params = duty_sanitize_params(params);
  state->next_sleep_s = state->have_speed ?
      duty_sleep_for_speed_with_params(state, state->last_speed_mps) :
      GNSS_DUTY_SLEEP_UNKNOWN_S;
  if (state->phase == GNSS_DUTY_SLEEPING) {
    state->sleep_deadline_us = duty_deadline(now_us, state->next_sleep_s);
  } else if (state->phase == GNSS_DUTY_PROBING) {
    state->probe_deadline_us =
        duty_deadline(state->probe_started_us, state->params.fix_timeout_s);
  }
  if (was_continuous && !state->params.continuous &&
      !state->camera_connected) {
    state->boot_deadline_us = 0;
    state->grace_deadline_us =
        duty_deadline(now_us, state->params.disconnect_grace_s);
  }
}

void gnss_duty_camera_event(gnss_duty_t *state, bool connected,
                            int64_t event_us) {
  if (!state) return;
  state->camera_connected = connected;
  state->grace_deadline_us = connected ? 0 :
      duty_deadline(event_us, state->params.disconnect_grace_s);
  state->probe_complete = false;
  if (state->phase != GNSS_DUTY_SLEEPING) {
    state->phase = GNSS_DUTY_WORKING;
  }
}

bool gnss_duty_note_fix(gnss_duty_t *state, const gps_fix_t *fix, int64_t now_us) {
  int64_t epoch_s;
  if (!state || state->phase == GNSS_DUTY_SLEEPING ||
      (state->phase == GNSS_DUTY_PROBING && state->probe_complete) ||
      !duty_fix_epoch(fix, &epoch_s) ||
      (state->have_previous_fix && epoch_s <= state->previous_epoch_s)) {
    return false;
  }
  if (state->phase == GNSS_DUTY_PROBING &&
      (now_us < state->probe_started_us || now_us >= state->probe_deadline_us ||
       epoch_s < state->wake_min_epoch_s)) {
    return false;
  }

  state->have_speed = state->have_previous_fix;
  if (state->have_previous_fix) {
    state->last_speed_mps = duty_distance_m(state->previous_lat_deg,
                                           state->previous_lon_deg,
                                           fix->lat_deg, fix->lon_deg) /
                            (double)(epoch_s - state->previous_epoch_s);
  } else {
    state->last_speed_mps = 0.0;
  }
  state->previous_lat_deg = fix->lat_deg;
  state->previous_lon_deg = fix->lon_deg;
  state->previous_epoch_s = epoch_s;
  state->previous_seen_us = now_us;
  state->have_previous_fix = true;

  state->next_sleep_s = state->have_speed ?
      duty_sleep_for_speed_with_params(state, state->last_speed_mps) :
      GNSS_DUTY_SLEEP_UNKNOWN_S;
  if (state->phase == GNSS_DUTY_PROBING) {
    state->probe_complete = true;
  }
  return true;
}

bool gnss_duty_is_sleeping(const gnss_duty_t *state) {
  return state && state->phase == GNSS_DUTY_SLEEPING;
}

bool gnss_duty_work_required(const gnss_duty_t *state, int64_t now_us) {
  if (!state) return false;
  if (state->params.require_camera && !state->camera_connected) return false;
  if (duty_priority_work(state, now_us)) return true;
  return state->phase == GNSS_DUTY_PROBING && !state->probe_complete &&
         now_us < state->probe_deadline_us;
}

gnss_duty_action_t gnss_duty_poll(gnss_duty_t *state, int64_t now_us) {
  if (!state) return GNSS_DUTY_NONE;
  if (state->phase == GNSS_DUTY_SLEEPING) {
    if (state->params.require_camera && !state->camera_connected)
      return GNSS_DUTY_NONE;
    return duty_priority_work(state, now_us) || now_us >= state->sleep_deadline_us ?
        GNSS_DUTY_WAKE : GNSS_DUTY_NONE;
  }
  if (gnss_duty_work_required(state, now_us)) return GNSS_DUTY_NONE;
  if (state->params.require_camera && !state->camera_connected) {
    state->next_sleep_s = 0;
    return GNSS_DUTY_SLEEP;
  }
  if (state->phase == GNSS_DUTY_PROBING && !state->probe_complete) {
    // A timeout cannot supply a new speed estimate. Keep existing position
    // history and use 30 s; if no valid position has ever existed, use 90 s.
    state->next_sleep_s = state->have_previous_fix ?
        state->params.moving_sleep_s : GNSS_DUTY_SLEEP_UNKNOWN_S;
  }
  return GNSS_DUTY_SLEEP;
}

void gnss_duty_sleep_committed(gnss_duty_t *state, int64_t now_us) {
  if (!state) return;
  state->phase = GNSS_DUTY_SLEEPING;
  state->sleep_deadline_us = duty_deadline(now_us, state->next_sleep_s);
}

void gnss_duty_wake_committed(gnss_duty_t *state, int64_t now_us) {
  if (!state) return;
  state->probe_complete = false;
  if (duty_priority_work(state, now_us)) {
    state->phase = GNSS_DUTY_WORKING;
    return;
  }
  state->phase = GNSS_DUTY_PROBING;
  state->probe_started_us = now_us;
  state->probe_deadline_us = duty_deadline(now_us, state->params.fix_timeout_s);
  state->wake_min_epoch_s = 0;
  if (state->have_previous_fix) {
    const int64_t elapsed_s = now_us > state->previous_seen_us ?
        (now_us - state->previous_seen_us) / DUTY_US_PER_SECOND : 0;
    // RMC exposes whole seconds only. Reject epochs older than the estimated
    // wake second; equal-to-floor may be a new measurement within that second.
    state->wake_min_epoch_s = state->previous_epoch_s + elapsed_s;
  }
}
