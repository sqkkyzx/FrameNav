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
  bool candidate_host;
  bool wifi_allowed;
  int64_t candidate_since_us;
} usb_config_gate_t;

// Source fingerprint measured on the XIAO USB pins:
// - any active USB data host (SOF) allows configuration;
// - without SOF, D+/D- == 1/1 allows the tested charger/power source;
// - without SOF, D+/D- == 1/0 keeps WiFi off for battery and Godox HSA-S.
bool usb_config_source_allows_wifi(bool usb_data_host, bool dp, bool dm);

// The caller supplies the classified source state. An allowed source must
// remain present for 2 s; loss must persist for 5 s.
void usb_config_gate_init(usb_config_gate_t *gate, int64_t now_us,
                          bool source_allowed);
bool usb_config_gate_update(usb_config_gate_t *gate, int64_t now_us,
                            bool source_allowed);

#endif
