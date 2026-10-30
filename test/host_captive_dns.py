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

static size_t make_named_query(uint8_t *q, const char *name, uint16_t type) {
  memset(q, 0, 512);
  q[0] = 0x12;
  q[1] = 0x34;
  q[2] = 0x01;
  q[5] = 0x01;
  size_t p = 12;
  while (*name) {
    const char *dot = strchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    assert(n > 0 && n <= 63 && p + n + 6 <= 512);
    q[p++] = (uint8_t)n;
    memcpy(q + p, name, n);
    p += n;
    if (!dot) break;
    name = dot + 1;
  }
  q[p++] = 0;
  q[p++] = (uint8_t)(type >> 8);
  q[p++] = (uint8_t)type;
  q[p++] = 0;
  q[p++] = 1;
  return p;
}

static size_t make_query(uint8_t *q, uint16_t type) {
  return make_named_query(q, "connectivitycheck.gstatic.com", type);
}

int main(void) {
  uint8_t query[512];
  uint8_t reply[600];
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

  const char *local_names[] = {
    "framenav.device.igp.wiki", "FrAmEnAv.DeViCe.IGP.WIKI",
    "captive.apple.com", "www.msftconnecttest.com", "connect.rom.miui.com"
  };
  for (size_t i = 0; i < sizeof(local_names) / sizeof(local_names[0]); ++i) {
    query_len = make_named_query(query, local_names[i], 1);
    reply_len = captive_dns_build_reply(query, query_len, ip, reply, sizeof(reply));
    assert(reply_len == query_len + 16 && be16(reply + 6) == 1);
    assert(be16(reply + 2) == 0x8180);
    assert(memcmp(reply + reply_len - 4, ip_bytes, 4) == 0);
  }
  const char *internet_names[] = {
    "www.googleapis.com", "play.googleapis.com", "api.amap.com",
    "location.services.mozilla.com", "framenav.igp.wiki",
    "framenav.device.igp.wiki.example.com", "evilframenav.device.igp.wiki",
    "connectivitycheck.gstatic.com.example.com"
  };
  for (size_t i = 0; i < sizeof(internet_names) / sizeof(internet_names[0]); ++i) {
    for (int type = 1; type <= 28; type += 27) {
      query_len = make_named_query(query, internet_names[i], type);
      reply_len = captive_dns_build_reply(query, query_len, ip, reply, sizeof(reply));
      assert(reply_len == query_len + (type == 1 ? 16 : 0));
      assert(be16(reply + 6) == (type == 1 ? 1 : 0));
      assert(be16(reply + 2) == 0x8180); // Preserve phone captive-portal behavior.
      if (type == 1) assert(memcmp(reply + reply_len - 4, ip_bytes, 4) == 0);
    }
  }
  query_len = make_named_query(query, "framenav.device.igp.wiki", 1);
  query[2] = 0;
  reply_len = captive_dns_build_reply(query, query_len, ip, reply, sizeof(reply));
  assert(reply_len == query_len + 16 && be16(reply + 2) == 0x8180);
  query[query_len - 1] = 3; // Non-IN classes produce no answers.
  reply_len = captive_dns_build_reply(query, query_len, ip, reply, sizeof(reply));
  assert(reply_len == query_len && be16(reply + 2) == 0x8180);

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
  for (size_t n = 0; n < query_len; ++n) {
    assert(captive_dns_build_reply(query, n, ip, reply, sizeof(reply)) == 0);
  }
  query_len = make_named_query(query, "framenav.device.igp.wiki", 1);
  query[13] = '.'; // A dot within a wire label must not spoof joined labels.
  assert(captive_dns_build_reply(query, query_len, ip, reply, sizeof(reply)) == 0);
  char long_name[256];
  memset(long_name, 'a', 255);
  long_name[63] = long_name[127] = long_name[191] = '.';
  long_name[255] = 0;
  query_len = make_named_query(query, long_name, 1);
  assert(captive_dns_build_reply(query, query_len, ip, reply, sizeof(reply)) == 0);
  return 0;
}
"""
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
