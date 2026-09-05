/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "location_observer.h"

#if ALPHALOC_LOCATION_OBSERVER
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"

static const char *TAG = "loc_observer";

#define OBS_MAX_CHARS 12
#define OBS_MAX_BYTES 128
#define OBS_READ_PERIOD_US 30000000LL

typedef struct {
  uint16_t uuid_short;
  uint16_t definition;
  uint16_t value;
  uint16_t end;
  uint16_t cccd;
  uint8_t properties;
  uint8_t subscribe_attempts;
  bool descriptors_done;
  bool subscribed;
  bool read_disabled;
  int64_t last_read_us;
} observed_chr_t;

static observed_chr_t s_chars[OBS_MAX_CHARS];
static size_t s_count;
static size_t s_active;
static size_t s_read_cursor;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uintptr_t s_generation;
static bool s_pending;
static uint32_t s_acks;
static uint32_t s_failures;
static int s_link_state = -1;
static int64_t s_link_event_us;
static int64_t s_last_summary_us;

int location_observer_decode_link_state(const uint8_t *data, size_t len) {
  // Public reverse engineering documents linkage on/off, NOT valid/invalid GPS:
  // github.com/whc2001/ILCE7M3ExternalGps/blob/master/PROTOCOL_EN.md
  if (!data || len != 4 || data[0] != 0x03 || data[1] != 0x01 ||
      data[2] != 0x02 || data[3] > 1) {
    return -1;
  }
  return data[3];
}

static bool observer_current(uint16_t conn, void *arg) {
  return conn != BLE_HS_CONN_HANDLE_NONE && conn == s_conn &&
         (uintptr_t)arg == s_generation;
}

void location_observer_reset(uint16_t conn_handle) {
  ++s_generation;
  if (s_generation == 0) {
    ++s_generation;
  }
  s_conn = conn_handle;
  memset(s_chars, 0, sizeof(s_chars));
  s_count = s_active = s_read_cursor = 0;
  s_pending = false;
  s_acks = s_failures = 0;
  s_link_state = -1;
  s_link_event_us = s_last_summary_us = 0;
}

void location_observer_discovered(uint16_t conn_handle,
                                  const struct ble_gatt_chr *chr,
                                  uint16_t service_end) {
  if (!chr || conn_handle != s_conn) {
    return;
  }
  // Caller restricts this to the discovered Sony DD00 service range.
  uint16_t short_uuid = 0;
  if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
    short_uuid = chr->uuid.u16.value;
  } else if (chr->uuid.u.type == BLE_UUID_TYPE_128) {
    short_uuid = chr->uuid.u128.value[12] | (chr->uuid.u128.value[13] << 8);
  }
  char uuid[BLE_UUID_STR_LEN];
  ble_uuid_to_str(&chr->uuid.u, uuid);
  ESP_LOGI(TAG, "Location characteristic uuid=%s def=%u value=%u props=0x%02X "
                "read=%d notify=%d indicate=%d", uuid, chr->def_handle,
           chr->val_handle, chr->properties,
           !!(chr->properties & BLE_GATT_CHR_PROP_READ),
           !!(chr->properties & BLE_GATT_CHR_PROP_NOTIFY),
           !!(chr->properties & BLE_GATT_CHR_PROP_INDICATE));
  for (size_t i = 0; i < s_count; ++i) {
    if (s_chars[i].value == chr->val_handle) {
      return;
    }
    if (chr->def_handle > s_chars[i].value &&
        chr->def_handle - 1 < s_chars[i].end) {
      s_chars[i].end = chr->def_handle - 1;
    }
  }
  if (s_count == OBS_MAX_CHARS) {
    ESP_LOGW(TAG, "Observer characteristic capacity reached; extra entry skipped");
    return;
  }
  observed_chr_t *entry = &s_chars[s_count++];
  *entry = (observed_chr_t){.uuid_short = short_uuid,
                          .definition = chr->def_handle,
                          .value = chr->val_handle,
                          .end = service_end,
                          .properties = chr->properties};
  for (size_t i = 0; i + 1 < s_count; ++i) {
    if (s_chars[i].definition > entry->value &&
        s_chars[i].definition - 1 < entry->end) {
      entry->end = s_chars[i].definition - 1;
    }
  }
}

static void observer_log_value(const char *source, observed_chr_t *entry,
                                struct os_mbuf *om) {
  if (!om) {
    return;
  }
  uint16_t total = OS_MBUF_PKTLEN(om);
  uint16_t copied = total > OBS_MAX_BYTES ? OBS_MAX_BYTES : total;
  uint8_t data[OBS_MAX_BYTES];
  if (os_mbuf_copydata(om, 0, copied, data) != 0) {
    ESP_LOGW(TAG, "Observer mbuf copy failed");
    return;
  }
  ESP_LOGI(TAG, "%s UUID=%04X handle=%u len=%u%s", source, entry->uuid_short,
           entry->value, total, total > copied ? " (truncated)" : "");
  if (copied) {
    ESP_LOG_BUFFER_HEX(TAG, data, copied);
  }
  if (entry->uuid_short == 0xDD01) {
    int state = location_observer_decode_link_state(data, total);
    if (state >= 0) {
      s_link_state = state;
      s_link_event_us = esp_timer_get_time();
      ESP_LOGI(TAG, "DD01 camera-side linkage=%s (reverse-engineered meaning); "
                    "camera_gps_valid=UNKNOWN; not EXIF confirmation",
               state ? "ENABLED" : "DISABLED");
    } else {
      ESP_LOGI(TAG, "DD01 payload not decoded; camera_gps_valid=UNKNOWN");
    }
  }
}

static int observer_descriptor_cb(uint16_t conn,
                                   const struct ble_gatt_error *error,
                                   uint16_t chr_val_handle,
                                   const struct ble_gatt_dsc *dsc, void *arg) {
  if (!observer_current(conn, arg)) {
    return 0;
  }
  observed_chr_t *entry = &s_chars[s_active];
  if (error->status == 0 && dsc) {
    if (chr_val_handle == entry->value &&
        dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
        dsc->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16) {
      entry->cccd = dsc->handle;
      ESP_LOGI(TAG, "UUID=%04X CCCD discovered handle=%u", entry->uuid_short,
               entry->cccd);
    }
    return 0;
  }
  s_pending = false;
  entry->descriptors_done = true;
  if (error->status != BLE_HS_EDONE) {
    ESP_LOGW(TAG, "Descriptor probe UUID=%04X status=%d; no guessed CCCD write",
             entry->uuid_short, error->status);
    entry->cccd = 0;
  }
  if (!entry->cccd) {
    ESP_LOGW(TAG, "UUID=%04X has no discovered CCCD; notification not subscribed",
             entry->uuid_short);
  }
  return 0;
}

static int observer_subscribe_cb(uint16_t conn,
                                  const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg) {
  (void)attr;
  if (!observer_current(conn, arg)) {
    return 0;
  }
  s_pending = false;
  observed_chr_t *entry = &s_chars[s_active];
  entry->subscribed = error->status == 0;
  ESP_LOGI(TAG, "UUID=%04X notification subscription %s status=%d",
           entry->uuid_short, entry->subscribed ? "ACKNOWLEDGED" : "FAILED",
           error->status);
  return 0;
}

static int observer_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
  if (!observer_current(conn, arg)) {
    return 0;
  }
  s_pending = false;
  observed_chr_t *entry = &s_chars[s_active];
  if (error->status != 0 || !attr || !attr->om) {
    ESP_LOGW(TAG, "Read-only probe UUID=%04X failed status=%d; skip until reconnect",
             entry->uuid_short, error->status);
    entry->read_disabled = true;
    return 0;
  }
  observer_log_value("READ", entry, attr->om);
  return 0;
}

void location_observer_after_write(uint16_t conn_handle, int status) {
  if (conn_handle != s_conn || conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }
  if (status) {
    ++s_failures;
    return;
  }
  ++s_acks;
  int64_t now = esp_timer_get_time();
  if (s_acks == 1 || now - s_last_summary_us >= OBS_READ_PERIOD_US) {
    s_last_summary_us = now;
    ESP_LOGI(TAG, "Evidence: DD11_ack=%lu failed=%lu last_DD01_linkage=%s "
                  "linkage_age_ms=%lld camera_gps_valid=UNKNOWN",
             (unsigned long)s_acks, (unsigned long)s_failures,
             s_link_state < 0 ? "UNKNOWN" : s_link_state ? "ENABLED" : "DISABLED",
             s_link_state < 0 ? -1LL : (long long)((now - s_link_event_us) / 1000));
  }
  // At most one observational operation per successful location write, with
  // at most one probe in flight. Never use a guessed handle or write DD11/30/31.
  if (s_pending) {
    return;
  }
  for (size_t i = 0; i < s_count; ++i) {
    observed_chr_t *entry = &s_chars[i];
    if (!(entry->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
      continue;
    }
    int rc;
    s_active = i;
    if (!entry->descriptors_done) {
      entry->descriptors_done = true;
      if (entry->end <= entry->value) {
        continue;
      }
      s_pending = true;
      rc = ble_gattc_disc_all_dscs(conn_handle, entry->value, entry->end,
                                   observer_descriptor_cb, (void *)s_generation);
    } else if (entry->cccd && !entry->subscribed && entry->subscribe_attempts < 2) {
      ++entry->subscribe_attempts;
      uint8_t enable[2] = {
          entry->properties & BLE_GATT_CHR_PROP_NOTIFY ? 0x01 : 0x02, 0x00};
      s_pending = true;
      rc = ble_gattc_write_flat(conn_handle, entry->cccd, enable, sizeof(enable),
                                 observer_subscribe_cb, (void *)s_generation);
    } else {
      continue;
    }
    if (rc) {
      s_pending = false;
      ESP_LOGW(TAG, "Notification probe start UUID=%04X failed rc=%d",
               entry->uuid_short, rc);
    }
    return;
  }
  for (size_t n = 0; n < s_count; ++n) {
    size_t i = (s_read_cursor + n) % s_count;
    observed_chr_t *entry = &s_chars[i];
    if (!(entry->properties & BLE_GATT_CHR_PROP_READ) || entry->read_disabled ||
        (entry->last_read_us && now - entry->last_read_us < OBS_READ_PERIOD_US)) {
      continue;
    }
    s_active = i;
    s_read_cursor = (i + 1) % s_count;
    entry->last_read_us = now;
    s_pending = true;
    int rc = ble_gattc_read(conn_handle, entry->value, observer_read_cb,
                             (void *)s_generation);
    if (rc) {
      s_pending = false;
      ESP_LOGW(TAG, "Read-only probe start UUID=%04X failed rc=%d", entry->uuid_short, rc);
    }
    return;
  }
}

void location_observer_notify(uint16_t conn_handle, uint16_t attr_handle,
                              struct os_mbuf *om, bool indication) {
  if (conn_handle != s_conn) {
    return;
  }
  for (size_t i = 0; i < s_count; ++i) {
    if (s_chars[i].value == attr_handle) {
      observer_log_value(indication ? "INDICATE" : "NOTIFY", &s_chars[i], om);
      return;
    }
  }
}
#endif
