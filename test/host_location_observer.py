#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Host checks for the observation-only probe, not a camera/protocol simulator."""

import re
import unittest

from host_phase1 import ROOT, PRELUDE, function_source, run_c

PATH = "src/location_observer.c"
SOURCE = (ROOT / PATH).read_text()
ENTRY = re.search(r"typedef struct \{.*?\} observed_chr_t;", SOURCE, re.S).group()
GLOBALS = SOURCE[SOURCE.index("static observed_chr_t s_chars"):
                 SOURCE.index("int location_observer_decode_link_state")]
BASE = PRELUDE + """
#define BLE_HS_CONN_HANDLE_NONE 65535
#define OBS_MAX_CHARS 12
#define OBS_READ_PERIOD_US 30000000LL
#define BLE_GATT_CHR_PROP_READ 2
#define BLE_GATT_CHR_PROP_NOTIFY 16
#define BLE_GATT_CHR_PROP_INDICATE 32
#define ESP_LOGI(tag, ...) ((void)printf(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)printf(__VA_ARGS__))
static int64_t now_us = 6000000;
#define esp_timer_get_time() (now_us)
""" + ENTRY + GLOBALS + function_source(PATH, "location_observer_reset")

PROBE_STUBS = """
static unsigned calls, operation, handle, end_handle;
static uint8_t written[2];
static void *callback_generation;
static int mock_rc;
static void observer_descriptor_cb(void) {}
static void observer_subscribe_cb(void) {}
static void observer_read_cb(void) {}
static int ble_gattc_disc_all_dscs(uint16_t conn, uint16_t start, uint16_t end,
                                  void (*cb)(void), void *arg) {
    assert(conn == 7 && cb == observer_descriptor_cb);
    ++calls; operation=1; handle=start; end_handle=end; callback_generation=arg;
    return mock_rc;
}
static int ble_gattc_write_flat(uint16_t conn, uint16_t attr, const void *data,
                               size_t size, void (*cb)(void), void *arg) {
    assert(conn == 7 && size == 2 && cb == observer_subscribe_cb);
    ++calls; operation=2; handle=attr; memcpy(written,data,2); callback_generation=arg;
    return mock_rc;
}
static int ble_gattc_read(uint16_t conn, uint16_t attr,
                         void (*cb)(void), void *arg) {
    assert(conn == 7 && cb == observer_read_cb);
    ++calls; operation=3; handle=attr; callback_generation=arg;
    return mock_rc;
}
"""


class ObserverChecks(unittest.TestCase):
    def test_dd01_decodes_only_documented_linkage_packets(self):
        run_c(PRELUDE + function_source(PATH, "location_observer_decode_link_state") + """
int main(void) {
    uint8_t data[] = {3,1,2,1,0};
    assert(location_observer_decode_link_state(data,4) == 1);
    data[3]=0;
    assert(location_observer_decode_link_state(data,4) == 0);
    assert(location_observer_decode_link_state(NULL,4) == -1);
    for (size_t n=0; n<4; ++n) assert(location_observer_decode_link_state(data,n) == -1);
    assert(location_observer_decode_link_state(data,5) == -1);
    assert(location_observer_decode_link_state(data,128) == -1);
    for (unsigned b=2; b<256; ++b) {
        data[3]=b;
        assert(location_observer_decode_link_state(data,4) == -1);
    }
    data[3]=1; data[2]=3;
    assert(location_observer_decode_link_state(data,4) == -1);
    data[2]=2; data[1]=0;
    assert(location_observer_decode_link_state(data,4) == -1);
    data[1]=1; data[0]=4;
    assert(location_observer_decode_link_state(data,4) == -1);
    return 0;
}
""")

    def test_probe_is_bounded_property_gated_and_only_writes_discovered_cccd(self):
        run_c(BASE + PROBE_STUBS + function_source(PATH, "location_observer_after_write") + """
int main(void) {
    location_observer_reset(7);
    s_count=3;
    s_chars[0]=(observed_chr_t){.uuid_short=0xdd01,.value=238,.end=239,.properties=16};
    s_chars[1]=(observed_chr_t){.uuid_short=0xdd11,.value=241,.end=241,.properties=8};
    s_chars[2]=(observed_chr_t){.uuid_short=0xdd21,.value=243,.end=243,.properties=2};
    location_observer_after_write(8,0); assert(calls==0 && s_acks==0);
    location_observer_after_write(7,0x103); assert(calls==0 && s_failures==1);
    location_observer_after_write(7,0);
    assert(calls==1 && operation==1 && handle==238 && end_handle==239 && s_pending);
    assert((uintptr_t)callback_generation==s_generation);
    location_observer_after_write(7,0); assert(calls==1); // one in flight
    s_pending=false; s_chars[0].cccd=239; // actual descriptor discovery result
    location_observer_after_write(7,0);
    assert(calls==2 && operation==2 && handle==239 && written[0]==1 && written[1]==0);
    s_pending=false; s_chars[0].subscribed=true;
    location_observer_after_write(7,0);
    assert(calls==3 && operation==3 && handle==243); // DD11 write-only not read
    s_pending=false;
    location_observer_after_write(7,0); assert(calls==3); // rate limited reads
    now_us+=30000000;
    location_observer_after_write(7,0); assert(calls==4 && handle==243);
    s_pending=false; s_chars[2].read_disabled=true;
    now_us+=30000000;
    location_observer_after_write(7,0); assert(calls==4); // no retry storm
    location_observer_reset(7);
    s_count=1;
    s_chars[0]=(observed_chr_t){.value=238,.end=239,.properties=32,.descriptors_done=true};
    location_observer_after_write(7,0); assert(calls==4); // no guessed CCCD
    s_chars[0].cccd=239;
    location_observer_after_write(7,0);
    assert(calls==5 && operation==2 && handle==239 && written[0]==2);
    s_pending=false;
    location_observer_after_write(7,0); assert(calls==6);
    s_pending=false;
    location_observer_after_write(7,0); assert(calls==6); // max two subscribe attempts
    return 0;
}
""")

    def test_generation_guards_reset_old_callbacks_and_failures_release_pending(self):
        run_c(BASE + PROBE_STUBS + function_source(PATH, "observer_current") +
              function_source(PATH, "location_observer_after_write") + """
int main(void) {
    location_observer_reset(7);
    void *old=(void *)s_generation;
    assert(observer_current(7,old));
    assert(!observer_current(8,old));
    s_link_state=1; s_acks=20; s_pending=true;
    location_observer_reset(7); // even reused connection handle rejects old callbacks
    assert(!observer_current(7,old));
    assert(s_link_state==-1 && s_acks==0 && !s_pending && s_count==0);
    s_count=1;
    s_chars[0]=(observed_chr_t){.value=243,.end=243,.properties=2};
    mock_rc=12;
    location_observer_after_write(7,0);
    assert(!s_pending && calls==1);
    location_observer_after_write(7,0); assert(calls==1); // backoff
    now_us+=30000000; mock_rc=0;
    location_observer_after_write(7,0); assert(s_pending && calls==2);
    location_observer_reset(BLE_HS_CONN_HANDLE_NONE);
    assert(!observer_current(BLE_HS_CONN_HANDLE_NONE,(void *)s_generation));
    return 0;
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
