/*
 * Copyright (c) 2026 the ota_client authors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief HTTP(S) image source for ota_client.
 *
 * Two plain HTTP GETs: a text file holding the offered version, and the image
 * itself. No TLS and no cloud protocol - authenticity and integrity are the
 * bootloader's job (the MCUboot sink validates the image signature), and the
 * transport is deliberately a deployment decision.
 *
 * @ingroup ota_client
 */

#ifndef OTA_CLIENT_SOURCE_HTTP_H_
#define OTA_CLIENT_SOURCE_HTTP_H_

#include <ota_client/ota_client.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP source instance.
 *
 * Allocate it statically or on a caller-owned struct; @a source must stay the
 * first member so &ctx->source is a valid ota_client_source pointer.
 */
struct ota_client_http {
	/** Hand this to ota_client_config::source. */
	struct ota_client_source source;
	struct ota_client_url version_url;
	struct ota_client_url image_url;
	/* Transfer state. The core serialises checks and updates, so only one
	 * request is ever in flight and no locking is needed here. */
	struct ota_client *client;
	struct ota_client_sink *sink;
	char version[CONFIG_OTA_CLIENT_VERSION_MAX_LEN];
	size_t version_len;
	uint32_t written;
	int64_t expected;
	int error;
	/*
	 * Zephyr's http_client_req() timeout is a budget for the whole exchange, not
	 * for one read, so the two requests need different values: a stalled version
	 * probe means the server is not there and should fail fast, while a megabyte
	 * of firmware over a busy 2.4 GHz link legitimately takes tens of seconds.
	 * ota_client_http_init() seeds them from Kconfig; override after init to tune
	 * per call site.
	 */
	int version_timeout_ms;
	int image_timeout_ms;
};

/**
 * @brief Prepare an HTTP source.
 *
 * @param ctx Instance to initialise (owned by the caller).
 * @param version_url e.g. "http://192.0.2.5:9998/version.txt".
 * @param image_url   e.g. "http://192.0.2.5:9998/firmware.bin".
 *
 * @retval 0 Both URLs parsed.
 * @retval -EINVAL NULL argument or a URL this parser rejects.
 */
int ota_client_http_init(struct ota_client_http *ctx, const char *version_url,
			 const char *image_url);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CLIENT_SOURCE_HTTP_H_ */
