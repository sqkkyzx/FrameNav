/*
 * Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "captive_dns_packet.h"

#include <string.h>

#define DNS_HEADER_LEN 12U
#define DNS_TYPE_A 1U
#define DNS_CLASS_IN 1U
#define DNS_A_ANSWER_LEN 16U

static uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_be16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

size_t captive_dns_build_reply(const uint8_t *request, size_t request_len,
                               uint32_t ipv4_network_order, uint8_t *reply,
                               size_t reply_capacity) {
  if (!request || !reply || request_len < DNS_HEADER_LEN ||
      reply_capacity < DNS_HEADER_LEN) {
    return 0;
  }

  uint16_t request_flags = read_be16(request + 2);
  uint16_t question_count = read_be16(request + 4);
  if ((request_flags & 0x8000U) != 0 || (request_flags & 0x7800U) != 0 ||
      question_count != 1) {
    return 0;
  }

  size_t offset = DNS_HEADER_LEN;
  size_t hostname_len = 0;
  for (;;) {
    if (offset >= request_len) {
      return 0;
    }
    uint8_t label_len = request[offset++];
    if (label_len == 0) {
      break;
    }
    // Compression is valid in replies but is neither needed nor accepted in
    // this small captive-portal query parser.
    if ((label_len & 0xc0U) != 0 || label_len > 63 ||
        offset + label_len > request_len) {
      return 0;
    }
    size_t separator = hostname_len != 0 ? 1 : 0;
    if (hostname_len + separator + label_len > 253) return 0;
    hostname_len += separator + label_len;
    for (size_t i = 0; i < label_len; ++i) {
      uint8_t c = request[offset++];
      if (c == 0 || c == '.') return 0;
    }
  }

  if (offset + 4 > request_len) {
    return 0;
  }
  uint16_t question_type = read_be16(request + offset);
  uint16_t question_class = read_be16(request + offset + 2);
  size_t question_end = offset + 4;
  if (question_end > reply_capacity) {
    return 0;
  }

  memcpy(reply, request, question_end);
  write_be16(reply + 2, 0x8180U); // Preserve the established captive-DNS response.
  write_be16(reply + 4, 1);
  write_be16(reply + 6, 0);
  write_be16(reply + 8, 0);
  write_be16(reply + 10, 0);

  if (question_type != DNS_TYPE_A || question_class != DNS_CLASS_IN) {
    return question_end;
  }
  if (question_end + DNS_A_ANSWER_LEN > reply_capacity) {
    return 0;
  }

  write_be16(reply + 6, 1);
  uint8_t *answer = reply + question_end;
  write_be16(answer, 0xc00cU); // Pointer to the question name at byte 12.
  write_be16(answer + 2, DNS_TYPE_A);
  write_be16(answer + 4, DNS_CLASS_IN);
  write_be32(answer + 6, 30); // Short TTL keeps portal detection responsive.
  write_be16(answer + 10, 4);
  memcpy(answer + 12, &ipv4_network_order, 4);
  return question_end + DNS_A_ANSWER_LEN;
}
