/*
 * Minimal private config for the referenced Mbed TLS 2.28 DHM/bignum/AES/MD5, used
 * only by BluFi key negotiation. Force-included through blufi_dhm_compat.h so these
 * translation units do not pick up the application-wide TF-PSA-Crypto config
 * (MBEDTLS_CONFIG_FILE), and so no upstream file has to be edited to state which
 * of its features are wanted here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BLUFI_DHM_CONFIG_H_
#define BLUFI_DHM_CONFIG_H_

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_DHM_C
#define MBEDTLS_AES_C
#define MBEDTLS_MD5_C
#define MBEDTLS_CIPHER_MODE_CFB

#endif /* BLUFI_DHM_CONFIG_H_ */
