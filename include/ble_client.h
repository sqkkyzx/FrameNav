/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_BLE_CLIENT_H
#define ALPHALOC_BLE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gps.h"
#include "config.h"

struct ble_gap_event;

typedef void (*ble_focus_cb_t)(void *ctx);

#define BLE_CLIENT_MAX_BONDS 8
#define BLE_CLIENT_CAMERA_NAME_MAX 48

typedef struct {
  uint8_t addr[6];
  uint8_t addr_type;
  bool connected;
  char name[BLE_CLIENT_CAMERA_NAME_MAX];
} ble_client_bond_t;

void ble_client_init(const app_config_t *cfg);
void ble_client_set_focus_callback(ble_focus_cb_t cb, void *ctx);
bool ble_client_is_connected(void);
bool ble_client_pause_for_sleep(bool pause);
bool ble_client_stop_for_sleep(void);
bool ble_client_is_bonded(void);
int ble_client_get_bonds(ble_client_bond_t *out, size_t capacity);
bool ble_client_delete_bond(const ble_client_bond_t *bond);
bool ble_client_start_pairing(uint32_t duration_s);
uint32_t ble_client_pairing_seconds_remaining(void);
void ble_client_config_changed(void);
bool ble_client_send_location(const gps_fix_t *fix);
int ble_client_gap_event_cb(struct ble_gap_event *event, void *arg);

#endif
