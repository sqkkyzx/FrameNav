/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_power.h"

#if ALPHALOC_GNSS_STANDBY
#include <string.h>

#define PM_ID_WAIT_US 1500000LL
#define PM_SLEEP_SETTLE_US 1500000LL
#define PM_NMEA_DRAIN_US 500000LL
#define PM_WAKE_WAIT_US 2000000LL
#define PM_MAX_ATTEMPTS 3U

static void power_report(gnss_power_t *p, gnss_power_event_t event) {
  if (p->log) p->log(event, p->arg);
}

static int64_t power_now(gnss_power_t *p, int64_t fallback_us) {
  return p->clock ? p->clock(p->arg) : fallback_us;
}

void gnss_power_init(gnss_power_t *p, int64_t now_us,
                     gnss_power_send_t send, gnss_power_log_t log, void *arg) {
  gnss_power_init_with_params(p, now_us, send, log, arg, NULL);
}

void gnss_power_init_with_params(gnss_power_t *p, int64_t now_us,
                                 gnss_power_send_t send,
                                 gnss_power_log_t log, void *arg,
                                 const gnss_duty_params_t *params) {
  memset(p, 0, sizeof(*p));
  gnss_duty_init_with_params(&p->duty, now_us, params);
  p->send = send;
  p->log = log;
  p->arg = arg;
  p->phase = GNSS_POWER_WORK;
  p->identity_deadline_us = now_us + 1000000LL;
}

void gnss_power_apply_params(gnss_power_t *p,
                             const gnss_duty_params_t *params,
                             int64_t now_us) {
  if (!p || !params) return;
  gnss_duty_apply_params(&p->duty, params, now_us);
}

void gnss_power_camera_event(gnss_power_t *p, bool connected, int64_t now_us) {
  gnss_duty_camera_event(&p->duty, connected, now_us);
}

bool gnss_power_receiving(const gnss_power_t *p) {
  return p->phase == GNSS_POWER_WORK || p->phase == GNSS_POWER_DISABLED;
}

void gnss_power_note_nmea(gnss_power_t *p, int64_t now_us) {
  p->seen_nmea = true;
  p->last_nmea_us = now_us;
  if (p->phase == GNSS_POWER_WAKING) {
    p->phase = p->disable_after_wake ? GNSS_POWER_DISABLED : GNSS_POWER_WORK;
    power_report(p, GNSS_POWER_WAKE_OK);
  }
}

bool gnss_power_note_fix(gnss_power_t *p, const gps_fix_t *fix, int64_t now_us) {
  if (!gnss_power_receiving(p)) return false;
  if (p->phase == GNSS_POWER_DISABLED) {
    return fix && fix->valid && fix->time_valid;
  }
  return gnss_duty_note_fix(&p->duty, fix, now_us);
}

void gnss_power_frame(gnss_power_t *p, uint8_t cls, uint8_t id,
                      const uint8_t *payload, size_t len, int64_t now_us) {
  (void)now_us;
  if (!p->identity_checked && p->identity_attempts && cls == 0x0a && id == 0x04) {
    gnss_pm_identity_t identity;
    if (!gnss_pm_parse_mon_ver(payload, len, &identity)) return;
    p->identity = identity;
    p->identity_checked = true;
    if (identity.supported) {
      power_report(p, GNSS_POWER_ID_OK);
    } else {
      p->phase = GNSS_POWER_DISABLED;
      power_report(p, GNSS_POWER_ID_UNSUPPORTED);
    }
  }
  if ((p->phase == GNSS_POWER_SLEEP_SETTLE || p->phase == GNSS_POWER_SLEEP) &&
      gnss_pm_is_request_nak(cls, id, payload, len)) {
    p->rejected = true;
  }
}

static void power_wake(gnss_power_t *p, int64_t now_us, bool retry) {
  // An edge on receiver RX is the configured wake source. This byte is not a
  // configuration command: wait for fresh NMEA before restoring RAM settings.
  static const uint8_t wake_byte = 0xff;
  if (!retry) {
    p->wake_attempts = 0;
    p->actions |= GNSS_POWER_INVALIDATE_FIX | GNSS_POWER_RESET_RX;
  }
  ++p->wake_attempts;
  bool ok = p->send && p->send(&wake_byte, 1, p->arg);
  now_us = power_now(p, now_us);
  if (!retry) gnss_duty_wake_committed(&p->duty, now_us);
  p->phase = GNSS_POWER_WAKING;
  p->wake_deadline_us = now_us + PM_WAKE_WAIT_US;
  power_report(p, retry ? GNSS_POWER_WAKE_RETRY : GNSS_POWER_WAKE_SENT);
  if (!ok) {
    p->disable_after_wake = true;
    power_report(p, GNSS_POWER_WAKE_FAILED);
  }
}

static void power_disable(gnss_power_t *p, int64_t now_us,
                           gnss_power_event_t reason) {
  p->disable_after_wake = true;
  power_report(p, reason);
  // The PMREQ could have reached the receiver even after a partial TX failure.
  // Always try to wake it; the finite PMREQ duration is an independent fallback.
  power_wake(p, now_us, false);
}

void gnss_power_poll(gnss_power_t *p, int64_t now_us) {
  if (p->phase == GNSS_POWER_DISABLED) return;
  if (p->phase == GNSS_POWER_WAKING) {
    if (now_us < p->wake_deadline_us) return;
    if (p->wake_attempts < PM_MAX_ATTEMPTS) {
      power_wake(p, now_us, true);
    } else {
      p->phase = GNSS_POWER_DISABLED;
      p->disable_after_wake = true;
      power_report(p, GNSS_POWER_WAKE_FAILED);
    }
    return;
  }
  if (!p->identity_checked) {
    if (!p->seen_nmea || now_us < p->identity_deadline_us) return;
    if (p->identity_attempts >= PM_MAX_ATTEMPTS) {
      p->phase = GNSS_POWER_DISABLED;
      power_report(p, GNSS_POWER_ID_TIMEOUT);
      return;
    }
    uint8_t packet[GNSS_PM_MON_VER_PACKET_SIZE];
    size_t len = gnss_pm_build_mon_ver(packet, sizeof(packet));
    ++p->identity_attempts;
    if (p->send) p->send(packet, len, p->arg);
    p->identity_deadline_us = now_us + PM_ID_WAIT_US;
    power_report(p, GNSS_POWER_ID_QUERY);
    return;
  }

  gnss_duty_action_t action = gnss_duty_poll(&p->duty, now_us);
  if (p->phase == GNSS_POWER_SLEEP_SETTLE || p->phase == GNSS_POWER_SLEEP) {
    if (p->rejected) {
      p->rejected = false;
      power_disable(p, now_us, GNSS_POWER_SLEEP_NAK);
    } else if (action == GNSS_DUTY_WAKE) {
      power_wake(p, now_us, false);
    } else if (p->last_nmea_us > p->sleep_started_us + PM_NMEA_DRAIN_US) {
      power_disable(p, now_us, GNSS_POWER_SLEEP_FAILED);
    } else if (p->phase == GNSS_POWER_SLEEP_SETTLE &&
               now_us >= p->sleep_started_us + PM_SLEEP_SETTLE_US) {
      p->phase = GNSS_POWER_SLEEP;
      power_report(p, GNSS_POWER_SLEEP_QUIET);
    }
    return;
  }
  if (action == GNSS_DUTY_SLEEP) {
    if (p->sleep_allowed && !p->sleep_allowed(p->arg)) return;
    uint8_t packet[GNSS_PM_REQUEST_PACKET_SIZE];
    size_t len = gnss_pm_build_request(packet, sizeof(packet),
                                        p->duty.next_sleep_s * 1000U);
    p->actions |= GNSS_POWER_INVALIDATE_FIX;
    if (!len || !p->send || !p->send(packet, len, p->arg)) {
      if (p->sleep_allowed && !p->sleep_allowed(p->arg)) {
        // A new BLE event vetoed transmission. A wake edge also covers a
        // partial packet already sent, without permanently disabling policy.
        power_wake(p, power_now(p, now_us), false);
        return;
      }
      power_disable(p, now_us, GNSS_POWER_SLEEP_FAILED);
      return;
    }
    now_us = power_now(p, now_us);
    gnss_duty_sleep_committed(&p->duty, now_us);
    p->sleep_started_us = now_us;
    p->phase = GNSS_POWER_SLEEP_SETTLE;
    power_report(p, GNSS_POWER_SLEEP_SENT);
  }
}

unsigned gnss_power_take_actions(gnss_power_t *p) {
  unsigned actions = p->actions;
  p->actions = 0;
  return actions;
}
#endif
