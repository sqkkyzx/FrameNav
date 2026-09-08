/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MOTION_POINTS 400
typedef struct {
  float points[MOTION_POINTS][3];
  uint32_t point_us[MOTION_POINTS];
  float previous[3], anchor[3], quiet_reference[3];
  unsigned count, moving_count;
  bool initialized, armed, capturing;
  int64_t last_sample, last_motion, quiet_since, started, cooldown_until;
} motion_policy_t;
void motion_policy_init(motion_policy_t *s, int64_t now);
bool motion_policy_sample(motion_policy_t *s, int64_t now, float x, float y, float z);
bool motion_circle(const float points[][3], unsigned n, float duration_s);
bool motion_circle_segment(const float points[][3], const uint32_t *times, unsigned n);
bool motion_sleep_due(int64_t now, int64_t last_motion, bool camera,
                      bool wifi_clients, bool web_active, bool sensor_ok);
