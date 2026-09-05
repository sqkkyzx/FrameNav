#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Actual GPS-task power helpers with deterministic BLE/UART race injection.

The C policy, transport controller, shared UBX parser and extracted gps.c
helpers run together. Host stubs model call ordering, not receiver hardware;
no serial ports are opened and electrical standby is not asserted.
"""

import unittest

from host_phase1 import PRELUDE, ROOT, function_source, run_c


def gps_source(*names):
    return "\n".join(function_source("src/gps.c", name) for name in names)


HARNESS = """
#define ALPHALOC_GNSS_STANDBY 1
#define ALPHALOC_GNSS_RATE_1HZ 1
#define ALPHALOC_PHASE2 1
#define ALPHALOC_FAKE_GPS 0
""" + PRELUDE + "".join(
    f'#include "{ROOT / ("src/" + name + ".c")}"\n'
    for name in ("gnss_pm_protocol", "gnss_duty", "gnss_power", "gnss_rate")
) + """
#define SEC 1000000LL
#define pdTRUE 1
#define ESP_OK 0
#define portMAX_DELAY INT_MAX
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define VLOGI(...) ((void)0)
static int64_t now_us;
#define esp_timer_get_time() (now_us)
static gnss_power_t s_power;
static gnss_rate_t s_rate;
static gps_config_t s_cfg={.uart_num=1};
static gps_fix_t s_latest_fix;
static gps_status_t s_status;
static int s_fix_mutex=1;
static int64_t s_last_no_fix_log_us;
static int s_camera_lock;
static bool s_camera_connected;
static uint32_t s_camera_generation, s_camera_seen_generation;
static int64_t s_camera_changed_us;
static location_power_policy_t s_pending_power_policy;
static uint32_t s_power_policy_generation, s_power_policy_seen_generation;
static uint32_t s_rmc_count, s_rmc_sample_count;
static int64_t s_rmc_sample_us;
static unsigned critical_depth, critical_exits, connect_after_exit;
static unsigned uart_writes, pm_requests, wake_bytes, config_writes, flushes;
static unsigned held_mutexes;
static bool connect_during_pm_tx;
static bool last_tx_pm;
static int64_t tx_duration_us=2000;
static uint8_t last_config[34];
static size_t last_config_len;
static size_t line_length;
static void mock_enter(int *lock);
static void mock_exit(int *lock);
#define portENTER_CRITICAL(lock) mock_enter(lock)
#define portEXIT_CRITICAL(lock) mock_exit(lock)
static int xSemaphoreTake(int mutex, int ticks) {
  assert(mutex==1 && (ticks==50 || ticks==portMAX_DELAY));
  assert(!held_mutexes); held_mutexes++; return pdTRUE;
}
static void xSemaphoreGive(int mutex) {
  assert(mutex==1 && held_mutexes==1); held_mutexes--;
}
""" + gps_source("gps_camera_connection_changed") + """
static void mock_enter(int *lock) {
  assert(lock==&s_camera_lock && critical_depth==0); critical_depth++;
}
static void mock_exit(int *lock) {
  assert(lock==&s_camera_lock && critical_depth==1); critical_depth--;
  critical_exits++;
  if(connect_after_exit && critical_exits==connect_after_exit) {
    connect_after_exit=0;
    gps_camera_connection_changed(true);
  }
}
static int uart_write_bytes(int uart, const uint8_t *data, size_t len) {
  assert(uart==1 && data && len && critical_depth==0 && !held_mutexes);
  uart_writes++;
  last_tx_pm=false;
  if(len==1) { assert(data[0]==0xff); wake_bytes++; return (int)len; }
  assert(data[0]==0xb5 && data[1]==0x62 && len>=8);
  if(data[2]==2 && data[3]==0x41) {
    assert(len==24); pm_requests++; last_tx_pm=true;
  } else if(data[2]==6) {
    assert(len<=sizeof(last_config));
    memcpy(last_config,data,len); last_config_len=len; config_writes++;
  } else {
    assert(data[2]==0x0a && data[3]==4);
  }
  return (int)len;
}
static int uart_wait_tx_done(int uart, int ticks) {
  assert(uart==1 && ticks==100 && critical_depth==0 && !held_mutexes);
  now_us+=tx_duration_us;
  if(last_tx_pm && connect_during_pm_tx) {
    connect_during_pm_tx=false;
    gps_camera_connection_changed(true);
  }
  return ESP_OK;
}
static void uart_flush_input(int uart) { assert(uart==1); flushes++; }
static int64_t host_clock(void *arg) { (void)arg; return now_us; }
static void rate_log(gnss_rate_event_t event, void *arg) { (void)event; (void)arg; }
static void power_log(gnss_power_event_t event, void *arg) { (void)event; (void)arg; }
""" + gps_source(
    "rate_uart_send", "power_policy_to_duty", "power_sleep_allowed",
    "power_uart_send", "power_frame",
    "power_step", "update_fix", "gps_get_latest", "gps_get_status"
) + function_source("src/main.c", "get_location_for_send") + """
static void init(void) {
  now_us=0; line_length=0;
  s_camera_connected=false;
  s_camera_generation=s_camera_seen_generation=0; s_camera_changed_us=0;
  s_power_policy_generation=s_power_policy_seen_generation=0;
  memset(&s_pending_power_policy,0,sizeof(s_pending_power_policy));
  critical_depth=critical_exits=connect_after_exit=0;
  uart_writes=pm_requests=wake_bytes=config_writes=flushes=held_mutexes=0;
  connect_during_pm_tx=last_tx_pm=false; tx_duration_us=2000;
  memset(&s_latest_fix,0,sizeof(s_latest_fix));
  memset(&s_status,0,sizeof(s_status));
  s_last_no_fix_log_us=0;
  s_rmc_count=s_rmc_sample_count=0; s_rmc_sample_us=0;
  gnss_power_init(&s_power,0,power_uart_send,power_log,NULL);
  s_power.clock=host_clock;
  s_power.sleep_allowed=power_sleep_allowed;
  // Identity validation itself is covered by host_gnss_power.py. Here start
  // at its supported result to isolate actual GPS-task integration behavior.
  s_power.identity_checked=true; s_power.identity.supported=true;
  gnss_rate_init(&s_rate,0,rate_uart_send,rate_log,NULL);
  gnss_rate_set_frame_observer(&s_rate,power_frame,NULL);
}
static gps_fix_t sample(unsigned seconds, double longitude) {
  gps_fix_t fix={.valid=true,.time_valid=true,.year=2026,.month=1,.day=1,
                 .lat_deg=0,.lon_deg=longitude,.last_fix_time_us=now_us};
  fix.hour=seconds/3600; fix.minute=(seconds/60)%60; fix.second=seconds%60;
  return fix;
}
static void step(int64_t at_us) { now_us=at_us; power_step(&line_length); }
"""


def check(body):
    run_c(HARNESS + "int main(void) {\n" +
          "(void)sample; (void)step; (void)update_fix; (void)get_location_for_send;\n" +
          body + "\nassert(!held_mutexes && !critical_depth); return 0; }\n")


class GnssIntegrationChecks(unittest.TestCase):
    def test_coalesced_connect_disconnect_keeps_actual_20_second_grace(self):
        check("""
init();
now_us=100*SEC; gps_camera_connection_changed(true);
now_us=105*SEC; gps_camera_connection_changed(false);
assert(s_camera_generation==2 && s_camera_seen_generation==0);
assert(uart_writes==0 && s_power.duty.grace_deadline_us==0);
step(106*SEC);
assert(!s_power.duty.camera_connected && s_camera_seen_generation==2);
assert(s_power.duty.grace_deadline_us==125*SEC);
assert(!pm_requests && s_power.phase==GNSS_POWER_WORK);
step(125*SEC-1); assert(!pm_requests);
step(125*SEC); assert(pm_requests==1 && !wake_bytes);
// Both edges can also be coalesced while asleep. A disconnect still wakes
// for its grace window, even though the GPS task never observed connected.
now_us=140*SEC; gps_camera_connection_changed(true);
now_us=140*SEC+SEC/2; gps_camera_connection_changed(false);
step(141*SEC);
assert(wake_bytes==1 && s_power.phase==GNSS_POWER_WAKING);
assert(s_power.duty.grace_deadline_us==160*SEC+SEC/2);
gnss_power_note_nmea(&s_power,142*SEC);
step(160*SEC+SEC/2-1); assert(pm_requests==1);
step(160*SEC+SEC/2); assert(pm_requests==2);
""")

    def test_reconnect_cancels_pending_grace_without_waiting_for_old_deadline(self):
        check("""
init();
now_us=50*SEC; gps_camera_connection_changed(true); step(now_us);
now_us=100*SEC; gps_camera_connection_changed(false); step(now_us);
assert(s_power.duty.grace_deadline_us==120*SEC);
now_us=119*SEC; gps_camera_connection_changed(true); step(now_us);
assert(s_power.duty.camera_connected && s_power.duty.grace_deadline_us==0);
step(1000*SEC); assert(!pm_requests && !wake_bytes);
assert(s_power.phase==GNSS_POWER_WORK);
""")

    def test_mailbox_change_vetoes_both_policy_and_final_uart_sleep_guards(self):
        check("""
for(unsigned boundary=1;boundary<=2;boundary++) {
  init();
  // 1: after initial mailbox snapshot; 2: after policy guard sampled its
  // answer, so only the final UART helper can still prevent PMREQ writing.
  connect_after_exit=boundary;
  step(45*SEC);
  assert(!pm_requests);
  assert(s_power.duty.camera_connected);
  assert(s_camera_seen_generation==s_camera_generation);
  assert(!s_power.disable_after_wake);
  if(boundary==1) {
    assert(!uart_writes && s_power.phase==GNSS_POWER_WORK);
  } else {
    assert(wake_bytes==1 && s_power.phase==GNSS_POWER_WAKING);
    gnss_power_note_nmea(&s_power,now_us+1);
  }
  step(500*SEC);
  assert(!pm_requests && s_power.phase==GNSS_POWER_WORK);
}
""")

    def test_camera_connect_during_sleep_tx_wakes_in_same_power_step(self):
        check("""
init(); connect_during_pm_tx=true; tx_duration_us=50000;
step(45*SEC);
assert(pm_requests==1 && wake_bytes==1 && uart_writes==2);
assert(now_us==45*SEC+100000); // PMREQ drain and immediate wake drain
assert(s_power.duty.camera_connected && s_power.phase==GNSS_POWER_WAKING);
assert(s_camera_seen_generation==s_camera_generation);
assert(flushes==1 && !s_power.disable_after_wake);
assert(s_power.sleep_started_us==45*SEC+50000);
assert(s_power.duty.sleep_deadline_us==135*SEC+50000);
gnss_power_note_nmea(&s_power,46*SEC);
step(500*SEC);
assert(s_power.phase==GNSS_POWER_WORK && pm_requests==1 && wake_bytes==1);
""")

    def test_sleep_invalidates_cached_fix_and_wake_resets_parser_and_ram_restore(self):
        check("""
init();
now_us=SEC; gps_fix_t original=sample(1,0); update_fix(&original,true);
s_status.has_lock=true; s_status.satellites=12;
gps_fix_t out; assert(get_location_for_send(&out));
s_rate.stage=GNSS_RATE_VERIFIED;
s_rate.rx_state=2; s_rate.payload_len=200; s_rate.rx_pos=20;
s_rate.ck_a=123; s_rate.ck_b=42;
s_rmc_count=88; s_rmc_sample_count=77; line_length=19;
step(45*SEC);
assert(pm_requests==1 && s_power.phase==GNSS_POWER_SLEEP_SETTLE);
assert(!get_location_for_send(&out));
assert(!s_latest_fix.valid && !s_latest_fix.time_valid);
assert(!s_status.has_lock && !s_status.satellites);
assert(!flushes); // no wake/reset has yet occurred
assert(s_power.duty.sleep_deadline_us==135*SEC+tx_duration_us);
// A snapshot accidentally retained by another path must also be invalidated
// on wake. Parser fragments and rate verification never cross the wake edge.
s_latest_fix=original; s_status.has_lock=true; s_status.satellites=12;
step(s_power.duty.sleep_deadline_us);
assert(s_power.phase==GNSS_POWER_WAKING && wake_bytes==1 && flushes==1);
assert(!get_location_for_send(&out) && !s_status.has_lock && !s_status.satellites);
assert(line_length==0 && s_rate.rx_state==0 && s_rate.rx_pos==0);
assert(s_rate.payload_len==0 && s_rate.ck_a==0 && s_rate.ck_b==0);
assert(s_rate.stage==GNSS_RATE_WAIT_DATA && !s_rate.nmea_seen);
assert(s_rate.frame_observer==power_frame && s_rate.frame_arg==NULL);
assert(!s_rmc_count && !s_rmc_sample_count && s_rmc_sample_us==now_us);
assert(s_power.duty.probe_started_us==now_us);
assert(s_power.duty.probe_deadline_us==now_us+45*SEC);
gnss_rate_poll(&s_rate,now_us+2*SEC); assert(!config_writes);
// Fresh NMEA permits RAM configuration; the retained VERIFIED flag did not.
now_us+=2*SEC; gnss_power_note_nmea(&s_power,now_us);
gnss_rate_note_nmea(&s_rate);
gnss_rate_poll(&s_rate,now_us);
assert(config_writes==1 && s_rate.stage==GNSS_RATE_WAIT_ACK);
assert(last_config_len==34 && last_config[2]==6 && last_config[3]==0x8a);
assert(last_config[6]==0 && last_config[7]==1); // VALSET version0, RAM only
""")

    def test_connected_real_time_fixes_never_sleep_across_speed_bands(self):
        check("""
init();
now_us=50*SEC; gps_camera_connection_changed(true); step(now_us);
const double longitude[]={0,0,0.00001,0.00101};
const unsigned interval[]={90,90,30,15};
for(unsigned i=0;i<4;i++) {
  now_us=(100+i)*SEC;
  gps_fix_t fix=sample(100+i,longitude[i]);
  assert(gnss_power_note_fix(&s_power,&fix,now_us));
  update_fix(&fix,true); power_step(&line_length);
  gps_fix_t out;
  assert(get_location_for_send(&out) && out.lon_deg==longitude[i]);
  assert(out.last_fix_time_us==now_us && s_power.duty.next_sleep_s==interval[i]);
  assert(s_power.phase==GNSS_POWER_WORK && !pm_requests && !wake_bytes);
}
step(10000*SEC);
assert(!pm_requests && !wake_bytes && s_power.phase==GNSS_POWER_WORK);
now_us=10000*SEC+1; gps_camera_connection_changed(false); step(now_us);
step(10020*SEC); assert(!pm_requests);
step(10020*SEC+1); assert(pm_requests==1); // only after full disconnect grace
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
