/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
enum {
  DBG_BOOT=1, DBG_SOURCE, DBG_AP_START, DBG_WIFI_STOP, DBG_WIFI_ERROR
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
