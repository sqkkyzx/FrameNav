#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise the actual duty policy C with deterministic time and GNSS samples.

No serial ports or hardware are opened; these are policy checks, not evidence
that a particular receiver accepts its standby command.
"""

import unittest

from host_phase1 import PRELUDE, ROOT, run_c


HARNESS = PRELUDE + f'#include "{ROOT / "src/gnss_duty.c"}"\n' + """
#define SEC 1000000LL
static gps_fix_t sample(int seconds, double latitude, double longitude) {
  gps_fix_t fix = {.valid=true, .time_valid=true, .year=2026,
                   .month=1, .day=1, .lat_deg=latitude, .lon_deg=longitude};
  fix.hour=seconds/3600; fix.minute=(seconds/60)%60; fix.second=seconds%60;
  return fix;
}
static void begin_probe(gnss_duty_t *state, bool history) {
  gnss_duty_init(state,0);
  if (history) {
    gps_fix_t a=sample(0,0,0), b=sample(1,0,0.00001);
    assert(gnss_duty_note_fix(state,&a,0));
    assert(gnss_duty_note_fix(state,&b,SEC));
  }
  assert(gnss_duty_poll(state,45*SEC)==GNSS_DUTY_SLEEP);
  gnss_duty_sleep_committed(state,45*SEC);
  int64_t wake=(history ? 75 : 135)*SEC;
  assert(gnss_duty_poll(state,wake)==GNSS_DUTY_WAKE);
  gnss_duty_wake_committed(state,wake);
  assert(state->phase==GNSS_DUTY_PROBING);
}
"""


def check(body, prefix=""):
    run_c(prefix + HARNESS + "int main(void) {\n" +
          "(void)sample; (void)begin_probe;\n" + body + "\nreturn 0; }\n")


class GnssDutyChecks(unittest.TestCase):
    def test_boot_45_seconds_and_sleep_clock_starts_only_at_commit(self):
        check("""
gnss_duty_t state;
gnss_duty_init(&state,10*SEC);
gps_fix_t fix=sample(10,0,0);
assert(state.next_sleep_s==90);
assert(gnss_duty_note_fix(&state,&fix,10*SEC));
assert(state.next_sleep_s==90 && !state.have_speed);
fix.second=11; fix.lon_deg=0.00001;
assert(gnss_duty_note_fix(&state,&fix,11*SEC));
assert(state.next_sleep_s==30 && state.have_speed);
assert(gnss_duty_work_required(&state,55*SEC-1));
assert(gnss_duty_poll(&state,55*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,55*SEC)==GNSS_DUTY_SLEEP);
assert(gnss_duty_poll(&state,100*SEC)==GNSS_DUTY_SLEEP);
assert(!gnss_duty_is_sleeping(&state));
gnss_duty_sleep_committed(&state,101*SEC);
assert(gnss_duty_is_sleeping(&state));
assert(gnss_duty_poll(&state,131*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,131*SEC)==GNSS_DUTY_WAKE);
assert(gnss_duty_poll(&state,200*SEC)==GNSS_DUTY_WAKE);
assert(gnss_duty_is_sleeping(&state));
gnss_duty_wake_committed(&state,202*SEC);
assert(gnss_duty_poll(&state,247*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,247*SEC)==GNSS_DUTY_SLEEP);
""")

    def test_camera_continuous_disconnect_grace_and_reconnect_cancel(self):
        check("""
gnss_duty_t state;
gnss_duty_init(&state,0);
gnss_duty_camera_event(&state,true,2*SEC);
assert(gnss_duty_poll(&state,200*SEC)==GNSS_DUTY_NONE);
gnss_duty_camera_event(&state,false,200*SEC);
assert(gnss_duty_poll(&state,220*SEC-1)==GNSS_DUTY_NONE);
gnss_duty_camera_event(&state,true,219*SEC);
assert(gnss_duty_poll(&state,300*SEC)==GNSS_DUTY_NONE);
gnss_duty_camera_event(&state,false,330*SEC);
assert(gnss_duty_poll(&state,350*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,350*SEC)==GNSS_DUTY_SLEEP);
assert(state.next_sleep_s==90);
gnss_duty_sleep_committed(&state,350*SEC);
// A disconnect alone still wakes for grace: a rapid connect was coalesced.
gnss_duty_camera_event(&state,false,360*SEC);
assert(gnss_duty_is_sleeping(&state));
assert(gnss_duty_work_required(&state,361*SEC));
assert(gnss_duty_poll(&state,361*SEC)==GNSS_DUTY_WAKE);
gnss_duty_wake_committed(&state,362*SEC);
assert(state.phase==GNSS_DUTY_WORKING);
assert(gnss_duty_poll(&state,380*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,380*SEC)==GNSS_DUTY_SLEEP);
// Delayed delivery retains the actual event deadline, not poll+20 seconds.
gnss_duty_camera_event(&state,false,400*SEC);
assert(gnss_duty_poll(&state,430*SEC)==GNSS_DUTY_SLEEP);
""")

    def test_camera_priority_at_sleep_commit_and_successful_probe_boundaries(self):
        check("""
gnss_duty_t state;
gnss_duty_init(&state,0);
assert(gnss_duty_poll(&state,45*SEC)==GNSS_DUTY_SLEEP);
gnss_duty_camera_event(&state,true,45*SEC+1);
assert(gnss_duty_poll(&state,45*SEC+2)==GNSS_DUTY_NONE);
// If transport already completed, preserve physical sleep and demand wake.
gnss_duty_sleep_committed(&state,45*SEC+3);
assert(gnss_duty_is_sleeping(&state));
assert(gnss_duty_poll(&state,45*SEC+4)==GNSS_DUTY_WAKE);
gnss_duty_wake_committed(&state,45*SEC+5);
assert(state.phase==GNSS_DUTY_WORKING);
assert(gnss_duty_poll(&state,1000*SEC)==GNSS_DUTY_NONE);
begin_probe(&state,true);
gps_fix_t fix=sample(76,0,0);
assert(gnss_duty_note_fix(&state,&fix,76*SEC));
assert(gnss_duty_poll(&state,76*SEC)==GNSS_DUTY_SLEEP);
gnss_duty_camera_event(&state,true,76*SEC+1);
assert(state.phase==GNSS_DUTY_WORKING);
assert(gnss_duty_poll(&state,500*SEC)==GNSS_DUTY_NONE);
gnss_duty_camera_event(&state,false,500*SEC);
assert(gnss_duty_poll(&state,520*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,520*SEC)==GNSS_DUTY_SLEEP);
""")

    def test_first_fresh_probe_fix_sleeps_immediately_and_no_history_uses_90(self):
        check("""
gnss_duty_t state;
begin_probe(&state,false);
gps_fix_t fix=sample(135,10,20);
assert(gnss_duty_note_fix(&state,&fix,135*SEC+1));
assert(!state.have_speed && state.next_sleep_s==90);
assert(!gnss_duty_work_required(&state,135*SEC+1));
assert(gnss_duty_poll(&state,135*SEC+1)==GNSS_DUTY_SLEEP);
int64_t first_epoch=state.previous_epoch_s;
fix.second=16; // new epoch at 136 s cannot replace first sample before commit
assert(!gnss_duty_note_fix(&state,&fix,136*SEC));
assert(state.previous_epoch_s==first_epoch);
gnss_duty_sleep_committed(&state,140*SEC);
assert(!gnss_duty_note_fix(&state,&fix,141*SEC));
assert(gnss_duty_poll(&state,230*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,230*SEC)==GNSS_DUTY_WAKE);
gnss_duty_wake_committed(&state,230*SEC);
fix=sample(230,10,20);
assert(gnss_duty_note_fix(&state,&fix,230*SEC));
assert(state.have_speed && state.last_speed_mps==0 && state.next_sleep_s==90);
""")

    def test_probe_rejects_repeated_out_of_order_and_pre_wake_epochs(self):
        check("""
gnss_duty_t state;
begin_probe(&state,true);
const int64_t previous=state.previous_epoch_s;
gps_fix_t fix=sample(1,0,0);
assert(!gnss_duty_note_fix(&state,&fix,75*SEC)); // cached prior fix
fix=sample(0,0,0);
assert(!gnss_duty_note_fix(&state,&fix,75*SEC)); // time moved backwards
fix=sample(74,0,0);
assert(!gnss_duty_note_fix(&state,&fix,75*SEC)); // advancing but pre-wake
fix=sample(75,0,0);
assert(!gnss_duty_note_fix(&state,&fix,75*SEC-1));
assert(state.previous_epoch_s==previous && !state.probe_complete);
assert(gnss_duty_poll(&state,76*SEC)==GNSS_DUTY_NONE);
assert(gnss_duty_note_fix(&state,&fix,75*SEC+1)); // wake second is fresh
assert(state.next_sleep_s==90);
assert(gnss_duty_poll(&state,75*SEC+1)==GNSS_DUTY_SLEEP);
""")

    def test_timeout_is_45_seconds_with_explicit_history_fallback(self):
        check("""
gnss_duty_t state;
begin_probe(&state,false);
assert(gnss_duty_poll(&state,180*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,180*SEC)==GNSS_DUTY_SLEEP);
assert(state.next_sleep_s==90 && !state.have_previous_fix);
begin_probe(&state,true);
const int64_t previous=state.previous_epoch_s;
assert(gnss_duty_poll(&state,120*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,120*SEC)==GNSS_DUTY_SLEEP);
assert(state.next_sleep_s==30 && state.previous_epoch_s==previous);
gnss_duty_sleep_committed(&state,121*SEC);
assert(gnss_duty_poll(&state,151*SEC)==GNSS_DUTY_WAKE);
gnss_duty_wake_committed(&state,151*SEC);
gps_fix_t fix=sample(150,0,0);
assert(!gnss_duty_note_fix(&state,&fix,152*SEC));
fix=sample(152,0,0);
assert(gnss_duty_note_fix(&state,&fix,152*SEC));
assert(state.next_sleep_s==90 && state.last_speed_mps<0.5);
""")

    def test_probe_deadline_rejects_late_fix_even_if_parser_precedes_poll(self):
        check("""
gnss_duty_t state;
begin_probe(&state,true);
gps_fix_t fix=sample(120,0,1);
const int64_t previous=state.previous_epoch_s;
assert(!gnss_duty_note_fix(&state,&fix,120*SEC));
assert(!gnss_duty_note_fix(&state,&fix,121*SEC));
assert(state.previous_epoch_s==previous && !state.probe_complete);
assert(gnss_duty_poll(&state,120*SEC)==GNSS_DUTY_SLEEP);
assert(state.next_sleep_s==30); // timeout, not the late fix's fast interval
begin_probe(&state,true);
fix=sample(119,0,1);
assert(gnss_duty_note_fix(&state,&fix,120*SEC-1));
assert(gnss_duty_poll(&state,120*SEC-1)==GNSS_DUTY_SLEEP);
assert(state.next_sleep_s==15); // the final microsecond remains eligible
""")

    def test_speed_uses_previous_position_and_gnss_time_not_delivery_time(self):
        check("""
gnss_duty_t state;
gnss_duty_init(&state,0);
gnss_duty_camera_event(&state,true,0);
gps_fix_t fix=sample(0,0,0);
assert(gnss_duty_note_fix(&state,&fix,0));
fix=sample(1,0,0.001); // 111.195 m in one GNSS second
assert(gnss_duty_note_fix(&state,&fix,40*SEC));
assert(state.last_speed_mps>111 && state.last_speed_mps<112);
assert(state.next_sleep_s==15);
fix=sample(2,0,0.001); // Compare with immediately preceding position
assert(gnss_duty_note_fix(&state,&fix,41*SEC));
assert(state.last_speed_mps==0 && state.next_sleep_s==90);
gnss_duty_camera_event(&state,false,41*SEC);
fix=sample(3,0,0.002);
assert(gnss_duty_note_fix(&state,&fix,42*SEC));
assert(state.next_sleep_s==15);
assert(gnss_duty_poll(&state,61*SEC)==GNSS_DUTY_SLEEP);
assert(state.next_sleep_s==15); // grace retains latest sample's interval
gnss_duty_sleep_committed(&state,61*SEC);
assert(gnss_duty_poll(&state,76*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,76*SEC)==GNSS_DUTY_WAKE);
gnss_duty_wake_committed(&state,76*SEC);
fix=sample(40,0,0.012); // 1112 m / 37 GNSS seconds, not callback delta
assert(gnss_duty_note_fix(&state,&fix,80*SEC));
assert(state.last_speed_mps>30 && state.last_speed_mps<31);
assert(state.next_sleep_s==15);
""")

    def test_haversine_antimeridian_poles_and_configurable_strict_threshold(self):
        check("""
gnss_duty_t state;
gnss_duty_init(&state,0);
gps_fix_t a=sample(0,0,179.999), b=sample(30,0,-179.999);
assert(gnss_duty_note_fix(&state,&a,0));
assert(gnss_duty_note_fix(&state,&b,30*SEC));
assert(state.last_speed_mps>7.4 && state.last_speed_mps<7.5);
assert(state.next_sleep_s==30);
a=sample(31,90,0); b=sample(32,-90,180);
assert(gnss_duty_note_fix(&state,&a,31*SEC));
assert(gnss_duty_note_fix(&state,&b,32*SEC));
assert(isfinite(state.last_speed_mps));
assert(state.last_speed_mps>20000000 && state.last_speed_mps<20100000);
assert(state.next_sleep_s==15);
""")
        check("""
gnss_duty_t state;
gnss_duty_init(&state,0);
gps_fix_t a=sample(0,0,0), b=sample(1,0,0.00001);
assert(gnss_duty_note_fix(&state,&a,0));
assert(gnss_duty_note_fix(&state,&b,SEC));
assert(state.last_speed_mps>1 && state.last_speed_mps<2);
assert(state.next_sleep_s==30);
b=sample(2,0,0.00004);
assert(gnss_duty_note_fix(&state,&b,2*SEC));
assert(state.next_sleep_s==15);
b=sample(3,0,0.000045);
assert(gnss_duty_note_fix(&state,&b,3*SEC));
assert(state.next_sleep_s==90);
assert(duty_sleep_for_speed(1.0)==90);
assert(duty_sleep_for_speed(1.000001)==30);
assert(duty_sleep_for_speed(2.0)==30); // equality must not take fast branch
assert(duty_sleep_for_speed(2.000001)==15);
""", prefix="#define GNSS_DUTY_FAST_SPEED_MPS 2.0\n"
            "#define GNSS_DUTY_STATIONARY_SPEED_MPS 1.0\n")

    def test_connected_keeps_working_for_stationary_normal_and_fast_samples(self):
        check("""
gnss_duty_t state;
gnss_duty_init(&state,0);
gnss_duty_camera_event(&state,true,0);
const double longitude[]={0,0,0.00001,0.00101};
const unsigned interval[]={90,90,30,15};
for (unsigned i=0;i<4;++i) {
  gps_fix_t fix=sample(100+i,0,longitude[i]);
  assert(gnss_duty_note_fix(&state,&fix,(100+i)*SEC));
  assert(state.next_sleep_s==interval[i]);
  assert(gnss_duty_work_required(&state,(100+i)*SEC));
  assert(gnss_duty_poll(&state,(100+i)*SEC)==GNSS_DUTY_NONE);
}
assert(gnss_duty_poll(&state,10000*SEC)==GNSS_DUTY_NONE);
assert(duty_sleep_for_speed(0.5)==90);
assert(duty_sleep_for_speed(0.500001)==30);
assert(duty_sleep_for_speed(8.0)==30);
assert(duty_sleep_for_speed(8.000001)==15);
""")

    def test_utc_calendar_rollovers_and_century_leap_rules(self):
        check("""
const unsigned years[]={2026,2027,2028,2028,2099,2100,2399,2400};
const unsigned months[]={12,2,2,2,12,2,12,2};
const unsigned days[]={31,28,28,29,31,28,31,28};
const unsigned next_years[]={2027,2027,2028,2028,2100,2100,2400,2400};
const unsigned next_months[]={1,3,2,3,1,3,1,2};
const unsigned next_days[]={1,1,29,1,1,1,1,29};
for (unsigned i=0;i<sizeof(years)/sizeof(years[0]);++i) {
  gnss_duty_t state; gnss_duty_init(&state,0);
  gps_fix_t a=sample(86399,0,0), b=sample(0,0,0.0001);
  a.year=years[i]; a.month=months[i]; a.day=days[i];
  b.year=next_years[i]; b.month=next_months[i]; b.day=next_days[i];
  assert(gnss_duty_note_fix(&state,&a,0));
  int64_t epoch=state.previous_epoch_s;
  assert(gnss_duty_note_fix(&state,&b,SEC));
  assert(state.previous_epoch_s==epoch+1);
  assert(state.last_speed_mps>11 && state.last_speed_mps<12);
  assert(state.next_sleep_s==15);
}
""")

    def test_invalid_coordinates_dates_and_epoch_zero_cannot_end_probe(self):
        check("""
gnss_duty_t state;
begin_probe(&state,false);
gps_fix_t base=sample(136,0,0), bad[20];
for (unsigned i=0;i<20;++i) bad[i]=base;
bad[0].valid=false; bad[1].time_valid=false;
bad[2].lat_deg=NAN; bad[3].lon_deg=INFINITY;
bad[4].lat_deg=-90.001; bad[5].lat_deg=90.001;
bad[6].lon_deg=-180.001; bad[7].lon_deg=180.001;
bad[8].year=0; bad[9].month=0; bad[10].month=13;
bad[11].day=0; bad[12].day=32;
bad[13].month=2; bad[13].day=29; // 2026 is not leap
bad[14].hour=24; bad[15].minute=60; bad[16].second=60;
bad[17].year=1970; bad[17].hour=bad[17].minute=bad[17].second=0;
bad[18].month=4; bad[18].day=31;
bad[19].year=2100; bad[19].month=2; bad[19].day=29;
assert(!gnss_duty_note_fix(&state,NULL,136*SEC));
for (unsigned i=0;i<20;++i) {
  assert(!gnss_duty_note_fix(&state,&bad[i],136*SEC));
  assert(!state.have_previous_fix && !state.probe_complete);
  assert(gnss_duty_poll(&state,136*SEC)==GNSS_DUTY_NONE);
}
base.lat_deg=-90; base.lon_deg=180;
assert(gnss_duty_note_fix(&state,&base,136*SEC));
assert(gnss_duty_poll(&state,136*SEC)==GNSS_DUTY_SLEEP);
""")

    def test_runtime_custom_parameters_and_continuous_track_mode(self):
        check("""
gnss_duty_t state;
gnss_duty_params_t custom={
  .boot_work_s=12, .disconnect_grace_s=7,
  .stationary_sleep_s=60, .moving_sleep_s=20, .fast_sleep_s=5,
  .fix_timeout_s=9, .fast_speed_mps=2.0,
};
gnss_duty_init_with_params(&state,0,&custom);
assert(state.boot_deadline_us==12*SEC);
assert(gnss_duty_poll(&state,12*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,12*SEC)==GNSS_DUTY_SLEEP);

gnss_duty_camera_event(&state,true,20*SEC);
gps_fix_t a=sample(20,0,0), b=sample(21,0,0.00001);
assert(gnss_duty_note_fix(&state,&a,20*SEC));
assert(gnss_duty_note_fix(&state,&b,21*SEC));
assert(state.next_sleep_s==20); // about 1.1m/s: moving
gnss_duty_camera_event(&state,false,21*SEC);
assert(gnss_duty_poll(&state,28*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,28*SEC)==GNSS_DUTY_SLEEP);
gnss_duty_sleep_committed(&state,28*SEC);
assert(gnss_duty_poll(&state,48*SEC)==GNSS_DUTY_WAKE);
gnss_duty_wake_committed(&state,48*SEC);
assert(state.probe_deadline_us==57*SEC);

custom.continuous=true;
gnss_duty_apply_params(&state,&custom,49*SEC);
assert(gnss_duty_poll(&state,1000*SEC)==GNSS_DUTY_NONE);
custom.continuous=false;
gnss_duty_apply_params(&state,&custom,1000*SEC);
assert(gnss_duty_poll(&state,1007*SEC-1)==GNSS_DUTY_NONE);
assert(gnss_duty_poll(&state,1007*SEC)==GNSS_DUTY_SLEEP);
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
