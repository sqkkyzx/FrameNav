/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_rate.h"

#if ALPHALOC_GNSS_RATE_1HZ
#include <string.h>

// u-blox M10 SPG 5.10: CFG-VALSET v0, RAM only; never BBR/Flash/save/reset.
// MEAS=1000ms, NAV=1, RMC/GGA on UART1 once per navigation epoch.
static const uint8_t s_rate_values[] = {
    0x01, 0x00, 0x21, 0x30, 0xe8, 0x03,
    0x02, 0x00, 0x21, 0x30, 0x01, 0x00,
    0xac, 0x00, 0x91, 0x20, 0x01,
    0xbb, 0x00, 0x91, 0x20, 0x01,
};
static const uint8_t s_rate_keys[] = {
    0x01, 0x00, 0x21, 0x30, 0x02, 0x00, 0x21, 0x30,
    0xac, 0x00, 0x91, 0x20, 0xbb, 0x00, 0x91, 0x20,
};
#define RESPONSE_WAIT_US 1500000LL
#define RETRY_WAIT_US 1000000LL
#define FRAME_TIMEOUT_US 1000000LL
#define MAX_ATTEMPTS 3

static void report(gnss_rate_t *s, gnss_rate_event_t event) {
  if (s->log) s->log(event, s->arg);
}

static bool send_config(gnss_rate_t *s, bool readback) {
  uint8_t frame[34] = {0xb5, 0x62, 0x06, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t *data = readback ? s_rate_keys : s_rate_values;
  size_t size = readback ? sizeof(s_rate_keys) : sizeof(s_rate_values);
  frame[3] = readback ? 0x8b : 0x8a;
  frame[4] = (uint8_t)(4 + size);
  // VALGET layer=0 means RAM; VALSET layers is a bitmap, RAM=1.
  frame[7] = readback ? 0 : 1;
  memcpy(frame + 10, data, size);
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < 10 + size; ++i) {
    a += frame[i]; b += a;
  }
  frame[10 + size] = a;
  frame[11 + size] = b;
  return s->send && s->send(frame, 12 + size, s->arg);
}

static void retry_or_stop(gnss_rate_t *s, int64_t now_us) {
  if (s->attempts >= MAX_ATTEMPTS) {
    s->stage = GNSS_RATE_FAILED;
    report(s, GNSS_RATE_GIVE_UP);
  } else {
    s->stage = GNSS_RATE_RETRY;
    s->deadline_us = now_us + RETRY_WAIT_US;
  }
}

void gnss_rate_init(gnss_rate_t *s, int64_t now_us,
                    gnss_rate_send_t send, gnss_rate_log_t log, void *arg) {
  memset(s, 0, sizeof(*s));
  s->send = send;
  s->log = log;
  s->arg = arg;
  s->stage = GNSS_RATE_WAIT_DATA;
  s->deadline_us = now_us + RETRY_WAIT_US;
}

void gnss_rate_note_nmea(gnss_rate_t *s) { s->nmea_seen = true; }

void gnss_rate_set_frame_observer(gnss_rate_t *s, gnss_rate_frame_t observer,
                                  void *arg) {
  s->frame_observer = observer;
  s->frame_arg = arg;
}

void gnss_rate_poll(gnss_rate_t *s, int64_t now_us) {
  if ((s->stage == GNSS_RATE_WAIT_DATA && s->nmea_seen) ||
      s->stage == GNSS_RATE_RETRY) {
    if (now_us < s->deadline_us) return;
    ++s->attempts;
    s->acknowledged = false;
    s->stage = GNSS_RATE_WAIT_ACK;
    s->deadline_us = now_us + RESPONSE_WAIT_US;
    if (send_config(s, false)) {
      report(s, GNSS_RATE_SENT);
    } else {
      report(s, GNSS_RATE_TX_ERROR);
      retry_or_stop(s, now_us);
    }
    return;
  }
  if (s->stage == GNSS_RATE_WAIT_ACK && now_us >= s->deadline_us) {
    report(s, GNSS_RATE_ACK_TIMEOUT);
    // An ACK might be lost or UBX output disabled. Read RAM before retrying.
    s->stage = GNSS_RATE_READBACK;
  }
  if (s->stage == GNSS_RATE_READBACK) {
    s->stage = GNSS_RATE_WAIT_READBACK;
    s->deadline_us = now_us + RESPONSE_WAIT_US;
    if (!send_config(s, true)) {
      report(s, GNSS_RATE_TX_ERROR);
      retry_or_stop(s, now_us);
    }
  } else if (s->stage == GNSS_RATE_WAIT_READBACK && now_us >= s->deadline_us) {
    report(s, GNSS_RATE_READ_TIMEOUT);
    retry_or_stop(s, now_us);
  }
}

static uint32_t read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool parse_readback(gnss_rate_t *s) {
  // Require the entire four-key result, RAM, first page, version 1.
  if (s->payload_len != 26 || s->payload[0] != 1 || s->payload[1] != 0 ||
      s->payload[2] != 0 || s->payload[3] != 0) return false;
  unsigned seen = 0;
  uint16_t meas = 0, nav = 0;
  uint8_t rmc = 0, gga = 0;
  for (size_t p = 4; p < s->payload_len;) {
    if (p + 5 > s->payload_len) return false;
    uint32_t key = read_u32(s->payload + p);
    p += 4;
    unsigned bit;
    switch (key) {
    case 0x30210001: case 0x30210002: {
      if (p + 2 > s->payload_len) return false;
      uint16_t value = s->payload[p] | ((uint16_t)s->payload[p + 1] << 8);
      p += 2;
      bit = key == 0x30210001 ? 1 : 2;
      if (bit == 1) meas = value; else nav = value;
      break;
    }
    case 0x209100ac: bit = 4; rmc = s->payload[p++]; break;
    case 0x209100bb: bit = 8; gga = s->payload[p++]; break;
    default: return false;
    }
    if (seen & bit) return false;
    seen |= bit;
  }
  if (seen != 15) return false;
  s->meas_ms = meas; s->nav_ratio = nav; s->rmc_rate = rmc; s->gga_rate = gga;
  return true;
}

static void receive_frame(gnss_rate_t *s, int64_t now_us) {
  if (s->cls == 0x05 && s->payload_len == 2 && s->payload[0] == 0x06) {
    bool for_set = s->payload[1] == 0x8a &&
        (s->stage == GNSS_RATE_WAIT_ACK || s->stage == GNSS_RATE_READBACK ||
         s->stage == GNSS_RATE_WAIT_READBACK);
    bool for_get = s->payload[1] == 0x8b && s->stage == GNSS_RATE_WAIT_READBACK;
    if (s->id == 0x00 && (for_set || for_get)) {
      s->stage = GNSS_RATE_FAILED;
      report(s, GNSS_RATE_NAK); // Unsupported config: no legacy fallback writes.
    } else if (s->id == 0x01 && for_set && !s->acknowledged) {
      s->acknowledged = true;
      report(s, GNSS_RATE_ACK);
      if (s->stage == GNSS_RATE_WAIT_ACK) s->stage = GNSS_RATE_READBACK;
    }
  } else if (s->cls == 0x06 && s->id == 0x8b &&
             s->stage == GNSS_RATE_WAIT_READBACK && parse_readback(s)) {
    if (s->meas_ms == 1000 && s->nav_ratio == 1 &&
        s->rmc_rate == 1 && s->gga_rate == 1) {
      s->stage = GNSS_RATE_VERIFIED;
      report(s, GNSS_RATE_CONFIRMED);
    } else {
      report(s, GNSS_RATE_MISMATCH);
      retry_or_stop(s, now_us);
    }
  }
}

bool gnss_rate_consume(gnss_rate_t *s, uint8_t byte, int64_t now_us) {
  if (s->rx_state && now_us - s->frame_started_us > FRAME_TIMEOUT_US) {
    s->rx_state = 0;
    ++s->ubx_bad;
  }
  if (s->rx_state == 0) {
    if (byte != 0xb5) return false;
    s->rx_state = 1;
    s->frame_started_us = now_us;
    return true;
  }
  if (s->rx_state == 1) {
    if (byte == 0x62) {
      s->rx_state = 2;
      s->rx_pos = 0;
      s->ck_a = s->ck_b = 0;
      s->payload_len = 0;
    } else if (byte != 0xb5) {
      s->rx_state = 0;
      return false; // Recover a '$' immediately following a false sync.
    }
    return true;
  }
  uint32_t pos = s->rx_pos++;
  if (pos < 4 || pos < (uint32_t)4 + s->payload_len) {
    s->ck_a += byte; s->ck_b += s->ck_a;
    if (pos == 0) s->cls = byte;
    else if (pos == 1) s->id = byte;
    else if (pos == 2) s->payload_len = byte;
    else if (pos == 3) s->payload_len |= (uint16_t)byte << 8;
    else if (pos - 4 < sizeof(s->payload)) s->payload[pos - 4] = byte;
  } else if (pos == (uint32_t)4 + s->payload_len) {
    s->received_ck_a = byte;
  } else {
    s->rx_state = 0;
    if (s->received_ck_a == s->ck_a && byte == s->ck_b) {
      ++s->ubx_ok;
      if (s->payload_len <= sizeof(s->payload)) {
        if (s->frame_observer) {
          s->frame_observer(s->cls, s->id, s->payload, s->payload_len,
                             now_us, s->frame_arg);
        }
        receive_frame(s, now_us);
      }
    } else {
      ++s->ubx_bad;
    }
  }
  return true;
}
#endif
