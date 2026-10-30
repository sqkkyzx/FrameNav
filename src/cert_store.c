/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cert_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "cert_store";
static const esp_partition_t *s_partition;

#define CERT_STORE_MAGIC 0x46544331U /* FTC1 */
#define CERT_STORE_VERSION 2U
#define CERT_STORE_SLOT_BYTES 0x8000U

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t generation;
  uint32_t cert_length;
  uint32_t key_length;
  uint32_t crc32;
  uint32_t header_crc32;
} cert_store_header_t;

typedef struct {
  uint32_t magic, version, length, crc32;
} legacy_header_t;

typedef struct {
  bool valid;
  unsigned slot;
  uint32_t generation, cert_length, key_length, crc32;
  size_t payload_offset;
} store_record_t;

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length) {
  while (length--) {
    crc ^= *data++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int)(crc & 1U));
  }
  return crc;
}

static void wipe_bytes(void *buffer, size_t length) {
  volatile uint8_t *bytes = buffer;
  while (length--) *bytes++ = 0;
}

static bool payload_crc(size_t offset, size_t length, uint32_t *out) {
  uint8_t block[256];
  uint32_t crc = 0xffffffffU;
  bool ok = true;
  while (length) {
    size_t count = length < sizeof(block) ? length : sizeof(block);
    if (esp_partition_read(s_partition, offset, block, count) != ESP_OK) {
      ok = false;
      break;
    }
    crc = crc_update(crc, block, count);
    offset += count;
    length -= count;
  }
  wipe_bytes(block, sizeof(block));
  *out = ~crc;
  return ok;
}

bool cert_store_init(void) {
  if (s_partition) return true;
  s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                         ESP_PARTITION_SUBTYPE_ANY,
                                         "tls_cert");
  if (!s_partition || s_partition->size < 2U * CERT_STORE_SLOT_BYTES) {
    ESP_LOGW(TAG, "TLS credential partition is unavailable");
    s_partition = NULL;
    return false;
  }
  return true;
}

/* Return false for I/O errors, distinct from a readable but invalid slot.
 * A writer must never erase a slot after failing to identify the active one. */
static bool read_slot(unsigned slot, store_record_t *record) {
  *record = (store_record_t){.slot = slot};
  size_t base = slot * CERT_STORE_SLOT_BYTES;
  cert_store_header_t header = {0};
  if (esp_partition_read(s_partition, base, &header, sizeof(header)) != ESP_OK)
    return false;
  if (header.magic != CERT_STORE_MAGIC) return true;
  if (slot == 0 && header.version == 1U) {
    legacy_header_t legacy;
    memcpy(&legacy, &header, sizeof(legacy));
    if (!legacy.length || legacy.length > FRAMENAV_CERT_MAX_BYTES) return true;
    record->cert_length = legacy.length;
    record->crc32 = legacy.crc32;
    record->payload_offset = base + sizeof(legacy);
  } else if (header.version == CERT_STORE_VERSION) {
    if (!header.cert_length || header.cert_length > FRAMENAV_CERT_MAX_BYTES ||
        !header.key_length || header.key_length > FRAMENAV_KEY_MAX_BYTES ||
        ~crc_update(0xffffffffU, (const uint8_t *)&header,
                      offsetof(cert_store_header_t, header_crc32)) !=
            header.header_crc32) return true;
    record->generation = header.generation;
    record->cert_length = header.cert_length;
    record->key_length = header.key_length;
    record->crc32 = header.crc32;
    record->payload_offset = base + sizeof(header);
  } else {
    return true;
  }
  uint32_t crc;
  if (!payload_crc(record->payload_offset,
                    record->cert_length + record->key_length, &crc)) return false;
  record->valid = crc == record->crc32;
  return true;
}

static bool newest_record(store_record_t *newest) {
  store_record_t slots[2];
  if (!read_slot(0, &slots[0]) || !read_slot(1, &slots[1])) return false;
  *newest = slots[0];
  uint32_t difference = slots[1].generation - slots[0].generation;
  if (slots[1].valid && (!slots[0].valid ||
                         (difference != 0 && difference < 0x80000000U))) {
    *newest = slots[1];
  }
  return true;
}

bool cert_store_read(uint8_t *cert, size_t cert_capacity, size_t *cert_length,
                     uint8_t *key, size_t key_capacity, size_t *key_length) {
  if (cert_length) *cert_length = 0;
  if (key_length) *key_length = 0;
  if (!cert || !key || !cert_length || !key_length || !cert_store_init())
    return false;
  store_record_t record;
  if (!newest_record(&record) || !record.valid ||
      cert_capacity <= record.cert_length || key_capacity <= record.key_length)
    return false;
  if (esp_partition_read(s_partition, record.payload_offset, cert,
                          record.cert_length) != ESP_OK ||
      (record.key_length &&
       esp_partition_read(s_partition, record.payload_offset + record.cert_length,
                            key, record.key_length) != ESP_OK)) {
    wipe_bytes(key, key_capacity);
    return false;
  }
  uint32_t crc = crc_update(0xffffffffU, cert, record.cert_length);
  crc = crc_update(crc, key, record.key_length);
  if (~crc != record.crc32) {
    wipe_bytes(key, key_capacity);
    return false;
  }
  cert[record.cert_length] = 0;
  key[record.key_length] = 0;
  *cert_length = record.cert_length;
  *key_length = record.key_length;
  return true;
}

bool cert_store_write(const uint8_t *cert, size_t cert_length,
                      const uint8_t *key, size_t key_length) {
  if (!cert || !key || !cert_length || !key_length ||
      cert_length > FRAMENAV_CERT_MAX_BYTES ||
      key_length > FRAMENAV_KEY_MAX_BYTES || !cert_store_init()) return false;
  store_record_t active;
  if (!newest_record(&active)) return false;
  unsigned slot = active.valid ? 1U - active.slot : 0U;
  size_t base = slot * CERT_STORE_SLOT_BYTES;
  uint32_t crc = crc_update(0xffffffffU, cert, cert_length);
  crc = crc_update(crc, key, key_length);
  cert_store_header_t header = {
      .magic = CERT_STORE_MAGIC,
      .version = CERT_STORE_VERSION,
      .generation = active.valid ? active.generation + 1U : 1U,
      .cert_length = (uint32_t)cert_length,
      .key_length = (uint32_t)key_length,
      .crc32 = ~crc,
  };
  header.header_crc32 = ~crc_update(0xffffffffU, (const uint8_t *)&header,
                                     offsetof(cert_store_header_t, header_crc32));
  size_t payload = base + sizeof(header);
  uint32_t stored_crc = 0;
  /* Keep the active slot intact. Commit the new header only after the entire
   * certificate/key pair has been written and read back successfully. */
  if (esp_partition_erase_range(s_partition, base, CERT_STORE_SLOT_BYTES) != ESP_OK ||
      esp_partition_write(s_partition, payload, cert, cert_length) != ESP_OK ||
      esp_partition_write(s_partition, payload + cert_length, key, key_length) != ESP_OK ||
      !payload_crc(payload, cert_length + key_length, &stored_crc) ||
      stored_crc != header.crc32 ||
      esp_partition_write(s_partition, base, &header, sizeof(header)) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to persist TLS credentials; prior slot retained");
    return false;
  }
  cert_store_header_t committed;
  return esp_partition_read(s_partition, base, &committed, sizeof(committed)) == ESP_OK &&
         memcmp(&committed, &header, sizeof(header)) == 0;
}
