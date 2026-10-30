/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAMENAV_TLS_HOSTNAME "framenav.device.igp.wiki"
#define FRAMENAV_CERT_MAX_BYTES 12288U
#define FRAMENAV_KEY_MAX_BYTES 8192U
#define FRAMENAV_TLS_ZIP_MAX_BYTES 65536U

typedef struct {
  uint8_t *cert;
  size_t cert_len;
  uint8_t *key;
  size_t key_len;
} tls_credentials_t;

/* Lengths in this struct exclude the extra PEM NUL terminator. */
bool tls_credentials_from_zip(const uint8_t *zip, size_t zip_len,
                              tls_credentials_t *out);
void tls_credentials_free(tls_credentials_t *credentials);
/* Both input lengths include the required PEM NUL terminator. */
bool tls_credentials_validate(const uint8_t *cert, size_t cert_size,
                               const uint8_t *key, size_t key_size);
