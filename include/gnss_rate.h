/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_GNSS_RATE_H
#define ALPHALOC_GNSS_RATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ALPHALOC_GNSS_RATE_1HZ
#define ALPHALOC_GNSS_RATE_1HZ 0
#endif

#if ALPHALOC_GNSS_RATE_1HZ
typedef enum {
  GNSS_RATE_WAIT_DATA, GNSS_RATE_WAIT_ACK, GNSS_RATE_READBACK,
  GNSS_RATE_WAIT_READBACK, GNSS_RATE_RETRY, GNSS_RATE_VERIFIED, GNSS_RATE_FAILED
} gnss_rate_stage_t;

typedef enum {
  GNSS_RATE_SENT, GNSS_RATE_ACK, GNSS_RATE_NAK, GNSS_RATE_ACK_TIMEOUT,
  GNSS_RATE_CONFIRMED, GNSS_RATE_MISMATCH, GNSS_RATE_READ_TIMEOUT,
  GNSS_RATE_TX_ERROR, GNSS_RATE_GIVE_UP
} gnss_rate_event_t;

typedef bool (*gnss_rate_send_t)(const uint8_t *data, size_t len, void *arg);
typedef void (*gnss_rate_log_t)(gnss_rate_event_t event, void *arg);
typedef void (*gnss_rate_frame_t)(uint8_t cls, uint8_t id, const uint8_t *payload,
                                  size_t len, int64_t now_us, void *arg);

// Single-owner state: all calls run in the GPS receive task; no heap/timers.
typedef struct {
  gnss_rate_stage_t stage;
  gnss_rate_send_t send;
  gnss_rate_log_t log;
  void *arg;
  gnss_rate_frame_t frame_observer;
  void *frame_arg;
  int64_t deadline_us;
  unsigned attempts;
  bool nmea_seen, acknowledged;
  uint16_t meas_ms, nav_ratio;
  uint8_t rmc_rate, gga_rate;
  uint8_t rx_state, cls, id, ck_a, ck_b, received_ck_a;
  uint16_t payload_len;
  uint32_t rx_pos, ubx_ok, ubx_bad;
  int64_t frame_started_us;
#if ALPHALOC_GNSS_STANDBY
  uint8_t payload[512]; // MON-VER fixed fields and bounded extension strings.
#else
  uint8_t payload[64];
#endif
} gnss_rate_t;

void gnss_rate_init(gnss_rate_t *state, int64_t now_us,
                    gnss_rate_send_t send, gnss_rate_log_t log, void *arg);
void gnss_rate_note_nmea(gnss_rate_t *state);
void gnss_rate_set_frame_observer(gnss_rate_t *state, gnss_rate_frame_t observer,
                                  void *arg);
void gnss_rate_poll(gnss_rate_t *state, int64_t now_us);
// true: byte belongs to UBX (including unknown messages), do not feed to NMEA.
bool gnss_rate_consume(gnss_rate_t *state, uint8_t byte, int64_t now_us);
#endif
#endif
