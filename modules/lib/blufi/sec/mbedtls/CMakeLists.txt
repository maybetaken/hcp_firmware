# Mbed TLS sources behind BluFi's encrypted provisioning.
#
# This library keeps no copy of any of this: BLUFI_MBEDTLS_DIR points at the
# deps/mbedtls submodule (or wherever the caller sent it with
# -DBLUFI_MBEDTLS_DIR), and the files compile exactly as upstream ships them.
#
# They live in a library of their own for one reason: the application already
# links TF-PSA-Crypto, which publishes an mbedtls/platform_util.h with the same
# name but different contents from 2.28's. Target-private include directories
# sort ahead of the interface ones, so these translation units see Mbed TLS' own
# headers first, and nothing else in the build gains a 2.28 include path.
#
# SPDX-License-Identifier: MIT

zephyr_library_named(blufi_mbedtls)

zephyr_library_sources(
  ${BLUFI_MBEDTLS_DIR}/library/dhm.c
  ${BLUFI_MBEDTLS_DIR}/library/bignum.c
  ${BLUFI_MBEDTLS_DIR}/library/constant_time.c
  ${BLUFI_MBEDTLS_DIR}/library/aes.c
  ${BLUFI_MBEDTLS_DIR}/library/md5.c)

target_include_directories(blufi_mbedtls PRIVATE
  ${BLUFI_MBEDTLS_DIR}/include
  ${BLUFI_MBEDTLS_DIR}/library
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

# blufi_dhm_compat.h redirects MBEDTLS_CONFIG_FILE to our minimal private config;
# blufi_mbedtls_rename.h renames only the AES symbols (TF-PSA-Crypto provides an
# AES already). Upstream predates this project's warning set, hence -w.
#
# COMPILE_FLAGS as one string, not COMPILE_OPTIONS as a list: the list form makes
# CMake hand GCC "-include" and the path as unrelated arguments, and the path then
# looks like a second input file.
set_target_properties(blufi_mbedtls PROPERTIES COMPILE_FLAGS
    "-w -include ${CMAKE_CURRENT_SOURCE_DIR}/../blufi_dhm_compat.h -include ${CMAKE_CURRENT_SOURCE_DIR}/../blufi_mbedtls_rename.h")
