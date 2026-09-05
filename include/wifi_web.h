/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_WIFI_WEB_H
#define ALPHALOC_WIFI_WEB_H

#include "config.h"

bool wifi_web_start(app_config_t *cfg);
void wifi_web_stop(void);
bool wifi_web_is_started(void);

#endif
