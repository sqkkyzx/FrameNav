/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FRAMENAV_TRACK_STORE_H
#define FRAMENAV_TRACK_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gps.h"

#ifndef ALPHALOC_TRACK_STORE
#define ALPHALOC_TRACK_STORE 0
#endif

#define TRACK_STORE_PARTITION_LABEL "track"
#define TRACK_STORE_PARTITION_BYTES (1500U * 1024U)
#define TRACK_STORE_INTERVAL_S 3U
#define TRACK_ALTITUDE_UNKNOWN INT32_MIN
#define TRACK_TIMESTAMP_MIN 1577836800U
#define TRACK_TIMESTAMP_MAX 4102444799U

// The timestamp is programmed last and acts as the record commit marker.
// There are exactly four payload values and no per-record padding.
typedef struct __attribute__((packed)) {
  int32_t latitude_e7;
  int32_t longitude_e7;
  int32_t altitude_cm;
  uint32_t timestamp_utc;
} track_record_t;

_Static_assert(sizeof(track_record_t) == 16,
               "FrameNav track records must remain 16 bytes");

typedef struct {
  bool available;
  uint32_t capacity_bytes;
  uint32_t used_bytes;
  uint32_t record_count;
  uint32_t earliest_timestamp_utc;
  uint32_t latest_timestamp_utc;
} track_store_status_t;

// Pure format helpers are exposed so the on-flash format can be tested without
// an ESP32. They do not access the partition.
bool track_epoch_from_fix(const gps_fix_t *fix, uint32_t *out_timestamp);
bool track_record_from_fix(const gps_fix_t *fix, track_record_t *out_record);
bool track_record_is_valid(const track_record_t *record);

bool track_store_init(void);
bool track_store_append_fix(const gps_fix_t *fix);
bool track_store_get_status(track_store_status_t *out_status);
void track_store_task(void *arg);
bool track_store_quiesce(bool pause);

#endif
