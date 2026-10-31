/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_WIFI_WEB_H
#define ALPHALOC_WIFI_WEB_H

#include "config.h"
#include <stddef.h>

bool wifi_web_start(app_config_t *cfg);
void wifi_web_stop(void);
bool wifi_web_is_started(void);
bool wifi_web_has_clients(void);
int64_t wifi_web_last_activity_us(void);
void wifi_web_request_wake(void);
bool wifi_web_take_wake_request(void);

/* Caller owns the returned cJSON-compatible allocation and must free it. */
char *wifi_web_get_config_json(void);
char *wifi_web_get_ble_config_json(void);
char *wifi_web_get_location_json(void);
char *wifi_web_get_ble_telemetry_json(void);
char *wifi_web_get_bonds_json(void);

#endif
