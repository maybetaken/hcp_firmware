/*
 * Compile-time rename of the Mbed TLS 2.28 AES symbols for BluFi.
 *
 * BluFi's payload encryption needs AES-128-CFB128, which the TF-PSA-Crypto built
 * into the application does not compile (MBEDTLS_CIPHER_MODE_CFB is off) - but it
 * *does* provide mbedtls_aes_setkey_enc and friends, so adding Mbed TLS' own
 * aes.c to the link would collide. Rather than keeping a patched copy of that
 * file in this repository, the referenced upstream source is compiled as is and
 * this header - force-included with -include - renames the symbols it defines.
 *
 * Only AES is renamed. dhm.c and bignum.c must come from the same tree as a pair
 * (their mbedtls_mpi layout is version specific) and nothing else in the image
 * provides those symbols; md5.c likewise needs no renaming. mbedtls_platform_
 * zeroize is deliberately left alone and resolves to the copy the application
 * already links.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BLUFI_MBEDTLS_RENAME_H_
#define BLUFI_MBEDTLS_RENAME_H_

#define mbedtls_aes_context        blufi_mbedtls_aes_context
#define mbedtls_aes_init           blufi_mbedtls_aes_init
#define mbedtls_aes_free           blufi_mbedtls_aes_free
#define mbedtls_aes_setkey_enc     blufi_mbedtls_aes_setkey_enc
#define mbedtls_aes_setkey_dec     blufi_mbedtls_aes_setkey_dec
#define mbedtls_aes_crypt_ecb      blufi_mbedtls_aes_crypt_ecb
#define mbedtls_aes_crypt_cbc      blufi_mbedtls_aes_crypt_cbc
#define mbedtls_aes_crypt_cfb128   blufi_mbedtls_aes_crypt_cfb128
#define mbedtls_aes_crypt_cfb8     blufi_mbedtls_aes_crypt_cfb8
#define mbedtls_aes_crypt_ctr      blufi_mbedtls_aes_crypt_ctr
#define mbedtls_aes_self_test      blufi_mbedtls_aes_self_test

#endif /* BLUFI_MBEDTLS_RENAME_H_ */
