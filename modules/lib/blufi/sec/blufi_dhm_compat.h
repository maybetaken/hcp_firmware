/*
 * Force-included (-include) for the vendored DHM/bignum translation units.
 * The app-wide mbedTLS interface defines MBEDTLS_CONFIG_FILE to the TF-PSA-Crypto
 * config; we must NOT use that for the vendored mbedtls-2.4 DHM sources, so
 * redirect it to our minimal private config before any mbedtls header is seen.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BLUFI_DHM_COMPAT_H_
#define BLUFI_DHM_COMPAT_H_

#ifdef MBEDTLS_CONFIG_FILE
#undef MBEDTLS_CONFIG_FILE
#endif
#define MBEDTLS_CONFIG_FILE "blufi_dhm_config.h"

#endif /* BLUFI_DHM_COMPAT_H_ */
