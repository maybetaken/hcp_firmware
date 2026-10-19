/*
 * Minimal private config for the vendored mbedtls 2.4 DHM/bignum used only by
 * BluFi key negotiation. Forced via blufi_dhm_compat.h so it does not collide
 * with the app-wide TF-PSA-Crypto 1.0 config (MBEDTLS_CONFIG_FILE).
 * Only classic finite-field DH + bignum are compiled here; AES-CFB and MD5 are
 * provided self-contained in blufi_security.c (TF-PSA-Crypto removed them).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BLUFI_DHM_CONFIG_H_
#define BLUFI_DHM_CONFIG_H_

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_DHM_C
#define MBEDTLS_AES_C
#define MBEDTLS_MD5_C
#define MBEDTLS_CIPHER_MODE_CFB

#endif /* BLUFI_DHM_CONFIG_H_ */
