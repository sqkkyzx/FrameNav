/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds one DNS response. IPv4 is supplied in network byte order. Returns
// zero for malformed/unsupported requests or insufficient reply capacity.
size_t captive_dns_build_reply(const uint8_t *request, size_t request_len,
                               uint32_t ipv4_network_order, uint8_t *reply,
                               size_t reply_capacity);

#ifdef __cplusplus
}
#endif
