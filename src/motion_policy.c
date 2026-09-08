/* Copyright 2026 FrameNav contributors
 * SPDX-License-Identifier: Apache-2.0 */
#include "motion_policy.h"
#include <math.h>
#include <string.h>
void motion_policy_init(motion_policy_t *s, int64_t now) {
  memset(s, 0, sizeof(*s)); s->last_motion = s->quiet_since = now;
}
bool motion_sleep_due(int64_t now, int64_t last_motion, bool camera,
                      bool wifi_clients, bool web_active, bool sensor_ok) {
  return sensor_ok && !camera && !wifi_clients && !web_active &&
         now >= last_motion && now-last_motion >= 300000000LL;
}
// Heuristic for a deliberate circular acceleration trajectory, not position
// integration. Two or more turns, consistent direction, two substantial axes.
bool motion_circle(const float p[][3], unsigned n, float seconds) {
  if (n < 60 || seconds < 1.5f || seconds > 8.0f) return false;
  float mean[3]={0}, energy[3]={0};
  for (unsigned i=0;i<n;i++) for(unsigned k=0;k<3;k++) mean[k]+=p[i][k]/n;
  for (unsigned i=0;i<n;i++) for(unsigned k=0;k<3;k++) {
    float d=p[i][k]-mean[k]; energy[k]+=d*d/n;
  }
  for (unsigned a=0;a<3;a++) for(unsigned b=a+1;b<3;b++) {
    if (energy[a]<.0064f || energy[b]<.0064f ||
        fminf(energy[a],energy[b]) / fmaxf(energy[a],energy[b]) < .25f) continue;
    float turn=0, total=0, prev=0, rad=0, rad2=0;
    unsigned good=0;
    for(unsigned i=0;i<n;i++) {
      float x=p[i][a]-mean[a], y=p[i][b]-mean[b];
      float r=sqrtf(x*x+y*y), angle=atan2f(y,x);
      rad+=r/n; rad2+=r*r/n;
      if (r>.08f && r<2.5f) good++;
      if(i) {
        float d=angle-prev;
        while(d>3.14159265f)d-=6.2831853f;
        while(d< -3.14159265f)d+=6.2831853f;
        turn+=d; total+=fabsf(d);
      }
      prev=angle;
    }
    float turns=fabsf(turn)/6.2831853f;
    if (good*100U >= n*80U && rad>.12f && rad<1.8f &&
        rad2-rad*rad < .36f*rad*rad && turns>=1.8f && turns<=5.0f &&
        total>0 && fabsf(turn)/total>.82f) return true;
  }
  return false;
}
bool motion_circle_segment(const float p[][3], const uint32_t *times, unsigned n) {
  if(n<60)return false;
  if(motion_circle(p,n,(times[n-1]-times[0])/1000000.0f))return true;
  // Bounded coarse windows tolerate pickup/putdown without lowering the
  // circle quality thresholds. At most ~100 candidate windows per gesture.
  for(unsigned start=0;start+60<=n;start+=25) {
    for(unsigned end=start+60;end<=n;end+=25) {
      float seconds=(times[end-1]-times[start])/1000000.0f;
      if(seconds<1.8f)continue;
      if(seconds>6.0f)break;
      if(motion_circle(p+start,end-start,seconds))return true;
    }
  }
  return false;
}
bool motion_policy_sample(motion_policy_t *s, int64_t now,float x,float y,float z) {
  float v[3]={x,y,z};
  if (!isfinite(x)||!isfinite(y)||!isfinite(z)) return false;
  if (!s->initialized || now-s->last_sample>200000 || now<s->last_sample) {
    int64_t cooldown = s->cooldown_until;
    motion_policy_init(s,now);s->initialized=true;
    s->cooldown_until = cooldown;
    memcpy(s->previous,v,sizeof(v));memcpy(s->anchor,v,sizeof(v));
    memcpy(s->quiet_reference,v,sizeof(v));
  }
  float delta=0, drift=0;
  for(unsigned k=0;k<3;k++) {
    delta+=(v[k]-s->previous[k])*(v[k]-s->previous[k]);
    drift+=(v[k]-s->anchor[k])*(v[k]-s->anchor[k]);
    s->anchor[k]+=.02f*(v[k]-s->anchor[k]);s->previous[k]=v[k];
  }
  s->last_sample=now;
  if(delta>.0004f || drift>.0025f) s->last_motion=now;
  // Compare against the last quiet reference, not only the preceding sample:
  // small per-sample changes can accumulate into a deliberate slow circle.
  float quiet_distance=0;
  for(unsigned k=0;k<3;k++) {
    float d=v[k]-s->quiet_reference[k];quiet_distance+=d*d;
  }
  bool unsettled=quiet_distance>.0004f;
  if(unsettled) {
    s->quiet_since=now;
    memcpy(s->quiet_reference,v,sizeof(v));
  }
  if(!s->capturing && now-s->quiet_since>=1000000) s->armed=true;
  if(!s->capturing && s->armed && now>=s->cooldown_until &&
      (delta>.0016f || drift>.01f)) {
    s->capturing=true;s->armed=false;s->count=0;s->moving_count=0;s->started=now;
  }
  if(s->capturing) {
    if(s->count<MOTION_POINTS) {
      memcpy(s->points[s->count],v,sizeof(v));
      s->point_us[s->count++]=(uint32_t)(now-s->started);
    }
    if(unsettled)s->moving_count=s->count;
    if(now-s->quiet_since>=800000 || s->count==MOTION_POINTS || now-s->started>9000000LL) {
      // Timestamp-derived end and point count work at both 50Hz and the lower
      // effective rate while the interrupt latch is being acknowledged.
      bool ok=now-s->quiet_since>=800000 &&
              motion_circle_segment(s->points,s->point_us,s->moving_count);
      s->capturing=false;s->armed=false;s->quiet_since=now;
      if(ok){s->cooldown_until=now+60000000LL;return true;}
    }
  }
  return false;
}
