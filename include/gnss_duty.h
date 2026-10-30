/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_GNSS_DUTY_H
#define ALPHALOC_GNSS_DUTY_H

#include <stdbool.h>
#include <stdint.h>

#include "gps.h"

#define GNSS_DUTY_WORK_S 45U
#define GNSS_DUTY_DISCONNECT_GRACE_S 20U
#define GNSS_DUTY_SLEEP_NORMAL_S 30U
#define GNSS_DUTY_SLEEP_FAST_S 15U
#define GNSS_DUTY_SLEEP_UNKNOWN_S 90U
#ifndef GNSS_DUTY_STATIONARY_SPEED_MPS
#define GNSS_DUTY_STATIONARY_SPEED_MPS 0.5
#endif
#ifndef GNSS_DUTY_FAST_SPEED_MPS
#define GNSS_DUTY_FAST_SPEED_MPS 8.0
#endif

typedef enum {
  GNSS_DUTY_NONE,
  GNSS_DUTY_SLEEP,
  GNSS_DUTY_WAKE,
} gnss_duty_action_t;

typedef enum {
  GNSS_DUTY_WORKING,
  GNSS_DUTY_PROBING,
  GNSS_DUTY_SLEEPING,
} gnss_duty_phase_t;

typedef struct {
  bool continuous;
  bool require_camera;
  uint32_t boot_work_s;
  uint32_t disconnect_grace_s;
  uint32_t stationary_sleep_s;
  uint32_t moving_sleep_s;
  uint32_t fast_sleep_s;
  uint32_t fix_timeout_s;
  double fast_speed_mps;
} gnss_duty_params_t;

// Pure, single-owner policy: call from the GPS task; no UART, timers or heap.
// Monotonic times are microseconds since MCU boot. GNSS times remain UTC.
// A comparable sample selects <=0.5 m/s: 90 s, >8 m/s: 15 s, otherwise 30 s.
// No comparable previous fix also selects 90 s. Camera work overrides these.
typedef struct {
  gnss_duty_phase_t phase;
  gnss_duty_params_t params;
  bool camera_connected;
  bool have_previous_fix;
  bool have_speed;
  bool probe_complete;
  uint32_t next_sleep_s;
  double last_speed_mps;
  double previous_lat_deg;
  double previous_lon_deg;
  int64_t previous_epoch_s;
  int64_t previous_seen_us;
  int64_t boot_deadline_us;
  int64_t grace_deadline_us;
  int64_t sleep_deadline_us;
  int64_t probe_started_us;
  int64_t probe_deadline_us;
  int64_t wake_min_epoch_s;
} gnss_duty_t;

void gnss_duty_init(gnss_duty_t *state, int64_t now_us);
void gnss_duty_init_with_params(gnss_duty_t *state, int64_t now_us,
                                const gnss_duty_params_t *params);
void gnss_duty_apply_params(gnss_duty_t *state,
                            const gnss_duty_params_t *params,
                            int64_t now_us);
// Every delivered disconnect starts grace, even if a rapid preceding connect
// was coalesced by the caller. Pass the BLE event's actual monotonic timestamp.
void gnss_duty_camera_event(gnss_duty_t *state, bool connected,
                            int64_t event_us);
// Call only for freshly parsed RMC positions, not gps_get_latest() snapshots.
// Returns true only when a valid, strictly advancing UTC sample was accepted.
bool gnss_duty_note_fix(gnss_duty_t *state, const gps_fix_t *fix, int64_t now_us);
// Actions repeat until committed. In particular SLEEP does not start the
// standby interval or mark the receiver asleep before transport completes.
gnss_duty_action_t gnss_duty_poll(gnss_duty_t *state, int64_t now_us);
void gnss_duty_sleep_committed(gnss_duty_t *state, int64_t now_us);
void gnss_duty_wake_committed(gnss_duty_t *state, int64_t now_us);
bool gnss_duty_is_sleeping(const gnss_duty_t *state);
bool gnss_duty_work_required(const gnss_duty_t *state, int64_t now_us);

#endif
