#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Deterministic USB SOF debounce checks; no USB, WiFi or serial is opened."""

import unittest

from host_phase1 import PRELUDE, ROOT, run_c


HARNESS = PRELUDE + f'#include "{ROOT / "src/usb_config_gate.c"}"\n'


def check(body, prefix=""):
    run_c(prefix + HARNESS + "int main(void) {\n" + body +
          "\nreturn 0; }\n")


class UsbConfigGateChecks(unittest.TestCase):
    def test_measured_source_fingerprints(self):
        check("""
assert(usb_config_source_allows_wifi(true, false, false));
assert(usb_config_source_allows_wifi(true, true, false));
assert(usb_config_source_allows_wifi(false, true, true));
assert(!usb_config_source_allows_wifi(false, true, false));
assert(!usb_config_source_allows_wifi(false, false, true));
assert(!usb_config_source_allows_wifi(false, false, false));
""")

    def test_connected_at_boot_requires_two_continuous_seconds(self):
        check("""
usb_config_gate_t gate;
usb_config_gate_init(&gate,1000000,true);
assert(!usb_config_gate_update(&gate,2999999,true));
assert(usb_config_gate_update(&gate,3000000,true));
assert(usb_config_gate_update(&gate,9000000,true));
""")

    def test_disallowed_source_never_opens_and_later_allowed_source_is_debounced(self):
        check("""
usb_config_gate_t gate;
usb_config_gate_init(&gate,0,false);
assert(!usb_config_gate_update(&gate,60000000,false));
assert(!usb_config_gate_update(&gate,61000000,true));
assert(!usb_config_gate_update(&gate,62999999,true));
assert(usb_config_gate_update(&gate,63000000,true));
""")

    def test_host_loss_must_persist_five_seconds(self):
        check("""
usb_config_gate_t gate;
usb_config_gate_init(&gate,0,true);
assert(usb_config_gate_update(&gate,2000000,true));
assert(usb_config_gate_update(&gate,3000000,false));
assert(usb_config_gate_update(&gate,7999999,false));
assert(!usb_config_gate_update(&gate,8000000,false));
""")

    def test_short_sof_drop_and_short_false_attach_do_not_toggle(self):
        check("""
usb_config_gate_t gate;
usb_config_gate_init(&gate,0,true);
assert(usb_config_gate_update(&gate,2000000,true));
assert(usb_config_gate_update(&gate,3000000,false));
assert(usb_config_gate_update(&gate,7000000,true));
assert(usb_config_gate_update(&gate,20000000,true));
assert(usb_config_gate_update(&gate,21000000,false));
assert(usb_config_gate_update(&gate,25999999,false));
assert(usb_config_gate_update(&gate,26000000,true));
""")

    def test_monotonic_clock_rollback_restarts_candidate_timer(self):
        check("""
usb_config_gate_t gate;
usb_config_gate_init(&gate,10000000,true);
assert(!usb_config_gate_update(&gate,11000000,true));
assert(!usb_config_gate_update(&gate,5000000,true));
assert(!usb_config_gate_update(&gate,6999999,true));
assert(usb_config_gate_update(&gate,7000000,true));
""")

    def test_thresholds_are_compile_time_configurable(self):
        check("""
usb_config_gate_t gate;
usb_config_gate_init(&gate,0,true);
assert(!usb_config_gate_update(&gate,999,true));
assert(usb_config_gate_update(&gate,1000,true));
assert(usb_config_gate_update(&gate,2000,false));
assert(!usb_config_gate_update(&gate,4000,false));
""", prefix=("#define ALPHALOC_USB_CONFIG_CONNECT_DEBOUNCE_MS 1\n"
                   "#define ALPHALOC_USB_CONFIG_DISCONNECT_DEBOUNCE_MS 2\n"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
