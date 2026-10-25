/*
 * BluFi security handshake (ESP-compatible): classic Diffie-Hellman key
 * negotiation (mbedtls DHM) -> MD5(shared) = 16-byte PSK -> AES-128-CFB128.
 * Ported faithfully from esp-idf examples/bluetooth/blufi/main/blufi_security.c.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BLUFI_SECURITY_H_
#define BLUFI_SECURITY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Negotiation payload sub-types (self-defined in the DH handshake). */
#define SEC_TYPE_DH_PARAM_LEN  0x00
#define SEC_TYPE_DH_PARAM_DATA 0x01
#define SEC_TYPE_DH_P          0x02
#define SEC_TYPE_DH_G          0x03
#define SEC_TYPE_DH_PUBLIC     0x04

int blufi_security_init(void);
void blufi_security_deinit(void);

/*
 * Handle a NEG (negotiation) frame payload. On SEC_TYPE_DH_PARAM_DATA this
 * completes the DH exchange and returns the device's DH public key in *out
 * (points to an internal static buffer; valid until next call).
 * Returns the number of output bytes, or negative on error.
 */
int blufi_negotiate(const uint8_t *data, uint16_t len, const uint8_t **out,
		    uint16_t *out_len);

/* AES-128-CFB128 encrypt/decrypt in place; iv8 seeds iv[0] (= packet seq). */
int blufi_aes_encrypt(uint8_t iv8, uint8_t *data, int len);
int blufi_aes_decrypt(uint8_t iv8, uint8_t *data, int len);

#ifdef __cplusplus
}
#endif

#endif /* BLUFI_SECURITY_H_ */
