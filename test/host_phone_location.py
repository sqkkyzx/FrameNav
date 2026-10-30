#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the actual location selector, camera publishers and browser code."""

import configparser
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from host_phase1 import ROOT, function_source, run_c


HARNESS = r"""
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include "location_service.h"
#include "track_store.h"
#include "freertos/FreeRTOS.h"
#define SEC 1000000LL
#define EPOCH_MS 1793361600000ULL
static int64_t now_us;
static app_config_t cfg;
static gps_fix_t raw;
int64_t esp_timer_get_time(void) { return now_us; }
bool gps_get_latest(gps_fix_t *out) { *out = raw; return true; }
bool config_get_snapshot(const app_config_t *in, app_config_t *out) { *out = *in; return true; }
void config_validate(app_config_t *in) { (void)in; }
static void reset(void) {
  now_us = SEC;
  cfg = (app_config_t){.phone_location_ttl_hours = PHONE_LOCATION_TTL_DEFAULT_HOURS,
                        .max_gps_age_s = 5, .gps_interval_ms = 2000};
  raw = (gps_fix_t){0};
  location_service_init(&cfg);
  location_service_clear_phone();
}
#define GNSS() (raw = (gps_fix_t){.valid=true, .time_valid=true, .lat_deg=22.5, .lon_deg=113.5, \
  .year=2026, .month=10, .day=30, .hour=12, .last_fix_time_us=now_us})
#define PHONE() assert(location_service_set_phone(23.5, 114.5, 25, EPOCH_MS, EPOCH_MS))
"""


def run_service_c(source, extra_sources=(), include_dirs=()):
    with tempfile.TemporaryDirectory(prefix="framenav-location-") as temporary:
        directory = Path(temporary)
        (directory / "freertos").mkdir()
        (directory / "freertos/FreeRTOS.h").write_text("""
#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
""")
        (directory / "esp_timer.h").write_text("#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
        harness = directory / "main.c"
        harness.write_text(HARNESS + source)
        executable = directory / "check"
        compiled = subprocess.run([
            "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
            "-I", str(directory), "-I", str(ROOT / "include"),
            str(ROOT / "src/location_service.c"), str(ROOT / "src/track_format.c"),
            str(harness), *map(str, extra_sources),
            *[flag for path in include_dirs for flag in ("-I", str(path))],
            "-lm", "-o", str(executable)], capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stderr)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr or f"C check exited {result.returncode}")


class PhoneLocationChecks(unittest.TestCase):
    def test_gps_priority_loss_recovery_and_reuse_of_unexpired_phone(self):
        run_service_c(r"""
int main(void) {
  reset(); PHONE(); GNSS();
  gps_fix_t fix; location_source_t source; uint32_t observed;
  assert(location_service_details(&fix, &source, &observed));
  assert(source == LOCATION_GNSS && fix.lat_deg == 22.5);
  raw.valid = false;
  assert(location_service_details(&fix, &source, &observed));
  assert(source == LOCATION_PHONE && fix.lat_deg == 23.5);
  now_us += 2 * SEC; GNSS();
  assert(location_service_details(&fix, &source, &observed) && source == LOCATION_GNSS);
  raw.valid = false;
  assert(location_service_details(&fix, &source, &observed) && source == LOCATION_PHONE);
  return 0;
}
""")

    def test_stale_or_invalid_gps_cannot_override_phone_or_be_replayed(self):
        run_service_c(r"""
int main(void) {
  reset(); GNSS();
  gps_fix_t fix; location_source_t source; uint32_t observed;
  now_us += 5 * SEC; assert(location_service_get(&fix));
  now_us++; assert(!location_service_get(&fix));
  PHONE();
  assert(location_service_details(&fix, &source, &observed) && source == LOCATION_PHONE);
  GNSS(); raw.time_valid = false;
  assert(location_service_details(&fix, &source, &observed) && source == LOCATION_PHONE);
  GNSS(); raw.day = 32;
  assert(location_service_details(&fix, &source, &observed) && source == LOCATION_PHONE);
  GNSS(); raw.lat_deg = NAN;
  assert(location_service_details(&fix, &source, &observed) && source == LOCATION_PHONE);
  location_service_clear_phone(); raw.valid = false;
  assert(!location_service_details(&fix, &source, &observed));
  assert(source == LOCATION_NONE && !fix.valid);
  return 0;
}
""")

    def test_default_six_hour_expiry_is_exact_and_polls_do_not_extend_it(self):
        run_service_c(r"""
int main(void) {
  reset(); PHONE();
  gps_fix_t fix; phone_location_status_t status;
  location_service_phone_status(&status);
  assert(status.available && status.remaining_s == 6 * 3600);
  now_us += 6 * 3600 * SEC - 1;
  assert(location_service_get(&fix));
  location_service_phone_status(&status);
  assert(status.available && status.remaining_s == 1);
  now_us++;
  assert(!location_service_get(&fix));
  location_service_phone_status(&status);
  assert(status.present && !status.available && !status.remaining_s);
  GNSS(); assert(location_service_get(&fix) && fix.lat_deg == 22.5);
  return 0;
}
""")

    def test_custom_hours_and_capture_age_are_counted_from_observation(self):
        run_service_c(r"""
int main(void) {
  reset(); cfg.phone_location_ttl_hours = 1;
  assert(location_service_set_phone(23.5, 114.5, 25, EPOCH_MS, EPOCH_MS + 2000));
  gps_fix_t fix; phone_location_status_t status;
  location_service_phone_status(&status);
  assert(status.remaining_s == 3598);
  now_us += 3598 * SEC;
  assert(!location_service_get(&fix));
  PHONE(); cfg.phone_location_ttl_hours = 12;
  now_us += 11 * 3600 * SEC; assert(location_service_get(&fix));
  cfg.phone_location_ttl_hours = 2; assert(!location_service_get(&fix));
  return 0;
}
""")

    def test_camera_utc_advances_but_observation_and_coordinates_stay_fixed(self):
        run_service_c(r"""
int main(void) {
  reset(); PHONE();
  gps_fix_t fix; location_source_t source; uint32_t observed, utc;
  now_us += 3600 * SEC;
  assert(location_service_details(&fix, &source, &observed));
  assert(source == LOCATION_PHONE && !fix.altitude_valid);
  assert(fix.lat_deg == 23.5 && fix.lon_deg == 114.5);
  assert(observed == EPOCH_MS / 1000);
  assert(track_epoch_from_fix(&fix, &utc) && utc == EPOCH_MS / 1000 + 3600);
  assert(fix.last_fix_time_us == SEC && fix.last_update_time_us == now_us);
  now_us += 60 * SEC;
  assert(location_service_details(&fix, &source, &observed));
  assert(observed == EPOCH_MS / 1000);
  assert(track_epoch_from_fix(&fix, &utc) && utc == EPOCH_MS / 1000 + 3660);
  return 0;
}
""")

    def test_invalid_phone_payloads_preserve_existing_location(self):
        run_service_c(r"""
int main(void) {
  reset(); PHONE();
  assert(!location_service_set_phone(NAN, 0, 1, EPOCH_MS, EPOCH_MS));
  assert(!location_service_set_phone(0, INFINITY, 1, EPOCH_MS, EPOCH_MS));
  assert(!location_service_set_phone(91, 0, 1, EPOCH_MS, EPOCH_MS));
  assert(!location_service_set_phone(0, -181, 1, EPOCH_MS, EPOCH_MS));
  assert(!location_service_set_phone(0, 0, -1, EPOCH_MS, EPOCH_MS));
  assert(!location_service_set_phone(0, 0, NAN, EPOCH_MS, EPOCH_MS));
  assert(!location_service_set_phone(0, 0, 1, 0, 0));
  assert(!location_service_set_phone(0, 0, 1, UINT64_MAX, UINT64_MAX));
  assert(!location_service_set_phone(0, 0, 1, EPOCH_MS + 1, EPOCH_MS));
  assert(!location_service_set_phone(0, 0, 1, EPOCH_MS, EPOCH_MS + 60001));
  gps_fix_t fix; assert(location_service_get(&fix) && fix.lat_deg == 23.5);
  return 0;
}
""")

    def test_zero_coordinates_clock_rollback_clear_and_reinit(self):
        run_service_c(r"""
int main(void) {
  reset(); assert(location_service_set_phone(0, 0, 0, EPOCH_MS, EPOCH_MS));
  gps_fix_t fix; assert(location_service_get(&fix) && fix.lat_deg == 0 && fix.lon_deg == 0);
  now_us = 0; assert(!location_service_get(&fix));
  now_us = 2 * SEC; location_service_init(&cfg);
  assert(location_service_get(&fix)); // Restarting Web must not reset this service.
  location_service_clear_phone(); assert(!location_service_get(&fix));
  phone_location_status_t status;
  location_service_phone_status(&status); assert(!status.present && !status.available);
  assert(!location_service_get(NULL));
  return 0;
}
""")

    def test_focus_callback_uses_phone_even_when_sample_is_hours_old(self):
        run_service_c("#define ALPHALOC_PHASE2 1\n" + r"""
static unsigned sends;
static gps_fix_t sent;
static void ble_client_send_location(const gps_fix_t *fix) { sent = *fix; ++sends; }
""" + function_source("src/main.c", "get_location_for_send") + "\n" +
                      function_source("src/main.c", "focus_update_cb") + r"""
int main(void) {
  reset(); PHONE(); now_us += 2 * 3600 * SEC;
  focus_update_cb(&cfg); assert(sends == 1 && sent.lat_deg == 23.5);
  GNSS(); focus_update_cb(&cfg); assert(sends == 2 && sent.lat_deg == 22.5);
  now_us += 5 * SEC + 1;
  focus_update_cb(&cfg); assert(sends == 3 && sent.lat_deg == 23.5);
  now_us += 5 * 3600 * SEC;
  focus_update_cb(&cfg); assert(sends == 3);
  return 0;
}
""")

    def test_periodic_publisher_retains_interval_and_uses_the_same_selector(self):
        for interval in (2000, 3000):
            with self.subTest(interval=interval):
                run_service_c("#define ALPHALOC_PHASE2 1\n" +
                              f"#define INTERVAL {interval}\n" + r"""
static jmp_buf stop;
static unsigned waits, sends;
static double expected_latitude;
static void ble_client_send_location(const gps_fix_t *fix) {
  assert(fix->lat_deg == expected_latitude); ++sends;
}
static void vTaskDelay(uint32_t ticks) {
  assert(ticks == INTERVAL);
  if (++waits == 4) longjmp(stop, 1);
  now_us += (int64_t)ticks * 1000;
}
""" + function_source("src/main.c", "get_location_for_send") + "\n" +
                              function_source("src/main.c", "location_publisher_task") + r"""
int main(void) {
  reset(); PHONE(); cfg.gps_interval_ms = INTERVAL;
  expected_latitude = 23.5;
  now_us += 2 * 3600 * SEC;
  if (!setjmp(stop)) location_publisher_task(&cfg);
  assert(sends == 4);
  waits = sends = 0; location_service_clear_phone(); GNSS(); expected_latitude = 22.5;
  if (!setjmp(stop)) location_publisher_task(&cfg);
  assert(sends == (INTERVAL == 2000 ? 3U : 2U));
  waits = sends = 0; raw.valid = false;
  if (!setjmp(stop)) location_publisher_task(&cfg);
  assert(sends == 0);
  return 0;
}
""")

    def test_settings_default_range_and_persistence_wiring(self):
        source = (ROOT / "src/config.c").read_text()
        defines = "\n".join(re.findall(r"^#define .+$", source, re.M))
        functions = "\n".join(function_source("src/config.c", name) for name in (
            "config_set_str_default", "config_terminate_str", "config_reset_default_str",
            "config_set_defaults", "config_validate"))
        run_c('#include <assert.h>\n#include <string.h>\n#include "config.h"\n' +
              defines + "\n" + functions + r"""
int main(void) {
  app_config_t cfg;
  config_set_defaults(&cfg); assert(cfg.phone_location_ttl_hours == 6);
  cfg.phone_location_ttl_hours = 1; config_validate(&cfg); assert(cfg.phone_location_ttl_hours == 1);
  cfg.phone_location_ttl_hours = 720; config_validate(&cfg); assert(cfg.phone_location_ttl_hours == 720);
  cfg.phone_location_ttl_hours = 0; config_validate(&cfg); assert(cfg.phone_location_ttl_hours == 6);
  cfg.phone_location_ttl_hours = UINT32_MAX; config_validate(&cfg); assert(cfg.phone_location_ttl_hours == 6);
  return 0;
}
""")
        self.assertIn('nvs_get_u32(nvs, "phone_ttl_h", &cfg->phone_location_ttl_hours)', source)
        self.assertIn('nvs_set_u32(nvs, "phone_ttl_h", cfg->phone_location_ttl_hours)', source)

    def test_continuous_receiver_and_shared_track_selector(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        flags = config["env:xiao-c6-gnss-power"]["build_flags"]
        self.assertIn("ALPHALOC_GNSS_STANDBY=0", flags)
        self.assertIn("ALPHALOC_GNSS_RATE_1HZ=1", flags)
        self.assertIn("location_service_get(&fix)", function_source("src/track_store.c", "track_store_task"))
        self.assertNotIn("location_service_clear_phone", function_source("src/wifi_web.c", "wifi_cleanup_resources"))

    def test_phone_http_handlers_validate_json_and_preserve_cache_on_failure(self):
        cjson = Path.home() / ".platformio/packages/framework-espidf/components/json/cJSON"
        if not (cjson / "cJSON.c").exists():
            self.skipTest("requires ESP-IDF cJSON sources")
        source = (ROOT / "src/wifi_web.c").read_text()
        handlers = source[source.index("static cJSON *read_phone_json"):
                          source.index("static esp_err_t handle_pair_start")]
        run_service_c(r"""
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#define ESP_OK 0
typedef int esp_err_t;
typedef struct { size_t content_len; } httpd_req_t;
static char request_body[1024];
static const char *request_type = "application/json";
static size_t body_size, body_read;
static unsigned response_status;
static void web_activity(void) {}
static int httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *out, size_t n) {
  (void)req; assert(!strcmp(name, "Content-Type"));
  assert(strlen(request_type) < n); strcpy(out, request_type); return ESP_OK;
}
static int httpd_req_recv(httpd_req_t *req, char *out, size_t n) {
  (void)req; if (n > 7) n = 7;
  if (n > body_size - body_read) n = body_size - body_read;
  memcpy(out, request_body + body_read, n); body_read += n; return (int)n;
}
static void httpd_resp_set_type(httpd_req_t *req, const char *type) {
  (void)req; assert(strstr(type, "charset=utf-8"));
}
static void httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value) {
  (void)req; assert(!strcmp(name, "Cache-Control") && !strcmp(value, "no-store"));
}
static void httpd_resp_set_status(httpd_req_t *req, const char *value) {
  (void)req; response_status = (unsigned)atoi(value);
}
static esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *text) {
  (void)req; assert(text && *text); return ESP_OK;
}
""" + handlers + r"""
static httpd_req_t request(const char *body) {
  strcpy(request_body, body); body_size = strlen(body); body_read = 0; response_status = 200;
  return (httpd_req_t){.content_len = body_size};
}
int main(void) {
  reset();
  const char *valid = "{\"latitude\":23.5,\"longitude\":114.5,\"accuracy\":25,"
                      "\"timestamp_ms\":1793361600000,\"now_ms\":1793361600000}";
  httpd_req_t req = request(valid);
  assert(handle_phone_location(&req) == ESP_OK && response_status == 200);
  gps_fix_t fix; assert(location_service_get(&fix) && fix.lat_deg == 23.5);
  const char *bad[] = {
    "{}", "[]", "null", "{garbage}",
    "{\"latitude\":1e300,\"longitude\":0,\"accuracy\":1,\"timestamp_ms\":1793361600000,\"now_ms\":1793361600000}",
    "{\"latitude\":\"0\",\"longitude\":0,\"accuracy\":1,\"timestamp_ms\":1793361600000,\"now_ms\":1793361600000}",
    "{\"latitude\":0,\"longitude\":0,\"accuracy\":1,\"timestamp_ms\":1e300,\"now_ms\":1e300}",
    "{\"latitude\":0,\"longitude\":0,\"accuracy\":1,\"timestamp_ms\":1793361600000.5,\"now_ms\":1793361600001}",
    "{\"latitude\":0,\"longitude\":0,\"accuracy\":1,\"timestamp_ms\":-1,\"now_ms\":1793361600001}",
    "{\"latitude\":0,\"longitude\":0,\"accuracy\":1,\"timestamp_ms\":1793361600000,\"now_ms\":1793361660001}",
    "{} trailing garbage"
  };
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    req = request(bad[i]); handle_phone_location(&req); assert(response_status == 400);
    assert(location_service_get(&fix) && fix.lat_deg == 23.5);
  }
  req = request(valid); request_body[3] = 0;
  handle_phone_location(&req); assert(response_status == 400);
  req = request(valid); --body_size;
  handle_phone_location(&req); assert(response_status == 400);
  req = request(valid); req.content_len = 1024;
  handle_phone_location(&req); assert(response_status == 400 && body_read == 0);
  req = request(valid); request_type = "text/plain";
  handle_phone_location(&req); assert(response_status == 400);
  req = request("{}"); handle_phone_clear(&req); assert(response_status == 400);
  assert(location_service_get(&fix));
  request_type = "application/json; charset=utf-8";
  req = request("{}"); handle_phone_clear(&req); assert(response_status == 200);
  assert(!location_service_get(&fix));
  return 0;
}
""", extra_sources=(cjson / "cJSON.c",), include_dirs=(cjson,))

    @unittest.skipUnless(shutil.which("node"), "requires Node.js for browser API mocks")
    def test_browser_requests_fresh_geolocation_only_on_click_and_handles_denial(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        start = source.index('      "<script>(()=>')
        end = source.index('      "</body></html>"', start)
        encoded = source[start:end].replace("FRAMENAV_HOSTNAME", '"framenav.device.igp.wiki"')
        script = "".join(json.loads(token) for token in re.findall(r'"(?:\\.|[^"\\])*"', encoded))
        script = script.removeprefix("<script>").removesuffix("</script>")
        script = script.replace("%u", "1").replace("%d", "0")
        harness = r"""
const vm = require('node:vm'), assert = require('node:assert/strict');
const nodes = new Map(), calls = [], geo = [];
function node(selector) {
  if (!nodes.has(selector)) nodes.set(selector, {disabled:false, value:'', textContent:'', events:{},
    addEventListener(type, callback) {this.events[type] = callback;}});
  return nodes.get(selector);
}
const context = {window:{isSecureContext:true}, AbortController, Date,
  setInterval(){}, setTimeout(){return 1;}, clearTimeout(){},
  document:{hidden:false, querySelector:node,
    querySelectorAll(){return [];}, addEventListener(){}},
  navigator:{geolocation:{getCurrentPosition(ok, error, options){geo.push({ok,error,options});}}},
  fetch:async(path, options={})=>{
    calls.push({path,options});
    return {ok:true, text:async()=> '已保存', json:async()=>({status:'有效'})};
  }
};
vm.createContext(context);
""" + "vm.runInContext(" + json.dumps(script, ensure_ascii=False) + r""", context);
(async()=>{
  await new Promise(setImmediate);
  assert.equal(geo.length, 0);
  node('#phone-get').events.click();
  assert.equal(geo.length, 1);
  assert.equal(geo[0].options.maximumAge, 600000);
  assert.equal(geo[0].options.timeout, 15000);
  assert.equal(geo[0].options.enableHighAccuracy, false);
  assert.equal(node('#phone-get').disabled, true);
  await geo[0].ok({coords:{latitude:23.5,longitude:114.5,accuracy:25},timestamp:Date.now()});
  const post = calls.find(c=>c.path === '/location/phone');
  assert.equal(post.options.method, 'POST');
  assert.equal(post.options.headers['Content-Type'], 'application/json');
  const data = JSON.parse(post.options.body);
  assert.equal(data.latitude, 23.5);
  assert.equal(data.longitude, 114.5);
  assert.equal(typeof data.now_ms, 'number');
  assert.equal(node('#phone-get').disabled, false);
  node('#manual-lat').value = '24.1'; node('#manual-lon').value = '115.2';
  node('#phone-manual').events.click();
  await new Promise(setImmediate);
  const manual = calls.filter(c=>c.path === '/location/phone').at(-1);
  assert.equal(JSON.parse(manual.options.body).latitude, 24.1);
  assert.equal(JSON.parse(manual.options.body).longitude, 115.2);
  assert.equal(JSON.parse(manual.options.body).accuracy, 0);
  node('#manual-lat').value = '91'; node('#manual-lon').value = '115.2';
  node('#phone-manual').events.click();
  assert.match(node('#phone-message').textContent, /有效/);
  await new Promise(setImmediate);
  const denialIndex = geo.length;
  node('#phone-get').events.click();
  assert.equal(geo.length, denialIndex + 1);
  geo[denialIndex].error({code:1});
  assert.match(node('#phone-message').textContent, /拒绝/);
  assert.equal(node('#phone-get').disabled, false);
  assert.equal(geo.length, denialIndex + 1, 'permission denial must not retry');
  await new Promise(setImmediate);
  const retryIndex = geo.length;
  node('#phone-get').events.click();
  assert.equal(geo.length, retryIndex + 1);
  geo[retryIndex].error({code:3});
  assert.equal(geo.length, retryIndex + 2);
  assert.equal(node('#phone-get').disabled, true);
  const highAccuracyIndex = retryIndex + 1;
  assert.equal(geo[highAccuracyIndex].options.enableHighAccuracy, true);
  assert.equal(geo[highAccuracyIndex].options.maximumAge, 60000);
  assert.equal(geo[highAccuracyIndex].options.timeout, 60000);
  assert.match(node('#phone-message').textContent, /高精度/);
  geo[highAccuracyIndex].error({code:3});
  assert.equal(geo.length, retryIndex + 2, 'high accuracy retries once');
  assert.match(node('#phone-message').textContent, /超时/);
  assert.equal(node('#phone-get').disabled, false);
  await new Promise(setImmediate);
  const cacheRetryIndex = geo.length;
  node('#phone-get').events.click();
  assert.equal(geo.length, cacheRetryIndex + 1);
  geo[cacheRetryIndex].error({code:2});
  assert.equal(geo.length, cacheRetryIndex + 2, 'cached location retry completes');
  await geo[cacheRetryIndex + 1].ok({coords:{latitude:24,longitude:115,accuracy:10},timestamp:Date.now()});
  const posts = calls.filter(c=>c.path === '/location/phone');
  assert.equal(posts.length, 3);
  assert.equal(JSON.parse(posts[2].options.body).latitude, 24);
  assert.equal(node('#phone-get').disabled, false);
  context.window.isSecureContext = false;
  node('#phone-get').events.click();
  assert.equal(geo.length, cacheRetryIndex + 2, 'insecure context must not call geolocation');
  assert.match(node('#phone-message').textContent, /https:\/\/framenav.device.igp.wiki/);
  await new Promise(setImmediate);
  await node('#phone-clear').events.click();
  assert(calls.some(c=>c.path === '/location/phone/clear' && c.options.method === 'POST'));
})().catch(error=>{console.error(error);process.exitCode=1;});
"""
        result = subprocess.run([shutil.which("node"), "-e", harness], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
