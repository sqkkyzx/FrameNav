/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_LOCATION_OBSERVER_H
#define ALPHALOC_LOCATION_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ble_gatt_chr;
struct os_mbuf;

// This observer never changes the publisher's ready/retry state or LED policy.
void location_observer_reset(uint16_t conn_handle);
void location_observer_discovered(uint16_t conn_handle,
                                  const struct ble_gatt_chr *chr,
                                  uint16_t service_end);
void location_observer_after_write(uint16_t conn_handle, int status);
void location_observer_notify(uint16_t conn_handle, uint16_t attr_handle,
                              struct os_mbuf *om, bool indication);
// Returns linkage enabled=1, disabled=0, unknown=-1; NOT GPS fix validity.
int location_observer_decode_link_state(const uint8_t *data, size_t len);

#endif
