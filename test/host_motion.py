"""Motion policy and USB deep-sleep veto regression checks."""
import unittest
from host_phase1 import ROOT, run_c, function_source
from host_gnss_power import check as check_gnss

POLICY = '#include <assert.h>\n' + (ROOT / 'src/motion_policy.c').read_text()

class MotionChecks(unittest.TestCase):
    def test_circle_with_pickup_putdown_and_non_circle_windows(self):
        run_c(POLICY + r'''
int main(void) {
 float p[350][3];uint32_t t[350];
 for(int i=0;i<350;i++) {
  t[i]=i*20000;p[i][2]=1;
  if(i>=100 && i<250) {
   float a=6.2831853f*3*(i-100)/150;
   p[i][0]=.4f*cosf(a);p[i][1]=.4f*sinf(a);
  } else {p[i][0]=i<100?1.5f:-1.5f;p[i][1]=0;}
 }
 assert(!motion_circle(p,350,7));
 assert(motion_circle_segment(p,t,350));
 for(int i=0;i<350;i++) {
  p[i][0]=.4f*sinf(i*.1f);p[i][1]=p[i][0];
 }
 assert(!motion_circle_segment(p,t,350));
 uint32_t seed=19;
 for(int i=0;i<350;i++)for(int j=0;j<3;j++) {
  seed=seed*1664525U+1013904223U;p[i][j]=(seed%1000)/1000.0f-.5f;
 }
 assert(!motion_circle_segment(p,t,350));
 return 0;
}
''')

    def test_gentle_circle_variable_sampling(self):
        run_c(POLICY + r'''
int main(void) {
 for(int dt=20000;dt<=30000;dt+=10000) {
  motion_policy_t p;motion_policy_init(&p,0);int hits=0;
  for(int64_t t=0;t<2000000;t+=dt)motion_policy_sample(&p,t,0,0,1);
  for(int64_t t=2000000;t<8000000;t+=dt) {
   float a=6.2831853f*3*(t-2000000)/6000000.0f;
   hits+=motion_policy_sample(&p,t,.15f*cosf(a),.15f*sinf(a),1);
  }
  for(int64_t t=8000000;t<9500000;t+=dt)hits+=motion_policy_sample(&p,t,0,0,1);
  assert(hits==1);
 }
 return 0;
}
''')

    def test_gnss_indefinite_and_camera_wake(self):
        check_gnss(r'''
ready(); power.deep_requested=true;
gnss_power_poll(&power,2*SEC);
assert(power.deep_committed && request_duration()==0 && requests==1);
gnss_power_poll(&power,4*SEC);assert(power.phase==GNSS_POWER_SLEEP);
gnss_power_poll(&power,10000*SEC);
assert(power.phase==GNSS_POWER_SLEEP && wakes==0 && requests==1);
power.deep_requested=false;gnss_power_poll(&power,10001*SEC);
assert(power.phase==GNSS_POWER_WAKING && wakes==1);
''')

    def test_full_circle_stream(self):
        run_c(POLICY + r'''
int main(void) {
  motion_policy_t p;motion_policy_init(&p,0);int hits=0;
  for(int i=0;i<100;i++)motion_policy_sample(&p,i*20000LL,0,0,1);
  for(int i=0;i<200;i++) {
    float a=6.2831853f*3*i/200;
    hits+=motion_policy_sample(&p,(i+100)*20000LL,.4f*cosf(a),.4f*sinf(a),1);
  }
  for(int i=300;i<360;i++)hits+=motion_policy_sample(&p,i*20000LL,0,0,1);
  assert(hits==1);
  return 0;
}
''')

    def test_circle_and_non_circle(self):
        run_c(POLICY + r'''
int main(void) {
  float p[200][3];
  for(int i=0;i<200;i++) {
    float a=6.2831853f*3*i/200;
    p[i][0]=.4f*cosf(a);p[i][1]=.4f*sinf(a);p[i][2]=1;
  }
  assert(motion_circle(p,200,4));
  assert(!motion_circle(p,200,1));
  for(int i=0;i<200;i++)p[i][1]=0; // back/forth, no circle
  assert(!motion_circle(p,200,4));
  memset(p,0,sizeof(p));assert(!motion_circle(p,200,4));
  p[50][0]=2;assert(!motion_circle(p,200,4));
  return 0;
}
''')

    def test_cooldown_survives_sample_gap(self):
        run_c(POLICY + r'''
int main(void) {
  motion_policy_t p;motion_policy_init(&p,0);
  motion_policy_sample(&p,1000,0,0,1);p.cooldown_until=60000000;
  motion_policy_sample(&p,500000,0,0,1);
  assert(p.cooldown_until==60000000);
  return 0;
}
''')

    def test_sleep_vetoes_and_usb(self):
        run_c(POLICY + r'''
static bool usb,cam,clients,healthy=true,running;
static int64_t now=300000000LL,last,activity;
bool usb_serial_jtag_is_connected(void){return usb;}
bool ble_client_is_connected(void){return cam;}
bool wifi_web_has_clients(void){return clients;}
bool wifi_web_is_started(void){return running;}
bool motion_service_status(int64_t *out){*out=last;return healthy;}
int64_t wifi_web_last_activity_us(void){return activity;}
int64_t esp_timer_get_time(void){return now;}
''' + function_source('src/main.c', 'system_sleep_allowed') + r'''
int main(void) {
  assert(system_sleep_allowed());
  usb=true;assert(!system_sleep_allowed());usb=false;
  cam=true;assert(!system_sleep_allowed());cam=false;
  clients=true;assert(!system_sleep_allowed());clients=false;
  healthy=false;assert(!system_sleep_allowed());healthy=true;
  last=1;assert(!system_sleep_allowed());last=0;
  running=true;activity=now-1000000;assert(!system_sleep_allowed());
  activity=now-30000000;assert(system_sleep_allowed());
  return 0;
}
''')

    def test_wake_pin_and_web_lease(self):
        service=(ROOT/'src/motion_service.c').read_text()
        web=(ROOT/'src/wifi_web.c').read_text()
        main=(ROOT/'src/main.c').read_text()
        self.assertIn('ESP_SLEEP_WAKEUP_ALL',service)
        self.assertIn('1ULL<<GPIO_NUM_0,ESP_EXT1_WAKEUP_ANY_HIGH',service)
        self.assertNotIn('esp_sleep_enable_timer_wakeup',service)
        self.assertIn('if(!document.hidden)',web)
        self.assertIn("fetch('/activity'",web)
        self.assertIn('now_us-last >= 30000000LL',main)
        self.assertIn('gps_system_sleep_ready()',main)
        self.assertIn('track_store_quiesce(true)',main)

if __name__ == '__main__': unittest.main()
