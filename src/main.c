/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "ble_client.h"
#include "location_service.h"
#ifndef ALPHALOC_BLE_LOCATION
#define ALPHALOC_BLE_LOCATION 0
#endif
#if ALPHALOC_BLE_LOCATION
#include "ble_location_server.h"
#endif
#if ALPHALOC_BLE_CONFIG
#include "ble_config_server.h"
#endif
#include "config.h"
#include "driver/gpio.h"
#if ALPHALOC_BOARD_XIAO_ESP32C6
#include "driver/ledc.h"
#include "driver/usb_serial_jtag.h"
#include "soc/usb_serial_jtag_struct.h"
#endif
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps.h"
#include "gnss_rate.h"
#include "power_policy.h"
#include "track_store.h"
#include "test_debug.h"
#if ALPHALOC_POWER_SOURCE_PROBE
#include "power_source_probe.h"
#endif
#if ALPHALOC_WIFI_WEB && ALPHALOC_BOARD_XIAO_ESP32C6
#include "usb_config_gate.h"
#endif
#if ALPHALOC_FAKE_GPS_USB_TIME
#include "fake_gps_usb_time.h"
#endif
#include "nvs_flash.h"

#ifndef ALPHALOC_BATTERY_MONITOR
#define ALPHALOC_BATTERY_MONITOR 0
#endif

#if ALPHALOC_BATTERY_MONITOR
#include "battery.h"
#endif

#ifdef ALPHALOC_NEOPIXEL_PIN
#include "neopixel.h"
#endif

#ifndef ALPHALOC_WIFI_WEB
#define ALPHALOC_WIFI_WEB 1
#endif

#if ALPHALOC_WIFI_WEB
#include "wifi_web.h"
#endif

#ifndef ALPHALOC_FAKE_GPS
#define ALPHALOC_FAKE_GPS 0
#endif

#if ALPHALOC_PHASE2 && (ALPHALOC_FAKE_GPS || ALPHALOC_FAKE_GPS_STEPPING || ALPHALOC_FAKE_GPS_USB_TIME)
#error "Phase 2 must not contain simulated GPS or USB host time"
#endif

#if ALPHALOC_POWER_SAVE && (!ALPHALOC_BOARD_XIAO_ESP32C6 || !ALPHALOC_PHASE2 || !CONFIG_PM_ENABLE || !CONFIG_BT_LE_SLEEP_ENABLE)
#error "Power profile requires XIAO Phase 2, PM enabled and BLE modem sleep"
#endif
#if ALPHALOC_POWER_SAVE && ALPHALOC_LIGHT_SLEEP
#error "Power profile must preserve continuous UART RX and USB; no CPU light sleep"
#endif

#if !ALPHALOC_FAKE_GPS
#define GPS_UART_NUM UART_NUM_1
#ifndef GPS_UART_TX_PIN
#error "GPS_UART_TX_PIN must be set via build_flags"
#endif
#ifndef GPS_UART_RX_PIN
#error "GPS_UART_RX_PIN must be set via build_flags"
#endif
#ifndef GPS_UART_BAUD
#define GPS_UART_BAUD 9600
#endif
#endif

#ifndef ALPHALOC_LIGHT_SLEEP
#define ALPHALOC_LIGHT_SLEEP 0
#endif

#ifndef ALPHALOC_USB_HOST_WIFI
#define ALPHALOC_USB_HOST_WIFI 0
#endif

#ifndef ALPHALOC_POWER_SOURCE_PROBE
#define ALPHALOC_POWER_SOURCE_PROBE 0
#endif

#if ALPHALOC_USB_HOST_WIFI && \
    (!ALPHALOC_WIFI_WEB || !ALPHALOC_BOARD_XIAO_ESP32C6)
#error "USB-host-gated WiFi requires XIAO ESP32-C6 and ALPHALOC_WIFI_WEB"
#endif

#define FAKE_LAT_DEG 48.137154
#define FAKE_LON_DEG 11.576124
#define FAKE_YEAR 2024
#define FAKE_MONTH 1
#define FAKE_DAY 1
#define FAKE_HOUR 12
#define FAKE_MINUTE 0
#define FAKE_SECOND 0

static const char *TAG = "main";
static app_config_t s_cfg;
#if !ALPHALOC_USB_HOST_WIFI
static int64_t s_config_window_end_time_us = 0;
#endif

static bool get_location_for_send(gps_fix_t *out_fix) {
#if ALPHALOC_PHASE2
  return location_service_get(out_fix);
#else
  gps_fix_t fix;
  if (gps_get_latest(&fix) && fix.valid) {
#if ALPHALOC_PHASE2
    // A coordinate without valid GNSS UTC must not change the camera clock.
    if (!fix.time_valid || fix.year < 2000 || fix.year > 2099 ||
        fix.month < 1 || fix.month > 12 || fix.day < 1 || fix.day > 31) {
      return false;
    }
#endif
    *out_fix = fix;
    return true;
  }
#if ALPHALOC_FAKE_GPS
  memset(&fix, 0, sizeof(fix));
  fix.lat_deg = FAKE_LAT_DEG;
  fix.lon_deg = FAKE_LON_DEG;
  fix.valid = true;
  fix.time_valid = true;
  fix.year = FAKE_YEAR;
  fix.month = FAKE_MONTH;
  fix.day = FAKE_DAY;
  fix.hour = FAKE_HOUR;
  fix.minute = FAKE_MINUTE;
  fix.second = FAKE_SECOND;
  fix.last_fix_time_us = esp_timer_get_time();
  fix.last_update_time_us = fix.last_fix_time_us;
#if ALPHALOC_FAKE_GPS_STEPPING
  // Keep the fallback path on the same clock; never fall back to frozen UTC.
  if (!gps_fake_apply_clock(&fix, fix.last_fix_time_us)) {
    return false;
  }
#endif
  *out_fix = fix;
  return true;
#else
  return false;
#endif
#endif
}

static void focus_update_cb(void *ctx) {
  app_config_t *cfg = (app_config_t *)ctx;
  app_config_t cfg_snapshot;
  if (!config_get_snapshot(cfg, &cfg_snapshot)) {
    cfg_snapshot = *cfg;
    config_validate(&cfg_snapshot);
  }
  gps_fix_t fix;
  if (!get_location_for_send(&fix)) {
    return;
  }
#if !ALPHALOC_PHASE2
  int64_t now = esp_timer_get_time();
  if ((now - fix.last_fix_time_us) >
      (int64_t)cfg_snapshot.max_gps_age_s * 1000000LL) {
    return;
  }
#endif
  ble_client_send_location(&fix);
}

static void location_publisher_task(void *arg) {
  const app_config_t *cfg = (const app_config_t *)arg;
  while (true) {
    app_config_t cfg_snapshot;
    if (!config_get_snapshot(cfg, &cfg_snapshot)) {
      cfg_snapshot = *cfg;
      config_validate(&cfg_snapshot);
    }
    gps_fix_t fix;
    if (get_location_for_send(&fix)) {
#if ALPHALOC_PHASE2
      ble_client_send_location(&fix);
#else
      int64_t now = esp_timer_get_time();
      if ((now - fix.last_fix_time_us) <=
          (int64_t)cfg_snapshot.max_gps_age_s * 1000000LL) {
        ble_client_send_location(&fix);
      }
#endif
    }
    vTaskDelay(pdMS_TO_TICKS(cfg_snapshot.gps_interval_ms));
  }
}

#if !ALPHALOC_USB_HOST_WIFI
static void config_window_task(void *arg) {
  app_config_t *cfg = (app_config_t *)arg;
  app_config_t cfg_snapshot;
  if (!config_get_snapshot(cfg, &cfg_snapshot)) {
    cfg_snapshot = *cfg;
    config_validate(&cfg_snapshot);
  }
#if ALPHALOC_BLE_CONFIG
  ble_config_server_start();
#endif
#if ALPHALOC_BLE_LOCATION
  ble_location_server_start();
#endif
#if ALPHALOC_WIFI_WEB
  (void)wifi_web_start(cfg);
#endif
  s_config_window_end_time_us =
      esp_timer_get_time() + ((int64_t)cfg_snapshot.config_window_s * 1000000LL);
  vTaskDelay(pdMS_TO_TICKS(cfg_snapshot.config_window_s * 1000ULL));
#if ALPHALOC_BLE_CONFIG
  ble_config_server_stop();
#endif
#if ALPHALOC_BLE_LOCATION
  ble_location_server_stop();
#endif
#if ALPHALOC_WIFI_WEB
  wifi_web_stop();
#endif
  vTaskDelete(NULL);
}
#endif

#if ALPHALOC_USB_HOST_WIFI
#define USB_CONFIG_POLL_MS 100U
#define USB_CONFIG_START_RETRY_MS 5000U
static void usb_host_wifi_task(void *arg) {
  app_config_t *cfg = (app_config_t *)arg;
  int64_t now_us = esp_timer_get_time();
  uint32_t phy_test = USB_SERIAL_JTAG.test.val;
  bool raw_host = usb_serial_jtag_is_connected();
  bool raw_allowed = usb_config_source_allows_wifi(
      raw_host, ((phy_test >> 5) & 1U) != 0, ((phy_test >> 6) & 1U) != 0);
  usb_config_gate_t gate;
  usb_config_gate_init(&gate, now_us, raw_allowed);
  int64_t retry_after_us = 0;
  bool previous_raw_allowed = raw_allowed;
  bool replug_required = false;
  int64_t no_client_since_us = 0;
  while (true) {
    now_us = esp_timer_get_time();
    raw_host = usb_serial_jtag_is_connected();
    phy_test = USB_SERIAL_JTAG.test.val;
    bool dp = ((phy_test >> 5) & 1U) != 0;
    bool dm = ((phy_test >> 6) & 1U) != 0;
    raw_allowed = usb_config_source_allows_wifi(raw_host, dp, dm);
    bool allowed = usb_config_gate_update(&gate, now_us, raw_allowed);
    bool running = wifi_web_is_started();
    if (raw_allowed != previous_raw_allowed) {
      test_debug_event(DBG_SOURCE,raw_allowed,allowed,running,0);
      ESP_LOGI(TAG, "USB cable candidate: data=%s D+/D-=%u%u connected=%s",
               raw_host ? "yes" : "no", dp, dm,
               raw_allowed ? "yes" : "no");
      previous_raw_allowed = raw_allowed;
    }

    app_config_t cfg_snapshot;
    if (!config_get_snapshot(cfg, &cfg_snapshot)) {
      cfg_snapshot = *cfg;
      config_validate(&cfg_snapshot);
    }

    if (!allowed) {
      retry_after_us = 0;
      replug_required = false;
      no_client_since_us = 0;
      if (running) {
        ESP_LOGI(TAG, "USB cable absent for 5s: stopping WiFi web");
        test_debug_event(DBG_WIFI_STOP,2,0,0,0);
        wifi_web_stop();
      }
    } else if (allowed && !running && !replug_required && now_us >= retry_after_us) {
      ESP_LOGI(TAG, "USB cable present for 2s: starting WiFi web");
      test_debug_event(DBG_AP_START,0,0,0,0);
      if (!wifi_web_start(cfg)) {
        retry_after_us = now_us +
                         (int64_t)USB_CONFIG_START_RETRY_MS * 1000LL;
        ESP_LOGW(TAG, "WiFi web start failed; retry in 5s while USB cable remains");
      }
    }

    running = wifi_web_is_started();
#if ALPHALOC_BLE_LOCATION
    if (running) {
      ble_location_server_start();
    } else {
      ble_location_server_stop();
    }
#endif
    if (allowed && running && !replug_required) {
      if (wifi_web_has_clients()) {
        no_client_since_us = 0;
      } else {
        if (!no_client_since_us) no_client_since_us = now_us;
        int64_t timeout_us = (int64_t)cfg_snapshot.wifi_idle_timeout_s * 1000000LL;
        if (timeout_us > 0 && now_us >= no_client_since_us &&
            now_us - no_client_since_us >= timeout_us) {
          ESP_LOGI(TAG, "No WiFi clients for %us: stopping until USB replug",
                   (unsigned)cfg_snapshot.wifi_idle_timeout_s);
          test_debug_event(DBG_WIFI_STOP,3,0,0,0);
          wifi_web_stop();
          replug_required = true;
          no_client_since_us = 0;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(USB_CONFIG_POLL_MS));
  }
}
#endif

#if ALPHALOC_BATTERY_MONITOR
static void battery_task(void *arg) {
  (void)arg;
  battery_read_now();
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(60000));
    battery_read_now();
  }
}
#endif

#ifdef ALPHALOC_NEOPIXEL_PIN
static void status_led_task(void *arg) {
  (void)arg;
  const uint8_t brightness = 8;
  const TickType_t on_ms = pdMS_TO_TICKS(150);
  const TickType_t off_ms = pdMS_TO_TICKS(150);
  const TickType_t cycle_ms = pdMS_TO_TICKS(5000);

  neopixel_init(ALPHALOC_NEOPIXEL_PIN, brightness);

  while (true) {
    TickType_t cycle_start = xTaskGetTickCount();

    // Camera status: green if connected and bonded, blue if connected only, red
    // if not connected.
    bool camera_connected = ble_client_is_connected();
    bool camera_bonded = ble_client_is_bonded();
    if (camera_connected && camera_bonded) {
      neopixel_set_rgb(0, 255, 0);
    } else if (camera_connected) {
      neopixel_set_rgb(0, 0, 255);
    } else {
      neopixel_set_rgb(255, 0, 0);
    }
    vTaskDelay(on_ms);
    neopixel_set_rgb(0, 0, 0);
    vTaskDelay(off_ms);

    // GPS status: green if fix is valid, red if not. Fake GPS shows violet.
    gps_fix_t fix;
    bool gps_ok = gps_get_latest(&fix) && fix.valid;
    if (gps_ok) {
#if ALPHALOC_FAKE_GPS
      neopixel_set_rgb(128, 0, 255);
#else
      neopixel_set_rgb(0, 255, 0);
#endif
    } else {
      neopixel_set_rgb(255, 0, 0);
    }
    vTaskDelay(on_ms);
    neopixel_set_rgb(0, 0, 0);
    vTaskDelay(off_ms);

    // Battery status: green >50%, yellow >30%, red otherwise (if enabled).
#if ALPHALOC_BATTERY_MONITOR
    battery_status_t bat = {0};
    if (battery_get_status(&bat) && bat.valid) {
      if (bat.percent > 50.0f) {
        neopixel_set_rgb(0, 255, 0);
      } else if (bat.percent > 30.0f) {
        neopixel_set_rgb(255, 180, 0);
      } else {
        neopixel_set_rgb(255, 0, 0);
      }
      vTaskDelay(on_ms);
      neopixel_set_rgb(0, 0, 0);
      vTaskDelay(off_ms);
    }
#endif

    // Wi-Fi status: blue if web server/AP active.
#if ALPHALOC_WIFI_WEB
#if ALPHALOC_USB_HOST_WIFI
    bool window_active = wifi_web_is_started();
#else
    int64_t now = esp_timer_get_time();
    bool window_active =
        (s_config_window_end_time_us == 0 || now < s_config_window_end_time_us);
#endif
    if (window_active) {
      neopixel_set_rgb(0, 0, 255);
      vTaskDelay(on_ms);
      neopixel_set_rgb(0, 0, 0);
      vTaskDelay(off_ms);
    }
#endif

    TickType_t elapsed = xTaskGetTickCount() - cycle_start;
    if (elapsed < cycle_ms) {
      vTaskDelay(cycle_ms - elapsed);
    }
  }
}
#endif

#if ALPHALOC_BOARD_XIAO_ESP32C6
static void xiao_status_led_set(bool on, bool usb_host) {
  // GPIO15 is active LOW. Hardware inversion makes zero duty fully OFF.
  // 8-bit PWM: 256/256 is full ON; 26/256 is the conservative dim fallback.
  // USB host detection is NOT VBUS sensing or battery/source identification.
  // This task exclusively owns the channel. Do not use the thread-safe
  // ledc_set_duty_and_update API: IDF requires a fade service for that API.
  const uint32_t duty = on ? (usb_host ? 256U : 26U) : 0U;
  ESP_ERROR_CHECK(ledc_set_duty(
      LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

static void xiao_status_led_cycle(bool connected, bool usb_host) {
  // Blink = running / no camera link; steady = BLE camera connected.
  // Neither promises a valid fix or GPS metadata in a photograph.
  xiao_status_led_set(true, usb_host);
  if (connected) {
    // Do not switch off between cycles: keep a steady indication.
    // Recheck both camera and USB host state each second.
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(100));
  xiao_status_led_set(false, usb_host);
  vTaskDelay(pdMS_TO_TICKS(900));
}

static void xiao_status_led_task(void *arg) {
  (void)arg;
  const ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_8_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = 2000,
#if ALPHALOC_POWER_SAVE
      .clk_cfg = LEDC_USE_XTAL_CLK, // Stable PWM while CPU/APB frequency changes.
#else
      .clk_cfg = LEDC_AUTO_CLK,
#endif
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  const ledc_channel_config_t channel = {
      .gpio_num = GPIO_NUM_15,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0,
      .flags.output_invert = 1,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
  ESP_LOGI(TAG, "XIAO user LED: GPIO15 active-low, USB host=full, otherwise=dim; "
                "no camera=100ms blink every 1s, BLE connected=steady "
                "(not EXIF confirmation)");
  unsigned cycles = 0;
  bool previous_usb_host = false;
  while (true) {
    bool connected = ble_client_is_connected();
    // SOF packets identify an active USB data host, even with no monitor open.
    // Chargers/power banks and suspended hosts may report false; use dim mode.
    bool usb_host = usb_serial_jtag_is_connected();
    if (cycles++ % 30 == 0 || usb_host != previous_usb_host) {
      ESP_LOGI(TAG, "Heartbeat: uptime=%llds camera=%s usb_host=%s LED=%s",
               (long long)(esp_timer_get_time() / 1000000LL),
               connected ? "connected" : "not connected",
               usb_host ? "present" : "absent", usb_host ? "100%" : "10%");
    }
    previous_usb_host = usb_host;
    xiao_status_led_cycle(connected, usb_host);
  }
}
#endif

static void configure_power_management(void) {
#if CONFIG_PM_ENABLE
  // DFS is independent of CPU light sleep. Keep UART RX and USB alive in v9.
  esp_pm_config_t pm_config = {
      .max_freq_mhz = ALPHALOC_PM_MAX_FREQ_MHZ,
      .min_freq_mhz = ALPHALOC_PM_MIN_FREQ_MHZ,
      .light_sleep_enable =
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE && ALPHALOC_LIGHT_SLEEP
          true,
#else
          false,
#endif
  };
  esp_err_t pm_err = esp_pm_configure(&pm_config);
  if (pm_err == ESP_OK) {
    ESP_LOGI(TAG, "Power management: CPU min=%d max=%d MHz; light sleep %s",
             pm_config.min_freq_mhz, pm_config.max_freq_mhz,
             pm_config.light_sleep_enable ? "enabled" : "disabled");
  } else {
    ESP_LOGW(TAG, "Power management configuration failed: %s",
             esp_err_to_name(pm_err));
  }
#if !ALPHALOC_LIGHT_SLEEP
  ESP_LOGI(TAG, "Automatic light sleep disabled for BLE camera reliability");
#elif !CONFIG_FREERTOS_USE_TICKLESS_IDLE
  ESP_LOGW(TAG, "Automatic light sleep disabled - enable "
                "CONFIG_FREERTOS_USE_TICKLESS_IDLE");
#endif
#else
  ESP_LOGI(TAG, "Power management disabled in sdkconfig");
#endif
}

void app_main(void) {
  const esp_app_desc_t *app_desc = esp_app_get_description();
  ESP_LOGI(TAG, "FrameNav %s starting",
           app_desc ? app_desc->version : "unknown");

#if ALPHALOC_BOARD_XIAO_ESP32C6
  // Seeed RF switch: GPIO3 LOW enables it, GPIO14 LOW selects ceramic antenna.
  // These are chip GPIO numbers, not the XIAO D3/D14 pin labels.
  ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_3, 0));
  ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_14, 0));
  const gpio_config_t antenna_cfg = {
      .pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_14),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&antenna_cfg));
  ESP_LOGI(TAG, "XIAO ESP32-C6: onboard ceramic antenna selected");
#endif

  configure_power_management();

#ifdef ALPHALOC_STEMMA_QT_DISABLE_PIN
  // Disable STEMMA QT power for lower power usage when unused.
  gpio_config_t stemma_cfg = {
      .pin_bit_mask = 1ULL << ALPHALOC_STEMMA_QT_DISABLE_PIN,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&stemma_cfg));
  ESP_ERROR_CHECK(gpio_set_level(ALPHALOC_STEMMA_QT_DISABLE_PIN, 0));
#endif
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
#ifdef ALPHALOC_FACTORY_RESET
    if (ALPHALOC_FACTORY_RESET) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ESP_ERROR_CHECK(nvs_flash_init());
    } else {
      ESP_LOGW(TAG, "NVS needs erase but factory reset disabled; keeping data");
    }
#else
    ESP_LOGW(TAG,
             "NVS needs erase but factory reset flag not set; keeping data");
#endif
  } else if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
  }

  test_debug_start();
  bool config_loaded = config_load(&s_cfg);
  (void)config_loaded;
#if ALPHALOC_PHASE1 || ALPHALOC_PHASE2
  // First boot uses known-safe XIAO defaults. Once Web config has created the
  // FrameNav NVS namespace, preserve user filters/time/max-age across reboots.
  if (!config_loaded) {
    s_cfg.camera_name_prefix[0] = '\0';
    s_cfg.camera_mac_prefix[0] = '\0';
    s_cfg.tz_offset_min = 480;
    s_cfg.dst_offset_min = 0;
    s_cfg.max_gps_age_s = 5;
  }
  // These are profile invariants, not Web settings.
  s_cfg.config_window_s = 0;
#if ALPHALOC_PHASE2
  s_cfg.gps_interval_ms = ALPHALOC_LOCATION_INTERVAL_MS;
#if ALPHALOC_POWER_SAVE
  ESP_LOGW(TAG, "PHASE 2 POWER SAVE: continuous GNSS with phone-location fallback; "
               "Web BLE power profiles; no CPU light sleep; "
               "no battery protection in firmware");
#if ALPHALOC_USB_HOST_WIFI
  ESP_LOGW(TAG, "USB-cable-gated WiFi web: data host or asserted D+/D- "
                "starts after 2s; cable loss stops after 5s; idle timeout is 5/15m");
#endif
#define PHASE2_BUILD_LABEL "v17"
#else
#define PHASE2_BUILD_LABEL "v8.1"
#endif
  ESP_LOGW(TAG, "GNSS UART1 TX=GPIO%d RX=GPIO%d baud=%d; "
               "publish interval=%ums; no fake fallback, no USB host time",
           GPS_UART_TX_PIN, GPS_UART_RX_PIN, GPS_UART_BAUD,
           (unsigned)s_cfg.gps_interval_ms);
  ESP_LOGI(TAG, "Location: fresh GNSS first, phone fallback TTL=%uh",
           (unsigned)s_cfg.phone_location_ttl_hours);
#else
  ESP_LOGW(TAG, "PHASE 1 FAKE GPS ONLY: lat=48.137154 lon=11.576124");
#if ALPHALOC_FAKE_GPS_USB_TIME
  ESP_LOGW(TAG, "EXPERIMENT v7.1: WAIT_HOST_TIME until USB host sync after every boot; "
               "never send build-time or frozen-2024 GPS time");
#elif ALPHALOC_FAKE_GPS_STEPPING
  ESP_LOGW(TAG, "EXPERIMENT v5: GPS UTC advances from build-host time + uptime; "
               "reboot resets the seed, camera clock may be changed");
#else
  ESP_LOGW(TAG, "Fake UTC is 2024-01-01 12:00:00; disable camera auto time/area adjustment");
#endif
#endif
#if ALPHALOC_LOCATION_OBSERVER
  ESP_LOGW(TAG, "EXPERIMENT v6: observe Sony location notifications/readable values; "
                "GATT ACK and linkage enabled are NOT GPS-valid confirmation");
#endif
  ESP_LOGI(TAG, "Camera filters: name=%s MAC=%s; Sony manufacturer filter retained",
           s_cfg.camera_name_prefix[0] ? "configured" : "disabled",
           s_cfg.camera_mac_prefix[0] ? "configured" : "disabled");
#endif
  config_validate(&s_cfg);

#if ALPHALOC_GNSS_STANDBY
  location_power_policy_t location_policy;
  config_resolve_location_power(&s_cfg, &location_policy);
  gps_apply_power_policy(&location_policy);
#endif

  gps_config_t gps_cfg = {
#if !ALPHALOC_FAKE_GPS
      .uart_num = GPS_UART_NUM,
      .tx_pin = GPS_UART_TX_PIN,
      .rx_pin = GPS_UART_RX_PIN,
      .baud_rate = GPS_UART_BAUD,
#endif
      .update_interval_ms = s_cfg.gps_interval_ms,
  };
  gps_init(&gps_cfg);
#if ALPHALOC_TRACK_STORE
  const bool track_store_ready = track_store_init();
  if (!track_store_ready) {
    ESP_LOGE(TAG, "Track storage unavailable; positioning continues");
  }
#endif
#if ALPHALOC_FAKE_GPS_USB_TIME
  gps_fake_usb_time_start();
#endif

  ble_client_init(&s_cfg);
  location_service_init(&s_cfg);
  ble_client_set_focus_callback(focus_update_cb, &s_cfg);

#if ALPHALOC_POWER_SOURCE_PROBE
  // Start only after BLE state has been initialized; before ble_client_init(),
  // its static connection handle is not yet a meaningful diagnostic input.
  power_source_probe_start();
#endif

  BaseType_t ret;
#if ALPHALOC_TRACK_STORE
  if (track_store_ready) {
    ret = xTaskCreate(track_store_task, "track_store", 4096, NULL, 4, NULL);
    if (ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create track_store_task");
    }
  }
#endif
#if ALPHALOC_BATTERY_MONITOR
  battery_init();
  ret = xTaskCreate(battery_task, "battery", 3072, NULL, 4, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create battery_task");
  }
#endif
  ret = xTaskCreate(location_publisher_task, "location_pub", 4096, &s_cfg, 5,
                    NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create location_publisher_task");
  }

#if !ALPHALOC_USB_HOST_WIFI
  if (s_cfg.config_window_s > 0) {
    ret =
        xTaskCreate(config_window_task, "config_window", 4096, &s_cfg, 5, NULL);
    if (ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create config_window_task");
    }
  }
#endif
#if ALPHALOC_USB_HOST_WIFI
  ret = xTaskCreate(usb_host_wifi_task, "usb_wifi", 4096, &s_cfg, 4, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create usb_host_wifi_task");
  }
#endif
#ifdef ALPHALOC_NEOPIXEL_PIN
  ret = xTaskCreate(status_led_task, "status_led", 3072, &s_cfg, 1, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create status_led_task");
  }
#endif
#if ALPHALOC_BOARD_XIAO_ESP32C6
  ret = xTaskCreate(xiao_status_led_task, "xiao_led", 3072, NULL, 1,
                   NULL
  );
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create xiao_status_led_task");
  }
#endif
  ESP_LOGI(TAG, "FrameNav started");
}
