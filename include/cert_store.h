/*
 * FrameNav modification of AlphaLoc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FRAMENAV_CERT_STORE_H
#define FRAMENAV_CERT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tls_credentials.h"

bool cert_store_init(void);
/* Capacities include space for a NUL; returned lengths exclude it.
 * Old certificate-only records return key_length == 0 (use the embedded key). */
bool cert_store_read(uint8_t *cert, size_t cert_capacity, size_t *cert_length,
                     uint8_t *key, size_t key_capacity, size_t *key_length);
bool cert_store_write(const uint8_t *cert, size_t cert_length,
                      const uint8_t *key, size_t key_length);

#endif
