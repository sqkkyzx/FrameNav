/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "track_store.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TRACK_SECTOR_SIZE 4096U
#define TRACK_SECTOR_MAGIC 0x46524E56U  // "FRNV"
#define TRACK_RECORDS_PER_SECTOR \
  ((TRACK_SECTOR_SIZE - sizeof(track_sector_header_t)) / sizeof(track_record_t))

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t sequence;
  uint16_t record_count;
  uint16_t reserved;
  uint32_t records_crc32;
} track_sector_header_t;

_Static_assert(sizeof(track_sector_header_t) == 16,
               "FrameNav track sector headers must remain 16 bytes");
_Static_assert(TRACK_RECORDS_PER_SECTOR == 255,
               "FrameNav track sectors must contain 255 records");

static const char *TAG = "track_store";
static const esp_partition_t *s_partition;
static SemaphoreHandle_t s_mutex;
static uint8_t *s_sector_buffer;
static track_store_status_t s_status;
static uint32_t s_next_sector;
static uint16_t s_next_record;
static uint32_t s_next_sequence = 1;

static uint32_t track_crc32(const uint8_t *data, size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
  }
  return ~crc;
}

static bool bytes_erased(const uint8_t *data, size_t length) {
  if (!data) return false;
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != 0xFF) return false;
  }
  return true;
}

static size_t record_offset(uint32_t sector, uint16_t record) {
  return (size_t)sector * TRACK_SECTOR_SIZE + sizeof(track_sector_header_t) +
         (size_t)record * sizeof(track_record_t);
}

static bool read_sector(uint32_t sector) {
  return s_partition && s_sector_buffer &&
         esp_partition_read(s_partition, (size_t)sector * TRACK_SECTOR_SIZE,
                            s_sector_buffer, TRACK_SECTOR_SIZE) == ESP_OK;
}

static bool scan_partition_locked(void) {
  const uint32_t sector_count = s_partition->size / TRACK_SECTOR_SIZE;
  uint32_t earliest = 0;
  uint32_t latest = 0;
  uint32_t latest_sector = 0;
  uint16_t latest_record = 0;
  uint32_t record_count = 0;
  uint32_t used_sectors = 0;
  uint32_t max_sequence = 0;

  for (uint32_t sector = 0; sector < sector_count; ++sector) {
    if (!read_sector(sector)) return false;
    const track_sector_header_t *header =
        (const track_sector_header_t *)s_sector_buffer;
    if (header->magic == TRACK_SECTOR_MAGIC &&
        header->record_count == TRACK_RECORDS_PER_SECTOR) {
      const uint8_t *payload = s_sector_buffer + sizeof(*header);
      const size_t payload_len =
          TRACK_RECORDS_PER_SECTOR * sizeof(track_record_t);
      if (track_crc32(payload, payload_len) == header->records_crc32 &&
          header->sequence > max_sequence) {
        max_sequence = header->sequence;
      }
    }

    bool sector_used = false;
    for (uint16_t slot = 0; slot < TRACK_RECORDS_PER_SECTOR; ++slot) {
      const track_record_t *record = (const track_record_t *)(
          s_sector_buffer + sizeof(*header) +
          (size_t)slot * sizeof(track_record_t));
      if (!track_record_is_valid(record)) continue;
      sector_used = true;
      ++record_count;
      if (earliest == 0 || record->timestamp_utc < earliest) {
        earliest = record->timestamp_utc;
      }
      if (latest == 0 || record->timestamp_utc > latest) {
        latest = record->timestamp_utc;
        latest_sector = sector;
        latest_record = slot;
      }
    }
    if (sector_used) ++used_sectors;
  }

  s_status.available = true;
  s_status.capacity_bytes = s_partition->size;
  s_status.record_count = record_count;
  s_status.used_bytes = record_count * sizeof(track_record_t) +
                        used_sectors * sizeof(track_sector_header_t);
  s_status.earliest_timestamp_utc = earliest;
  s_status.latest_timestamp_utc = latest;
  s_next_sequence = max_sequence + 1U;
  if (s_next_sequence == 0) s_next_sequence = 1;

  if (record_count == 0) {
    s_next_sector = 0;
    s_next_record = 0;
    return true;
  }

  if (latest_record + 1U < TRACK_RECORDS_PER_SECTOR) {
    const uint16_t candidate = latest_record + 1U;
    if (!read_sector(latest_sector)) return false;
    const uint8_t *next = s_sector_buffer + sizeof(track_sector_header_t) +
                          (size_t)candidate * sizeof(track_record_t);
    if (bytes_erased(next, sizeof(track_record_t))) {
      s_next_sector = latest_sector;
      s_next_record = candidate;
      return true;
    }
  }
  s_next_sector = (latest_sector + 1U) % sector_count;
  s_next_record = 0;
  return true;
}

static bool prepare_next_slot_locked(void) {
  const uint32_t sector_count = s_partition->size / TRACK_SECTOR_SIZE;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (!read_sector(s_next_sector)) return false;
    if (s_next_record == 0) {
      if (!bytes_erased(s_sector_buffer, TRACK_SECTOR_SIZE)) {
        esp_err_t err = esp_partition_erase_range(
            s_partition, (size_t)s_next_sector * TRACK_SECTOR_SIZE,
            TRACK_SECTOR_SIZE);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Track sector erase failed: %s", esp_err_to_name(err));
          return false;
        }
        if (!scan_partition_locked()) return false;
      }
      return true;
    }

    const uint8_t *slot = s_sector_buffer + sizeof(track_sector_header_t) +
                          (size_t)s_next_record * sizeof(track_record_t);
    if (bytes_erased(slot, sizeof(track_record_t))) return true;

    // A torn write must never make us erase earlier valid records in the same
    // active sector. Abandon its remaining slots and start the next sector.
    s_next_sector = (s_next_sector + 1U) % sector_count;
    s_next_record = 0;
  }
  return false;
}

static void finalize_sector_locked(uint32_t sector) {
  if (!read_sector(sector)) return;
  track_sector_header_t header = {
      .magic = TRACK_SECTOR_MAGIC,
      .sequence = s_next_sequence,
      .record_count = TRACK_RECORDS_PER_SECTOR,
      .reserved = 0,
      .records_crc32 = track_crc32(
          s_sector_buffer + sizeof(track_sector_header_t),
          TRACK_RECORDS_PER_SECTOR * sizeof(track_record_t)),
  };
  const size_t base = (size_t)sector * TRACK_SECTOR_SIZE;
  // Header magic is the final commit word. Records remain recoverable by their
  // own timestamp commit marker if power disappears during this header write.
  esp_err_t err = esp_partition_write(s_partition, base + sizeof(header.magic),
                                      (const uint8_t *)&header +
                                          sizeof(header.magic),
                                      sizeof(header) - sizeof(header.magic));
  if (err == ESP_OK) {
    err = esp_partition_write(s_partition, base, &header.magic,
                              sizeof(header.magic));
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Track sector header write failed: %s",
             esp_err_to_name(err));
  } else {
    ++s_next_sequence;
    if (s_next_sequence == 0) s_next_sequence = 1;
  }
}

bool track_store_init(void) {
#if !ALPHALOC_TRACK_STORE
  return false;
#else
  if (s_mutex) return s_status.available;
  s_mutex = xSemaphoreCreateMutex();
  s_sector_buffer = malloc(TRACK_SECTOR_SIZE);
  if (!s_mutex || !s_sector_buffer) {
    ESP_LOGE(TAG, "Unable to allocate track-store runtime state");
    return false;
  }
  s_partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
      TRACK_STORE_PARTITION_LABEL);
  if (!s_partition || s_partition->size != TRACK_STORE_PARTITION_BYTES ||
      s_partition->size % TRACK_SECTOR_SIZE != 0) {
    ESP_LOGE(TAG, "Missing or invalid %u-byte '%s' partition",
             (unsigned)TRACK_STORE_PARTITION_BYTES,
             TRACK_STORE_PARTITION_LABEL);
    return false;
  }
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  bool ok = scan_partition_locked();
  xSemaphoreGive(s_mutex);
  if (ok) {
    ESP_LOGI(TAG, "Track store ready: %u/%u bytes, %u records, interval=%us",
             (unsigned)s_status.used_bytes,
             (unsigned)s_status.capacity_bytes,
             (unsigned)s_status.record_count,
             (unsigned)TRACK_STORE_INTERVAL_S);
  }
  return ok;
#endif
}

bool track_store_append_fix(const gps_fix_t *fix) {
#if !ALPHALOC_TRACK_STORE
  (void)fix;
  return false;
#else
  track_record_t record;
  if (!track_record_from_fix(fix, &record) || !s_status.available || !s_mutex ||
      xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
    return false;
  }
  if (s_status.latest_timestamp_utc != 0 &&
      (record.timestamp_utc <= s_status.latest_timestamp_utc ||
       record.timestamp_utc - s_status.latest_timestamp_utc <
           TRACK_STORE_INTERVAL_S)) {
    xSemaphoreGive(s_mutex);
    return false;
  }
  if (!prepare_next_slot_locked()) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  const uint32_t sector = s_next_sector;
  const uint16_t slot = s_next_record;
  const size_t offset = record_offset(sector, slot);
  // Coordinates/altitude first; timestamp last is the record commit marker.
  esp_err_t err = esp_partition_write(
      s_partition, offset, &record, offsetof(track_record_t, timestamp_utc));
  if (err == ESP_OK) {
    err = esp_partition_write(s_partition,
                              offset + offsetof(track_record_t, timestamp_utc),
                              &record.timestamp_utc,
                              sizeof(record.timestamp_utc));
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Track record write failed: %s", esp_err_to_name(err));
    xSemaphoreGive(s_mutex);
    return false;
  }

  if (slot == 0) s_status.used_bytes += sizeof(track_sector_header_t);
  s_status.used_bytes += sizeof(track_record_t);
  ++s_status.record_count;
  if (s_status.earliest_timestamp_utc == 0 ||
      record.timestamp_utc < s_status.earliest_timestamp_utc) {
    s_status.earliest_timestamp_utc = record.timestamp_utc;
  }
  s_status.latest_timestamp_utc = record.timestamp_utc;

  ++s_next_record;
  if (s_next_record == TRACK_RECORDS_PER_SECTOR) {
    finalize_sector_locked(sector);
    s_next_sector = (sector + 1U) %
                    (s_partition->size / TRACK_SECTOR_SIZE);
    s_next_record = 0;
  }
  xSemaphoreGive(s_mutex);
  return true;
#endif
}

bool track_store_get_status(track_store_status_t *out_status) {
  if (!out_status) return false;
  memset(out_status, 0, sizeof(*out_status));
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false;
  }
  *out_status = s_status;
  xSemaphoreGive(s_mutex);
  return out_status->available;
}

void track_store_task(void *arg) {
  (void)arg;
  while (true) {
    gps_fix_t fix;
    if (gps_get_latest(&fix) && fix.valid && fix.time_valid) {
      (void)track_store_append_fix(&fix);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
