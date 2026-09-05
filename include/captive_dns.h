/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct captive_dns_server *captive_dns_handle_t;

// Starts a wildcard IPv4 DNS responder on UDP/53. The address is supplied in
// network byte order and is returned for every valid A query.
captive_dns_handle_t captive_dns_start(uint32_t ipv4_network_order);

// Stops the task/socket and frees the handle. Safe to call with NULL.
void captive_dns_stop(captive_dns_handle_t handle);

#ifdef __cplusplus
}
#endif
