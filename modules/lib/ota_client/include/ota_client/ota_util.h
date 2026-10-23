/*
 * Copyright (c) 2026 the ota_client authors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief Pure helpers shared by ota_client and its source/sink backends.
 *
 * No kernel, network or flash dependency: these are safe to call from any
 * context and to unit test on a host.
 *
 * @ingroup ota_client
 */

#ifndef OTA_CLIENT_UTIL_H_
#define OTA_CLIENT_UTIL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest version string ("1.2.3") the library handles. */
#ifndef CONFIG_OTA_CLIENT_VERSION_MAX_LEN
#define CONFIG_OTA_CLIENT_VERSION_MAX_LEN 32
#endif

/** Longest URL the library handles. */
#ifndef CONFIG_OTA_CLIENT_URL_MAX_LEN
#define CONFIG_OTA_CLIENT_URL_MAX_LEN 128
#endif

/**
 * @brief Compare dotted-numeric versions, ignoring a leading @c 'v' or @c 'V'.
 *
 * Missing trailing fields count as zero, so "1.2" equals "1.2.0". Up to four
 * fields are compared; anything else (suffixes, spaces, overflow, more than
 * four fields) is rejected rather than guessed at, because a firmware policy
 * that silently mis-parses a version can brick a fleet.
 *
 * @retval -1 left is older.
 * @retval 0 equal.
 * @retval 1 left is newer.
 * @retval -EINVAL either side is empty or not a plain dotted version.
 */
int ota_client_version_cmp(const char *left, const char *right);

/** A split "http://" / "https://" URL. */
struct ota_client_url {
	/** Host or dotted-IPv4 literal, NUL-terminated. */
	char host[CONFIG_OTA_CLIENT_URL_MAX_LEN / 2];
	/** Decimal port; "80" for http and "443" for https when the URL did not
	 *  carry one. */
	char port[6];
	/** Absolute path, "/" when the URL did not carry one. */
	char path[CONFIG_OTA_CLIENT_URL_MAX_LEN];
	/** True for "https://". */
	bool secure;
};

/**
 * @brief Split an HTTP(S) URL into host, port and path.
 *
 * @param url NUL-terminated input.
 * @param out Parsed result, cleared before use.
 *
 * @retval 0 Parsed; out is filled.
 * @retval -EINVAL Missing scheme, empty or bracketed host, host/path too long,
 *         or a non-numeric or out-of-range port.
 */
int ota_client_url_parse(const char *url, struct ota_client_url *out);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CLIENT_UTIL_H_ */
