/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "power_source_probe.h"

#ifndef ALPHALOC_POWER_SOURCE_PROBE
#define ALPHALOC_POWER_SOURCE_PROBE 0
#endif

#if ALPHALOC_POWER_SOURCE_PROBE

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "ble_client.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "soc/usb_serial_jtag_struct.h"

#define PROBE_NAMESPACE "src_probe"
#define PROBE_LEGACY_KEY "last"
#define PROBE_HISTORY_SLOTS 8U
#define PROBE_MAGIC 0x53524350U
#define PROBE_VERSION 1U
#define PROBE_DURATION_MS 20000U
#define PROBE_SAMPLE_MS 50U

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t sequence;
  uint32_t duration_ms;
  uint32_t line_count[4];
  uint32_t first_camera_ms;
  uint8_t usb_host_seen;
  uint8_t camera_seen;
  uint8_t final_dp;
  uint8_t final_dm;
} power_source_record_t;

static const char *TAG = "source_probe";

static bool load_record(const char *key, power_source_record_t *record) {
  nvs_handle_t nvs;
  if (nvs_open(PROBE_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
    return false;
  }
  size_t size = sizeof(*record);
  esp_err_t err = nvs_get_blob(nvs, key, record, &size);
  nvs_close(nvs);
  return err == ESP_OK && size == sizeof(*record) &&
         record->magic == PROBE_MAGIC && record->version == PROBE_VERSION &&
         record->size == sizeof(*record);
}

static void log_record(const char *label, const power_source_record_t *record) {
  ESP_LOGW(TAG,
           "%s seq=%lu duration=%lums SOF-host=%s camera=%s first-camera=%lums "
           "D+/D- counts 00=%lu 01=%lu 10=%lu 11=%lu final=%u%u",
           label, (unsigned long)record->sequence,
           (unsigned long)record->duration_ms,
           record->usb_host_seen ? "yes" : "no",
           record->camera_seen ? "yes" : "no",
           (unsigned long)record->first_camera_ms,
           (unsigned long)record->line_count[0],
           (unsigned long)record->line_count[1],
           (unsigned long)record->line_count[2],
           (unsigned long)record->line_count[3], record->final_dp,
           record->final_dm);
}

static void save_record(const power_source_record_t *record) {
  nvs_handle_t nvs = 0;
  char key[8];
  snprintf(key, sizeof(key), "s%u",
           (unsigned)(record->sequence % PROBE_HISTORY_SLOTS));
  esp_err_t err = nvs_open(PROBE_NAMESPACE, NVS_READWRITE, &nvs);
  if (err == ESP_OK) {
    err = nvs_set_blob(nvs, key, record, sizeof(*record));
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  if (nvs) {
    nvs_close(nvs);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save source probe: %s", esp_err_to_name(err));
  }
}

static void probe_task(void *arg) {
  power_source_record_t record = {
      .magic = PROBE_MAGIC,
      .version = PROBE_VERSION,
      .size = sizeof(power_source_record_t),
      .sequence = (uint32_t)(uintptr_t)arg,
      .duration_ms = PROBE_DURATION_MS,
      .first_camera_ms = UINT32_MAX,
  };
  int64_t start_us = esp_timer_get_time();
  const int64_t end_us = start_us + (int64_t)PROBE_DURATION_MS * 1000LL;

  while (esp_timer_get_time() < end_us) {
    if (usb_serial_jtag_is_connected()) {
      record.usb_host_seen = 1;
    }
    if (ble_client_is_connected() && !record.camera_seen) {
      record.camera_seen = 1;
      record.first_camera_ms =
          (uint32_t)((esp_timer_get_time() - start_us) / 1000LL);
    }

    // Read-only PHY debug inputs. Do not enable test mode or change pull-ups.
    uint32_t test = USB_SERIAL_JTAG.test.val;
    uint8_t dp = (uint8_t)((test >> 5) & 1U);
    uint8_t dm = (uint8_t)((test >> 6) & 1U);
    record.line_count[(dp << 1) | dm]++;
    record.final_dp = dp;
    record.final_dm = dm;
    vTaskDelay(pdMS_TO_TICKS(PROBE_SAMPLE_MS));
  }

  save_record(&record);
  log_record("CURRENT", &record);
  ESP_LOGW(TAG, "Probe saved; keep this power source connected for at least "
                "20s before moving back to the Mac");
  vTaskDelete(NULL);
}

void power_source_probe_start(void) {
  power_source_record_t legacy = {0};
  uint32_t latest_sequence = 0;
  bool found = false;

  // Keep compatibility with the first one-record diagnostic build so the
  // sequence remains monotonic after upgrading to the history build.
  if (load_record(PROBE_LEGACY_KEY, &legacy)) {
    latest_sequence = legacy.sequence;
    log_record("LEGACY", &legacy);
    found = true;
  }

  for (unsigned i = 0; i < PROBE_HISTORY_SLOTS; ++i) {
    char key[8];
    power_source_record_t record = {0};
    snprintf(key, sizeof(key), "s%u", i);
    if (load_record(key, &record) && record.sequence > latest_sequence) {
      latest_sequence = record.sequence;
      found = true;
    }
  }

  if (latest_sequence > 0) {
    uint32_t first = latest_sequence >= (PROBE_HISTORY_SLOTS - 1U)
                         ? latest_sequence - (PROBE_HISTORY_SLOTS - 1U)
                         : 1U;
    for (uint32_t sequence = first; sequence <= latest_sequence; ++sequence) {
      char key[8];
      power_source_record_t record = {0};
      snprintf(key, sizeof(key), "s%u",
               (unsigned)(sequence % PROBE_HISTORY_SLOTS));
      if (load_record(key, &record) && record.sequence == sequence) {
        log_record("HISTORY", &record);
      }
    }
  }

  if (!found) {
    ESP_LOGW(TAG, "No previous source probe record");
  }

  BaseType_t result = xTaskCreate(probe_task, "source_probe", 3072,
                                  (void *)(uintptr_t)(latest_sequence + 1U), 2,
                                  NULL);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create source probe task");
  } else {
    ESP_LOGW(TAG, "20s source probe started (diagnostic only; no WiFi policy "
                  "changes)");
  }
}

#endif
