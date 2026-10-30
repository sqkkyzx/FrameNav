/* SPDX-License-Identifier: Apache-2.0 */
#include "tls_credentials.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "miniz.h"

static void wipe_bytes(void *buffer, size_t length) {
  volatile uint8_t *bytes = buffer;
  while (length--) *bytes++ = 0;
}

void tls_credentials_free(tls_credentials_t *credentials) {
  if (!credentials) return;
  if (credentials->key) {
    wipe_bytes(credentials->key, credentials->key_len + 1U);
    free(credentials->key);
  }
  free(credentials->cert);
  *credentials = (tls_credentials_t){0};
}

static int tls_random(void *context, unsigned char *out, size_t length) {
  (void)context;
  esp_fill_random(out, length);
  return 0;
}

bool tls_credentials_validate(const uint8_t *cert, size_t cert_size,
                               const uint8_t *key, size_t key_size) {
  if (!cert || !key || cert_size < 2 || key_size < 2 ||
      cert_size > FRAMENAV_CERT_MAX_BYTES + 1U ||
      key_size > FRAMENAV_KEY_MAX_BYTES + 1U ||
      cert[cert_size - 1] || key[key_size - 1] ||
      memchr(cert, 0, cert_size - 1) || memchr(key, 0, key_size - 1) ||
      !strstr((const char *)cert, "-----BEGIN CERTIFICATE-----") ||
      strstr((const char *)cert, "PRIVATE KEY")) return false;

  mbedtls_x509_crt chain;
  mbedtls_pk_context private_key;
  mbedtls_x509_crt_init(&chain);
  mbedtls_pk_init(&private_key);
  bool ok = false;
  if (mbedtls_x509_crt_parse(&chain, cert, cert_size) != 0 ||
      mbedtls_pk_parse_key(&private_key, key, key_size, NULL, 0,
                            tls_random, NULL) != 0) goto done;

  /* The archive is authenticated by its HTTPS transport. Check the leaf SAN
   * explicitly so a renewal for another domain cannot replace our identity. */
  bool domain_matches = false;
  for (const mbedtls_x509_sequence *san = &chain.subject_alt_names;
       san && san->buf.p; san = san->next) {
    if (san->buf.tag == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | 2) &&
        san->buf.len == strlen(FRAMENAV_TLS_HOSTNAME) &&
        strncasecmp((const char *)san->buf.p, FRAMENAV_TLS_HOSTNAME,
                     san->buf.len) == 0) {
      domain_matches = true;
    }
  }
  ok = domain_matches &&
       mbedtls_pk_check_pair(&chain.pk, &private_key, tls_random, NULL) == 0;
done:
  mbedtls_pk_free(&private_key);
  mbedtls_x509_crt_free(&chain);
  return ok;
}

static uint16_t zip_u16(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t zip_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t zip_crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffffU;
  while (length--) {
    crc ^= *data++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int)(crc & 1U));
  }
  return ~crc;
}

typedef struct {
  const uint8_t *data;
  size_t compressed, expanded;
  uint32_t crc;
  uint16_t method;
} zip_file_t;

static bool zip_extract(const zip_file_t *entry, size_t limit,
                         uint8_t **out, size_t *out_len) {
  if (!entry->data || !entry->expanded || entry->expanded > limit) return false;
  uint8_t *data = malloc(entry->expanded + 1U);
  if (!data) return false;
  bool ok = false;
  if (entry->method == 0 && entry->compressed == entry->expanded) {
    memcpy(data, entry->data, entry->expanded);
    ok = true;
  } else if (entry->method == 8) {
    ok = tinfl_decompress_mem_to_mem(data, entry->expanded, entry->data,
                                    entry->compressed, 0) == entry->expanded;
  }
  ok = ok && zip_crc32(data, entry->expanded) == entry->crc;
  if (!ok) {
    wipe_bytes(data, entry->expanded + 1U);
    free(data);
    return false;
  }
  data[entry->expanded] = 0;
  *out = data;
  *out_len = entry->expanded;
  return true;
}

bool tls_credentials_from_zip(const uint8_t *zip, size_t zip_len,
                              tls_credentials_t *out) {
  if (!out) return false;
  *out = (tls_credentials_t){0};
  if (!zip || zip_len < 22 || zip_len > FRAMENAV_TLS_ZIP_MAX_BYTES) return false;
  size_t end = zip_len - 22;
  for (;;) {
    if (zip_u32(zip + end) == 0x06054b50U &&
        zip_u16(zip + end + 20) == zip_len - end - 22) break;
    if (!end) return false;
    --end;
  }
  const uint8_t *eocd = zip + end;
  size_t count = zip_u16(eocd + 10);
  size_t directory = zip_u32(eocd + 16);
  size_t directory_size = zip_u32(eocd + 12);
  if (zip_u16(eocd + 4) || zip_u16(eocd + 6) || !count || count > 64 ||
      zip_u16(eocd + 8) != count || directory > end ||
      directory_size != end - directory) return false;

  zip_file_t pem = {0}, crt = {0}, key = {0};
  size_t cursor = directory;
  for (size_t i = 0; i < count; ++i) {
    if (cursor > end || end - cursor < 46 ||
        zip_u32(zip + cursor) != 0x02014b50U) return false;
    const uint8_t *entry = zip + cursor;
    size_t name_len = zip_u16(entry + 28);
    size_t extra_len = zip_u16(entry + 30), comment_len = zip_u16(entry + 32);
    size_t entry_size = 46 + name_len + extra_len + comment_len;
    if (!name_len || entry_size > end - cursor || zip_u16(entry + 34)) return false;
    const uint8_t *name = entry + 46;
    if (memchr(name, 0, name_len)) return false;
    const uint8_t *leaf = name;
    for (size_t j = 0; j < name_len; ++j) if (name[j] == '/') leaf = name + j + 1;
    size_t leaf_len = name_len - (size_t)(leaf - name);
    zip_file_t *target = NULL;
    const char *names[] = {FRAMENAV_TLS_HOSTNAME "_bundle.pem",
                          FRAMENAV_TLS_HOSTNAME "_bundle.crt",
                          FRAMENAV_TLS_HOSTNAME ".key"};
    zip_file_t *targets[] = {&pem, &crt, &key};
    for (unsigned j = 0; j < 3; ++j)
      if (leaf_len == strlen(names[j]) && !memcmp(leaf, names[j], leaf_len))
        target = targets[j];
    if (target) {
      uint16_t flags = zip_u16(entry + 8), method = zip_u16(entry + 10);
      size_t local = zip_u32(entry + 42);
      size_t compressed = zip_u32(entry + 20);
      if (target->data || (flags & ~0x080eU) || (method != 0 && method != 8) ||
          local > directory || directory - local < 30 ||
          zip_u32(zip + local) != 0x04034b50U) return false;
      const uint8_t *header = zip + local;
      size_t local_name = zip_u16(header + 26), local_extra = zip_u16(header + 28);
      size_t data_offset = 30 + local_name + local_extra;
      if (zip_u16(header + 6) != flags || zip_u16(header + 8) != method ||
          local_name != name_len || data_offset > directory - local ||
          memcmp(header + 30, name, name_len) ||
          compressed > directory - local - data_offset) return false;
      if (!(flags & 8U) &&
          (zip_u32(header + 14) != zip_u32(entry + 16) ||
           zip_u32(header + 18) != compressed ||
           zip_u32(header + 22) != zip_u32(entry + 24))) return false;
      *target = (zip_file_t){.data = header + data_offset, .method = method,
                            .compressed = compressed,
                            .expanded = zip_u32(entry + 24),
                            .crc = zip_u32(entry + 16)};
    }
    cursor += entry_size;
  }
  if (cursor != end ||
      !zip_extract(pem.data ? &pem : &crt, FRAMENAV_CERT_MAX_BYTES,
                    &out->cert, &out->cert_len) ||
      !zip_extract(&key, FRAMENAV_KEY_MAX_BYTES, &out->key, &out->key_len) ||
      !tls_credentials_validate(out->cert, out->cert_len + 1U,
                                 out->key, out->key_len + 1U)) {
    tls_credentials_free(out);
    return false;
  }
  return true;
}
