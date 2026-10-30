/*
 * FrameNav phone-location BLE peripheral.
 *
 * The service is deliberately separate from the legacy configuration service:
 * it accepts only a bounded location frame and never exposes Wi-Fi secrets.
 * Advertising is enabled only while the local Wi-Fi configuration service is
 * awake, which gives the USB wake window a physical access boundary.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_location_server.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "location_service.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

#define LOCATION_FRAME_LEN 28U
#define LOCATION_CLEAR_LEN 1U

#ifndef BLE_ERR_REM_USER_CONN_TERM
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#endif

static const char *TAG = "ble_location";
static bool s_synced;
static bool s_start_requested;
static bool s_advertising;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static const ble_uuid128_t k_service_uuid =
    /* NimBLE stores 128-bit UUID bytes little-endian. */
    BLE_UUID128_INIT(0xA1, 0x7E, 0x00, 0x02, 0x1F, 0x4A, 0x4F, 0x5B,
                     0x9E, 0x5A, 0x7B, 0x79, 0xD5, 0xB4, 0xF0, 0xB1);
static const ble_uuid128_t k_write_uuid =
    BLE_UUID128_INIT(0xA1, 0x7E, 0x10, 0x02, 0x1F, 0x4A, 0x4F, 0x5B,
                     0x9E, 0x5A, 0x7B, 0x79, 0xD5, 0xB4, 0xF0, 0xB1);
static const ble_uuid128_t k_clear_uuid =
    BLE_UUID128_INIT(0xA1, 0x7E, 0x11, 0x02, 0x1F, 0x4A, 0x4F, 0x5B,
                     0x9E, 0x5A, 0x7B, 0x79, 0xD5, 0xB4, 0xF0, 0xB1);

/* Keep a short name in the primary packet so scanners that omit scan
 * responses still show the device. The full identity remains in the response. */
static const char k_short_name[] = "FrameNav";

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= (uint64_t)p[i] << (8U * i);
  }
  return value;
}

static int location_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn_handle;
  (void)attr_handle;
  bool clear = (bool)(uintptr_t)arg;
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  }

  const size_t expected = clear ? LOCATION_CLEAR_LEN : LOCATION_FRAME_LEN;
  if (ctxt->om->om_len != expected) {
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }
  uint8_t payload[LOCATION_FRAME_LEN] = {0};
  int rc = ble_hs_mbuf_to_flat(ctxt->om, payload, expected, NULL);
  if (rc != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (clear) {
    if (payload[0] != 1U) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    location_service_clear_phone();
    ESP_LOGI(TAG, "Phone location cleared over BLE");
    return 0;
  }

  const int32_t lat_e7 = (int32_t)read_le32(payload);
  const int32_t lon_e7 = (int32_t)read_le32(payload + 4);
  const uint32_t accuracy_cm = read_le32(payload + 8);
  const uint64_t captured_ms = read_le64(payload + 12);
  const uint64_t now_ms = read_le64(payload + 20);
  const double latitude = (double)lat_e7 / 10000000.0;
  const double longitude = (double)lon_e7 / 10000000.0;
  const double accuracy_m = (double)accuracy_cm / 100.0;
  if (!isfinite(latitude) || !isfinite(longitude) ||
      !location_service_set_phone(latitude, longitude, accuracy_m, captured_ms,
                                  now_ms)) {
    return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
  }
  ESP_LOGI(TAG, "Phone location saved over BLE: %.7f, %.7f", latitude,
           longitude);
  return 0;
}

static const struct ble_gatt_svc_def k_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &k_write_uuid.u,
             .access_cb = location_access_cb,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
             .arg = (void *)(uintptr_t)false},
            {.uuid = &k_clear_uuid.u,
             .access_cb = location_access_cb,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
             .arg = (void *)(uintptr_t)true},
            {0},
        },
    },
    {0},
};

static void restart_advertising(void);

static int location_gap_event_cb(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        s_conn_handle = event->connect.conn_handle;
        s_advertising = false;
        ESP_LOGI(TAG, "Phone BLE connected");
        int rc = ble_gap_security_initiate(s_conn_handle);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
          ESP_LOGW(TAG, "BLE security start failed: %d", rc);
        }
      } else {
        s_advertising = false;
        ESP_LOGW(TAG, "Phone BLE connection failed: %d", event->connect.status);
        restart_advertising();
      }
      return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.status != 0) {
        ESP_LOGW(TAG, "Phone BLE encryption failed: %d", event->enc_change.status);
      }
      return 0;
    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle == s_conn_handle) {
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_advertising = false;
        ESP_LOGI(TAG, "Phone BLE disconnected");
        restart_advertising();
      }
      return 0;
    default:
      return 0;
  }
}

static void restart_advertising(void) {
  if (s_start_requested && s_synced) {
    ble_location_server_start();
  }
}

void ble_location_server_register(void) {
  ble_gatts_count_cfg(k_services);
  ble_gatts_add_svcs(k_services);
}

void ble_location_server_on_sync(void) {
  s_synced = true;
  if (s_start_requested) ble_location_server_start();
}

void ble_location_server_start(void) {
  s_start_requested = true;
  if (!s_synced || s_advertising ||
      s_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;

  struct ble_hs_adv_fields fields = {0};
  ble_uuid128_t service_uuid = k_service_uuid;
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name = (const uint8_t *)k_short_name;
  fields.name_len = sizeof(k_short_name) - 1;
  fields.name_is_complete = 0;
  fields.uuids128 = &service_uuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGW(TAG, "BLE location advertising fields failed: %d", rc);
    return;
  }

  struct ble_hs_adv_fields response_fields = {0};
  const char *name = ble_svc_gap_device_name();
  response_fields.name = (const uint8_t *)name;
  response_fields.name_len = strlen(name);
  response_fields.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&response_fields);
  if (rc != 0) {
    ESP_LOGW(TAG, "BLE location scan-response fields failed: %d", rc);
    return;
  }

  struct ble_gap_adv_params params = {0};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  uint8_t addr_type = 0;
  rc = ble_hs_id_infer_auto(0, &addr_type);
  if (rc != 0) {
    ESP_LOGW(TAG, "BLE location address type failed: %d", rc);
    return;
  }
  rc = ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &params,
                         location_gap_event_cb, NULL);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(TAG, "BLE location advertising start failed: %d", rc);
    return;
  }
  s_advertising = true;
  ESP_LOGI(TAG, "BLE location advertising started");
}

void ble_location_server_stop(void) {
  if (!s_start_requested && !s_advertising &&
      s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }
  s_start_requested = false;
  if (s_synced) {
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
      ESP_LOGW(TAG, "BLE location advertising stop failed: %d", rc);
    }
  }
  s_advertising = false;
  if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
      ESP_LOGW(TAG, "Phone BLE disconnect failed: %d", rc);
    }
  }
  ESP_LOGI(TAG, "BLE location advertising stopped");
}
