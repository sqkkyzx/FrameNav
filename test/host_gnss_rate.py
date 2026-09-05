#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Actual UBX control/parser C with host I/O stubs; never open a serial port."""

import configparser
import unittest

from host_phase1 import PRELUDE, ROOT, function_source, run_c
from host_phase2 import BASE, RMC, STATE, nmea, source


MODULE = '#define ALPHALOC_GNSS_RATE_1HZ 1\n' + \
    f'#include "{ROOT / "src/gnss_rate.c"}"\n'

HARNESS = PRELUDE + MODULE + """
static gnss_rate_t state;
static unsigned sends, events[GNSS_RATE_GIVE_UP+1];
static bool tx_ok=true;
static uint8_t last_tx[64];
static size_t last_len;
static bool send_bytes(const uint8_t *p, size_t n, void *arg) {
  assert(arg==&state && n<=sizeof(last_tx));
  memcpy(last_tx,p,n); last_len=n; sends++; return tx_ok;
}
static void log_event(gnss_rate_event_t event, void *arg) {
  assert(arg==&state); events[event]++;
}
static void init(void) {
  sends=0; memset(events,0,sizeof(events)); tx_ok=true;
  gnss_rate_init(&state,0,send_bytes,log_event,&state);
}
static void start(void) {
  init(); gnss_rate_note_nmea(&state); gnss_rate_poll(&state,1000000);
  assert(state.stage==GNSS_RATE_WAIT_ACK && sends==1);
}
static void frame(uint8_t cls, uint8_t id, const uint8_t *data,
                  size_t n, int64_t now, bool corrupt) {
  assert(n<65536);
  uint8_t head[]={cls,id,n & 255,n >> 8};
  assert(gnss_rate_consume(&state,0xb5,now));
  assert(gnss_rate_consume(&state,0x62,now));
  uint8_t a=0,b=0;
  for(size_t i=0;i<4;i++) {
    a+=head[i]; b+=a; assert(gnss_rate_consume(&state,head[i],now));
  }
  for(size_t i=0;i<n;i++) {
    a+=data[i]; b+=a; assert(gnss_rate_consume(&state,data[i],now));
  }
  assert(gnss_rate_consume(&state,a,now));
  assert(gnss_rate_consume(&state,corrupt ? (uint8_t)(b^1):b,now));
}
static void ack(bool accept, uint8_t msg, int64_t now) {
  uint8_t body[]={6,msg}; frame(5,accept?1:0,body,2,now,false);
}
// Independent reference encoding, deliberately a different key order.
static const uint8_t good_readback[]={
  1,0,0,0, 0xbb,0,0x91,0x20,1, 2,0,0x21,0x30,1,0,
  0xac,0,0x91,0x20,1, 1,0,0x21,0x30,0xe8,3
};
"""


def check(body):
    # Suppress unused test helpers only; production code still compiles -Werror.
    run_c(HARNESS + "int main(void) {\n" +
          "(void)start; (void)ack; (void)good_readback;\n" + body + "\nreturn 0; }\n")


class GnssRateChecks(unittest.TestCase):
    def test_ram_only_command_bytes_and_readback_required(self):
        check("""
init();
gnss_rate_poll(&state,60000000); assert(sends==0); // no connected module
gnss_rate_note_nmea(&state);
gnss_rate_poll(&state,999999); assert(sends==0);
gnss_rate_poll(&state,1000000); assert(sends==1 && last_len==34);
const uint8_t expected[]={
  0xb5,0x62,6,0x8a,26,0, 0,1,0,0,
  1,0,0x21,0x30,0xe8,3, 2,0,0x21,0x30,1,0,
  0xac,0,0x91,0x20,1, 0xbb,0,0x91,0x20,1
};
assert(memcmp(last_tx,expected,sizeof(expected))==0);
uint8_t a=0,b=0;
for(size_t i=2;i<sizeof(expected);i++) { a+=expected[i]; b+=a; }
assert(last_tx[32]==a && last_tx[33]==b);
ack(true,0x8a,1100000);
assert(state.acknowledged && state.stage==GNSS_RATE_READBACK);
assert(!events[GNSS_RATE_CONFIRMED]);
gnss_rate_poll(&state,1200000);
assert(sends==2 && last_len==28 && last_tx[3]==0x8b);
assert(last_tx[6]==0 && last_tx[7]==0 && last_tx[8]==0 && last_tx[9]==0);
const uint8_t keys[]={1,0,0x21,0x30,2,0,0x21,0x30,0xac,0,0x91,0x20,0xbb,0,0x91,0x20};
assert(memcmp(last_tx+10,keys,sizeof(keys))==0);
frame(6,0x8b,good_readback,sizeof(good_readback),1300000,false);
assert(state.stage==GNSS_RATE_VERIFIED && events[GNSS_RATE_CONFIRMED]==1);
assert(state.meas_ms==1000 && state.nav_ratio==1 && state.rmc_rate==1 && state.gga_rate==1);
for(int i=0;i<100;i++) gnss_rate_poll(&state,100000000LL+i*1000000);
assert(sends==2); // no repeated writes once verified
""")

    def test_lost_ack_can_be_verified_by_readback_but_not_by_unrelated_ack(self):
        check("""
start(); ack(true,0x01,1100000);
assert(!state.acknowledged && state.stage==GNSS_RATE_WAIT_ACK);
gnss_rate_poll(&state,2500000);
assert(sends==2 && events[GNSS_RATE_ACK_TIMEOUT]==1);
frame(6,0x8b,good_readback,sizeof(good_readback),2600000,false);
assert(state.stage==GNSS_RATE_VERIFIED && !state.acknowledged);
ack(false,0x8a,2700000); assert(state.stage==GNSS_RATE_VERIFIED); // late response
""")

    def test_rejection_stops_commands_without_disabling_nmea(self):
        check("""
start(); ack(false,0x8a,1100000);
assert(state.stage==GNSS_RATE_FAILED && events[GNSS_RATE_NAK]==1);
gnss_rate_poll(&state,100000000); assert(sends==1);
assert(!gnss_rate_consume(&state,'$',100000000));
start(); ack(true,0x8a,1100000); gnss_rate_poll(&state,1200000);
ack(false,0x8b,1300000);
assert(state.stage==GNSS_RATE_FAILED && sends==2);
""")

    def test_timeouts_and_uart_failures_have_a_three_attempt_limit(self):
        check("""
start();
for(int64_t t=1000000;t<60000000;t+=200000) gnss_rate_poll(&state,t);
assert(state.stage==GNSS_RATE_FAILED && state.attempts==3);
assert(sends==6 && events[GNSS_RATE_GIVE_UP]==1);
assert(events[GNSS_RATE_ACK_TIMEOUT]==3 && events[GNSS_RATE_READ_TIMEOUT]==3);
init(); tx_ok=false; gnss_rate_note_nmea(&state);
for(int64_t t=0;t<60000000;t+=200000) gnss_rate_poll(&state,t);
assert(sends==3 && state.stage==GNSS_RATE_FAILED && events[GNSS_RATE_TX_ERROR]==3);
""")

    def test_readback_mismatch_retries_without_claiming_success(self):
        check("""
start(); ack(true,0x8a,1100000); gnss_rate_poll(&state,1200000);
uint8_t bad[sizeof(good_readback)]; memcpy(bad,good_readback,sizeof(bad));
bad[24]=100; bad[25]=0; // still 10Hz
frame(6,0x8b,bad,sizeof(bad),1300000,false);
assert(state.stage==GNSS_RATE_RETRY && events[GNSS_RATE_MISMATCH]==1);
assert(!events[GNSS_RATE_CONFIRMED]);
gnss_rate_poll(&state,2200000); assert(sends==2);
gnss_rate_poll(&state,2300000); assert(sends==3 && state.attempts==2);
ack(true,0x8a,2400000); gnss_rate_poll(&state,2500000);
frame(6,0x8b,good_readback,sizeof(good_readback),2600000,false);
assert(state.stage==GNSS_RATE_VERIFIED && events[GNSS_RATE_CONFIRMED]==1);
""")

    def test_readback_requires_version_layer_all_keys_and_no_duplicates(self):
        check("""
start(); ack(true,0x8a,1100000); gnss_rate_poll(&state,1200000);
uint8_t bad[sizeof(good_readback)];
const unsigned offsets[]={0,1,2,3,4,9,15,20};
for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
  memcpy(bad,good_readback,sizeof(bad)); bad[offsets[i]]^=0x80;
  frame(6,0x8b,bad,sizeof(bad),1300000,false);
  assert(state.stage==GNSS_RATE_WAIT_READBACK && !events[GNSS_RATE_CONFIRMED]);
}
memcpy(bad,good_readback,sizeof(bad)); bad[20]=2; // duplicate NAV, missing MEAS
frame(6,0x8b,bad,sizeof(bad),1300000,false);
assert(state.stage==GNSS_RATE_WAIT_READBACK);
for(unsigned n=0;n<sizeof(good_readback);n++) {
  frame(6,0x8b,good_readback,n,1300000,false);
  assert(state.stage==GNSS_RATE_WAIT_READBACK);
}
""")

    def test_bad_checksums_and_unknown_binary_do_not_look_like_nmea(self):
        check("""
start(); uint8_t body[]={6,0x8a};
frame(5,1,body,2,1100000,true);
assert(state.stage==GNSS_RATE_WAIT_ACK && !state.acknowledged && state.ubx_bad==1);
uint8_t oversized[300]; memset(oversized,'$',sizeof(oversized));
oversized[5]='\\r'; oversized[6]='\\n'; oversized[7]=0xb5; oversized[8]=0x62;
frame(1,7,oversized,sizeof(oversized),1200000,false);
assert(state.ubx_ok==1 && !state.acknowledged);
assert(!gnss_rate_consume(&state,'$',1300000));
ack(true,0x8a,1400000); assert(state.acknowledged);
""")

    def test_fragmented_frames_false_sync_and_truncated_frame_recovery(self):
        check("""
start();
assert(gnss_rate_consume(&state,0xb5,1100000));
assert(gnss_rate_consume(&state,0xb5,1100100));
assert(!gnss_rate_consume(&state,'$',1100200));
const uint8_t ack_bytes[]={0xb5,0x62,5,1,2,0,6,0x8a,0x98,0xc1};
for(unsigned i=0;i<sizeof(ack_bytes);i++)
  assert(gnss_rate_consume(&state,ack_bytes[i],1200000+i*20000));
assert(state.acknowledged && state.ubx_ok==1);
const uint8_t corrupt_length[]={0xb5,0x62,1,7,0xff,0xff};
for(unsigned i=0;i<sizeof(corrupt_length);i++)
  assert(gnss_rate_consume(&state,corrupt_length[i],1500000));
assert(!gnss_rate_consume(&state,'$',2500001));
assert(state.ubx_bad==1 && state.rx_state==0);
""")

    def test_actual_gps_task_keeps_nmea_valid_across_interleaved_binary(self):
        good = nmea("GNRMC,120000.00,A,2234.5678,N,11345.1234,E,0,0,030926,,,A,V")
        gga = nmea("GNGGA,120000.00,2234.5678,N,11345.1234,E,1,08,0.9,1.0,M,0.0,M,,")
        # Embed a valid fake-location NMEA line inside an unknown UBX payload.
        embedded = (nmea("GNRMC,120001.00,A,4808.22924,N,1134.56744,E,0,0,030926,,,A,V") + "\r\n").encode()
        ubx = bytearray([0xb5,0x62,1,7,len(embedded),0]) + embedded
        a = b = 0
        for byte in ubx[2:]:
            a = (a + byte) & 255
            b = (b + a) & 255
        ubx += bytes([a,b])
        data = (good+"\r\n").encode() + ubx + (gga+"\r\n"+good+"\r\n").encode()
        fixture = ','.join(str(b) for b in data)
        run_c(BASE + MODULE + RMC + STATE + source(
            "hex_value", "nmea_checksum_valid", "parse_int_range", "update_status_gga", "parse_zda") + """
#include <setjmp.h>
static jmp_buf stop;
static gnss_rate_t s_rate;
static gps_config_t s_cfg={.uart_num=1};
static uint32_t s_uart_bytes,s_nmea_ok,s_nmea_bad,s_rmc_count;
static int64_t s_rmc_sample_us;
static void log_uart_status(void) {}
static void vTaskDelay(int ms) { (void)ms; }
static bool rate_uart_send(const uint8_t *data,size_t len,void *arg) {
  (void)data; (void)len; (void)arg; return true;
}
static void rate_log(gnss_rate_event_t event,void *arg) { (void)event; (void)arg; }
""" + f"static const uint8_t fixture[]={{ {fixture} }};\n" + """
static int uart_read_bytes(int uart, uint8_t *out,size_t max,int ticks) {
  static size_t offset;
  assert(uart==1 && ticks==200 && max>=17);
  if(offset==sizeof(fixture)) longjmp(stop,1);
  size_t n=sizeof(fixture)-offset; if(n>17)n=17;
  memcpy(out,fixture+offset,n); offset+=n; now_us+=10000; return (int)n;
}
""" + source("gps_task") + """
int main(void) {
  if(setjmp(stop)==0) gps_task(NULL);
  assert(s_nmea_ok==3 && s_nmea_bad==0 && s_rmc_count==2);
  assert(s_rate.ubx_ok==1 && s_rate.nmea_seen && s_rmc_sample_us>0);
  assert(s_latest_fix.valid && s_latest_fix.time_valid);
  assert(fabs(s_latest_fix.lat_deg-(22+34.5678/60))<1e-9);
  assert(s_status.satellites==8 && s_uart_bytes==sizeof(fixture));
  return 0;
}
""")

    def test_observed_rmc_rate_is_measured_not_assumed_from_ack(self):
        run_c(PRELUDE + MODULE + """
#include <inttypes.h>
#define ALPHALOC_GNSS_STATUS_PERIOD_US 30000000LL
static gnss_rate_t s_rate;
static uint32_t s_uart_bytes,s_nmea_ok,s_nmea_bad,s_rmc_count,s_rmc_sample_count;
static int64_t now_us,s_last_uart_log_us,s_rmc_sample_us;
static char last_log[256];
#define esp_timer_get_time() (now_us)
#define ESP_LOGI(tag,fmt,...) snprintf(last_log,sizeof(last_log),fmt,__VA_ARGS__)
bool gps_get_latest(gps_fix_t *fix) { (void)fix; return true; }
bool gps_get_status(gps_status_t *status) { (void)status; return true; }
""" + source("log_uart_status") + """
int main(void) {
  now_us=30000000; s_rmc_count=300; log_uart_status();
  assert(strstr(last_log,"received=10.00Hz") && strstr(last_log,"unconfirmed"));
  s_rate.stage=GNSS_RATE_VERIFIED;
  now_us=60000000; s_rmc_count+=30; log_uart_status();
  assert(strstr(last_log,"received=1.00Hz") && strstr(last_log,"1Hz_RAM=verified"));
  now_us=90000000; log_uart_status();
  assert(strstr(last_log,"received=0.00Hz")); // ACK/readback never invent input
  return 0;
}
""")

    def test_1hz_only_enabled_in_power_profile_independently_of_ble_period(self):
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(ROOT / 'platformio.ini')
        self.assertIn('ALPHALOC_GNSS_RATE_1HZ=1', cfg['env:xiao-c6-gnss-power']['build_flags'])
        for env in ('xiao-c6-debug', 'xiao-c6-gnss-debug'):
            self.assertNotIn('ALPHALOC_GNSS_RATE_1HZ=1', cfg[f'env:{env}']['build_flags'])
        boot = function_source('src/main.c', 'app_main')
        self.assertIn('s_cfg.gps_interval_ms = ALPHALOC_LOCATION_INTERVAL_MS;', boot)
        self.assertIn('s_cfg.max_gps_age_s = 5;', boot)
        self.assertIn('GPS_UART_BAUD=115200', cfg['env:xiao-c6-gnss-power']['build_flags'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
