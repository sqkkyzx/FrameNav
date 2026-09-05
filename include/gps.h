/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ALPHALOC_GPS_H
#define ALPHALOC_GPS_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef struct {
  double lat_deg;
  double lon_deg;
  double altitude_m;
  bool altitude_valid;
  bool valid;
  bool time_valid;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  int64_t last_fix_time_us;
  int64_t last_update_time_us;
} gps_fix_t;

typedef enum {
  GPS_CONSTELLATION_NONE = 0,
  GPS_CONSTELLATION_GPS = 1 << 0,
  GPS_CONSTELLATION_GLONASS = 1 << 1,
} gps_constellation_t;

typedef struct {
  bool has_lock;
  uint8_t satellites;
  gps_constellation_t constellations;
} gps_status_t;

typedef struct {
  int uart_num;
  int tx_pin;
  int rx_pin;
  int baud_rate;
  uint32_t update_interval_ms;
} gps_config_t;

void gps_init(const gps_config_t *cfg);
bool gps_get_latest(gps_fix_t *out_fix);
bool gps_get_status(gps_status_t *out_status);
#if ALPHALOC_GNSS_STANDBY
// Notify a successful camera BLE link or its disconnect, not bond/auth status.
void gps_camera_connection_changed(bool connected);
// Safe from the HTTP task; the GPS task applies the policy on its next pass.
void gps_apply_power_policy(const location_power_policy_t *policy);
#endif

#if ALPHALOC_FAKE_GPS && ALPHALOC_FAKE_GPS_STEPPING
// Test-only clock. USB-time mode returns false until the host sets this boot's
// time; legacy stepping mode uses a build-time seed.
bool gps_fake_apply_clock(gps_fix_t *fix, int64_t uptime_us);
#endif

#endif
