/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_pm_protocol.h"

#include <string.h>

/* u-blox M10 SPG 5.10 / protocol 34.10, UBX-21035062 R03:
 * MON-VER pp. 89-90; RXM-PMREQ pp. 116-117.
 * https://content.u-blox.com/sites/default/files/
 * u-blox-M10-SPG-5.10_InterfaceDescription_UBX-21035062.pdf
 */
static void finish_checksum(uint8_t *frame, size_t len) {
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < len - 2; ++i) {
    a += frame[i];
    b += a;
  }
  frame[len - 2] = a;
  frame[len - 1] = b;
}

size_t gnss_pm_build_mon_ver(uint8_t *out, size_t cap) {
  if (!out || cap < GNSS_PM_MON_VER_PACKET_SIZE) return 0;
  const uint8_t request[] = {0xb5, 0x62, 0x0a, 0x04, 0, 0, 0x0e, 0x34};
  memcpy(out, request, sizeof(request));
  return sizeof(request);
}

size_t gnss_pm_build_request(uint8_t *out, size_t cap, uint32_t sleep_ms) {
  if (!out || cap < GNSS_PM_REQUEST_PACKET_SIZE ||
      sleep_ms > GNSS_PM_MAX_SLEEP_MS) return 0;
  memset(out, 0, GNSS_PM_REQUEST_PACKET_SIZE);
  out[0] = 0xb5;
  out[1] = 0x62;
  out[2] = 0x02;
  out[3] = 0x41;
  out[4] = 16; /* Version 0, 16-byte payload; reserved bytes remain zero. */
  for (unsigned i = 0; i < 4; ++i) out[10 + i] = (uint8_t)(sleep_ms >> (8 * i));
  out[14] = 0x06; /* backup | force */
  out[18] = 0x08; /* UART RX edge wakes the receiver. */
  finish_checksum(out, GNSS_PM_REQUEST_PACKET_SIZE);
  return GNSS_PM_REQUEST_PACKET_SIZE;
}

/* Each MON-VER slot includes its own terminator. Reject binary and unterminated
 * text instead of turning a malformed/truncated identity into a supported one.
 * Bytes after the terminator are padding and are deliberately not interpreted.
 */
static bool copy_ascii_slot(char *out, const uint8_t *slot, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    uint8_t c = slot[i];
    if (!c) {
      out[i] = '\0';
      return true;
    }
    if (c < 0x20 || c > 0x7e) return false;
    out[i] = (char)c;
  }
  return false;
}

bool gnss_pm_parse_mon_ver(const uint8_t *payload, size_t len,
                          gnss_pm_identity_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!payload || len < 40 || len > GNSS_PM_MON_VER_MAX_PAYLOAD ||
      (len - 40) % 30) return false;
  gnss_pm_identity_t parsed = {0};
  if (!copy_ascii_slot(parsed.swversion, payload, 30) ||
      !copy_ascii_slot(parsed.hw, payload + 30, 10) ||
      !parsed.swversion[0] || !parsed.hw[0]) return false;
  unsigned seen = 0;
  for (size_t pos = 40; pos < len; pos += 30) {
    char extension[31] = {0};
    if (!copy_ascii_slot(extension, payload + pos, 30)) return false;
    const char *value;
    char *destination;
    unsigned bit;
    if (strncmp(extension, "FWVER=", 6) == 0) {
      value = extension + 6; destination = parsed.fwversion; bit = 1;
    } else if (strncmp(extension, "PROTVER=", 8) == 0) {
      value = extension + 8; destination = parsed.protversion; bit = 2;
    } else if (strncmp(extension, "MOD=", 4) == 0) {
      value = extension + 4; destination = parsed.mod; bit = 4;
    } else {
      continue;
    }
    if ((seen & bit) || !value[0]) return false;
    seen |= bit;
    memcpy(destination, value, strlen(value) + 1);
  }
  /* MOD is optional in MON-VER. Never infer support from an M10-like model
   * name, ROM string, a protocol prefix, or an unverified firmware version.
   */
  parsed.supported = strcmp(parsed.fwversion, "SPG 5.10") == 0 &&
                     strcmp(parsed.protversion, "34.10") == 0;
  *out = parsed;
  return true;
}

bool gnss_pm_is_request_nak(uint8_t cls, uint8_t id,
                           const uint8_t *payload, size_t len) {
  return cls == 0x05 && id == 0x00 && payload && len == 2 &&
         payload[0] == 0x02 && payload[1] == 0x41;
}
