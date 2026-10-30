#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Actual bounded PMREQ/MON-VER C helpers; no serial ports or GNSS hardware."""

import struct
import unittest

from host_phase1 import ROOT, run_c


HARNESS = """
#include <assert.h>
#include <stdint.h>
#include <string.h>
""" + f'#include "{ROOT / "src/gnss_pm_protocol.c"}"\n'

IDENTITY = """
static void put_slot(uint8_t *out, size_t width, const char *value) {
  assert(strlen(value) < width);
  memset(out, 0, width); memcpy(out, value, strlen(value));
}
static void identity(uint8_t *payload) {
  memset(payload, 0, 130);
  put_slot(payload, 30, "ROM SPG 5.10 (10ca7e)");
  put_slot(payload + 30, 10, "000A0000");
  put_slot(payload + 40, 30, "MOD=MAX-M10S");
  put_slot(payload + 70, 30, "FWVER=SPG 5.10");
  put_slot(payload + 100, 30, "PROTVER=34.10");
}
"""


def check(body, identity_helpers=False):
    run_c(HARNESS + (IDENTITY if identity_helpers else "") +
          "int main(void) {\n" + body + "\nreturn 0; }\n")


def pmreq_reference(duration):
    body = bytes([2, 0x41, 16, 0]) + struct.pack("<4I", 0, duration, 6, 8)
    # Weighted sums independently check both UBX checksums and their coverage.
    a = sum(body) & 255
    b = sum((len(body) - i) * value for i, value in enumerate(body)) & 255
    return bytes([0xb5, 0x62]) + body + bytes([a, b])


class GnssPmProtocolChecks(unittest.TestCase):
    def test_mon_ver_poll_exact_frame(self):
        check("""
uint8_t frame[10]; memset(frame, 0xa5, sizeof(frame));
const uint8_t expected[]={0xb5,0x62,0x0a,0x04,0,0,0x0e,0x34};
assert(gnss_pm_build_mon_ver(frame+1,8)==sizeof(expected));
assert(memcmp(frame+1,expected,sizeof(expected))==0);
assert(frame[0]==0xa5 && frame[9]==0xa5);
""")

    def test_finite_backup_force_uart_wake_frames(self):
        for duration in (1, 15000, 30000, 90000, 1036800000):
            with self.subTest(duration=duration):
                expected = ",".join(str(b) for b in pmreq_reference(duration))
                check(f"""
uint8_t frame[26]; memset(frame,0xa5,sizeof(frame));
const uint8_t expected[]={{ {expected} }};
assert(gnss_pm_build_request(frame+1,24,{duration}U)==sizeof(expected));
assert(memcmp(frame+1,expected,sizeof(expected))==0);
assert(frame[0]==0xa5 && frame[25]==0xa5);
""")

    def test_builders_reject_small_capacity_null_and_invalid_duration_without_writes(self):
        check("""
uint8_t frame[32], original[32]; memset(frame,0xa5,sizeof(frame));
memcpy(original,frame,sizeof(frame));
for(size_t cap=0;cap<24;cap++) {
  assert(!gnss_pm_build_request(frame,cap,30000));
  assert(memcmp(frame,original,sizeof(frame))==0);
}
for(size_t cap=0;cap<8;cap++) {
  assert(!gnss_pm_build_mon_ver(frame,cap));
  assert(memcmp(frame,original,sizeof(frame))==0);
}
assert(!gnss_pm_build_mon_ver(NULL,8));
assert(!gnss_pm_build_request(NULL,24,30000));
assert(!gnss_pm_build_request(frame,32,GNSS_PM_MAX_SLEEP_MS+1));
assert(!gnss_pm_build_request(frame,32,UINT32_MAX));
assert(memcmp(frame,original,sizeof(frame))==0);
assert(gnss_pm_build_request(frame,32,0)==24);
assert(frame[10]==0 && frame[11]==0 && frame[12]==0 && frame[13]==0);
""")

    def test_known_identity_module_optional_and_extension_order_independent(self):
        check("""
uint8_t payload[160]; identity(payload);
gnss_pm_identity_t result;
assert(gnss_pm_parse_mon_ver(payload,130,&result) && result.supported);
assert(strcmp(result.swversion,"ROM SPG 5.10 (10ca7e)")==0);
assert(strcmp(result.hw,"000A0000")==0);
assert(strcmp(result.fwversion,"SPG 5.10")==0);
assert(strcmp(result.protversion,"34.10")==0);
assert(strcmp(result.mod,"MAX-M10S")==0);
put_slot(payload+130,30,"GPS;GLO;GAL;BDS");
assert(gnss_pm_parse_mon_ver(payload,160,&result) && result.supported);
uint8_t temp[30]; memcpy(temp,payload+70,30);
memcpy(payload+70,payload+100,30); memcpy(payload+100,temp,30);
put_slot(payload+40,30,"");
assert(gnss_pm_parse_mon_ver(payload,130,&result) && result.supported);
assert(!result.mod[0]);
""", identity_helpers=True)

    def test_unknown_missing_versions_and_prefix_matches_never_enable_sleep(self):
        check("""
uint8_t payload[130]; gnss_pm_identity_t result;
const char *unknown_fw[]={"SPG 5.00","SPG 5.20","SPG 5.10X","SPG 5.10 ","HPG 5.10"};
for(size_t i=0;i<sizeof(unknown_fw)/sizeof(unknown_fw[0]);i++) {
  identity(payload); char extension[30]="FWVER=";
  strcat(extension,unknown_fw[i]); put_slot(payload+70,30,extension);
  assert(gnss_pm_parse_mon_ver(payload,130,&result) && !result.supported);
}
const char *unknown_proto[]={"34.00","34.20","34.100","34.10 ","34"};
for(size_t i=0;i<sizeof(unknown_proto)/sizeof(unknown_proto[0]);i++) {
  identity(payload); char extension[30]="PROTVER=";
  strcat(extension,unknown_proto[i]); put_slot(payload+100,30,extension);
  assert(gnss_pm_parse_mon_ver(payload,130,&result) && !result.supported);
}
identity(payload);
assert(gnss_pm_parse_mon_ver(payload,40,&result) && !result.supported);
assert(gnss_pm_parse_mon_ver(payload,70,&result) && !result.supported);
assert(gnss_pm_parse_mon_ver(payload,100,&result) && !result.supported);
put_slot(payload+70,30,"OTHER=SPG 5.10");
assert(gnss_pm_parse_mon_ver(payload,130,&result) && !result.supported);
identity(payload); put_slot(payload+100,30,"OTHER=34.10");
assert(gnss_pm_parse_mon_ver(payload,130,&result) && !result.supported);
""", identity_helpers=True)

    def test_malformed_lengths_clear_previous_supported_identity(self):
        check("""
uint8_t payload[GNSS_PM_MON_VER_MAX_PAYLOAD+30]; identity(payload);
gnss_pm_identity_t result, zero={0};
for(size_t len=0;len<130;len++) {
  if(len>=40 && (len-40)%30==0) continue;
  assert(gnss_pm_parse_mon_ver(payload,130,&result) && result.supported);
  assert(!gnss_pm_parse_mon_ver(payload,len,&result));
  assert(memcmp(&result,&zero,sizeof(result))==0);
}
assert(!gnss_pm_parse_mon_ver(NULL,130,&result));
assert(!gnss_pm_parse_mon_ver(payload,130,NULL));
assert(!gnss_pm_parse_mon_ver(payload,512,&result));
assert(!gnss_pm_parse_mon_ver(payload,520,&result));
assert(!gnss_pm_parse_mon_ver(payload,SIZE_MAX,&result));
memset(payload+130,0,360);
assert(gnss_pm_parse_mon_ver(payload,490,&result) && result.supported);
""", identity_helpers=True)

    def test_ascii_slot_bounds_and_termination(self):
        check("""
uint8_t payload[130]; gnss_pm_identity_t result, zero={0};
const size_t offsets[]={0,30,40,70,100};
const size_t widths[]={30,10,30,30,30};
for(size_t i=0;i<5;i++) {
  identity(payload); memset(payload+offsets[i],'A',widths[i]);
  assert(!gnss_pm_parse_mon_ver(payload,130,&result));
  assert(memcmp(&result,&zero,sizeof(result))==0);
  identity(payload); payload[offsets[i]]=0x1f;
  assert(!gnss_pm_parse_mon_ver(payload,130,&result));
  identity(payload); payload[offsets[i]]=0x80;
  assert(!gnss_pm_parse_mon_ver(payload,130,&result));
}
identity(payload); memset(payload,'A',29); payload[29]=0;
memset(payload+30,'B',9); payload[39]=0;
assert(gnss_pm_parse_mon_ver(payload,130,&result) && result.supported);
assert(strlen(result.swversion)==29 && strlen(result.hw)==9);
assert(result.swversion[30]==0 && result.hw[10]==0);
// Bytes beyond each string terminator are padding, not another identity.
identity(payload); payload[29]=0xff; payload[39]=0xff; payload[69]=0xff;
assert(gnss_pm_parse_mon_ver(payload,130,&result) && result.supported);
identity(payload); payload[0]=0;
assert(!gnss_pm_parse_mon_ver(payload,130,&result));
identity(payload); payload[30]=0;
assert(!gnss_pm_parse_mon_ver(payload,130,&result));
""", identity_helpers=True)

    def test_empty_or_duplicate_known_identity_fields_are_rejected(self):
        check("""
uint8_t payload[160]; gnss_pm_identity_t result, zero={0};
const char *duplicates[]={"FWVER=SPG 5.10","FWVER=SPG 5.20",
                         "PROTVER=34.10","PROTVER=34.20","MOD=MAX-M10S"};
for(size_t i=0;i<sizeof(duplicates)/sizeof(duplicates[0]);i++) {
  identity(payload); put_slot(payload+130,30,duplicates[i]);
  assert(!gnss_pm_parse_mon_ver(payload,160,&result));
  assert(memcmp(&result,&zero,sizeof(result))==0);
}
const char *empty[]={"MOD=","FWVER=","PROTVER="};
for(size_t i=0;i<3;i++) {
  identity(payload); put_slot(payload+40+i*30,30,empty[i]);
  assert(!gnss_pm_parse_mon_ver(payload,130,&result));
}
""", identity_helpers=True)

    def test_request_nak_requires_exact_class_id_payload_and_length(self):
        check("""
const uint8_t pmreq[]={2,0x41}, other[]={6,0x8a}, bad[]={2,0x40};
assert(gnss_pm_is_request_nak(5,0,pmreq,2));
assert(!gnss_pm_is_request_nak(5,1,pmreq,2));
assert(!gnss_pm_is_request_nak(4,0,pmreq,2));
assert(!gnss_pm_is_request_nak(5,0,other,2));
assert(!gnss_pm_is_request_nak(5,0,bad,2));
assert(!gnss_pm_is_request_nak(5,0,NULL,2));
assert(!gnss_pm_is_request_nak(5,0,pmreq,0));
assert(!gnss_pm_is_request_nak(5,0,pmreq,1));
assert(!gnss_pm_is_request_nak(5,0,pmreq,3));
""")


if __name__ == "__main__":
    unittest.main()
