/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_USB_CONFIG_GATE_H
#define ALPHALOC_USB_CONFIG_GATE_H

#include <stdbool.h>
#include <stdint.h>

#ifndef ALPHALOC_USB_CONFIG_CONNECT_DEBOUNCE_MS
#define ALPHALOC_USB_CONFIG_CONNECT_DEBOUNCE_MS 2000U
#endif

#ifndef ALPHALOC_USB_CONFIG_DISCONNECT_DEBOUNCE_MS
#define ALPHALOC_USB_CONFIG_DISCONNECT_DEBOUNCE_MS 5000U
#endif

typedef struct {
  bool candidate_cable;
  bool wifi_allowed;
  int64_t candidate_since_us;
} usb_config_gate_t;

// Cable-presence classification on the XIAO USB PHY. An active USB data host
// or the measured non-host charger signature (both D+/D- lines asserted) is
// treated as a connected cable. The plain 1/0 idle signature is deliberately
// excluded: it is also what the board reports while running from its battery.
// The caller debounces this signal before controlling Wi-Fi.
bool usb_config_source_allows_wifi(bool usb_data_host, bool dp, bool dm);

// The caller supplies the classified source state. An allowed source must
// remain present for 2 s; loss must persist for 5 s.
void usb_config_gate_init(usb_config_gate_t *gate, int64_t now_us,
                          bool source_allowed);
bool usb_config_gate_update(usb_config_gate_t *gate, int64_t now_us,
                            bool source_allowed);

#endif
