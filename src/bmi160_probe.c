/* Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "bmi160_probe.h"
#if FRAMENAV_BMI160_PROBE
#include <math.h>
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bmi160_probe";
static uint8_t address;
static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t n) {
  return i2c_master_write_read_device(I2C_NUM_0, address, &reg, 1, data, n,
                                      pdMS_TO_TICKS(100));
}
static esp_err_t write_reg(uint8_t reg, uint8_t value) {
  uint8_t data[] = {reg, value};
  esp_err_t err = i2c_master_write_to_device(I2C_NUM_0, address, data, 2,
                                           pdMS_TO_TICKS(100));
  // Suspend-mode register writes need at least 450 us between accesses.
  vTaskDelay(pdMS_TO_TICKS(10));
  return err;
}
static void probe_task(void *arg) {
  (void)arg;
  bool found = false;
  vTaskDelay(pdMS_TO_TICKS(100));
  for (unsigned a = 0x68; a <= 0x69; ++a) {
    address = a;
    uint8_t id = 0;
    esp_err_t err = read_reg(0, &id, 1);
    ESP_LOGI(TAG, "address=0x%02x result=%s CHIP_ID=0x%02x (expected D1)",
             a, esp_err_to_name(err), id);
    if (err == ESP_OK && id == 0xd1) { found = true; break; }
  }
  if (!found) goto cleanup;
  // Motion experiment: 100 Hz, +/-4g; no GNSS policy changes.
  if (write_reg(0x7e, 0xb6) != ESP_OK) goto suspend;
  vTaskDelay(pdMS_TO_TICKS(100));
  if (write_reg(0x40, 0x28) != ESP_OK ||
      write_reg(0x41, 0x05) != ESP_OK) goto suspend;
  uint8_t pmu = 0, cfg[2] = {0}, error = 0;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (write_reg(0x7e, 0x11) != ESP_OK) goto suspend;
    vTaskDelay(pdMS_TO_TICKS(500));
    if (read_reg(3, &pmu, 1) != ESP_OK) goto suspend;
    ESP_LOGI(TAG, "Wake attempt %u PMU=%02x", attempt + 1, pmu);
    if ((pmu & 0x30) == 0x10) break;
  }
  if (read_reg(3, &pmu, 1) != ESP_OK ||
      read_reg(0x40, cfg, 2) != ESP_OK ||
      read_reg(2, &error, 1) != ESP_OK ||
      (pmu & 0x3c) != 0x10 || cfg[0] != 0x28 || cfg[1] != 5 || error) {
    ESP_LOGE(TAG, "Config check failed pmu=%02x config=%02x/%02x error=%02x",
             pmu, cfg[0], cfg[1], error);
    goto suspend;
  }
  gpio_config_t input = {.pin_bit_mask = 1ULL << GPIO_NUM_0,
      .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE};
  if (gpio_config(&input) != ESP_OK) goto suspend;
  // No interrupt sources yet: active-low output idles high, active-high idles low.
  if (write_reg(0x53, 0x08) != ESP_OK) goto suspend;
  int high = gpio_get_level(GPIO_NUM_0);
  if (write_reg(0x53, 0x0a) != ESP_OK) goto suspend;
  int low = gpio_get_level(GPIO_NUM_0);
  ESP_LOGI(TAG, "INT1 wire test D0: driven-high=%d driven-low=%d %s",
           high, low, high == 1 && low == 0 ? "PASS" : "FAIL");
  if (high != 1 || low != 0) goto suspend;
  // XYZ any-motion, 3 consecutive samples, threshold ~0.125g at +/-4g.
  if (write_reg(0x5f, 0x02) != ESP_OK || write_reg(0x60, 0x10) != ESP_OK ||
      write_reg(0x55, 0x04) != ESP_OK || write_reg(0x54, 0x0f) != ESP_OK ||
      write_reg(0x50, 0x07) != ESP_OK) goto suspend;
  ESP_LOGI(TAG, "READY: INT1 -> D0 motion test, latched active high; no deep sleep");
  const int64_t end_us = esp_timer_get_time() + 180000000LL;
  unsigned events = 0;
  for (unsigned i = 0; esp_timer_get_time() < end_us; ++i) {
    uint8_t raw[6], status;
    if (gpio_get_level(GPIO_NUM_0)) {
      uint8_t motion = 0;
      esp_err_t err = read_reg(0x1c, &motion, 1);
      ESP_LOGI(TAG, "INT1 event=%u D0=HIGH status=%02x read=%s any_motion=%d",
               ++events, motion, esp_err_to_name(err), !!(motion & 0x04));
      if (write_reg(0x7e, 0xb1) != ESP_OK) break;
    }
    if (read_reg(0x1b, &status, 1) != ESP_OK) break;
    if ((status & 0x80) && read_reg(0x12, raw, sizeof(raw)) == ESP_OK) {
      float x = (int16_t)(raw[0] | raw[1] << 8) / 8192.0f;
      float y = (int16_t)(raw[2] | raw[3] << 8) / 8192.0f;
      float z = (int16_t)(raw[4] | raw[5] << 8) / 8192.0f;
      if (i % 100 == 0) ESP_LOGI(TAG, "sample=%u x=%.3fg y=%.3fg z=%.3fg norm=%.3fg",
               i, x, y, z, sqrtf(x*x+y*y+z*z));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
suspend:
  (void)write_reg(0x50, 0);
  (void)write_reg(0x53, 0);
  if (write_reg(0x7e, 0x10) != ESP_OK)
    ESP_LOGE(TAG, "Failed to suspend accelerometer");
  else ESP_LOGI(TAG, "Test ended; accelerometer suspend command sent");
cleanup:
  i2c_driver_delete(I2C_NUM_0);
  vTaskDelete(NULL);
}
void bmi160_probe_start(void) {
  i2c_config_t cfg = {.mode = I2C_MODE_MASTER, .sda_io_num = 20,
    .scl_io_num = 18, .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE, .master.clk_speed = 100000};
  esp_err_t err = i2c_param_config(I2C_NUM_0, &cfg);
  if (err == ESP_OK) err = i2c_driver_install(I2C_NUM_0, cfg.mode, 0, 0, 0);
  if (err != ESP_OK) { ESP_LOGE(TAG, "I2C init: %s", esp_err_to_name(err)); return; }
  if (xTaskCreate(probe_task, "bmi160_probe", 3072, NULL, 3, NULL) != pdPASS) {
    i2c_driver_delete(I2C_NUM_0);
    ESP_LOGE(TAG, "Task allocation failed");
  }
}
#else
void bmi160_probe_start(void) {}
#endif
