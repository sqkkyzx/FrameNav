/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_client.h"
#include "power_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if ALPHALOC_BLE_CONFIG
#include "ble_config_server.h"
#endif
#if ALPHALOC_BLE_LOCATION
#include "ble_location_server.h"
#endif
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#if ALPHALOC_LOCATION_OBSERVER
#include "location_observer.h"
#endif
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

#ifndef ALPHALOC_BLE_LOCATION
#define ALPHALOC_BLE_LOCATION 0
#endif

static const char *TAG = "ble_client";

#ifndef ALPHALOC_VERBOSE
#define ALPHALOC_VERBOSE 0
#endif

#if ALPHALOC_VERBOSE
#define VLOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define VLOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define VLOGI(...) ((void)0)
#define VLOGW(...) ((void)0)
#endif

#ifndef BLE_ERR_REM_USER_CONN_TERM
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#endif

extern void ble_store_config_init(void);
extern int ble_store_util_delete_peer(const ble_addr_t *addr);

typedef struct {
  uint16_t conn_handle;
  uint16_t loc_svc_start;
  uint16_t loc_svc_end;
  uint16_t rem_svc_start;
  uint16_t rem_svc_end;
  uint16_t chr_dd11;
  uint16_t chr_dd21;
  uint16_t chr_dd30;
  uint16_t chr_dd31;
  uint16_t chr_ff02;
  uint16_t end_ff02;
  uint16_t cccd_ff02;
} ble_handles_t;

typedef enum {
  DISC_NONE = 0,
  DISC_LOC_SVC,
  DISC_ALL_SVC,
  DISC_LOC_CHR,
  DISC_ALL_CHR,
  DISC_REM_SVC,
  DISC_REM_CHR,
  DISC_REM_DSC,
} disc_state_t;

static const uint8_t SONY_MFG_PREFIX[4] = {0x2D, 0x01, 0x00, 0x03};

static ble_handles_t s_handles;
static disc_state_t s_disc_state;
static uint8_t s_own_addr_type;
static const app_config_t *s_cfg;
static ble_focus_cb_t s_focus_cb;
static void *s_focus_ctx;
static bool s_require_tz_dst;
static int16_t s_tz_off_min;
static int16_t s_dst_off_min;
static bool s_connecting_camera;
static uint8_t s_dd21_retry;
static bool s_location_enabled;
static bool s_dd21_ready;
static bool s_dd21_pending;
static bool s_encrypted;
static bool s_notify_pending;
static bool s_remote_disc_started;
static bool s_dsc_pending_ff02;
static bool s_dsc_in_progress;
static int64_t s_last_loc_enable_attempt_us;
static int64_t s_last_dd21_attempt_us;
static int64_t s_last_security_attempt_us;
static int64_t s_last_disc_attempt_us;
typedef enum {
  DSC_NONE = 0,
  DSC_FF02,
} dsc_target_t;
static dsc_target_t s_dsc_target;
static uint8_t s_dsc_retry_count;
static esp_timer_handle_t s_dsc_retry_timer;
static esp_timer_handle_t s_identity_retry_timer;
static int8_t s_last_chr_interest;
static bool s_ff02_cccd_deferred;
static bool s_ff02_cccd_sent;
static bool s_service_changed_reconnect_attempted;
#if ALPHALOC_VERBOSE
static bool s_payload_logged;
#endif
static bool s_bonded_camera;
static bool s_retried_disc_after_enc;
static bool s_host_synced;
#if ALPHALOC_POWER_SAVE
static bool s_scan_restart_requested;
#endif
static int64_t s_pairing_mode_until_us;
static portMUX_TYPE s_control_mux = portMUX_INITIALIZER_UNLOCKED;

#define CAMERA_NAME_NVS_NAMESPACE "fn_camera"
#define CAMERA_NAME_NVS_KEY_SIZE 16

typedef enum {
  IDENTITY_WANT_MODEL = 0,
  IDENTITY_MODEL_PENDING,
  IDENTITY_WANT_DEVICE_NAME,
  IDENTITY_DEVICE_NAME_PENDING,
  IDENTITY_DONE,
} identity_read_state_t;

static identity_read_state_t s_identity_state = IDENTITY_DONE;
static bool s_identity_value_found;
static bool s_camera_identity_valid;
static ble_addr_t s_camera_identity_addr;
static char s_adv_camera_name[BLE_CLIENT_CAMERA_NAME_MAX];

static void ble_start_scan(void);
#if ALPHALOC_POWER_SAVE
static void ble_scan_complete(int reason);
#endif
static void addr_to_str(const ble_addr_t *addr, char *out, size_t len);
static void schedule_dsc_retry(void);
static void try_start_ff02_dsc(uint16_t conn_handle);
static void try_start_pending_dsc(uint16_t conn_handle);
static void enable_location_updates(uint16_t conn_handle);
static void retry_camera_setup(void);
static void reset_camera_setup_state(void);
static void schedule_identity_retry(uint64_t delay_us);
static bool enc_failure_needs_rebond(int status);
static void delete_camera_bond(const struct ble_gap_conn_desc *desc);
static int cccd_write_cb(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg);
static int cccd_read_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg);
typedef struct {
  const char *label;
  uint16_t handle;
  uint16_t chr_handle;
} cccd_ctx_t;

static cccd_ctx_t s_cccd_ff02_ctx = {"FF02", 0, 0};

static void camera_name_nvs_key(uint8_t addr_type, const uint8_t addr[6],
                                char out[CAMERA_NAME_NVS_KEY_SIZE]) {
  static const char hex[] = "0123456789ABCDEF";
  out[0] = 'n';
  out[1] = hex[addr_type & 0x0F];
  size_t pos = 2;
  for (int i = 5; i >= 0; --i) {
    out[pos++] = hex[addr[i] >> 4];
    out[pos++] = hex[addr[i] & 0x0F];
  }
  out[pos] = '\0';
}

static bool camera_name_copy(char *out, size_t out_len, const uint8_t *src,
                             size_t src_len) {
  if (!out || out_len == 0) {
    return false;
  }
  out[0] = '\0';
  if (!src || src_len == 0) {
    return false;
  }

  size_t start = 0;
  while (start < src_len && src[start] <= 0x20) {
    start++;
  }
  size_t written = 0;
  for (size_t i = start; i < src_len && src[i] != '\0'; ++i) {
    uint8_t ch = src[i];
    if (ch < 0x20 || ch == 0x7F) {
      continue;
    }
    if (written + 1 >= out_len) {
      break;
    }
    out[written++] = (char)ch;
  }
  while (written > 0 && (uint8_t)out[written - 1] <= 0x20) {
    written--;
  }
  out[written] = '\0';
  return written > 0;
}

static bool camera_name_load(const ble_addr_t *addr, char *out,
                             size_t out_len) {
  if (!addr || !out || out_len == 0) {
    return false;
  }
  out[0] = '\0';
  char key[CAMERA_NAME_NVS_KEY_SIZE];
  camera_name_nvs_key(addr->type, addr->val, key);
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(CAMERA_NAME_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return false;
  }
  size_t len = out_len;
  err = nvs_get_str(nvs, key, out, &len);
  nvs_close(nvs);
  if (err != ESP_OK) {
    out[0] = '\0';
    return false;
  }
  out[out_len - 1] = '\0';
  return out[0] != '\0';
}

static bool camera_name_save(const ble_addr_t *addr, const char *name) {
  if (!addr || !name || name[0] == '\0') {
    return false;
  }
  char current[BLE_CLIENT_CAMERA_NAME_MAX];
  if (camera_name_load(addr, current, sizeof(current)) &&
      strcmp(current, name) == 0) {
    return true;
  }

  char key[CAMERA_NAME_NVS_KEY_SIZE];
  camera_name_nvs_key(addr->type, addr->val, key);
  nvs_handle_t nvs = 0;
  esp_err_t err = nvs_open(CAMERA_NAME_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, key, name);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  if (nvs) {
    nvs_close(nvs);
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Camera name save failed: %s", esp_err_to_name(err));
    return false;
  }
  char address[18];
  addr_to_str(addr, address, sizeof(address));
  ESP_LOGI(TAG, "Camera identity saved: %s (%s)", name, address);
  return true;
}

static void camera_name_delete(const ble_addr_t *addr) {
  if (!addr) {
    return;
  }
  char key[CAMERA_NAME_NVS_KEY_SIZE];
  camera_name_nvs_key(addr->type, addr->val, key);
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(CAMERA_NAME_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return;
  }
  err = nvs_erase_key(nvs, key);
  if (err == ESP_OK) {
    (void)nvs_commit(nvs);
  }
  nvs_close(nvs);
}

static int camera_identity_read_cb(uint16_t conn_handle,
                                   const struct ble_gatt_error *error,
                                   struct ble_gatt_attr *attr, void *arg) {
  uint16_t uuid = (uint16_t)(uintptr_t)arg;
  bool reading_model = uuid == 0x2A24;
  if (conn_handle != s_handles.conn_handle || !s_camera_identity_valid) {
    return 0;
  }

  if (error->status == 0 && attr && attr->om) {
    uint8_t raw[BLE_CLIENT_CAMERA_NAME_MAX] = {0};
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(attr->om, raw, sizeof(raw) - 1, &copied);
    if (rc == 0 || rc == BLE_HS_EMSGSIZE) {
      char name[BLE_CLIENT_CAMERA_NAME_MAX];
      if (camera_name_copy(name, sizeof(name), raw, copied)) {
        s_identity_value_found = true;
        camera_name_save(&s_camera_identity_addr, name);
      }
    }
    return 0;
  }

  if (reading_model && !s_identity_value_found) {
    s_identity_state = IDENTITY_WANT_DEVICE_NAME;
    schedule_identity_retry(250000);
  } else {
    s_identity_state = IDENTITY_DONE;
  }
  return 0;
}

static void identity_retry_cb(void *arg) {
  (void)arg;
  if (s_handles.conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_encrypted ||
      !s_camera_identity_valid) {
    return;
  }

  uint16_t uuid = 0;
  identity_read_state_t pending_state = IDENTITY_DONE;
  if (s_identity_state == IDENTITY_WANT_MODEL) {
    uuid = 0x2A24; // Device Information: Model Number String.
    pending_state = IDENTITY_MODEL_PENDING;
  } else if (s_identity_state == IDENTITY_WANT_DEVICE_NAME) {
    uuid = 0x2A00; // Generic Access: Device Name.
    pending_state = IDENTITY_DEVICE_NAME_PENDING;
  } else {
    return;
  }

  s_identity_value_found = false;
  int rc = ble_gattc_read_by_uuid(
      s_handles.conn_handle, 1, 0xFFFF, BLE_UUID16_DECLARE(uuid),
      camera_identity_read_cb, (void *)(uintptr_t)uuid);
  if (rc == 0) {
    s_identity_state = pending_state;
    return;
  }

  // Sony service discovery and location setup take priority.  Retry the
  // cosmetic identity read once the ATT channel becomes idle.
  schedule_identity_retry(1000000);
}

static void schedule_identity_retry(uint64_t delay_us) {
  if (!s_identity_retry_timer) {
    return;
  }
  (void)esp_timer_stop(s_identity_retry_timer);
  (void)esp_timer_start_once(s_identity_retry_timer, delay_us);
}

static void begin_camera_identity_lookup(const struct ble_gap_conn_desc *desc) {
  if (!desc) {
    return;
  }
  s_camera_identity_addr = desc->peer_id_addr;
  s_camera_identity_valid = true;
  if (s_adv_camera_name[0] != '\0') {
    camera_name_save(&s_camera_identity_addr, s_adv_camera_name);
  }
  s_identity_state = IDENTITY_WANT_MODEL;
  schedule_identity_retry(1500000);
}

static void reset_camera_identity_lookup(void) {
  if (s_identity_retry_timer) {
    (void)esp_timer_stop(s_identity_retry_timer);
  }
  s_identity_state = IDENTITY_DONE;
  s_identity_value_found = false;
  s_camera_identity_valid = false;
  memset(&s_camera_identity_addr, 0, sizeof(s_camera_identity_addr));
  s_adv_camera_name[0] = '\0';
}

static int64_t pairing_deadline_get(void) {
  portENTER_CRITICAL(&s_control_mux);
  int64_t deadline = s_pairing_mode_until_us;
  portEXIT_CRITICAL(&s_control_mux);
  return deadline;
}

static void pairing_deadline_set(int64_t deadline_us) {
  portENTER_CRITICAL(&s_control_mux);
  s_pairing_mode_until_us = deadline_us;
  portEXIT_CRITICAL(&s_control_mux);
}

static bool pairing_mode_active(void) {
  return pairing_deadline_get() > esp_timer_get_time();
}

static bool peer_is_bonded(const ble_addr_t *addr) {
  if (!addr) {
    return false;
  }
  ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
  int num = 0;
  if (ble_store_util_bonded_peers(peers, &num,
                                  MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) {
    return false;
  }
  for (int i = 0; i < num; ++i) {
    if (memcmp(peers[i].val, addr->val, sizeof(addr->val)) == 0 &&
        peers[i].type == addr->type) {
      return true;
    }
  }
  return false;
}

static bool addr_equal(const ble_addr_t *a, const ble_addr_t *b) {
  return a && b && a->type == b->type &&
         memcmp(a->val, b->val, sizeof(a->val)) == 0;
}

static void delete_peer_bond(const ble_addr_t *addr) {
  if (!addr) {
    return;
  }
  int rc = ble_store_util_delete_peer(addr);
  if (rc != 0) {
    ESP_LOGW(TAG, "Bond delete failed: %d", rc);
  }
  camera_name_delete(addr);
}

static void delete_camera_bond(const struct ble_gap_conn_desc *desc) {
  if (!desc) {
    return;
  }
  delete_peer_bond(&desc->peer_ota_addr);
  if (!addr_equal(&desc->peer_id_addr, &desc->peer_ota_addr)) {
    delete_peer_bond(&desc->peer_id_addr);
  }
}

static bool enc_failure_needs_rebond(int status) {
  if (status == 0) {
    return false;
  }
#ifdef BLE_HS_ERR_HCI_BASE
#ifdef BLE_ERR_PINKEY_MISSING
  if (status == (BLE_HS_ERR_HCI_BASE + BLE_ERR_PINKEY_MISSING)) {
    return true;
  }
#endif
#ifdef BLE_ERR_KEY_MISSING
  if (status == (BLE_HS_ERR_HCI_BASE + BLE_ERR_KEY_MISSING)) {
    return true;
  }
#endif
#ifdef BLE_ERR_AUTH_FAIL
  if (status == (BLE_HS_ERR_HCI_BASE + BLE_ERR_AUTH_FAIL)) {
    return true;
  }
#endif
#endif
#ifdef BLE_HS_ERR_SM_AUTHFAIL
  if (status == BLE_HS_ERR_SM_AUTHFAIL) {
    return true;
  }
#endif
#ifdef BLE_HS_ERR_SM_KEY_MISSING
  if (status == BLE_HS_ERR_SM_KEY_MISSING) {
    return true;
  }
#endif
  return false;
}

static void reset_camera_setup_state(void) {
  uint16_t conn_handle = s_handles.conn_handle;
#if ALPHALOC_LOCATION_OBSERVER
  location_observer_reset(conn_handle);
#endif
  memset(&s_handles, 0, sizeof(s_handles));
  s_handles.conn_handle = conn_handle;
  s_disc_state = DISC_NONE;
  s_location_enabled = false;
  s_dd21_retry = 0;
  s_dd21_ready = false;
  s_dd21_pending = false;
  s_notify_pending = false;
  s_remote_disc_started = false;
  s_dsc_pending_ff02 = false;
  s_dsc_in_progress = false;
  s_dsc_target = DSC_NONE;
  s_dsc_retry_count = 0;
  s_last_chr_interest = 0;
  s_ff02_cccd_deferred = false;
  s_ff02_cccd_sent = false;
  s_retried_disc_after_enc = false;
  s_last_disc_attempt_us = 0;
  s_last_dd21_attempt_us = 0;
  s_last_loc_enable_attempt_us = 0;
}

static void addr_to_str(const ble_addr_t *addr, char *out, size_t len) {
  snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", addr->val[5],
           addr->val[4], addr->val[3], addr->val[2], addr->val[1],
           addr->val[0]);
}

static bool str_prefix_match(const char *value, const char *prefix) {
  if (prefix == NULL || prefix[0] == '\0') {
    return true;
  }
  if (value == NULL) {
    return false;
  }
  size_t prefix_len = strlen(prefix);
  return strncasecmp(value, prefix, prefix_len) == 0;
}

static bool name_prefix_match(const struct ble_hs_adv_fields *fields,
                              const char *prefix) {
  if (prefix == NULL || prefix[0] == '\0') {
    return true;
  }
  if (fields->name == NULL || fields->name_len == 0) {
    return false;
  }
  size_t prefix_len = strlen(prefix);
  if (fields->name_len < prefix_len) {
    return false;
  }
  return strncasecmp((const char *)fields->name, prefix, prefix_len) == 0;
}

static void log_mfg_data(const uint8_t *data, uint8_t len) {
  if (!ALPHALOC_VERBOSE) {
    return;
  }
  char buf[64];
  size_t pos = 0;
  for (uint8_t i = 0; i < len && pos + 3 < sizeof(buf); ++i) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", data[i]);
  }
  buf[pos] = '\0';
  VLOGI("Sony ADV mfg_data len=%u data=%s", (unsigned)len, buf);
}

static void make_sony_uuid(uint16_t first, uint16_t second,
                           ble_uuid128_t *out) {
  out->u.type = BLE_UUID_TYPE_128;
  uint8_t bytes[16] = {0xFF,
                       0xFF,
                       0xFF,
                       0xFF,
                       0xFF,
                       0xFF,
                       0xFF,
                       0xFF,
                       0xFF,
                       0xFF,
                       (uint8_t)(second & 0xFF),
                       (uint8_t)(second >> 8),
                       (uint8_t)(first & 0xFF),
                       (uint8_t)(first >> 8),
                       0x00,
                       0x80};
  memcpy(out->value, bytes, sizeof(bytes));
}

static bool is_sony_camera_adv(const struct ble_gap_disc_desc *desc,
                               const struct ble_hs_adv_fields *fields) {
  app_config_t cfg;
  if (!config_get_snapshot(s_cfg, &cfg)) {
    cfg = *s_cfg;
    config_validate(&cfg);
  }
  if (fields->mfg_data_len < sizeof(SONY_MFG_PREFIX)) {
    return false;
  }
  uint16_t company_id =
      (uint16_t)fields->mfg_data[1] << 8 | fields->mfg_data[0];
  uint16_t product_id =
      (uint16_t)fields->mfg_data[3] << 8 | fields->mfg_data[2];
  if (company_id != 0x012D) {
    return false;
  }
  log_mfg_data(fields->mfg_data, fields->mfg_data_len);
  if (product_id != 0x0003) {
    VLOGI("Sony ADV ignored: product ID=0x%04X", product_id);
    return false;
  }

  char addr_str[18];
  addr_to_str(&desc->addr, addr_str, sizeof(addr_str));

  if (!str_prefix_match(addr_str, cfg.camera_mac_prefix)) {
    VLOGI("ADV ignored: mac %s != %s", addr_str, cfg.camera_mac_prefix);
    return false;
  }

  if (cfg.camera_name_prefix[0] != '\0') {
    if (!name_prefix_match(fields, cfg.camera_name_prefix)) {
      if (fields->name == NULL || fields->name_len == 0) {
        VLOGI("ADV matched Sony camera without local name: %s", addr_str);
        return true;
      }
      VLOGI("ADV ignored: name does not match %s", cfg.camera_name_prefix);
      return false;
    }
  }

  VLOGI("ADV matched Sony camera: %s", addr_str);
  return true;
}

static int gatt_disc_chrs_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg);
static int gatt_disc_svc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg);
static int gatt_disc_dsc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc, void *arg);
static int gatt_disc_all_svc_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_svc *svc, void *arg);
static bool uuid_matches(const ble_uuid_any_t *uuid, uint16_t first,
                         uint16_t second);
static bool uuid16_matches(const ble_uuid_any_t *uuid, uint16_t short_uuid);
static void start_all_char_discovery(uint16_t conn_handle);

static void start_location_service_discovery(uint16_t conn_handle) {
  ble_uuid128_t svc_uuid;
  make_sony_uuid(0xDD00, 0xDD00, &svc_uuid);
  s_disc_state = DISC_LOC_SVC;
  int rc =
      ble_gattc_disc_svc_by_uuid(conn_handle, &svc_uuid.u, gatt_disc_svc_cb, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "Location service discovery start failed: %d", rc);
  }
}

static void start_remote_service_discovery(uint16_t conn_handle) {
  ble_uuid128_t svc_uuid;
  make_sony_uuid(0xFF00, 0xFF00, &svc_uuid);
  s_disc_state = DISC_REM_SVC;
  VLOGI("Starting remote service discovery");
  int rc =
      ble_gattc_disc_svc_by_uuid(conn_handle, &svc_uuid.u, gatt_disc_svc_cb, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "Remote service discovery start failed: %d", rc);
  }
}

static void start_all_char_discovery(uint16_t conn_handle) {
  s_disc_state = DISC_ALL_CHR;
  int rc = ble_gattc_disc_all_chrs(conn_handle, 1, 0xFFFF, gatt_disc_chrs_cb,
                                   NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "Full characteristic discovery start failed: %d", rc);
  }
}

static int dd21_read_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg) {
  (void)conn_handle;
  (void)arg;
  if (error->status != 0 || attr == NULL || attr->om == NULL) {
    ESP_LOGW(TAG, "DD21 read failed: %d", error->status);
    if (s_dd21_retry < 2 && s_handles.chr_dd21 != 0) {
      s_dd21_retry++;
      int rc = ble_gattc_read(conn_handle, s_handles.chr_dd21, dd21_read_cb,
                              NULL);
      if (rc != 0) {
        ESP_LOGW(TAG, "DD21 retry start failed: %d", rc);
      }
    }
    return 0;
  }

  uint8_t buf[7] = {0};
  int rc = ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "DD21 read decode failed: %d", rc);
    return 0;
  }
  s_require_tz_dst = (buf[4] & 0x02) != 0;
  s_dd21_ready = true;
  ESP_LOGI(TAG, "DD21 flag byte=0x%02X require_tz_dst=%d", buf[4],
           s_require_tz_dst);
  enable_location_updates(conn_handle);
  return 0;
}

static void enable_notifications(uint16_t conn_handle) {
  if (s_handles.cccd_ff02 == 0) {
    ESP_LOGW(TAG, "CCCD handle not found");
    return;
  }
  if (!s_encrypted) {
    s_notify_pending = true;
    VLOGI("Deferring notifications until encryption");
    return;
  }
  s_cccd_ff02_ctx.handle = s_handles.cccd_ff02;
  s_cccd_ff02_ctx.chr_handle = s_handles.chr_ff02;
  uint8_t val_le[2] = {0x01, 0x00};
  int rc = ble_gattc_write_flat(conn_handle, s_handles.cccd_ff02, val_le,
                                sizeof(val_le), cccd_write_cb,
                                &s_cccd_ff02_ctx);
  if (rc != 0) {
    ESP_LOGW(TAG, "FF02 CCCD write start failed: %d", rc);
    return;
  }
  ESP_LOGI(TAG, "Subscribing to FF02 notifications");
  s_notify_pending = false;
}

static int cccd_write_cb(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
  (void)attr;
  cccd_ctx_t *ctx = (cccd_ctx_t *)arg;
  const char *label = ctx ? ctx->label : "CCCD";
  if (error->status != 0) {
    ESP_LOGW(TAG, "%s CCCD write failed: %d", label, error->status);
    return 0;
  }
  ESP_LOGI(TAG, "%s CCCD write ok", label);
  if (ctx) {
    int rc = ble_gattc_read(conn_handle, ctx->handle, cccd_read_cb, ctx);
    if (rc != 0) {
      ESP_LOGW(TAG, "%s CCCD read start failed: %d", label, rc);
    }
  }
  return 0;
}

static int cccd_read_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg) {
  (void)conn_handle;
  cccd_ctx_t *ctx = (cccd_ctx_t *)arg;
  const char *label = ctx ? ctx->label : "CCCD";
  if (error->status != 0 || attr == NULL || attr->om == NULL) {
    ESP_LOGW(TAG, "%s CCCD read failed: %d", label, error->status);
    return 0;
  }
  uint8_t buf[2] = {0};
  int rc = ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "%s CCCD read decode failed: %d", label, rc);
    return 0;
  }
  uint16_t val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
  ESP_LOGI(TAG, "%s CCCD value=0x%04X", label, val);
  return 0;
}

static void try_start_ff02_dsc(uint16_t conn_handle) {
  if (s_handles.chr_ff02 == 0 || s_handles.cccd_ff02 != 0) {
    return;
  }
  if (s_dsc_in_progress) {
    s_dsc_pending_ff02 = true;
    return;
  }
  if (!s_encrypted) {
    s_dsc_pending_ff02 = true;
    schedule_dsc_retry();
    return;
  }
  s_disc_state = DISC_REM_DSC;
  s_dsc_in_progress = true;
  s_dsc_target = DSC_FF02;
  uint16_t end = s_handles.end_ff02
                     ? s_handles.end_ff02
                     : (s_handles.rem_svc_end ? s_handles.rem_svc_end : 0xFFFF);
  int rc = ble_gattc_disc_all_dscs(conn_handle, s_handles.chr_ff02, end,
                                   gatt_disc_dsc_cb, NULL);
  if (rc == 0) {
    s_dsc_pending_ff02 = false;
    return;
  }
  s_dsc_in_progress = false;
  s_dsc_pending_ff02 = true;
  VLOGW("Descriptor discovery start failed: %d", rc);
  schedule_dsc_retry();
}

static void try_start_pending_dsc(uint16_t conn_handle) {
  if (s_dsc_in_progress) {
    return;
  }
  if (s_dsc_pending_ff02) {
    try_start_ff02_dsc(conn_handle);
  }
}

static void dsc_retry_cb(void *arg) {
  (void)arg;
  if (s_handles.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }
  if (s_dsc_retry_count >= 4) {
    if (s_dsc_pending_ff02 && s_handles.chr_ff02 != 0 &&
        s_handles.cccd_ff02 == 0) {
      s_dsc_pending_ff02 = false;
      s_handles.cccd_ff02 = (uint16_t)(s_handles.chr_ff02 + 1);
      ESP_LOGW(TAG, "FF02 CCCD not found; using fallback handle=%u",
               s_handles.cccd_ff02);
      enable_notifications(s_handles.conn_handle);
    }
    return;
  }
  s_dsc_retry_count++;
  try_start_pending_dsc(s_handles.conn_handle);
}

static void schedule_dsc_retry(void) {
  if (!s_dsc_retry_timer) {
    return;
  }
  esp_timer_stop(s_dsc_retry_timer);
  esp_timer_start_once(s_dsc_retry_timer, 500000);
}

static int location_enable_write_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     struct ble_gatt_attr *attr, void *arg) {
  (void)attr;
  const char *label = arg ? (const char *)arg : "unknown";
  // Observation only. Preserve setup/retry behavior for the clock A/B test.
  if (error->status == 0) {
    VLOGI("%s enable write acknowledged: conn=%u", label, conn_handle);
  } else {
    ESP_LOGW(TAG, "%s enable write rejected/failed: conn=%u status=%d (0x%X)",
             label, conn_handle, error->status, (unsigned)error->status);
  }
  return 0;
}

static void enable_location_updates(uint16_t conn_handle) {
  if (s_handles.chr_dd21 != 0) {
    if (!s_encrypted) {
      s_dd21_pending = true;
      VLOGI("Deferring DD21 read until encryption");
    } else if (!s_dd21_ready) {
      s_dd21_pending = false;
      int rc = ble_gattc_read(conn_handle, s_handles.chr_dd21, dd21_read_cb,
                              NULL);
      if (rc != 0) {
        ESP_LOGW(TAG, "DD21 read start failed: %d", rc);
      }
    }
  }
  if (!s_encrypted || !s_dd21_ready) {
    VLOGI("Deferring DD30/DD31 writes until encrypted and DD21 read");
    return;
  }
  bool dd30_started = false;
  bool dd31_started = false;
  if (s_handles.chr_dd30 != 0) {
    uint8_t on = 0x01;
    ESP_LOGI(TAG, "Unlocking location");
    int rc = ble_gattc_write_flat(conn_handle, s_handles.chr_dd30, &on,
                                  sizeof(on), location_enable_write_cb, "DD30");
    if (rc != 0) {
      ESP_LOGW(TAG, "DD30 write start failed: %d", rc);
    } else {
      dd30_started = true;
    }
  }
  if (s_handles.chr_dd31 != 0) {
    uint8_t on = 0x01;
    ESP_LOGI(TAG, "Enabling location updates");
    int rc = ble_gattc_write_flat(conn_handle, s_handles.chr_dd31, &on,
                                  sizeof(on), location_enable_write_cb, "DD31");
    if (rc != 0) {
      ESP_LOGW(TAG, "DD31 write start failed: %d", rc);
    } else {
      dd31_started = true;
    }
  }
  s_location_enabled = dd30_started && dd31_started;
  if (s_location_enabled) {
    s_service_changed_reconnect_attempted = false;
    ESP_LOGI(TAG, "Location enable requests queued (not camera confirmation)");
  }
}

static bool location_characteristics_discovered(void) {
  return s_handles.chr_dd11 != 0 && s_handles.chr_dd21 != 0 &&
         s_handles.chr_dd30 != 0 && s_handles.chr_dd31 != 0;
}

static void retry_camera_setup(void) {
  if (s_handles.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }

  int64_t now = esp_timer_get_time();
  if (!s_encrypted) {
    if (now - s_last_security_attempt_us <= 3000000) {
      return;
    }
    s_last_security_attempt_us = now;
    ESP_LOGI(TAG, "Retrying BLE security");
    int rc = ble_gap_security_initiate(s_handles.conn_handle);
    if (rc != 0) {
      ESP_LOGW(TAG, "Security retry start failed: %d", rc);
    }
    return;
  }

  if (!location_characteristics_discovered()) {
    if (now - s_last_disc_attempt_us <= 3000000) {
      return;
    }
    s_last_disc_attempt_us = now;
    s_disc_state = DISC_NONE;
    s_remote_disc_started = false;
    ESP_LOGI(TAG, "Retrying camera service discovery");
    start_location_service_discovery(s_handles.conn_handle);
    return;
  }

  if (!s_dd21_ready) {
    if (now - s_last_dd21_attempt_us <= 3000000) {
      return;
    }
    s_last_dd21_attempt_us = now;
    ESP_LOGI(TAG, "Retrying DD21 read");
    int rc = ble_gattc_read(s_handles.conn_handle, s_handles.chr_dd21,
                            dd21_read_cb, NULL);
    if (rc != 0) {
      ESP_LOGW(TAG, "DD21 retry start failed: %d", rc);
    }
    return;
  }

  if (!s_location_enabled && now - s_last_loc_enable_attempt_us > 3000000) {
    s_last_loc_enable_attempt_us = now;
    ESP_LOGI(TAG, "Retrying location enable");
    enable_location_updates(s_handles.conn_handle);
  }
}

static int gatt_disc_svc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg) {
  if (error->status == 0 && svc != NULL) {
    if (s_disc_state == DISC_LOC_SVC) {
      if (s_handles.loc_svc_start == 0) {
        s_handles.loc_svc_start = svc->start_handle;
        s_handles.loc_svc_end = svc->end_handle;
        VLOGI("Location service found: start=%u end=%u", svc->start_handle,
              svc->end_handle);
      }
    } else if (s_disc_state == DISC_REM_SVC) {
      if (s_handles.rem_svc_start == 0) {
        s_handles.rem_svc_start = svc->start_handle;
        s_handles.rem_svc_end = svc->end_handle;
        VLOGI("Remote service found: start=%u end=%u", svc->start_handle,
              svc->end_handle);
      }
    }
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    if (s_disc_state == DISC_LOC_SVC && s_handles.loc_svc_start != 0) {
      s_disc_state = DISC_LOC_CHR;
      return ble_gattc_disc_all_chrs(conn_handle, s_handles.loc_svc_start,
                                     s_handles.loc_svc_end, gatt_disc_chrs_cb,
                                     NULL);
    }
    if (s_disc_state == DISC_LOC_SVC && s_handles.loc_svc_start == 0) {
      VLOGI("Location service not found by UUID, falling back to all services");
      s_disc_state = DISC_ALL_SVC;
      return ble_gattc_disc_all_svcs(conn_handle, gatt_disc_all_svc_cb, NULL);
    }
    if (s_disc_state == DISC_REM_SVC && s_handles.rem_svc_start != 0) {
      s_disc_state = DISC_REM_CHR;
      return ble_gattc_disc_all_chrs(conn_handle, s_handles.rem_svc_start,
                                     s_handles.rem_svc_end, gatt_disc_chrs_cb,
                                     NULL);
    }
    if (s_disc_state == DISC_REM_SVC && s_handles.rem_svc_start == 0) {
      VLOGI("Remote service not found by UUID, falling back to all services");
      s_disc_state = DISC_ALL_SVC;
      return ble_gattc_disc_all_svcs(conn_handle, gatt_disc_all_svc_cb, NULL);
    }
  }
  if (error->status != BLE_HS_EDONE) {
    if (s_disc_state == DISC_LOC_SVC && s_handles.loc_svc_start != 0) {
      ESP_LOGW(TAG, "Location service discovery ended with %d; continuing",
               error->status);
      s_disc_state = DISC_LOC_CHR;
      return ble_gattc_disc_all_chrs(conn_handle, s_handles.loc_svc_start,
                                     s_handles.loc_svc_end, gatt_disc_chrs_cb,
                                     NULL);
    }
    if (s_disc_state == DISC_REM_SVC && s_handles.rem_svc_start != 0) {
      ESP_LOGW(TAG, "Remote service discovery ended with %d; continuing",
               error->status);
      s_disc_state = DISC_REM_CHR;
      return ble_gattc_disc_all_chrs(conn_handle, s_handles.rem_svc_start,
                                     s_handles.rem_svc_end, gatt_disc_chrs_cb,
                                     NULL);
    }
    if (s_disc_state == DISC_LOC_SVC) {
      ESP_LOGW(TAG,
               "Location service discovery failed with %d; scanning all services",
               error->status);
      s_disc_state = DISC_ALL_SVC;
      return ble_gattc_disc_all_svcs(conn_handle, gatt_disc_all_svc_cb, NULL);
    }
    if (s_disc_state == DISC_REM_SVC) {
      ESP_LOGW(TAG,
               "Remote service discovery failed with %d; scanning all services",
               error->status);
      s_disc_state = DISC_ALL_SVC;
      return ble_gattc_disc_all_svcs(conn_handle, gatt_disc_all_svc_cb, NULL);
    }
    VLOGW("Service discovery error=%d state=%d", error->status, s_disc_state);
  }
  return error->status;
}

static int gatt_disc_all_svc_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_svc *svc, void *arg) {
  if (error->status == 0 && svc != NULL) {
    if ((uuid_matches(&svc->uuid, 0xDD00, 0xDD00) ||
         uuid16_matches(&svc->uuid, 0xDD00)) &&
        s_handles.loc_svc_start == 0) {
      s_handles.loc_svc_start = svc->start_handle;
      s_handles.loc_svc_end = svc->end_handle;
      VLOGI("Location service found in all-svc: start=%u end=%u",
            svc->start_handle, svc->end_handle);
    } else if ((uuid_matches(&svc->uuid, 0xFF00, 0xFF00) ||
                uuid16_matches(&svc->uuid, 0xFF00)) &&
               s_handles.rem_svc_start == 0) {
      s_handles.rem_svc_start = svc->start_handle;
      s_handles.rem_svc_end = svc->end_handle;
      VLOGI("Remote service found in all-svc: start=%u end=%u",
            svc->start_handle, svc->end_handle);
    }
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    if (s_handles.loc_svc_start != 0 && s_handles.chr_dd11 == 0) {
      s_disc_state = DISC_LOC_CHR;
      return ble_gattc_disc_all_chrs(conn_handle, s_handles.loc_svc_start,
                                     s_handles.loc_svc_end, gatt_disc_chrs_cb,
                                     NULL);
    }
    if (s_handles.rem_svc_start != 0) {
      s_disc_state = DISC_REM_CHR;
      return ble_gattc_disc_all_chrs(conn_handle, s_handles.rem_svc_start,
                                     s_handles.rem_svc_end, gatt_disc_chrs_cb,
                                     NULL);
    }
    ESP_LOGW(TAG,
             "Sony services not found in all-svc scan; scanning all characteristics");
    start_all_char_discovery(conn_handle);
    return 0;
  }
  return error->status;
}

static bool uuid_matches(const ble_uuid_any_t *uuid, uint16_t first,
                         uint16_t second) {
  ble_uuid128_t target;
  make_sony_uuid(first, second, &target);
  return ble_uuid_cmp(&uuid->u, &target.u) == 0;
}

static bool uuid16_matches(const ble_uuid_any_t *uuid, uint16_t short_uuid) {
  return uuid->u.type == BLE_UUID_TYPE_16 && uuid->u16.value == short_uuid;
}

static int gatt_disc_chrs_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg) {
  if (error->status == 0 && chr != NULL) {
#if ALPHALOC_LOCATION_OBSERVER
    if ((s_disc_state == DISC_LOC_CHR || s_disc_state == DISC_ALL_CHR) &&
        s_handles.loc_svc_start != 0 &&
        chr->val_handle >= s_handles.loc_svc_start &&
        chr->val_handle <= s_handles.loc_svc_end) {
      location_observer_discovered(conn_handle, chr, s_handles.loc_svc_end);
    }
#endif
    if (s_last_chr_interest == 1 && s_handles.end_ff02 == 0 &&
        chr->def_handle > 0) {
      s_handles.end_ff02 = (uint16_t)(chr->def_handle - 1);
      s_last_chr_interest = 0;
    }
    if (s_disc_state == DISC_LOC_CHR || s_disc_state == DISC_ALL_CHR) {
      if (uuid_matches(&chr->uuid, 0xDD11, 0xDD00) ||
          uuid16_matches(&chr->uuid, 0xDD11)) {
        s_handles.chr_dd11 = chr->val_handle;
        VLOGI("Found DD11 handle=%u", chr->val_handle);
      } else if (uuid_matches(&chr->uuid, 0xDD21, 0xDD00) ||
                 uuid16_matches(&chr->uuid, 0xDD21)) {
        s_handles.chr_dd21 = chr->val_handle;
        VLOGI("Found DD21 handle=%u", chr->val_handle);
      } else if (uuid_matches(&chr->uuid, 0xDD30, 0xDD00) ||
                 uuid16_matches(&chr->uuid, 0xDD30)) {
        s_handles.chr_dd30 = chr->val_handle;
        VLOGI("Found DD30 handle=%u", chr->val_handle);
      } else if (uuid_matches(&chr->uuid, 0xDD31, 0xDD00) ||
                 uuid16_matches(&chr->uuid, 0xDD31)) {
        s_handles.chr_dd31 = chr->val_handle;
        VLOGI("Found DD31 handle=%u", chr->val_handle);
      }
    } else if (s_disc_state == DISC_REM_CHR) {
      if (uuid_matches(&chr->uuid, 0xFF02, 0xFF00) ||
          uuid16_matches(&chr->uuid, 0xFF02)) {
        s_handles.chr_ff02 = chr->val_handle;
        VLOGI("Found FF02 handle=%u props=0x%02X (remote svc)", chr->val_handle,
              chr->properties);
        s_last_chr_interest = 1;
      }
    }
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    if (s_last_chr_interest == 1 && s_handles.end_ff02 == 0) {
      s_handles.end_ff02 =
          s_handles.rem_svc_end ? s_handles.rem_svc_end : 0xFFFF;
    }
    s_last_chr_interest = 0;
    if (s_disc_state == DISC_LOC_CHR) {
      if (s_handles.chr_dd11 == 0) {
        VLOGI("DD11 not found in service range, scanning all characteristics");
        start_all_char_discovery(conn_handle);
        return 0;
      }
      enable_location_updates(conn_handle);
      if (s_handles.rem_svc_start != 0) {
        s_disc_state = DISC_REM_CHR;
        return ble_gattc_disc_all_chrs(conn_handle, s_handles.rem_svc_start,
                                       s_handles.rem_svc_end, gatt_disc_chrs_cb,
                                       NULL);
      }
      if (!s_remote_disc_started) {
        s_remote_disc_started = true;
        start_remote_service_discovery(conn_handle);
      }
      return 0;
    }
    if (s_disc_state == DISC_ALL_CHR) {
      if (s_handles.chr_dd11 == 0) {
        ESP_LOGW(TAG, "DD11 not found in full characteristic scan");
      } else {
        enable_location_updates(conn_handle);
      }
      if (s_handles.rem_svc_start != 0) {
        s_disc_state = DISC_REM_CHR;
        return ble_gattc_disc_all_chrs(conn_handle, s_handles.rem_svc_start,
                                       s_handles.rem_svc_end, gatt_disc_chrs_cb,
                                       NULL);
      }
      if (!s_remote_disc_started) {
        s_remote_disc_started = true;
        start_remote_service_discovery(conn_handle);
      }
      return 0;
    }
    if (s_disc_state == DISC_REM_CHR) {
      if (s_handles.chr_ff02 != 0) {
        s_disc_state = DISC_REM_DSC;
        return ble_gattc_disc_all_dscs(conn_handle, s_handles.rem_svc_start,
                                       s_handles.rem_svc_end, gatt_disc_dsc_cb,
                                       NULL);
      }
      ESP_LOGW(TAG, "FF02 not found in remote service");
    }
  }
  if (error->status != BLE_HS_EDONE) {
    VLOGW("Characteristic discovery error=%d state=%d", error->status,
          s_disc_state);
  }
  return error->status;
}

static int gatt_disc_dsc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc, void *arg) {
  if (error->status == 0 && dsc != NULL) {
    if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
        dsc->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16) {
      if (chr_val_handle == s_handles.chr_ff02) {
        s_handles.cccd_ff02 = dsc->handle;
        VLOGI("Found FF02 CCCD handle=%u", dsc->handle);
      }
    }
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    if (s_dsc_target == DSC_FF02) {
      if (s_handles.cccd_ff02 != 0) {
        s_ff02_cccd_deferred = true;
      } else {
        s_handles.cccd_ff02 = (uint16_t)(s_handles.chr_ff02 + 1);
        ESP_LOGW(TAG, "FF02 CCCD not found; using fallback handle=%u",
                 s_handles.cccd_ff02);
        s_ff02_cccd_deferred = true;
      }
      s_dsc_in_progress = false;
      s_dsc_target = DSC_NONE;
      try_start_pending_dsc(conn_handle);
    }
  }
  return error->status;
}

static bool build_location_payload(const gps_fix_t *fix, bool require_tz_dst,
                                   int16_t tz_off_min, int16_t dst_off_min,
                                   uint8_t *out, size_t *out_len) {
  if (!fix || !fix->valid) {
    return false;
  }

  const bool send_tz_dst =
      require_tz_dst || (tz_off_min != 0 || dst_off_min != 0);
  const size_t total_len = send_tz_dst ? 95 : 91;

  int32_t lat_scaled = (int32_t)llround(fix->lat_deg * 1e7);
  int32_t lon_scaled = (int32_t)llround(fix->lon_deg * 1e7);

  out[0] = (uint8_t)((send_tz_dst ? 0x5D : 0x59) >> 8);
  out[1] = (uint8_t)((send_tz_dst ? 0x5D : 0x59) & 0xFF);

  out[2] = 0x08;
  out[3] = 0x02;
  out[4] = 0xFC;
  out[5] = send_tz_dst ? 0x03 : 0x00;
  out[6] = 0x00;
  out[7] = 0x00;
  out[8] = 0x10;
  out[9] = 0x10;
  out[10] = 0x10;

  out[11] = (uint8_t)((uint32_t)lat_scaled >> 24);
  out[12] = (uint8_t)((uint32_t)lat_scaled >> 16);
  out[13] = (uint8_t)((uint32_t)lat_scaled >> 8);
  out[14] = (uint8_t)((uint32_t)lat_scaled);
  out[15] = (uint8_t)((uint32_t)lon_scaled >> 24);
  out[16] = (uint8_t)((uint32_t)lon_scaled >> 16);
  out[17] = (uint8_t)((uint32_t)lon_scaled >> 8);
  out[18] = (uint8_t)((uint32_t)lon_scaled);

  out[19] = (uint8_t)(fix->year >> 8);
  out[20] = (uint8_t)(fix->year & 0xFF);
  out[21] = fix->month;
  out[22] = fix->day;
  out[23] = fix->hour;
  out[24] = fix->minute;
  out[25] = fix->second;

  memset(out + 26, 0, (send_tz_dst ? 95 : 91) - 26);

  if (send_tz_dst) {
    uint16_t tz = (uint16_t)tz_off_min;
    uint16_t dst = (uint16_t)dst_off_min;
    out[91] = (uint8_t)(tz >> 8);
    out[92] = (uint8_t)(tz & 0xFF);
    out[93] = (uint8_t)(dst >> 8);
    out[94] = (uint8_t)(dst & 0xFF);
  }

  *out_len = total_len;
  return true;
}

static int location_write_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
  (void)attr;
  (void)arg;
  if (error->status == 0) {
    VLOGI("DD11 location write acknowledged by camera: conn=%u", conn_handle);
  } else {
    ESP_LOGW(TAG, "DD11 location write rejected/failed: conn=%u status=%d (0x%X)",
             conn_handle, error->status, (unsigned)error->status);
  }
#if ALPHALOC_LOCATION_OBSERVER
  location_observer_after_write(conn_handle, error->status);
#endif
  return 0;
}

bool ble_client_send_location(const gps_fix_t *fix) {
  if (s_handles.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    VLOGW("Skip location send: no connection");
    return false;
  }
  if (!s_location_enabled) {
    VLOGW("Skip location send: location updates not enabled");
    retry_camera_setup();
    return false;
  }
  if (s_handles.chr_dd21 != 0 && !s_dd21_ready) {
    VLOGW("Skip location send: DD21 not ready");
    return false;
  }
  if (s_handles.chr_dd11 == 0) {
    VLOGW("Skip location send: DD11 not discovered");
    return false;
  }
  app_config_t cfg;
  int16_t tz_off_min = s_tz_off_min;
  int16_t dst_off_min = s_dst_off_min;
  if (config_get_snapshot(s_cfg, &cfg)) {
    tz_off_min = cfg.tz_offset_min;
    dst_off_min = cfg.dst_offset_min;
  }
  uint8_t payload[95];
  size_t payload_len = 0;
  if (!build_location_payload(fix, s_require_tz_dst, tz_off_min, dst_off_min,
                              payload, &payload_len)) {
    ESP_LOGW(TAG, "Location payload unavailable");
    return false;
  }
  uint16_t mtu = ble_att_mtu(s_handles.conn_handle);
  VLOGI("DD11 sample: conn=%u UTC=%04u-%02u-%02uT%02u:%02u:%02uZ "
        "lat=%.7f lon=%.7f encrypted=%d mtu=%u",
        s_handles.conn_handle, fix->year, fix->month, fix->day,
        fix->hour, fix->minute, fix->second, fix->lat_deg, fix->lon_deg,
        s_encrypted, mtu);
  uint16_t max_payload = mtu > 3 ? (uint16_t)(mtu - 3) : 0;
  int rc = 0;
  if (payload_len > max_payload) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, (uint16_t)payload_len);
    if (om == NULL) {
      ESP_LOGW(TAG, "Location write failed: no mbuf");
      return false;
    }
    rc = ble_gattc_write_long(s_handles.conn_handle, s_handles.chr_dd11, 0, om,
                              location_write_cb, NULL);
  } else {
    rc = ble_gattc_write_flat(s_handles.conn_handle, s_handles.chr_dd11,
                              payload, payload_len, location_write_cb, NULL);
  }
#if ALPHALOC_VERBOSE
  if (!s_payload_logged) {
    s_payload_logged = true;
    ESP_LOGI(TAG, "Location payload (%u bytes):", (unsigned)payload_len);
    ESP_LOG_BUFFER_HEX(TAG, payload, payload_len);
  }
#endif
  VLOGI("Location write %s (%u bytes), rc=%d", rc == 0 ? "queued" : "failed",
        (unsigned)payload_len, rc);
  if (rc == 0 && s_ff02_cccd_deferred && !s_ff02_cccd_sent && s_encrypted &&
      s_handles.cccd_ff02 != 0) {
    s_ff02_cccd_sent = true;
    ESP_LOGI(TAG, "Enabling FF02 notifications after first location write");
    enable_notifications(s_handles.conn_handle);
  }
  return rc == 0;
}

bool ble_client_is_connected(void) {
  return s_handles.conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_client_is_bonded(void) { return s_bonded_camera; }

int ble_client_get_bonds(ble_client_bond_t *out, size_t capacity) {
  if (!out || capacity == 0 || !s_host_synced) {
    return 0;
  }
  ble_addr_t peers[BLE_CLIENT_MAX_BONDS];
  int count = 0;
  int rc = ble_store_util_bonded_peers(peers, &count,
                                       BLE_CLIENT_MAX_BONDS);
  if (rc != 0) {
    ESP_LOGW(TAG, "Bond list read failed: %d", rc);
    return -1;
  }
  struct ble_gap_conn_desc current = {0};
  bool have_current = ble_client_is_connected() &&
                      ble_gap_conn_find(s_handles.conn_handle, &current) == 0;
  size_t copied = (size_t)count < capacity ? (size_t)count : capacity;
  for (size_t i = 0; i < copied; ++i) {
    memcpy(out[i].addr, peers[i].val, sizeof(out[i].addr));
    out[i].addr_type = peers[i].type;
    out[i].name[0] = '\0';
    (void)camera_name_load(&peers[i], out[i].name, sizeof(out[i].name));
    out[i].connected =
        have_current &&
        (addr_equal(&peers[i], &current.peer_id_addr) ||
         addr_equal(&peers[i], &current.peer_ota_addr));
  }
  return (int)copied;
}

bool ble_client_delete_bond(const ble_client_bond_t *bond) {
  if (!bond || !s_host_synced) {
    return false;
  }
  ble_addr_t target = {.type = bond->addr_type};
  memcpy(target.val, bond->addr, sizeof(target.val));

  // Only accept an address currently present in the bond store.  This keeps
  // the HTTP endpoint from becoming an arbitrary NimBLE store mutation API.
  ble_addr_t peers[BLE_CLIENT_MAX_BONDS];
  int count = 0;
  int rc = ble_store_util_bonded_peers(peers, &count,
                                       BLE_CLIENT_MAX_BONDS);
  if (rc != 0) {
    ESP_LOGW(TAG, "Bond list read before delete failed: %d", rc);
    return false;
  }
  bool found = false;
  for (int i = 0; i < count; ++i) {
    if (addr_equal(&target, &peers[i])) {
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }

  bool disconnect_current = false;
  uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
  if (ble_client_is_connected()) {
    struct ble_gap_conn_desc current;
    conn_handle = s_handles.conn_handle;
    if (ble_gap_conn_find(conn_handle, &current) == 0) {
      disconnect_current = addr_equal(&target, &current.peer_id_addr) ||
                           addr_equal(&target, &current.peer_ota_addr);
    }
  }

  rc = ble_store_util_delete_peer(&target);
  if (rc != 0) {
    ESP_LOGW(TAG, "Bond delete failed: %d", rc);
    return false;
  }
  camera_name_delete(&target);
  char addr[18];
  addr_to_str(&target, addr, sizeof(addr));
  ESP_LOGI(TAG, "Deleted bond %s type=%u", addr, (unsigned)target.type);

  if (disconnect_current) {
    rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
      ESP_LOGW(TAG, "Disconnect after bond delete failed: %d", rc);
    }
  } else if (!s_connecting_camera) {
    if (ble_gap_disc_active()) {
      (void)ble_gap_disc_cancel();
    }
    ble_start_scan();
  }
  return true;
}

uint32_t ble_client_pairing_seconds_remaining(void) {
  int64_t remaining_us = pairing_deadline_get() - esp_timer_get_time();
  if (remaining_us <= 0) {
    return 0;
  }
  return (uint32_t)((remaining_us + 999999LL) / 1000000LL);
}

bool ble_client_start_pairing(uint32_t duration_s) {
  if (!s_host_synced || duration_s < 10 || duration_s > 300) {
    return false;
  }
  pairing_deadline_set(esp_timer_get_time() +
                       (int64_t)duration_s * 1000000LL);
  ESP_LOGI(TAG, "New-camera pairing mode enabled for %us",
           (unsigned)duration_s);

  if (ble_client_is_connected()) {
    int rc = ble_gap_terminate(s_handles.conn_handle,
                               BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
      pairing_deadline_set(0);
      return false;
    }
    return true;
  }
  if (s_connecting_camera) {
    int rc = ble_gap_conn_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
      pairing_deadline_set(0);
      return false;
    }
    return true;
  }
  if (ble_gap_disc_active()) {
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
      pairing_deadline_set(0);
      return false;
    }
  }
  ble_start_scan();
  return true;
}

int ble_client_gap_event_cb(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
  case BLE_GAP_EVENT_DISC: {
    if (s_connecting_camera || ble_client_is_connected()) {
      return 0;
    }
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data) != 0) {
      return 0;
    }
    if (!is_sony_camera_adv(&event->disc, &fields)) {
      return 0;
    }
    if (pairing_mode_active() && peer_is_bonded(&event->disc.addr)) {
      VLOGI("Pairing mode ignored an already bonded camera");
      return 0;
    }
    (void)camera_name_copy(s_adv_camera_name, sizeof(s_adv_camera_name),
                           fields.name, fields.name_len);
    s_connecting_camera = true;
    int rc = ble_gap_disc_cancel();
    if (rc != 0) {
      VLOGW("Scan cancel failed: %d", rc);
    }
    rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000, NULL,
                         ble_client_gap_event_cb, NULL);
    if (rc != 0) {
      ESP_LOGW(TAG, "Connect start failed: %d", rc);
      s_connecting_camera = false;
      ble_start_scan();
      return 0;
    }
    VLOGI("Connecting to Sony camera");
    return 0;
  }
  case BLE_GAP_EVENT_CONNECT: {
    if (event->connect.status == 0) {
      if (s_connecting_camera) {
        s_handles.conn_handle = event->connect.conn_handle;
#if ALPHALOC_GNSS_STANDBY
        gps_camera_connection_changed(true);
#endif
#if ALPHALOC_LOCATION_OBSERVER
        location_observer_reset(s_handles.conn_handle);
#endif
        ESP_LOGI(TAG, "Connected to camera");
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(s_handles.conn_handle, &desc) == 0 &&
            peer_is_bonded(&desc.peer_ota_addr)) {
          ESP_LOGI(TAG, "Existing bond found; skipping pairing");
        }
        VLOGI("Start security");
        s_last_security_attempt_us = esp_timer_get_time();
        int rc = ble_gap_security_initiate(s_handles.conn_handle);
        if (rc != 0) {
          ESP_LOGW(TAG, "Security start failed: %d", rc);
        }
        s_connecting_camera = false;
      } else {
        ESP_LOGI(TAG, "Config client connected");
      }
    } else {
      ESP_LOGW(TAG, "Connect failed: %d", event->connect.status);
      s_connecting_camera = false;
      s_adv_camera_name[0] = '\0';
      ble_start_scan();
    }
    return 0;
  }
  case BLE_GAP_EVENT_DISCONNECT: {
    if (event->disconnect.conn.conn_handle == s_handles.conn_handle) {
      ESP_LOGI(TAG, "Camera disconnected: reason=%d (0x%X)",
               event->disconnect.reason, (unsigned)event->disconnect.reason);
      memset(&s_handles, 0, sizeof(s_handles));
      s_handles.conn_handle = BLE_HS_CONN_HANDLE_NONE;
#if ALPHALOC_GNSS_STANDBY
      gps_camera_connection_changed(false);
#endif
#if ALPHALOC_LOCATION_OBSERVER
      location_observer_reset(BLE_HS_CONN_HANDLE_NONE);
#endif
      s_connecting_camera = false;
      s_location_enabled = false;
      s_dd21_retry = 0;
      s_dd21_ready = false;
      s_dd21_pending = false;
      s_encrypted = false;
      s_notify_pending = false;
      s_remote_disc_started = false;
      s_dsc_pending_ff02 = false;
      s_dsc_in_progress = false;
      s_dsc_target = DSC_NONE;
      s_dsc_retry_count = 0;
      s_last_chr_interest = 0;
      s_last_security_attempt_us = 0;
      s_last_disc_attempt_us = 0;
      s_ff02_cccd_deferred = false;
      s_ff02_cccd_sent = false;
      s_bonded_camera = false;
      s_retried_disc_after_enc = false;
      reset_camera_identity_lookup();
      // Use saved bonds and MCU uptime; disconnects never renew the boot window.
      ble_start_scan();
    } else {
      ESP_LOGI(TAG, "Config client disconnected");
    }
    return 0;
  }
#if ALPHALOC_POWER_SAVE
  case BLE_GAP_EVENT_DISC_COMPLETE:
    // The finite bonded boot scan expired. Cancellation/preemption must not launch a
    // competing scan while a connection is being established.
    if (s_scan_restart_requested) {
      s_scan_restart_requested = false;
      ble_start_scan();
      return 0;
    }
    ble_scan_complete(event->disc_complete.reason);
    return 0;
#endif
  case BLE_GAP_EVENT_NOTIFY_RX: {
#if ALPHALOC_LOCATION_OBSERVER
    location_observer_notify(event->notify_rx.conn_handle,
                              event->notify_rx.attr_handle, event->notify_rx.om,
                              event->notify_rx.indication);
#endif
#if ALPHALOC_VERBOSE
    ESP_LOGI(TAG, "Notify rx handle=%u len=%u", event->notify_rx.attr_handle,
             (unsigned)event->notify_rx.om->om_len);
    ESP_LOG_BUFFER_HEX(TAG, event->notify_rx.om->om_data,
                       event->notify_rx.om->om_len);
#endif
    if (event->notify_rx.attr_handle == 3 && event->notify_rx.om->om_len == 4 &&
        event->notify_rx.om->om_data[0] == 0x01 &&
        event->notify_rx.om->om_data[1] == 0x00 &&
        event->notify_rx.om->om_data[2] == 0xFF &&
        event->notify_rx.om->om_data[3] == 0xFF) {
      ESP_LOGW(TAG, "Camera reported GATT service change");
      reset_camera_setup_state();
      if (!s_service_changed_reconnect_attempted) {
        s_service_changed_reconnect_attempted = true;
        int rc = ble_gap_terminate(event->notify_rx.conn_handle,
                                   BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) {
          ESP_LOGW(TAG, "Service-change reconnect failed: %d", rc);
        } else {
          ESP_LOGI(TAG, "Reconnecting after camera service change");
        }
      }
      return 0;
    }
    if (event->notify_rx.attr_handle == s_handles.chr_ff02) {
      uint8_t focus_msg[] = {0x02, 0x3F, 0x20};
      if (event->notify_rx.om->om_len == sizeof(focus_msg) &&
          memcmp(event->notify_rx.om->om_data, focus_msg, sizeof(focus_msg)) ==
              0) {
        ESP_LOGI(TAG, "Focus acquired notification");
        if (s_focus_cb) {
          s_focus_cb(s_focus_ctx);
        }
      }
    }
    return 0;
  }
  case BLE_GAP_EVENT_PASSKEY_ACTION: {
    struct ble_sm_io pkey = {0};
    pkey.action = event->passkey.params.action;
    if (pkey.action == BLE_SM_IOACT_DISP || pkey.action == BLE_SM_IOACT_INPUT) {
      app_config_t cfg;
      pkey.passkey = 0;
      if (config_get_snapshot(s_cfg, &cfg)) {
        pkey.passkey = cfg.ble_passkey;
      }
      ble_sm_inject_io(event->passkey.conn_handle, &pkey);
      ESP_LOGI(TAG, "Passkey used for pairing");
    }
    return 0;
  }
  case BLE_GAP_EVENT_REPEAT_PAIRING: {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
      delete_camera_bond(&desc);
      ESP_LOGW(TAG, "Repeat pairing requested; deleted existing bond");
    }
    return BLE_GAP_REPEAT_PAIRING_RETRY;
  }
  case BLE_GAP_EVENT_ENC_CHANGE:
    if (event->enc_change.conn_handle != s_handles.conn_handle) {
      ESP_LOGI(TAG, "Config client encryption changed");
      return 0;
    }
    s_encrypted = (event->enc_change.status == 0);
    if (!s_encrypted) {
      ESP_LOGW(TAG, "Encryption failed: %d", event->enc_change.status);
    }
    if (!s_encrypted) {
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
        delete_camera_bond(&desc);
        ESP_LOGW(TAG, "Deleted camera bond after encryption failure");
        if (enc_failure_needs_rebond(event->enc_change.status)) {
          int rc = ble_gap_security_initiate(event->enc_change.conn_handle);
          if (rc != 0) {
            ESP_LOGW(TAG, "Security retry start failed: %d", rc);
          }
        }
      }
    }
    if (s_encrypted) {
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
        s_bonded_camera = desc.sec_state.bonded;
        if (s_bonded_camera && pairing_mode_active()) {
          pairing_deadline_set(0);
          ESP_LOGI(TAG, "New-camera pairing completed");
        }
        begin_camera_identity_lookup(&desc);
      } else {
        s_bonded_camera = false;
      }
      if (!s_retried_disc_after_enc && s_disc_state == DISC_NONE &&
          s_handles.loc_svc_start == 0 && s_handles.rem_svc_start == 0) {
        s_retried_disc_after_enc = true;
        s_disc_state = DISC_NONE;
        s_remote_disc_started = false;
        s_last_disc_attempt_us = esp_timer_get_time();
        start_location_service_discovery(s_handles.conn_handle);
      }
    } else {
      s_bonded_camera = false;
    }
    ESP_LOGI(TAG, "Encryption %s", s_encrypted ? "enabled" : "failed");
    if (s_encrypted && s_dd21_pending && s_handles.chr_dd21 != 0 &&
        !s_dd21_ready) {
      s_dd21_pending = false;
      int rc = ble_gattc_read(s_handles.conn_handle, s_handles.chr_dd21,
                              dd21_read_cb, NULL);
      if (rc != 0) {
        ESP_LOGW(TAG, "Pending DD21 read start failed: %d", rc);
      }
    }
    if (s_encrypted && s_notify_pending && s_handles.cccd_ff02 != 0) {
      enable_notifications(s_handles.conn_handle);
    }
    return 0;
  default:
    return 0;
  }
}

#if ALPHALOC_POWER_SAVE
static bool scan_has_saved_bonds(void) {
  // s_bonded_camera describes the CURRENT connection and is cleared on disconnect.
  // Read NimBLE's restored bond store instead, including after a first pairing.
  ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
  int num = 0;
  int rc = ble_store_util_bonded_peers(peers, &num,
                                      MYNEWT_VAL(BLE_STORE_MAX_BONDS));
  if (rc != 0) {
    // Unknown is not unpaired: do not fall back to continuous scanning on errors.
    ESP_LOGW(TAG, "Scan bond lookup failed: %d; keeping duty-cycled scanning", rc);
    return true;
  }
  ESP_LOGI(TAG, "Scan policy: saved_bonds=%d; %s", num,
           num > 0 ? "duty-cycled" : "continuous pairing discovery");
  return num > 0;
}

static int scan_ms_to_units(uint32_t milliseconds) {
  uint32_t units = (milliseconds * 8U + 4U) / 5U;
  if (units < 4U) units = 4U;
  if (units > 0x4000U) units = 0x4000U;
  return (int)units;
}
#endif

static void configure_scan_params(struct ble_gap_disc_params *params,
                                  int32_t *duration_ms) {
  memset(params, 0, sizeof(*params));
  params->passive = 0; // Preserve active scanning and scan-response handling.
  params->filter_duplicates = 1;
  params->itvl = ALPHALOC_SCAN_PAIRING_INTERVAL;
  params->window = ALPHALOC_SCAN_PAIRING_WINDOW;
  *duration_ms = BLE_HS_FOREVER;
  int64_t pairing_remaining_us =
      pairing_deadline_get() - esp_timer_get_time();
  if (pairing_remaining_us > 0) {
    *duration_ms = (int32_t)((pairing_remaining_us + 999) / 1000);
    return;
  }
#if ALPHALOC_POWER_SAVE
  if (!scan_has_saved_bonds()) {
    return;
  }
  app_config_t cfg;
  if (!config_get_snapshot(s_cfg, &cfg)) {
    config_set_defaults(&cfg);
  }
  bluetooth_power_policy_t policy;
  config_resolve_bluetooth_power(&cfg, &policy);
  // esp_timer_get_time is monotonic time since MCU startup. Neither a host
  // resync nor a connection retry/disconnect may restart this boot period.
  int64_t remaining_us =
      (int64_t)policy.boot_fast_search_s * 1000000LL - esp_timer_get_time();
  if (remaining_us > 0) {
    params->itvl = ALPHALOC_SCAN_BONDED_BOOT_INTERVAL;
    params->window = ALPHALOC_SCAN_BONDED_BOOT_WINDOW;
    // Round up to avoid a zero-duration scan at the boot boundary.
    *duration_ms = (int32_t)((remaining_us + 999) / 1000);
  } else {
    params->itvl = scan_ms_to_units(policy.scan_period_ms);
    params->window = scan_ms_to_units(policy.scan_window_ms);
  }
#endif
}

static void ble_start_scan(void) {
  if (s_connecting_camera || ble_client_is_connected() || ble_gap_disc_active()) {
    return;
  }
  struct ble_gap_disc_params params;
  int32_t duration_ms;
  configure_scan_params(&params, &duration_ms);
  int rc = ble_gap_disc(s_own_addr_type, duration_ms, &params,
                        ble_client_gap_event_cb, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "BLE scan start failed: %d", rc);
    return;
  }
  ESP_LOGI(TAG, "BLE scanning: interval=%u window=%u (0.625ms units), duration_ms=%ld; "
               "interval_ms=%u window_ms=%u uptime_ms=%lld",
           (unsigned)params.itvl, (unsigned)params.window, (long)duration_ms,
           (unsigned)params.itvl * 5U / 8U, (unsigned)params.window * 5U / 8U,
           (long long)(esp_timer_get_time() / 1000));
}

#if ALPHALOC_POWER_SAVE
static void ble_scan_complete(int reason) {
  if (reason == 0) {
    ble_start_scan();
  }
}
#endif

void ble_client_config_changed(void) {
#if ALPHALOC_POWER_SAVE
  if (!s_host_synced || s_connecting_camera || ble_client_is_connected()) {
    return;
  }
  if (ble_gap_disc_active()) {
    s_scan_restart_requested = true;
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
      s_scan_restart_requested = false;
      ESP_LOGW(TAG, "BLE scan restart after config failed: %d", rc);
    }
  } else {
    ble_start_scan();
  }
#endif
}

#if ALPHALOC_BOARD_XIAO_ESP32C6
static void format_device_name(const uint8_t addr[6], char *out, size_t size) {
  // NimBLE stores address bytes least-significant first. Use the BLE identity,
  // not the distinct USB/base MAC, and preserve leading zeros in the suffix.
  snprintf(out, size, "FrameNav-%02X%02X%02X", addr[2], addr[1], addr[0]);
}
#endif

static void ble_on_sync(void) {
  int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGW(TAG, "Address type infer failed: %d", rc);
    return;
  }
  uint8_t own_addr[6];
  rc = ble_hs_id_copy_addr(s_own_addr_type, own_addr, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "BLE identity address read failed: %d", rc);
    return;
  }
#if ALPHALOC_BOARD_XIAO_ESP32C6
  char device_name[sizeof("FrameNav-FFFFFF")];
  format_device_name(own_addr, device_name, sizeof(device_name));
  rc = ble_svc_gap_device_name_set(device_name);
  if (rc != 0) {
    ESP_LOGE(TAG, "BLE device name set failed: %d", rc);
    return;
  }
#endif
  s_host_synced = true;
  ESP_LOGI(TAG, "BLE identity: name=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X",
           ble_svc_gap_device_name(), own_addr[5], own_addr[4], own_addr[3],
           own_addr[2], own_addr[1], own_addr[0]);
#if !ALPHALOC_BLE_CONFIG
  ESP_LOGI(TAG, "Camera central only: configuration advertising is OFF; "
                "not discoverable in phone/computer Bluetooth lists");
#endif
  ble_start_scan();
#if ALPHALOC_BLE_CONFIG
  ble_config_server_on_sync();
#endif
#if ALPHALOC_BLE_LOCATION
  ble_location_server_on_sync();
#endif
}

static void ble_host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void ble_client_init(const app_config_t *cfg) {
  s_cfg = cfg;
  memset(&s_handles, 0, sizeof(s_handles));
  s_bonded_camera = false;
  s_handles.conn_handle = BLE_HS_CONN_HANDLE_NONE;
  s_disc_state = DISC_NONE;
  s_require_tz_dst = false;
  s_tz_off_min = cfg ? cfg->tz_offset_min : 0;
  s_dst_off_min = cfg ? cfg->dst_offset_min : 0;
  s_connecting_camera = false;
  s_dd21_retry = 0;
  s_location_enabled = false;
  s_dd21_ready = false;
  s_dd21_pending = false;
  s_encrypted = false;
  s_notify_pending = false;
  s_remote_disc_started = false;
  s_dsc_pending_ff02 = false;
  s_dsc_in_progress = false;
  s_last_loc_enable_attempt_us = 0;
  s_last_dd21_attempt_us = 0;
  s_dsc_target = DSC_NONE;
  s_dsc_retry_count = 0;
  s_last_chr_interest = 0;
  s_last_security_attempt_us = 0;
  s_last_disc_attempt_us = 0;
  s_ff02_cccd_deferred = false;
  s_ff02_cccd_sent = false;
  s_service_changed_reconnect_attempted = false;
  s_retried_disc_after_enc = false;
  s_host_synced = false;
#if ALPHALOC_POWER_SAVE
  s_scan_restart_requested = false;
#endif
  reset_camera_identity_lookup();
  pairing_deadline_set(0);
#if ALPHALOC_VERBOSE
  s_payload_logged = false;
#endif

  ESP_ERROR_CHECK(nimble_port_init());
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_store_config_init();
#if ALPHALOC_BLE_CONFIG
  ble_config_server_register((app_config_t *)cfg);
#endif
#if ALPHALOC_BLE_LOCATION
  ble_location_server_register();
#endif

  ble_svc_gap_device_name_set("FrameNav");

  ble_hs_cfg.sync_cb = ble_on_sync;
  ble_hs_cfg.reset_cb = NULL;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  const esp_timer_create_args_t dsc_timer_args = {
      .callback = dsc_retry_cb,
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ff02_dsc_retry",
      .skip_unhandled_events = true,
  };
  int rc = esp_timer_create(&dsc_timer_args, &s_dsc_retry_timer);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "Descriptor retry timer create failed: %d", rc);
    s_dsc_retry_timer = NULL;
  }

  const esp_timer_create_args_t identity_timer_args = {
      .callback = identity_retry_cb,
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "camera_identity",
      .skip_unhandled_events = true,
  };
  rc = esp_timer_create(&identity_timer_args, &s_identity_retry_timer);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "Camera identity timer create failed: %d", rc);
    s_identity_retry_timer = NULL;
  }

  nimble_port_freertos_init(ble_host_task);
}

void ble_client_set_focus_callback(ble_focus_cb_t cb, void *ctx) {
  s_focus_cb = cb;
  s_focus_ctx = ctx;
}

void ble_client_deinit(void) {
  // Stop and delete the retry timer to prevent resource leak
  if (s_dsc_retry_timer) {
    esp_timer_stop(s_dsc_retry_timer);
    esp_timer_delete(s_dsc_retry_timer);
    s_dsc_retry_timer = NULL;
  }
  if (s_identity_retry_timer) {
    esp_timer_stop(s_identity_retry_timer);
    esp_timer_delete(s_identity_retry_timer);
    s_identity_retry_timer = NULL;
  }
}
