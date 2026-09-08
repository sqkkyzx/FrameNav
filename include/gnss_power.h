/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_GNSS_POWER_H
#define ALPHALOC_GNSS_POWER_H

#ifndef ALPHALOC_GNSS_STANDBY
#define ALPHALOC_GNSS_STANDBY 0
#endif

#if ALPHALOC_GNSS_STANDBY
#include "gnss_duty.h"
#include "gnss_pm_protocol.h"

typedef enum {
  GNSS_POWER_WORK, GNSS_POWER_SLEEP_SETTLE, GNSS_POWER_SLEEP,
  GNSS_POWER_WAKING, GNSS_POWER_DISABLED
} gnss_power_phase_t;
typedef enum {
  GNSS_POWER_ID_QUERY, GNSS_POWER_ID_OK, GNSS_POWER_ID_UNSUPPORTED,
  GNSS_POWER_ID_TIMEOUT, GNSS_POWER_SLEEP_SENT, GNSS_POWER_SLEEP_QUIET,
  GNSS_POWER_SLEEP_NAK, GNSS_POWER_SLEEP_FAILED, GNSS_POWER_WAKE_SENT,
  GNSS_POWER_WAKE_OK, GNSS_POWER_WAKE_RETRY, GNSS_POWER_WAKE_FAILED
} gnss_power_event_t;

// send must drain TX before returning. Only the GPS task calls this interface.
typedef bool (*gnss_power_send_t)(const uint8_t *, size_t, void *);
typedef void (*gnss_power_log_t)(gnss_power_event_t, void *);
enum { GNSS_POWER_INVALIDATE_FIX = 1, GNSS_POWER_RESET_RX = 2 };

typedef struct {
  gnss_duty_t duty;
  gnss_pm_identity_t identity;
  gnss_power_phase_t phase;
  gnss_power_send_t send;
  gnss_power_log_t log;
  int64_t (*clock)(void *); // Optional transport-completion timestamp.
  bool (*sleep_allowed)(void *); // Recheck BLE events before issuing PMREQ.
  void *arg;
  bool identity_checked, seen_nmea, disable_after_wake, rejected;
  bool deep_requested, deep_committed;
  unsigned identity_attempts, wake_attempts, actions;
  int64_t identity_deadline_us, last_nmea_us, sleep_started_us, wake_deadline_us;
} gnss_power_t;

void gnss_power_init(gnss_power_t *, int64_t, gnss_power_send_t,
                     gnss_power_log_t, void *);
void gnss_power_init_with_params(gnss_power_t *, int64_t,
                                 gnss_power_send_t, gnss_power_log_t, void *,
                                 const gnss_duty_params_t *);
void gnss_power_apply_params(gnss_power_t *, const gnss_duty_params_t *,
                             int64_t);
void gnss_power_camera_event(gnss_power_t *, bool, int64_t);
void gnss_power_note_nmea(gnss_power_t *, int64_t);
bool gnss_power_note_fix(gnss_power_t *, const gps_fix_t *, int64_t);
void gnss_power_frame(gnss_power_t *, uint8_t, uint8_t,
                      const uint8_t *, size_t, int64_t);
void gnss_power_poll(gnss_power_t *, int64_t);
bool gnss_power_receiving(const gnss_power_t *);
unsigned gnss_power_take_actions(gnss_power_t *);
#endif
#endif
