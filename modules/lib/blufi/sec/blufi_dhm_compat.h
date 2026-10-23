/*
 * Force-included (-include) into the referenced Mbed TLS 2.28 translation units
 * (dhm, bignum, aes, md5) and into blufi_security.c.
 *
 * The application-wide mbedTLS interface defines MBEDTLS_CONFIG_FILE to the
 * TF-PSA-Crypto config; those sources must not use it, so redirect it to our
 * minimal private config before any mbedtls header is seen. blufi_mbedtls_rename.h
 * is force-included next to it, and both are ours - no upstream file is edited.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BLUFI_DHM_COMPAT_H_
#define BLUFI_DHM_COMPAT_H_

#ifdef MBEDTLS_CONFIG_FILE
#undef MBEDTLS_CONFIG_FILE
#endif
#define MBEDTLS_CONFIG_FILE "blufi_dhm_config.h"

#endif /* BLUFI_DHM_COMPAT_H_ */
