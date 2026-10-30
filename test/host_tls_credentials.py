#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the actual ZIP/crypto and dual-slot flash code with host adapters."""

import ctypes as ct
import importlib.util
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

from host_phase1 import function_source, run_c

ROOT = Path(__file__).resolve().parents[1]
HOST = "framenav.device.igp.wiki"
CERT = HOST + "_bundle.pem"
CRT = HOST + "_bundle.crt"
KEY = HOST + ".key"


def compile_library(directory, source, extra=()):
    output = directory / "test.so"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-shared", "-fPIC", "-I", str(directory),
                    "-I", str(ROOT / "include"), str(source), *extra,
                    "-o", str(output)], check=True, capture_output=True)
    return ct.CDLL(str(output))


class Credentials(ct.Structure):
    _fields_ = [("cert", ct.c_void_p), ("cert_len", ct.c_size_t),
                ("key", ct.c_void_p), ("key_len", ct.c_size_t)]


class ZipCredentialsChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        candidates = [os.environ.get("MBEDTLS_ROOT", ""),
                      "/opt/homebrew/opt/mbedtls", "/usr/local/opt/mbedtls"]
        mbed = next((Path(p) for p in candidates if p and
                     (Path(p) / "include/mbedtls/version.h").exists()), None)
        if not mbed or not shutil.which("openssl"):
            raise unittest.SkipTest("requires Mbed TLS 3 (MBEDTLS_ROOT) and openssl")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        (cls.directory / "esp_random.h").write_text("""
#include <stddef.h>
#include <stdlib.h>
static inline void esp_fill_random(void *out, size_t size) {
  arc4random_buf(out, size);
}
""")
        # ESP32-C6 provides this raw-deflate API in ROM. Use zlib on the host.
        (cls.directory / "miniz.h").write_text("""
#include <stddef.h>
#include <zlib.h>
static inline size_t tinfl_decompress_mem_to_mem(
    void *out, size_t capacity, const void *input, size_t size, int flags) {
  (void)flags;
  z_stream stream = {0};
  stream.next_in = (Bytef *)input; stream.avail_in = (uInt)size;
  stream.next_out = out; stream.avail_out = (uInt)capacity;
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return (size_t)-1;
  int result = inflate(&stream, Z_FINISH);
  size_t count = stream.total_out;
  inflateEnd(&stream);
  return result == Z_STREAM_END ? count : (size_t)-1;
}
""")
        cls.lib = compile_library(cls.directory, ROOT / "src/tls_credentials.c", [
            "-D_DEFAULT_SOURCE", "-I", str(mbed / "include"),
            "-L", str(mbed / "lib"), "-Wl,-rpath," + str(mbed / "lib"),
            "-lmbedx509", "-lmbedcrypto", "-lz"])
        cls.lib.tls_credentials_from_zip.argtypes = [ct.c_void_p, ct.c_size_t,
                                                    ct.POINTER(Credentials)]
        cls.lib.tls_credentials_from_zip.restype = ct.c_bool
        cls.lib.tls_credentials_free.argtypes = [ct.POINTER(Credentials)]
        cls.lib.tls_credentials_free.restype = None
        cls.lib.tls_credentials_validate.argtypes = [ct.c_void_p, ct.c_size_t,
                                                    ct.c_void_p, ct.c_size_t]
        cls.lib.tls_credentials_validate.restype = ct.c_bool
        cls.cert, cls.key = cls.make_pair("first")
        cls.rotated_cert, cls.rotated_key = cls.make_pair("rotated")
        cls.ec_cert, cls.ec_key = cls.make_pair("ec", ec=True)
        cls.wrong_cert, cls.wrong_key = cls.make_pair("wrong", domain="other.example")
        cls.wrong_san_cert, cls.wrong_san_key = cls.make_pair(
            "wrong-san", domain="other.example", common_name=HOST)
        encrypted = cls.directory / "encrypted.key"
        subprocess.run(["openssl", "pkey", "-in", str(cls.directory / "first.key"),
                        "-aes-256-cbc", "-passout", "pass:synthetic-test-only",
                        "-out", str(encrypted)], check=True, capture_output=True)
        cls.encrypted_key = encrypted.read_bytes()

    @classmethod
    def make_pair(cls, name, domain=HOST, ec=False, common_name=None):
        key, cert = cls.directory / (name + ".key"), cls.directory / (name + ".pem")
        options = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"] if ec else ["-newkey", "rsa:2048"]
        subprocess.run(["openssl", "req", "-x509", *options, "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "2",
                        "-subj", "/CN=" + (common_name or domain),
                        "-addext", "subjectAltName=DNS:" + domain],
                       check=True, capture_output=True)
        return cert.read_bytes(), key.read_bytes()

    def archive(self, entries=None, compression=zipfile.ZIP_DEFLATED, descriptor=False):
        class Unseekable(io.BytesIO):
            def seekable(self):
                return False

            def seek(self, *args):
                raise io.UnsupportedOperation()
        buffer = Unseekable() if descriptor else io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=compression) as archive:
            for name, data in (entries if entries is not None else
                               [(CERT, self.cert), (KEY, self.key)]):
                archive.writestr(name, data)
        return buffer.getvalue()

    def check_archive(self, archive, expected=None):
        output = Credentials()
        ok = self.lib.tls_credentials_from_zip(archive, len(archive), ct.byref(output))
        try:
            self.assertEqual(ok, expected is not None)
            if expected:
                self.assertTrue(ct.string_at(output.cert, output.cert_len) == expected[0])
                self.assertTrue(ct.string_at(output.key, output.key_len) == expected[1],
                                "extracted private key differs")
                self.assertEqual(ct.string_at(output.cert + output.cert_len, 1), b"\0")
                self.assertEqual(ct.string_at(output.key + output.key_len, 1), b"\0")
            else:
                self.assertFalse(output.cert or output.key or output.cert_len or output.key_len)
        finally:
            self.lib.tls_credentials_free(ct.byref(output))
        self.assertFalse(output.cert or output.key or output.cert_len or output.key_len)

    def test_root_and_nested_folder_stored_and_deflate(self):
        for prefix in ("", HOST + "_nginx/", "outer/inner/"):
            for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                with self.subTest(prefix=prefix, method=method):
                    archive = self.archive([(prefix + CERT, self.cert),
                                            (prefix + KEY, self.key),
                                            ("__MACOSX/._" + KEY, b"ignored metadata"),
                                            (prefix + HOST + ".csr", b"ignored")], method)
                    self.check_archive(archive, (self.cert, self.key))

    def test_rotated_rsa_and_ec_private_keys_are_accepted(self):
        for cert, key in ((self.rotated_cert, self.rotated_key), (self.ec_cert, self.ec_key)):
            self.check_archive(self.archive([(CERT, cert), (KEY, key)]), (cert, key))

    def test_pem_preferred_and_crt_fallback(self):
        self.check_archive(self.archive([(CRT, self.cert), (KEY, self.key)]),
                           (self.cert, self.key))
        self.check_archive(self.archive([(CERT, self.cert), (CRT, b"ignored"),
                                         (KEY, self.key)]), (self.cert, self.key))
        self.check_archive(self.archive([(CERT, b"bad PEM"), (CRT, self.cert),
                                         (KEY, self.key)]))

    def test_data_descriptor_and_archive_comment(self):
        self.check_archive(self.archive(descriptor=True), (self.cert, self.key))
        archive = bytearray(self.archive())
        struct.pack_into("<H", archive, len(archive) - 2, 5)
        self.check_archive(bytes(archive) + b"hello", (self.cert, self.key))

    def test_wrong_domain_mismatched_key_or_malformed_pem(self):
        for cert, key in ((self.cert, self.rotated_key), (self.wrong_cert, self.wrong_key),
                          (self.wrong_san_cert, self.wrong_san_key),
                          (self.cert, self.encrypted_key),
                          (b"-----BEGIN CERTIFICATE-----\ninvalid", self.key),
                          (self.cert, b"invalid private key"),
                          (self.cert + b"\0hidden", self.key),
                          (self.cert + b"PRIVATE KEY", self.key)):
            self.check_archive(self.archive([(CERT, cert), (KEY, key)]))

    def test_full_chain_parsed_with_leaf_identity_and_key(self):
        chain = self.cert + self.wrong_cert
        self.check_archive(self.archive([(CERT, chain), (KEY, self.key)]),
                           (chain, self.key))
        self.check_archive(self.archive([(CERT, self.wrong_cert + self.cert),
                                         (KEY, self.key)]))

    def test_missing_or_duplicate_credentials(self):
        for entries in ([], [(CERT, self.cert)], [(KEY, self.key)],
                        [(CERT, self.cert), ("other/" + CERT, self.cert), (KEY, self.key)],
                        [(CERT, self.cert), (KEY, self.key), ("other/" + KEY, self.key)]):
            self.check_archive(self.archive(entries))

    def test_size_limits_and_deflate_bomb(self):
        self.check_archive(b"x" * 65537)
        for cert, key in ((b"x" * 12289, self.key), (self.cert, b"x" * 8193),
                          (b"x" * 1000000, b"x")):
            self.check_archive(self.archive([(CERT, cert), (KEY, key)]))

    def test_encryption_unsupported_method_and_header_disagreement(self):
        archive = self.archive()
        central = archive.index(b"PK\x01\x02")
        for local_offset, central_offset, value in ((6, 8, 1), (8, 10, 99)):
            modified = bytearray(archive)
            struct.pack_into("<H", modified, local_offset, value)
            struct.pack_into("<H", modified, central + central_offset, value)
            self.check_archive(bytes(modified))
        modified = bytearray(archive)
        modified[30] ^= 1  # Central/local names must agree.
        self.check_archive(bytes(modified))

    def test_bad_crc_truncation_offsets_and_multidisk(self):
        archive = self.archive(compression=zipfile.ZIP_STORED)
        modified = bytearray(archive)
        modified[30 + len(CERT) + 100] ^= 1
        self.check_archive(bytes(modified))
        for cut in (0, 1, 21, len(archive) // 2, len(archive) - 1):
            self.check_archive(archive[:cut])
        for offset, format_string, value in ((4, "<H", 1), (8, "<H", 1),
                                             (10, "<H", 65), (16, "<I", 0xffffffff)):
            modified = bytearray(archive)
            struct.pack_into(format_string, modified, len(archive) - 22 + offset, value)
            self.check_archive(bytes(modified))

    def test_pem_validation_requires_sizes_with_nul(self):
        validate = self.lib.tls_credentials_validate
        self.assertTrue(validate(self.cert + b"\0", len(self.cert) + 1,
                                 self.key + b"\0", len(self.key) + 1))
        self.assertFalse(validate(self.cert, len(self.cert), self.key, len(self.key)))


class FlashStoreChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        (directory / "esp_log.h").write_text("""
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
""")
        (directory / "esp_partition.h").write_text("""
#pragma once
#include <stddef.h>
typedef struct { size_t size; } esp_partition_t;
#define ESP_OK 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
const esp_partition_t *esp_partition_find_first(int, int, const char *);
int esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
int esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
int esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
""")
        harness = directory / "flash.c"
        harness.write_text('#include "' + str(ROOT / 'src/cert_store.c') + '"\n' + """
#include <assert.h>
static uint8_t flash[65536];
static esp_partition_t partition = {.size = sizeof(flash)};
static unsigned operation, failure;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label) {
  (void)type; (void)subtype;
  assert(!strcmp(label, "tls_cert")); return &partition;
}
int esp_partition_read(const esp_partition_t *p, size_t off, void *out, size_t n) {
  assert(p == &partition && off + n <= sizeof(flash));
  if (++operation == failure) return -1;
  memcpy(out, flash + off, n); return ESP_OK;
}
int esp_partition_write(const esp_partition_t *p, size_t off, const void *in, size_t n) {
  assert(p == &partition && off + n <= sizeof(flash));
  bool fail = ++operation == failure;
  const uint8_t *bytes = in;
  for (size_t i = 0; i < (fail ? n / 2 : n); ++i) {
    assert((flash[off+i] & bytes[i]) == bytes[i]);
    flash[off+i] &= bytes[i];
  }
  return fail ? -1 : ESP_OK;
}
int esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n) {
  assert(p == &partition && !(off % 4096) && !(n % 4096));
  bool fail = ++operation == failure;
  memset(flash + off, 0xff, fail ? n / 2 : n); return fail ? -1 : ESP_OK;
}
void reset_flash(void) {
  memset(flash, 0xff, sizeof(flash)); s_partition = NULL;
  operation = failure = 0; partition.size = sizeof(flash);
}
void inject_failure(unsigned at) { operation = 0; failure = at; }
unsigned operations(void) { return operation; }
void set_flash(size_t off, const void *data, size_t n) {
  assert(off + n <= sizeof(flash)); memcpy(flash + off, data, n);
}
void get_flash(size_t off, void *out, size_t n) { memcpy(out, flash + off, n); }
void partition_size(size_t size) { s_partition = NULL; partition.size = size; }
""")
        cls.lib = compile_library(directory, harness)
        cls.lib.cert_store_read.argtypes = [ct.c_void_p, ct.c_size_t, ct.POINTER(ct.c_size_t),
                                            ct.c_void_p, ct.c_size_t, ct.POINTER(ct.c_size_t)]
        cls.lib.cert_store_read.restype = ct.c_bool
        cls.lib.cert_store_write.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_void_p, ct.c_size_t]
        cls.lib.cert_store_write.restype = ct.c_bool
        cls.lib.cert_store_init.restype = ct.c_bool
        cls.lib.set_flash.argtypes = cls.lib.get_flash.argtypes = [ct.c_size_t, ct.c_void_p, ct.c_size_t]
        cls.lib.partition_size.argtypes = [ct.c_size_t]
        cls.lib.inject_failure.argtypes = [ct.c_uint]

    def setUp(self):
        self.lib.reset_flash()

    def write(self, cert=b"old certificate", key=b"old private key"):
        return self.lib.cert_store_write(cert, len(cert), key, len(key))

    def read(self, cert_capacity=12289, key_capacity=8193):
        cert = ct.create_string_buffer(cert_capacity)
        key = ct.create_string_buffer(key_capacity)
        cert_len, key_len = ct.c_size_t(99), ct.c_size_t(99)
        ok = self.lib.cert_store_read(cert, cert_capacity, ct.byref(cert_len),
                                      key, key_capacity, ct.byref(key_len))
        if not ok:
            self.assertEqual((cert_len.value, key_len.value), (0, 0))
            return None
        self.assertEqual(cert[cert_len.value], b"\0")
        self.assertEqual(key[key_len.value], b"\0")
        return cert.raw[:cert_len.value], key.raw[:key_len.value]

    def test_pair_rotation_and_reload(self):
        self.assertIsNone(self.read())
        for n in range(4):
            cert, key = ("certificate %d" % n).encode(), ("key %d" % n).encode()
            self.assertTrue(self.write(cert, key))
            self.assertTrue(self.read() == (cert, key))

    def test_every_io_failure_or_partial_write_preserves_a_complete_pair(self):
        self.assertTrue(self.write())
        self.lib.inject_failure(0)
        self.assertTrue(self.write(b"new certificate", b"new private key"))
        operations = self.lib.operations()
        self.assertGreater(operations, 6)
        for fault in range(1, operations + 1):
            with self.subTest(operation=fault):
                self.lib.reset_flash()
                self.assertTrue(self.write())
                previous = ct.create_string_buffer(32768)
                self.lib.get_flash(0, previous, 32768)
                self.lib.inject_failure(fault)
                self.assertFalse(self.write(b"new certificate", b"new private key"))
                self.lib.inject_failure(0)
                preserved = ct.create_string_buffer(32768)
                self.lib.get_flash(0, preserved, 32768)
                self.assertTrue(previous.raw == preserved.raw)
                self.assertIn(self.read(), [(b"old certificate", b"old private key"),
                                            (b"new certificate", b"new private key")])

    def test_corrupt_latest_header_certificate_or_key_falls_back(self):
        for offset in (0, 8, 16, 24, 28, 28 + len(b"new certificate")):
            with self.subTest(offset=offset):
                self.lib.reset_flash()
                self.assertTrue(self.write())
                self.assertTrue(self.write(b"new certificate", b"new private key"))
                self.lib.set_flash(32768 + offset, b"\x80", 1)
                self.assertTrue(self.read() == (b"old certificate", b"old private key"))

    def test_legacy_certificate_only_record_migrates_without_erasing_it(self):
        cert = b"legacy certificate"
        record = struct.pack("<4I", 0x46544331, 1, len(cert), zlib.crc32(cert)) + cert
        self.lib.set_flash(0, record, len(record))
        self.assertTrue(self.read() == (cert, b""))
        self.assertTrue(self.write())
        self.assertTrue(self.read() == (b"old certificate", b"old private key"))
        previous = ct.create_string_buffer(len(record))
        self.lib.get_flash(0, previous, len(record))
        self.assertTrue(previous.raw[:len(record)] == record)

    def test_generation_wrap_chooses_newest(self):
        for slot, generation, payload in ((0, 0xffffffff, b"AB"), (1, 0, b"CD")):
            header = struct.pack("<6I", 0x46544331, 2, generation, 1, 1, zlib.crc32(payload))
            record = header + struct.pack("<I", zlib.crc32(header)) + payload
            self.lib.set_flash(slot * 32768, record, len(record))
        self.assertTrue(self.read() == (b"C", b"D"))
        self.assertTrue(self.write())
        self.assertTrue(self.read() == (b"old certificate", b"old private key"))

    def test_bounds_and_partition_capacity(self):
        self.assertTrue(self.write())
        self.assertIsNone(self.read(4, 8193))
        self.assertIsNone(self.read(12289, 4))
        self.assertFalse(self.write(b"", b"key"))
        self.assertFalse(self.write(b"cert", b""))
        self.assertFalse(self.write(b"x" * 12289, b"key"))
        self.assertFalse(self.write(b"cert", b"x" * 8193))
        self.lib.partition_size(32768)
        self.assertFalse(self.lib.cert_store_init())


class EmbeddedPemChecks(unittest.TestCase):
    def test_generated_arrays_include_exactly_one_nul_and_private_permissions(self):
        spec = importlib.util.spec_from_file_location("embed_tls", ROOT / "scripts/embed_tls_cert.py")
        embed = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(embed)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            cert = b"-----BEGIN CERTIFICATE-----\nsynthetic\n-----END CERTIFICATE-----\n"
            key = b"-----BEGIN PRIVATE KEY-----\nsynthetic\n-----END PRIVATE KEY-----\n"
            (directory / "cert").write_bytes(cert)
            (directory / "key").write_bytes(key)
            output = directory / "embedded.c"
            subprocess.run([sys.executable, str(ROOT / "scripts/embed_tls_cert.py"),
                            "--cert", str(directory / "cert"), "--key", str(directory / "key"),
                            "--output", str(output)], check=True, capture_output=True)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            text = output.read_text()
            self.assertIn(embed.c_array("framenav_server_cert", cert), text)
            self.assertIn(embed.c_array("framenav_server_key", key), text)
            self.assertIn(f"framenav_server_cert_len = {len(cert) + 1};", text)
            self.assertEqual(text.count("0x00,"), 2)


class DownloadChecks(unittest.TestCase):
    def test_http_status_cached_body_truncation_limits_and_cleanup(self):
        source = (ROOT / "src/wifi_web.c").read_text()
        start = source.index("typedef struct {\n  uint8_t *buffer;\n  size_t length, capacity;")
        end = source.index("} cert_download_ctx_t;", start) + len("} cert_download_ctx_t;")
        run_c("""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tls_credentials.h"
#define FRAMENAV_CERT_UPDATE_URL "https://framenav.igp.wiki/download/certs.zip"
#define ESP_OK 0
#define HTTP_METHOD_GET 0
typedef void *esp_http_client_handle_t;
typedef struct {
  const char *url;
  int method, timeout_ms, buffer_size, buffer_size_tx;
  void (*crt_bundle_attach)(void);
  bool disable_auto_redirect;
} esp_http_client_config_t;
static uint8_t body[65537];
static size_t position, size;
static int status, reads, writes, parses;
static int64_t declared;
static bool complete, headers_cached, open_fail, save_fail, connected;
static int64_t esp_timer_get_time(void) { return 0; }
static void esp_crt_bundle_attach(void) {}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
  assert(!strcmp(config->url, FRAMENAV_CERT_UPDATE_URL));
  assert(config->disable_auto_redirect && config->crt_bundle_attach == esp_crt_bundle_attach);
  assert(config->method == HTTP_METHOD_GET);
  connected = true; return (void *)1;
}
static int esp_http_client_open(void *client, int length) {
  (void)client; assert(length == 0); return open_fail ? -1 : ESP_OK;
}
static int64_t esp_http_client_fetch_headers(void *client) { (void)client; return declared; }
static int esp_http_client_get_status_code(void *client) { (void)client; return status; }
static bool esp_http_client_is_complete_data_received(void *client) {
  (void)client; return complete && (headers_cached || position == size);
}
static int esp_http_client_read(void *client, char *out, int capacity) {
  (void)client; ++reads;
  size_t count = size - position;
  if (count > (size_t)capacity) count = capacity;
  memcpy(out, body + position, count); position += count; return (int)count;
}
static void esp_http_client_cleanup(void *client) { (void)client; connected = false; }
bool tls_credentials_from_zip(const uint8_t *zip, size_t length, tls_credentials_t *out) {
  assert(!connected); ++parses;
  if (!length || length != size || memcmp(zip, body, length)) return false;
  out->cert = malloc(5); out->key = malloc(8);
  memcpy(out->cert, "cert", 5); memcpy(out->key, "new-key", 8);
  out->cert_len = 4; out->key_len = 7; return true;
}
void tls_credentials_free(tls_credentials_t *out) {
  free(out->cert); free(out->key); *out = (tls_credentials_t){0};
}
bool cert_store_write(const uint8_t *cert, size_t cert_len,
                       const uint8_t *key, size_t key_len) {
  assert(!connected && cert_len == 4 && key_len == 7);
  assert(!strcmp((const char *)cert, "cert") && !strcmp((const char *)key, "new-key"));
  ++writes; return !save_fail;
}
""" + source[start:end] + "\n" +
              function_source("src/wifi_web.c", "cert_download_free") + "\n" +
              function_source("src/wifi_web.c", "cert_download_append") + "\n" +
              function_source("src/wifi_web.c", "download_updated_tls_credentials") + """
static void reset(void) {
  memset(body, 'A', sizeof(body)); size = 6000; position = 0; status = 200;
  declared = size; complete = true; headers_cached = open_fail = save_fail = false;
  reads = writes = parses = 0;
}
int main(void) {
  reset(); assert(download_updated_tls_credentials()); assert(writes == 1 && !connected);
  reset(); headers_cached = true; // fetch_headers already received the body.
  assert(download_updated_tls_credentials()); assert(reads > 0 && writes == 1);
  reset(); declared = 0; // Chunked response, unknown length until complete.
  assert(download_updated_tls_credentials()); assert(writes == 1);
  reset(); size = declared = 65536;
  assert(download_updated_tls_credentials()); assert(writes == 1);
  reset(); size = declared = 65537;
  assert(!download_updated_tls_credentials()); assert(!reads && !writes && !parses);
  reset(); size = 65537; declared = 0;
  assert(!download_updated_tls_credentials()); assert(!writes && !parses && !connected);
  reset(); status = 302;
  assert(!download_updated_tls_credentials()); assert(!reads && !writes);
  reset(); complete = false;
  assert(!download_updated_tls_credentials()); assert(!writes && !parses);
  reset(); open_fail = true;
  assert(!download_updated_tls_credentials()); assert(!reads && !writes && !connected);
  reset(); declared = -1;
  assert(!download_updated_tls_credentials()); assert(!reads && !writes);
  reset(); size = declared = 0;
  assert(!download_updated_tls_credentials()); assert(!writes);
  reset(); save_fail = true;
  assert(!download_updated_tls_credentials()); assert(writes == 1 && !connected);
  return 0;
}
""")


if __name__ == "__main__":
    unittest.main(verbosity=2)
