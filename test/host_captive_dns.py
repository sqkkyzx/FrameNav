#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Compile and exercise the actual bounded captive-DNS packet builder."""

import unittest

from host_phase1 import ROOT, run_c


class CaptiveDnsPacketChecks(unittest.TestCase):
    def test_a_aaaa_and_malformed_queries(self):
        packet_source = (ROOT / "src/captive_dns_packet.c").read_text()
        run_c(
            packet_source
            + r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static size_t make_query(uint8_t *q, uint16_t type) {
  memset(q, 0, 128);
  q[0] = 0x12;
  q[1] = 0x34;
  q[2] = 0x01;
  q[5] = 0x01;
  size_t p = 12;
  const char *labels[] = {"connectivitycheck", "gstatic", "com"};
  for (size_t i = 0; i < 3; ++i) {
    size_t n = strlen(labels[i]);
    q[p++] = (uint8_t)n;
    memcpy(q + p, labels[i], n);
    p += n;
  }
  q[p++] = 0;
  q[p++] = (uint8_t)(type >> 8);
  q[p++] = (uint8_t)type;
  q[p++] = 0;
  q[p++] = 1;
  return p;
}

int main(void) {
  uint8_t query[128];
  uint8_t reply[256];
  const uint8_t ip_bytes[4] = {10, 254, 254, 1};
  uint32_t ip;
  memcpy(&ip, ip_bytes, sizeof(ip));

  size_t query_len = make_query(query, 1);
  size_t reply_len = captive_dns_build_reply(query, query_len, ip, reply,
                                               sizeof(reply));
  assert(reply_len == query_len + 16);
  assert(reply[0] == 0x12 && reply[1] == 0x34);
  assert(be16(reply + 2) == 0x8180);
  assert(be16(reply + 4) == 1 && be16(reply + 6) == 1);
  assert(reply[query_len] == 0xc0 && reply[query_len + 1] == 0x0c);
  assert(memcmp(reply + reply_len - 4, ip_bytes, 4) == 0);

  query_len = make_query(query, 28);
  reply_len = captive_dns_build_reply(query, query_len, ip, reply,
                                       sizeof(reply));
  assert(reply_len == query_len);
  assert(be16(reply + 6) == 0);

  query_len = make_query(query, 1);
  query[5] = 2;
  assert(captive_dns_build_reply(query, query_len, ip, reply,
                                 sizeof(reply)) == 0);

  query_len = make_query(query, 1);
  query[2] = 0x81;
  assert(captive_dns_build_reply(query, query_len, ip, reply,
                                 sizeof(reply)) == 0);

  memset(query, 0, sizeof(query));
  query[5] = 1;
  query[12] = 0xc0;
  query[13] = 0x0c;
  query[14] = 0;
  query[15] = 1;
  query[16] = 0;
  query[17] = 1;
  assert(captive_dns_build_reply(query, 18, ip, reply, sizeof(reply)) == 0);

  query_len = make_query(query, 1);
  assert(captive_dns_build_reply(query, query_len, ip, reply,
                                 query_len + 15) == 0);
  return 0;
}
"""
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
