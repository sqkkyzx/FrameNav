/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_GNSS_PM_PROTOCOL_H
#define ALPHALOC_GNSS_PM_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GNSS_PM_MON_VER_PACKET_SIZE 8U
#define GNSS_PM_REQUEST_PACKET_SIZE 24U
#define GNSS_PM_MON_VER_MAX_PAYLOAD 512U
#define GNSS_PM_MAX_SLEEP_MS UINT32_C(1036800000) /* 12 days */

typedef struct {
  char swversion[31];
  char hw[11];
  char fwversion[31];
  char protversion[31];
  char mod[31];
  bool supported;
} gnss_pm_identity_t;

/* Return the complete UBX frame length, or zero without writing on failure. */
size_t gnss_pm_build_mon_ver(uint8_t *out, size_t cap);
/* Reject zero (indefinite sleep) and durations above the documented maximum. */
size_t gnss_pm_build_request(uint8_t *out, size_t cap, uint32_t sleep_ms);

/* Pass only a complete, checksum-verified MON-VER payload, not its UBX header.
 * False means malformed: out is cleared. True means parsed: supported is true
 * only for an explicitly verified firmware/protocol pair. Unknown identities
 * must stay in continuous operation. All output strings are nul-terminated.
 */
bool gnss_pm_parse_mon_ver(const uint8_t *payload, size_t len,
                          gnss_pm_identity_t *out);

/* Call only after the enclosing UBX frame checksum has been verified.
 * ACK is not proof of backup entry; this detects an explicit PMREQ rejection.
 */
bool gnss_pm_is_request_nak(uint8_t cls, uint8_t id,
                           const uint8_t *payload, size_t len);

#endif
