#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Actual GNSS power, duty, UBX and rate C with deterministic host transport.

These tests verify protocol/control behavior. UART silence does not establish
that a receiver entered backup electrically or measure its current consumption.
No serial ports or attached hardware are accessed.
"""

import unittest

from host_phase1 import ROOT, run_c


HARNESS = """
#define ALPHALOC_GNSS_STANDBY 1
#define ALPHALOC_GNSS_RATE_1HZ 1
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
""" + "".join(f'#include "{ROOT / ("src/" + name + ".c")}"\n'
              for name in ("gnss_pm_protocol", "gnss_duty", "gnss_power", "gnss_rate")) + """
#define SEC 1000000LL
static gnss_power_t power;
static unsigned sends, polls, requests, wakes, cfg_sends;
static unsigned observed_frames;
static unsigned events[GNSS_POWER_WAKE_FAILED+1];
static uint8_t last_request[24];
static bool fail_pm, fail_wake, fail_identity;
static int64_t fake_now_us, pm_tx_delay_us, wake_tx_delay_us;
static bool allow_sleep, cancel_pm_in_send;
static unsigned guard_checks;
static int64_t clock_now(void *arg) {
  assert(arg==&power); return fake_now_us;
}
static bool sleep_guard(void *arg) {
  assert(arg==&power); guard_checks++; return allow_sleep;
}
static bool send_bytes(const uint8_t *data, size_t len, void *arg) {
  assert(arg==&power && data && len);
  sends++;
  if (len==1) {
    assert(data[0]==0xff); wakes++; fake_now_us+=wake_tx_delay_us;
    return !fail_wake;
  }
  assert(data[0]==0xb5 && data[1]==0x62);
  if (data[2]==0x0a && data[3]==4) {
    assert(len==8); polls++; return !fail_identity;
  }
  if (data[2]==2 && data[3]==0x41) {
    assert(len==24); memcpy(last_request,data,len); requests++;
    fake_now_us+=pm_tx_delay_us;
    if(cancel_pm_in_send) { allow_sleep=false; return false; }
    return !fail_pm;
  }
  assert(data[2]==6); cfg_sends++; return true;
}
static void log_power(gnss_power_event_t event, void *arg) {
  assert(arg==&power); events[event]++;
}
static void init(void) {
  sends=polls=requests=wakes=cfg_sends=observed_frames=0;
  fail_pm=fail_wake=fail_identity=false;
  fake_now_us=pm_tx_delay_us=wake_tx_delay_us=0;
  allow_sleep=true; cancel_pm_in_send=false; guard_checks=0;
  memset(events,0,sizeof(events)); memset(last_request,0,sizeof(last_request));
  gnss_power_init(&power,0,send_bytes,log_power,&power);
}
static void mon_ver(bool known, int64_t now) {
  uint8_t payload[130]={0};
  memcpy(payload,"ROM SPG 5.10 (10ca7e)",21);
  memcpy(payload+30,"000A0000",8);
  memcpy(payload+40,known ? "FWVER=SPG 5.10":"FWVER=SPG 5.20",14);
  memcpy(payload+70,known ? "PROTVER=34.10":"PROTVER=34.20",14);
  memcpy(payload+100,"MOD=MAX-M10S",12);
  gnss_power_frame(&power,0x0a,4,payload,sizeof(payload),now);
}
static void ready(void) {
  init(); gnss_power_note_nmea(&power,0); gnss_power_poll(&power,SEC);
  assert(polls==1); mon_ver(true,SEC+1);
  assert(power.identity_checked && power.identity.supported);
  assert(power.phase==GNSS_POWER_WORK);
}
static void asleep(void) {
  ready(); gnss_power_poll(&power,45*SEC);
  assert(power.phase==GNSS_POWER_SLEEP_SETTLE && requests==1);
  gnss_power_poll(&power,46*SEC+SEC/2);
  assert(power.phase==GNSS_POWER_SLEEP && events[GNSS_POWER_SLEEP_QUIET]==1);
}
static gps_fix_t sample(unsigned seconds, double longitude) {
  gps_fix_t fix={.valid=true,.time_valid=true,.year=2026,.month=1,.day=1,
                 .lat_deg=0,.lon_deg=longitude};
  fix.hour=seconds/3600; fix.minute=(seconds/60)%60; fix.second=seconds%60;
  return fix;
}
static uint32_t request_duration(void) {
  return (uint32_t)last_request[10] | ((uint32_t)last_request[11]<<8) |
         ((uint32_t)last_request[12]<<16) | ((uint32_t)last_request[13]<<24);
}
static void observe(uint8_t cls,uint8_t id,const uint8_t *data,size_t len,
                    int64_t now,void *arg) {
  assert(arg==&power); observed_frames++;
  gnss_power_frame(&power,cls,id,data,len,now);
}
static void receive_ubx(gnss_rate_t *rate,uint8_t cls,uint8_t id,
                        const uint8_t *data,size_t len,int64_t now,bool corrupt) {
  const uint8_t head[]={cls,id,len&255,len>>8};
  assert(gnss_rate_consume(rate,0xb5,now));
  assert(gnss_rate_consume(rate,0x62,now));
  uint8_t a=0,b=0;
  for(size_t i=0;i<4;i++) {
    a+=head[i]; b+=a; assert(gnss_rate_consume(rate,head[i],now));
  }
  for(size_t i=0;i<len;i++) {
    a+=data[i]; b+=a; assert(gnss_rate_consume(rate,data[i],now));
  }
  assert(gnss_rate_consume(rate,a,now));
  assert(gnss_rate_consume(rate,corrupt ? (uint8_t)(b^1):b,now));
}
"""


def check(body):
    run_c(HARNESS + "int main(void) {\n" + """
(void)ready; (void)asleep; (void)sample; (void)request_duration;
(void)observe; (void)receive_ubx;
(void)clock_now; (void)sleep_guard;
""" + body + "\nreturn 0; }\n")


class GnssPowerChecks(unittest.TestCase):
    def test_sleep_deadline_and_drain_window_start_after_transmit_completes(self):
        check("""
ready(); power.clock=clock_now; power.sleep_allowed=sleep_guard;
fake_now_us=45*SEC; pm_tx_delay_us=300000;
gnss_power_poll(&power,45*SEC);
assert(requests==1 && guard_checks==1 && request_duration()==90000);
assert(power.sleep_started_us==45*SEC+300000);
assert(power.duty.sleep_deadline_us==135*SEC+300000);
// Buffered output 0.4 s after TX drain is still within the drain allowance.
gnss_power_note_nmea(&power,45*SEC+700000);
gnss_power_poll(&power,46*SEC+799999);
assert(power.phase==GNSS_POWER_SLEEP_SETTLE && !wakes);
gnss_power_poll(&power,46*SEC+800000);
assert(power.phase==GNSS_POWER_SLEEP && events[GNSS_POWER_SLEEP_QUIET]==1);
gnss_power_poll(&power,135*SEC+299999); assert(!wakes);
fake_now_us=135*SEC+300000;
gnss_power_poll(&power,135*SEC+300000);
assert(power.phase==GNSS_POWER_WAKING && wakes==1);
""")

    def test_wake_wait_and_acquisition_deadlines_use_command_completion(self):
        check("""
asleep(); power.clock=clock_now;
fake_now_us=135*SEC; wake_tx_delay_us=250000;
gnss_power_poll(&power,135*SEC);
assert(power.duty.probe_started_us==135*SEC+250000);
assert(power.duty.probe_deadline_us==180*SEC+250000);
assert(power.wake_deadline_us==137*SEC+250000);
gnss_power_poll(&power,137*SEC+249999); assert(wakes==1);
fake_now_us=137*SEC+250000;
gnss_power_poll(&power,137*SEC+250000);
assert(wakes==2 && power.wake_deadline_us==139*SEC+500000);
assert(power.duty.probe_started_us==135*SEC+250000);
assert(power.duty.probe_deadline_us==180*SEC+250000);
// Neither a retry nor delayed first NMEA restarts the 45-second acquisition.
gnss_power_note_nmea(&power,138*SEC);
assert(power.phase==GNSS_POWER_WORK && !power.disable_after_wake);
gnss_power_poll(&power,180*SEC+249999); assert(requests==1);
fake_now_us=180*SEC+250000;
gnss_power_poll(&power,180*SEC+250000);
assert(requests==2 && power.phase==GNSS_POWER_SLEEP_SETTLE);
assert(power.sleep_started_us==180*SEC+250000);
""")

    def test_final_mailbox_guard_prevents_request_and_cache_invalidation(self):
        check("""
ready(); power.sleep_allowed=sleep_guard; allow_sleep=false;
gnss_power_poll(&power,45*SEC);
gnss_power_poll(&power,100*SEC);
assert(guard_checks==2 && requests==0 && wakes==0);
assert(sends==1 && power.phase==GNSS_POWER_WORK);
assert(!gnss_power_take_actions(&power));
assert(!gnss_duty_is_sleeping(&power.duty));
// The pending BLE event is subsequently delivered by the GPS task.
gnss_power_camera_event(&power,true,45*SEC);
allow_sleep=true;
gnss_power_poll(&power,200*SEC); assert(!requests && guard_checks==2);
gnss_power_camera_event(&power,false,200*SEC);
gnss_power_poll(&power,220*SEC);
assert(requests==1 && guard_checks==3 && power.phase==GNSS_POWER_SLEEP_SETTLE);
""")

    def test_ble_arrival_during_sleep_transmit_cancels_with_wake_without_disabling(self):
        check("""
ready(); power.clock=clock_now; power.sleep_allowed=sleep_guard;
fake_now_us=45*SEC; pm_tx_delay_us=300000; wake_tx_delay_us=100000;
cancel_pm_in_send=true;
gnss_power_poll(&power,45*SEC);
assert(requests==1 && wakes==1 && guard_checks==2);
assert(power.phase==GNSS_POWER_WAKING && !power.disable_after_wake);
assert(!events[GNSS_POWER_SLEEP_FAILED] && !events[GNSS_POWER_SLEEP_SENT]);
assert(!events[GNSS_POWER_WAKE_FAILED]);
assert(power.wake_deadline_us==47*SEC+400000);
assert(power.duty.probe_started_us==45*SEC+400000);
assert(gnss_power_take_actions(&power)==(GNSS_POWER_INVALIDATE_FIX|GNSS_POWER_RESET_RX));
// Deliver the successful link event from the mailbox after TX returns.
gnss_power_camera_event(&power,true,45*SEC+100000);
gnss_power_note_nmea(&power,46*SEC);
assert(power.phase==GNSS_POWER_WORK && power.duty.camera_connected);
gnss_power_poll(&power,200*SEC); assert(requests==1 && wakes==1);
// A canceled race must not disable future standby after an ordinary disconnect.
cancel_pm_in_send=false; allow_sleep=true;
gnss_power_camera_event(&power,false,200*SEC);
fake_now_us=220*SEC; gnss_power_poll(&power,220*SEC);
assert(requests==2 && power.phase==GNSS_POWER_SLEEP_SETTLE);
assert(!power.disable_after_wake && events[GNSS_POWER_SLEEP_SENT]==1);
""")

    def test_boot_work_45_seconds_capability_gate_and_silence_without_ack(self):
        check("""
init(); assert(gnss_power_receiving(&power));
mon_ver(true,0); assert(!power.identity_checked); // no outstanding query
gnss_power_poll(&power,100*SEC); assert(!sends); // no GNSS traffic yet
gnss_power_note_nmea(&power,0);
gnss_power_poll(&power,SEC-1); assert(!sends);
gnss_power_poll(&power,SEC); assert(polls==1 && !requests);
gnss_power_poll(&power,SEC+1); assert(!requests);
mon_ver(true,SEC+2); assert(events[GNSS_POWER_ID_OK]==1);
gnss_power_poll(&power,45*SEC-1); assert(!requests);
gnss_power_poll(&power,45*SEC);
assert(requests==1 && power.phase==GNSS_POWER_SLEEP_SETTLE);
assert(!gnss_power_receiving(&power) && request_duration()==90000);
assert(gnss_power_take_actions(&power)==GNSS_POWER_INVALIDATE_FIX);
assert(gnss_power_take_actions(&power)==0);
gps_fix_t fix=sample(45,0);
assert(!gnss_power_note_fix(&power,&fix,45*SEC));
// Buffered bytes during the documented drain window do not prove failure.
gnss_power_note_nmea(&power,45*SEC+499999);
gnss_power_poll(&power,46*SEC+499999);
assert(power.phase==GNSS_POWER_SLEEP_SETTLE);
assert(!events[GNSS_POWER_SLEEP_QUIET]);
// No ACK was supplied. This transition records UART quiet, not power/current.
gnss_power_poll(&power,46*SEC+500000);
assert(power.phase==GNSS_POWER_SLEEP && events[GNSS_POWER_SLEEP_QUIET]==1);
assert(sends==2 && !wakes && !gnss_power_receiving(&power));
""")

    def test_actual_speed_policy_drives_exact_finite_request_and_deadline(self):
        check("""
const double step[]={0,0.00001,0.001};
const uint32_t duration[]={90000,30000,15000};
for(unsigned i=0;i<3;i++) {
  ready(); gps_fix_t a=sample(10,0),b=sample(11,step[i]);
  assert(gnss_power_note_fix(&power,&a,10*SEC));
  assert(gnss_power_note_fix(&power,&b,11*SEC));
  gnss_power_poll(&power,45*SEC);
  assert(request_duration()==duration[i]);
  assert(last_request[6]==0 && last_request[7]==0 && last_request[8]==0 && last_request[9]==0);
  assert(last_request[14]==6 && last_request[15]==0 && last_request[16]==0 && last_request[17]==0);
  assert(last_request[18]==8 && last_request[19]==0 && last_request[20]==0 && last_request[21]==0);
  assert(power.duty.sleep_deadline_us==45*SEC+(int64_t)duration[i]*1000);
  gnss_power_poll(&power,power.duty.sleep_deadline_us-1); assert(!wakes);
  gnss_power_poll(&power,power.duty.sleep_deadline_us);
  assert(wakes==1 && power.phase==GNSS_POWER_WAKING);
}
""")

    def test_ble_connect_wakes_during_settle_or_sleep_and_stays_continuous(self):
        check("""
for(unsigned settled=0;settled<2;settled++) {
  ready(); gnss_power_poll(&power,45*SEC);
  if(settled) gnss_power_poll(&power,47*SEC);
  gnss_power_take_actions(&power);
  gnss_power_camera_event(&power,true,50*SEC);
  gnss_power_poll(&power,50*SEC);
  assert(wakes==1 && power.phase==GNSS_POWER_WAKING);
  assert(power.duty.camera_connected && !gnss_power_receiving(&power));
  assert(gnss_power_take_actions(&power)==(GNSS_POWER_INVALIDATE_FIX|GNSS_POWER_RESET_RX));
  assert(!gnss_power_take_actions(&power));
  gnss_power_note_nmea(&power,50*SEC+1000);
  assert(power.phase==GNSS_POWER_WORK && events[GNSS_POWER_WAKE_OK]==1);
  assert(gnss_power_receiving(&power));
  for(unsigned sec=51;sec<1000;sec++) {
    gps_fix_t fix=sample(sec,0);
    assert(gnss_power_note_fix(&power,&fix,sec*SEC));
    gnss_power_poll(&power,sec*SEC);
  }
  assert(requests==1 && wakes==1 && power.phase==GNSS_POWER_WORK);
}
""")

    def test_disconnect_20_second_grace_reconnect_cancel_and_actual_event_time(self):
        check("""
ready(); gnss_power_camera_event(&power,true,2*SEC);
gnss_power_poll(&power,200*SEC); assert(!requests);
gnss_power_camera_event(&power,false,200*SEC);
gnss_power_poll(&power,220*SEC-1); assert(!requests);
gnss_power_camera_event(&power,true,219*SEC);
gnss_power_poll(&power,300*SEC); assert(!requests);
gnss_power_camera_event(&power,false,330*SEC);
gnss_power_poll(&power,350*SEC-1); assert(!requests);
gnss_power_poll(&power,350*SEC); assert(requests==1);
// Even a disconnect whose short preceding connect was coalesced wakes for grace.
gnss_power_camera_event(&power,false,360*SEC);
gnss_power_poll(&power,360*SEC); assert(wakes==1);
gnss_power_note_nmea(&power,361*SEC);
gnss_power_poll(&power,380*SEC-1); assert(requests==1);
gnss_power_poll(&power,380*SEC); assert(requests==2);
ready(); gnss_power_camera_event(&power,true,2*SEC);
gnss_power_camera_event(&power,false,400*SEC);
gnss_power_poll(&power,430*SEC); assert(requests==1);
""")

    def test_unknown_or_missing_identity_falls_back_continuously_bounded_queries(self):
        check("""
init(); gnss_power_note_nmea(&power,0); gnss_power_poll(&power,SEC);
mon_ver(false,SEC+1);
assert(power.phase==GNSS_POWER_DISABLED && events[GNSS_POWER_ID_UNSUPPORTED]==1);
assert(gnss_power_receiving(&power));
for(unsigned sec=2;sec<200;sec++) {
  gps_fix_t fix=sample(sec,0);
  assert(gnss_power_note_fix(&power,&fix,sec*SEC));
  gnss_power_poll(&power,sec*SEC);
}
assert(polls==1 && !requests && !wakes);
for(unsigned fail=0;fail<2;fail++) {
  init(); fail_identity=fail; gnss_power_note_nmea(&power,0);
  gnss_power_poll(&power,SEC); assert(polls==1);
  uint8_t malformed[39]={0};
  gnss_power_frame(&power,0x0a,4,malformed,sizeof(malformed),SEC+1);
  assert(!power.identity_checked);
  gnss_power_poll(&power,2500000-1); assert(polls==1);
  gnss_power_poll(&power,2500000); assert(polls==2);
  gnss_power_poll(&power,4000000); assert(polls==3);
  gnss_power_poll(&power,5500000);
  assert(power.phase==GNSS_POWER_DISABLED && events[GNSS_POWER_ID_TIMEOUT]==1);
  gnss_power_poll(&power,1000*SEC); mon_ver(true,1001*SEC);
  gnss_power_poll(&power,1002*SEC);
  assert(polls==3 && !requests && !wakes && gnss_power_receiving(&power));
}
""")

    def test_failed_pm_transmit_wakes_and_disables_even_after_partial_delivery(self):
        check("""
ready(); fail_pm=true; gnss_power_poll(&power,45*SEC);
assert(requests==1 && wakes==1 && power.phase==GNSS_POWER_WAKING);
assert(power.disable_after_wake && events[GNSS_POWER_SLEEP_FAILED]==1);
assert(!events[GNSS_POWER_SLEEP_SENT] && !events[GNSS_POWER_SLEEP_QUIET]);
assert(gnss_power_take_actions(&power)==(GNSS_POWER_INVALIDATE_FIX|GNSS_POWER_RESET_RX));
gnss_power_note_nmea(&power,45*SEC+1000);
assert(power.phase==GNSS_POWER_DISABLED && gnss_power_receiving(&power));
for(unsigned sec=46;sec<100;sec++) {
  gps_fix_t fix=sample(sec,0);
  assert(gnss_power_note_fix(&power,&fix,sec*SEC));
  gnss_power_poll(&power,sec*SEC);
}
assert(requests==1 && wakes==1); // no further PMREQ after failure
""")

    def test_pm_nak_and_continued_nmea_trigger_wake_then_permanent_fallback(self):
        check("""
for(unsigned failure=0;failure<3;failure++) {
  ready(); const uint8_t pm[]={2,0x41};
  gnss_power_frame(&power,5,0,pm,2,2*SEC); assert(!power.rejected);
  gnss_power_poll(&power,45*SEC);
  if(failure==1) gnss_power_poll(&power,47*SEC);
  if(failure<2) gnss_power_frame(&power,5,0,pm,2,48*SEC);
  else gnss_power_note_nmea(&power,48*SEC);
  gnss_power_poll(&power,48*SEC);
  assert(power.phase==GNSS_POWER_WAKING && wakes==1 && power.disable_after_wake);
  assert(events[failure<2 ? GNSS_POWER_SLEEP_NAK:GNSS_POWER_SLEEP_FAILED]==1);
  gnss_power_note_nmea(&power,48*SEC+1000);
  assert(power.phase==GNSS_POWER_DISABLED && gnss_power_receiving(&power));
  gps_fix_t a=sample(49,0),b=sample(50,0);
  assert(gnss_power_note_fix(&power,&a,49*SEC));
  assert(gnss_power_note_fix(&power,&b,50*SEC));
  gnss_power_poll(&power,1000*SEC); assert(requests==1 && wakes==1);
}
""")

    def test_ack_does_not_replace_quiet_check_or_mask_sleep_failure(self):
        check("""
ready(); gnss_power_poll(&power,45*SEC);
const uint8_t pm[]={2,0x41}, other[]={6,0x8a};
gnss_power_frame(&power,5,1,pm,2,45*SEC+100000);
gnss_power_frame(&power,5,0,other,2,45*SEC+200000);
gnss_power_poll(&power,45*SEC+400000);
assert(power.phase==GNSS_POWER_SLEEP_SETTLE && !power.rejected);
assert(!events[GNSS_POWER_SLEEP_QUIET]);
gnss_power_note_nmea(&power,45*SEC+500001);
gnss_power_poll(&power,45*SEC+500001);
assert(power.phase==GNSS_POWER_WAKING && events[GNSS_POWER_SLEEP_FAILED]==1);
assert(!events[GNSS_POWER_SLEEP_QUIET]);
""")

    def test_wake_retries_stop_at_three_and_late_timer_output_can_recover_fixes(self):
        check("""
for(unsigned fail=0;fail<2;fail++) {
  asleep(); gnss_power_take_actions(&power); fail_wake=fail;
  gnss_power_poll(&power,135*SEC);
  assert(power.phase==GNSS_POWER_WAKING && wakes==1);
  assert(request_duration()==90000); // receiver timer independently bounds sleep
  assert(gnss_power_take_actions(&power)==(GNSS_POWER_INVALIDATE_FIX|GNSS_POWER_RESET_RX));
  gnss_power_poll(&power,137*SEC-1); assert(wakes==1);
  gnss_power_poll(&power,137*SEC); assert(wakes==2);
  assert(!gnss_power_take_actions(&power));
  gnss_power_poll(&power,139*SEC); assert(wakes==3);
  gnss_power_poll(&power,141*SEC);
  assert(power.phase==GNSS_POWER_DISABLED && power.disable_after_wake);
  assert(events[GNSS_POWER_WAKE_RETRY]==2 && events[GNSS_POWER_WAKE_FAILED]>=1);
  assert(gnss_power_receiving(&power));
  gnss_power_note_nmea(&power,142*SEC);
  for(unsigned sec=142;sec<250;sec++) {
    gps_fix_t fix=sample(sec,0);
    assert(gnss_power_note_fix(&power,&fix,sec*SEC));
    gnss_power_poll(&power,sec*SEC);
  }
  assert(wakes==3 && requests==1 && power.phase==GNSS_POWER_DISABLED);
}
""")

    def test_delayed_wake_response_and_stale_utc_cannot_become_probe_fix(self):
        check("""
ready(); gps_fix_t old=sample(44,0);
assert(gnss_power_note_fix(&power,&old,44*SEC));
gnss_power_poll(&power,45*SEC); gnss_power_poll(&power,135*SEC);
assert(power.phase==GNSS_POWER_WAKING && !gnss_power_receiving(&power));
gps_fix_t fresh=sample(136,0.0001);
assert(!gnss_power_note_fix(&power,&fresh,136*SEC));
gnss_power_poll(&power,137*SEC); assert(wakes==2);
// New UART traffic establishes receiver activity; sample UTC still needs validation.
gnss_power_note_nmea(&power,138*SEC);
assert(power.phase==GNSS_POWER_WORK && events[GNSS_POWER_WAKE_OK]==1);
assert(!gnss_power_note_fix(&power,&old,138*SEC));
gps_fix_t stale=sample(134,0);
assert(!gnss_power_note_fix(&power,&stale,138*SEC));
fresh=sample(138,0.0001);
assert(gnss_power_note_fix(&power,&fresh,138*SEC));
assert(power.duty.probe_complete);
gnss_power_poll(&power,138*SEC);
assert(power.phase==GNSS_POWER_SLEEP_SETTLE && requests==2);
""")

    def test_rate_poll_gates_and_reset_actions_restore_configuration_after_nmea(self):
        check("""
asleep(); gnss_rate_t rate;
gnss_rate_init(&rate,0,send_bytes,NULL,&power);
gnss_rate_set_frame_observer(&rate,observe,&power);
gnss_rate_note_nmea(&rate);
if(gnss_power_receiving(&power)) gnss_rate_poll(&rate,50*SEC);
assert(!cfg_sends && !gnss_power_receiving(&power));
gnss_power_take_actions(&power);
gnss_power_camera_event(&power,true,50*SEC); gnss_power_poll(&power,50*SEC);
unsigned actions=gnss_power_take_actions(&power);
assert(actions==(GNSS_POWER_INVALIDATE_FIX|GNSS_POWER_RESET_RX));
// Driver consumes RESET_RX by reinitializing its shared binary parser/rate state.
gnss_rate_init(&rate,50*SEC,send_bytes,NULL,&power);
gnss_rate_set_frame_observer(&rate,observe,&power);
if(gnss_power_receiving(&power)) gnss_rate_poll(&rate,51*SEC);
assert(!cfg_sends && rate.stage==GNSS_RATE_WAIT_DATA);
gnss_power_note_nmea(&power,51*SEC); gnss_rate_note_nmea(&rate);
if(gnss_power_receiving(&power)) gnss_rate_poll(&rate,51*SEC);
assert(cfg_sends==1 && rate.stage==GNSS_RATE_WAIT_ACK);
assert(rate.frame_observer==observe && rate.frame_arg==&power);
""")

    def test_shared_frame_observer_requires_checksum_and_bounded_payload(self):
        check("""
init(); gnss_power_note_nmea(&power,0); gnss_power_poll(&power,SEC);
gnss_rate_t rate; gnss_rate_init(&rate,0,send_bytes,NULL,&power);
gnss_rate_set_frame_observer(&rate,observe,&power);
uint8_t payload[520]={0};
memcpy(payload,"ROM SPG 5.10 (10ca7e)",21);
memcpy(payload+30,"000A0000",8);
memcpy(payload+40,"FWVER=SPG 5.10",14);
memcpy(payload+70,"PROTVER=34.10",14);
receive_ubx(&rate,0x0a,4,payload,100,SEC+1,true);
assert(!power.identity_checked && !observed_frames && rate.ubx_bad==1);
receive_ubx(&rate,0x0a,4,payload,520,SEC+2,false);
assert(!power.identity_checked && !observed_frames && rate.ubx_ok==1);
receive_ubx(&rate,0x0a,4,payload,100,SEC+3,false);
assert(power.identity_checked && power.identity.supported && observed_frames==1);
gnss_power_poll(&power,45*SEC);
const uint8_t nak[]={2,0x41};
receive_ubx(&rate,5,0,nak,2,45*SEC+100000,true);
assert(!power.rejected && observed_frames==1);
gnss_power_poll(&power,45*SEC+200000); assert(!wakes);
receive_ubx(&rate,5,0,nak,2,45*SEC+300000,false);
assert(power.rejected && observed_frames==2);
gnss_power_poll(&power,45*SEC+300000);
assert(wakes==1 && events[GNSS_POWER_SLEEP_NAK]==1);
assert(!gnss_rate_consume(&rate,'$',46*SEC));
""")


if __name__ == "__main__":
    unittest.main()
