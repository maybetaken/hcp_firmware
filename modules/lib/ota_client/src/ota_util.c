/*
 * Copyright (c) 2026 the ota_client authors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief Pure helpers shared by ota_client users: version ordering and URL
 * splitting.
 *
 * Deliberately free of kernel, network and flash dependencies so they can be
 * exercised on the host and reused by any source/sink implementation.
 */

#include <ota_client/ota_util.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <strings.h>

#define VERSION_FIELDS 4
#define HTTP_SCHEME	 "http://"
#define HTTPS_SCHEME	 "https://"
#define HTTP_PORT	 "80"
#define HTTPS_PORT	 "443"

/* Read up to VERSION_FIELDS dotted numbers. Returns the number of fields
 * parsed, or a negative errno for anything that is not a plain version. */
static int parse_version(const char *version, uint32_t *fields)
{
	const char *p = version;
	int count = 0;

	if (version == NULL) {
		return -EINVAL;
	}

	/* Both the SemVer and the ESP-IDF style spellings are common in the
	 * wild ("v1.2.3" and "1.2.3"); they must compare equal. */
	if (*p == 'v' || *p == 'V') {
		p++;
	}

	if (!isdigit((unsigned char)*p)) {
		return -EINVAL;
	}

	while (true) {
		uint32_t value = 0;
		int digits = 0;

		while (isdigit((unsigned char)*p)) {
			const uint32_t digit = (uint32_t)(*p - '0');

			/* Reject rather than wrap: a version that overflows a
			 * 32-bit field is not comparable in any useful way. */
			if (value > (UINT32_MAX - digit) / 10U) {
				return -EINVAL;
			}
			value = value * 10U + digit;
			p++;
			digits++;
		}

		if (digits == 0) {
			return -EINVAL; /* "1..2", "1.", "" */
		}

		if (count == VERSION_FIELDS) {
			return -EINVAL; /* more fields than we can compare */
		}
		fields[count++] = value;

		if (*p == '\0') {
			break;
		}
		if (*p != '.') {
			return -EINVAL; /* spaces, suffixes, ... */
		}
		p++;
	}

	return count;
}

int ota_client_version_cmp(const char *left, const char *right)
{
	uint32_t l[VERSION_FIELDS] = {0};
	uint32_t r[VERSION_FIELDS] = {0};
	int lcount;
	int rcount;

	lcount = parse_version(left, l);
	if (lcount < 0) {
		return lcount;
	}

	rcount = parse_version(right, r);
	if (rcount < 0) {
		return rcount;
	}

	/* Absent trailing fields were zero-filled above, so "1.2" < "1.2.1". */
	for (int i = 0; i < VERSION_FIELDS; i++) {
		if (l[i] != r[i]) {
			return (l[i] < r[i]) ? -1 : 1;
		}
	}

	return 0;
}

static bool copy_field(const char *src, size_t len, char *dst, size_t dst_size)
{
	if (len == 0 || len >= dst_size) {
		return false;
	}

	memcpy(dst, src, len);
	dst[len] = '\0';
	return true;
}

static bool valid_port(const char *port)
{
	long value;

	for (const char *p = port; *p != '\0'; p++) {
		if (!isdigit((unsigned char)*p)) {
			return false;
		}
	}

	value = strtol(port, NULL, 10);
	return value > 0 && value <= 65535;
}

int ota_client_url_parse(const char *url, struct ota_client_url *out)
{
	const char *authority;
	const char *colon;
	const char *slash;
	size_t authority_len;
	size_t host_len;

	if (url == NULL || out == NULL) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));

	if (strncasecmp(url, HTTPS_SCHEME, strlen(HTTPS_SCHEME)) == 0) {
		out->secure = true;
		authority = url + strlen(HTTPS_SCHEME);
	} else if (strncasecmp(url, HTTP_SCHEME, strlen(HTTP_SCHEME)) == 0) {
		out->secure = false;
		authority = url + strlen(HTTP_SCHEME);
	} else {
		return -EINVAL;
	}

	slash = strchr(authority, '/');
	authority_len = slash != NULL ? (size_t)(slash - authority)
				      : strlen(authority);

	/* Bracketed IPv6 literals are not supported: they need a port
	 * delimiter of their own and no HTTP server here uses them. */
	if (authority_len == 0 || authority[0] == '[') {
		return -EINVAL;
	}

	colon = memchr(authority, ':', authority_len);
	host_len = colon != NULL ? (size_t)(colon - authority) : authority_len;

	if (!copy_field(authority, host_len, out->host, sizeof(out->host))) {
		return -EINVAL;
	}

	if (colon != NULL) {
		size_t port_len = authority_len - host_len - 1;

		if (port_len >= sizeof(out->port) ||
		    !copy_field(colon + 1, port_len, out->port,
				sizeof(out->port))) {
			return -EINVAL;
		}
		if (!valid_port(out->port)) {
			return -EINVAL;
		}
	} else {
		strcpy(out->port, out->secure ? HTTPS_PORT : HTTP_PORT);
	}

	if (slash == NULL) {
		strcpy(out->path, "/");
	} else if (!copy_field(slash, strlen(slash), out->path,
			       sizeof(out->path))) {
		return -EINVAL;
	}

	return 0;
}
