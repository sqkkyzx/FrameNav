/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
enum {
  DBG_BOOT=1, DBG_SENSOR_INIT, DBG_SENSOR_ERROR, DBG_CAPTURE_START,
  DBG_CAPTURE_END, DBG_SENSOR_STATE, DBG_CIRCLE, DBG_AP_START, DBG_AP_RESULT,
  DBG_WIFI_STOP, DBG_SOURCE, DBG_SLEEP_PREP, DBG_SLEEP_ABORT, DBG_SLEEP_ENTER,
  DBG_WIFI_ERROR
};
#if FRAMENAV_TEST_DEBUG
void test_debug_start(void);
void test_debug_event(unsigned event,int32_t a,int32_t b,int32_t c,int32_t d);
void test_debug_flush(void);
#else
static inline void test_debug_start(void) {}
#define test_debug_event(...) ((void)0)
static inline void test_debug_flush(void) {}
#endif
