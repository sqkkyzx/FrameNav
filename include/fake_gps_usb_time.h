/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_FAKE_GPS_USB_TIME_H
#define ALPHALOC_FAKE_GPS_USB_TIME_H

#include <stdbool.h>
#include <stdint.h>

// Sanity bounds: 2020-01-01 <= host UTC < 2100-01-01. Host time is trusted,
// not an authenticated time service or a replacement for a GNSS clock.
#define ALPHALOC_USB_TIME_MIN_MS 1577836800000LL
#define ALPHALOC_USB_TIME_MAX_MS 4102444800000LL

bool gps_fake_usb_time_start(void);
bool gps_fake_parse_usb_time(const char *line, uint64_t expected_nonce,
                              int64_t *epoch_ms);
bool gps_fake_set_usb_time(int64_t epoch_ms, int64_t uptime_us);

#endif
