#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Exercise the BLE location write callback with host-side NimBLE stubs."""

import unittest

from host_phase1 import ROOT, run_c


class BleLocationChecks(unittest.TestCase):
    def test_location_frame_and_clear_are_validated_before_update(self):
        source = (ROOT / "src/ble_location_server.c").read_text()
        handlers = source[source.index("static uint32_t read_le32"):
                          source.index("static const struct ble_gatt_svc_def k_services")]
        run_c(r"""
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LOCATION_FRAME_LEN 28U
#define LOCATION_CLEAR_LEN 1U
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_ATT_ERR_WRITE_NOT_PERMITTED 3
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_VALUE_NOT_ALLOWED 19
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define TAG "test"

struct os_mbuf { size_t om_len; uint8_t data[32]; };
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
static bool accept_location = true, cleared;
static double saved_lat, saved_lon, saved_accuracy;
static uint64_t saved_captured, saved_now;
static int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *dst, uint16_t len,
                               uint16_t *out_len) {
  assert(om && len <= om->om_len);
  memcpy(dst, om->data, len);
  if (out_len) *out_len = len;
  return 0;
}
static bool location_service_set_phone(double lat, double lon, double accuracy,
                                       uint64_t captured, uint64_t now) {
  saved_lat = lat; saved_lon = lon; saved_accuracy = accuracy;
  saved_captured = captured; saved_now = now;
  return accept_location;
}
static void location_service_clear_phone(void) { cleared = true; }
""" + handlers + r"""
static void put_le32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void put_le64(uint8_t *p, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
int main(void) {
  struct os_mbuf mbuf = {.om_len = LOCATION_FRAME_LEN};
  struct ble_gatt_access_ctxt ctx = {.op = BLE_GATT_ACCESS_OP_WRITE_CHR,
                                      .om = &mbuf};
  put_le32(mbuf.data, (uint32_t)(int32_t)123456789);
  put_le32(mbuf.data + 4, (uint32_t)(int32_t)-1221234567);
  put_le32(mbuf.data + 8, 1234);
  put_le64(mbuf.data + 12, 1793361600000ULL);
  put_le64(mbuf.data + 20, 1793361620000ULL);
  assert(location_access_cb(1, 2, &ctx, (void *)0) == 0);
  assert(fabs(saved_lat - 12.3456789) < 0.00000001);
  assert(fabs(saved_lon + 122.1234567) < 0.00000001);
  assert(fabs(saved_accuracy - 12.34) < 0.000001);
  assert(saved_captured == 1793361600000ULL && saved_now == 1793361620000ULL);

  mbuf.om_len = LOCATION_FRAME_LEN - 1;
  assert(location_access_cb(1, 2, &ctx, (void *)0) ==
         BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
  mbuf.om_len = LOCATION_FRAME_LEN;
  accept_location = false;
  assert(location_access_cb(1, 2, &ctx, (void *)0) ==
         BLE_ATT_ERR_VALUE_NOT_ALLOWED);

  mbuf.data[0] = 1;
  mbuf.om_len = LOCATION_CLEAR_LEN;
  assert(location_access_cb(1, 3, &ctx, (void *)1) == 0 && cleared);
  mbuf.data[0] = 2;
  assert(location_access_cb(1, 3, &ctx, (void *)1) ==
         BLE_ATT_ERR_VALUE_NOT_ALLOWED);
  return 0;
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
