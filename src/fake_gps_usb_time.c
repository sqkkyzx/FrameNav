/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_gps_usb_time.h"

#if ALPHALOC_FAKE_GPS && ALPHALOC_FAKE_GPS_USB_TIME
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_time";

bool gps_fake_parse_usb_time(const char *line, uint64_t expected_nonce,
                              int64_t *epoch_ms) {
  static const char prefix[] = "ALPHALOC_TIME_SET_V1 ";
  const size_t start = sizeof(prefix) - 1;
  if (!line || !epoch_ms || strlen(line) != start + 16 + 1 + 13 ||
      memcmp(line, prefix, start) != 0 || line[start + 16] != ' ') {
    return false;
  }
  uint64_t nonce = 0;
  for (size_t i = 0; i < 16; ++i) {
    char c = line[start + i];
    int digit = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) {
      return false;
    }
    nonce = (nonce << 4) | (unsigned)digit;
  }
  if (!expected_nonce || nonce != expected_nonce) {
    return false;
  }
  int64_t value = 0;
  for (size_t i = start + 17; i < start + 30; ++i) {
    if (line[i] < '0' || line[i] > '9') {
      return false;
    }
    value = value * 10 + line[i] - '0';
  }
  if (value < ALPHALOC_USB_TIME_MIN_MS || value >= ALPHALOC_USB_TIME_MAX_MS) {
    return false;
  }
  *epoch_ms = value;
  return true;
}

static void usb_time_task(void *arg) {
  (void)arg;
  char line[80];
  size_t used = 0;
  bool discard_line = false;
  uint64_t nonce = 0;
  int64_t issued_us = 0;
  bool synced = false;
  while (true) {
    int64_t now = esp_timer_get_time();
    if (!synced && (!nonce || now - issued_us >= 3000000)) {
      nonce = ((uint64_t)esp_random() << 32) | esp_random();
      if (!nonce) {
        nonce = 1;
      }
      issued_us = now;
      ESP_LOGI(TAG, "ALPHALOC_TIME_REQUEST_V1 %016" PRIx64
                    " (WAIT_HOST_TIME; location publishing blocked)", nonce);
    }
    // VFS is backed by the installed driver's RX ring buffer. IDF 5.5.3's
    // polling backend + O_NONBLOCK otherwise reports zero available bytes.
    char rx[64];
    ssize_t count = read(STDIN_FILENO, rx, sizeof(rx));
    for (ssize_t i = 0; i < count; ++i) {
      unsigned char c = (unsigned char)rx[i];
      if (c == '\n') {
        line[used] = '\0';
        int64_t epoch_ms;
        now = esp_timer_get_time();
        if (!discard_line && !synced &&
            gps_fake_parse_usb_time(line, nonce, &epoch_ms) &&
            now >= issued_us && now - issued_us <= 2500000 &&
            gps_fake_set_usb_time(epoch_ms, now)) {
          synced = true;
          ESP_LOGI(TAG, "ALPHALOC_TIME_SYNCED_V1 %016" PRIx64
                        " epoch_ms=%" PRId64 " uptime_us=%" PRId64
                        " source=USB_HOST; location publishing allowed",
                   nonce, epoch_ms, now);
        } else if (!synced && used) {
          ESP_LOGW(TAG, "USB time command rejected (malformed, stale or wrong challenge)");
        }
        used = 0;
        discard_line = false;
      } else if (c == '\r') {
        continue;
      } else if (c < 0x20 || c > 0x7e || used >= sizeof(line) - 1) {
        discard_line = true;
      } else if (!discard_line) {
        line[used++] = (char)c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool gps_fake_usb_time_start(void) {
  // The default console is polling-only; it has no installed RX driver.
  // Switch once, before BLE/tasks start. Do not install a duplicate driver.
  if (!usb_serial_jtag_is_driver_installed()) {
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 256,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Cannot install USB console RX driver: %s; publishing blocked",
               esp_err_to_name(err));
      return false;
    }
  }
  fflush(stdout);
  usb_serial_jtag_vfs_use_driver();
  int flags = fcntl(STDIN_FILENO, F_GETFL);
  if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
    ESP_LOGE(TAG, "Cannot enable USB console input; location publishing stays blocked");
    return false;
  }
  if (xTaskCreate(usb_time_task, "usb_time", 4096, NULL, 4, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Cannot start USB time task; location publishing stays blocked");
    return false;
  }
  return true;
}
#endif
