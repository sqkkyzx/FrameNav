/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "gps.h"

typedef enum { LOCATION_NONE, LOCATION_GNSS, LOCATION_PHONE } location_source_t;

typedef struct {
  bool present;
  bool available;
  double latitude, longitude, accuracy_m;
  uint32_t captured_utc;
  uint32_t remaining_s;
} phone_location_status_t;

void location_service_init(app_config_t *cfg);
bool location_service_get(gps_fix_t *fix);
bool location_service_details(gps_fix_t *fix, location_source_t *source,
                               uint32_t *observed_utc);
/* Browser Geolocation coordinates are WGS-84. Both timestamps are Unix ms.
 * The phone clock advances camera UTC; expiry uses monotonic device uptime. */
bool location_service_set_phone(double latitude, double longitude,
                                 double accuracy_m, uint64_t captured_ms,
                                 uint64_t phone_now_ms);
void location_service_clear_phone(void);
void location_service_phone_status(phone_location_status_t *out);
