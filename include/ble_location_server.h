/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Web Bluetooth identifiers used by the public location page. */
#define FRAMENAV_LOCATION_SERVICE_UUID \
  "b1f0b4d5-797b-5a9e-5b4f-4a1f02007ea1"
#define FRAMENAV_LOCATION_WRITE_UUID \
  "b1f0b4d5-797b-5a9e-5b4f-4a1f02107ea1"
#define FRAMENAV_LOCATION_CLEAR_UUID \
  "b1f0b4d5-797b-5a9e-5b4f-4a1f02117ea1"

void ble_location_server_register(void);
void ble_location_server_on_sync(void);
void ble_location_server_start(void);
void ble_location_server_stop(void);

#ifdef __cplusplus
}
#endif
