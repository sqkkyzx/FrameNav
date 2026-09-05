/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_config_gate.h"

#include <stddef.h>

#define USB_CONFIG_US_PER_MS 1000LL

static int64_t debounce_us(bool host) {
  const uint32_t delay_ms = host ? ALPHALOC_USB_CONFIG_CONNECT_DEBOUNCE_MS
                                 : ALPHALOC_USB_CONFIG_DISCONNECT_DEBOUNCE_MS;
  return (int64_t)delay_ms * USB_CONFIG_US_PER_MS;
}

bool usb_config_source_allows_wifi(bool usb_data_host, bool dp, bool dm) {
  return usb_data_host || (dp && dm);
}

void usb_config_gate_init(usb_config_gate_t *gate, int64_t now_us,
                          bool source_allowed) {
  if (!gate) return;
  gate->candidate_host = source_allowed;
  gate->wifi_allowed = false;
  gate->candidate_since_us = now_us;
}

bool usb_config_gate_update(usb_config_gate_t *gate, int64_t now_us,
                            bool source_allowed) {
  if (!gate) return false;
  if (source_allowed != gate->candidate_host ||
      now_us < gate->candidate_since_us) {
    gate->candidate_host = source_allowed;
    gate->candidate_since_us = now_us;
  }
  if (gate->wifi_allowed != gate->candidate_host &&
      now_us - gate->candidate_since_us >= debounce_us(gate->candidate_host)) {
    gate->wifi_allowed = gate->candidate_host;
  }
  return gate->wifi_allowed;
}
