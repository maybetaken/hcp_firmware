/*
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief HTTP image source implementation.
 */

#include <ota_client/ota_source_http.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(ota_client, CONFIG_OTA_CLIENT_LOG_LEVEL);

#ifndef CONFIG_OTA_CLIENT_HTTP_RECV_BUF_SIZE
#define CONFIG_OTA_CLIENT_HTTP_RECV_BUF_SIZE 2048
#endif

#ifndef CONFIG_OTA_CLIENT_HTTP_TIMEOUT_MS
#define CONFIG_OTA_CLIENT_HTTP_TIMEOUT_MS 10000
#endif

#ifndef CONFIG_OTA_CLIENT_HTTP_IMAGE_TIMEOUT_MS
#define CONFIG_OTA_CLIENT_HTTP_IMAGE_TIMEOUT_MS 300000
#endif

/* The core runs one transfer at a time, so a single receive buffer is enough
 * and no per-instance scratch has to be carried by the caller. */
static uint8_t http_recv_buf[CONFIG_OTA_CLIENT_HTTP_RECV_BUF_SIZE];

static bool status_ok(const struct http_response *rsp)
{
	return rsp->http_status_code == 0 ||
	       IN_RANGE(rsp->http_status_code, 200, 299);
}

static int on_version(struct http_response *rsp, enum http_final_call final_data,
		      void *user_data)
{
	struct ota_client_http *ctx = user_data;
	size_t room;

	ARG_UNUSED(final_data);

	if (!status_ok(rsp)) {
		LOG_ERR("version request answered %u", rsp->http_status_code);
		ctx->error = -EIO;
		return -EIO;
	}

	room = sizeof(ctx->version) - ctx->version_len - 1;
	if (rsp->body_frag_len > 0 && room > 0) {
		size_t n = MIN(rsp->body_frag_len, room);

		memcpy(ctx->version + ctx->version_len, rsp->body_frag_start, n);
		ctx->version_len += n;
		ctx->version[ctx->version_len] = '\0';
	}

	return 0;
}

static int on_image(struct http_response *rsp, enum http_final_call final_data,
		    void *user_data)
{
	struct ota_client_http *ctx = user_data;
	int ret;

	ARG_UNUSED(final_data);

	if (!status_ok(rsp)) {
		LOG_ERR("image request answered %u", rsp->http_status_code);
		ctx->error = -EIO;
		return -EIO;
	}

	if (rsp->cl_present && rsp->content_length > 0) {
		ctx->expected = (int64_t)rsp->content_length;
		ota_client_set_total(ctx->client, ctx->expected);
	}

	if (rsp->body_frag_len == 0) {
		return 0;
	}

	if (ota_client_aborted(ctx->client)) {
		/* Propagating an error makes http_client_req() drop the rest of
		 * the response instead of downloading an image nobody wants. */
		ctx->error = -ECANCELED;
		return -ECANCELED;
	}

	ret = ctx->sink->api->write(ctx->sink, rsp->body_frag_start,
				    rsp->body_frag_len);
	if (ret < 0) {
		ctx->error = ret;
		return ret;
	}

	ctx->written += rsp->body_frag_len;
	return 0;
}

static int http_get(struct ota_client_http *ctx, const struct ota_client_url *url,
		    http_response_cb_t cb, int timeout_ms)
{
	struct sockaddr_in addr;
	struct http_request req = {0};
	struct zsock_addrinfo hints = {0};
	struct zsock_addrinfo *res = NULL;
	int sock;
	int ret;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	ret = zsock_getaddrinfo(url->host, url->port, &hints, &res);
	if (ret != 0 || res == NULL) {
		const int err = (ret != 0) ? ret : -ENOENT;

		LOG_ERR("resolve %s:%s failed (%d)", url->host, url->port, err);
		if (res != NULL) {
			zsock_freeaddrinfo(res);
		}
		return err;
	}

	memset(&addr, 0, sizeof(addr));
	memcpy(&addr, res->ai_addr, MIN(sizeof(addr), (size_t)res->ai_addrlen));
	zsock_freeaddrinfo(res);

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		const int err = -errno;

		LOG_ERR("socket() failed (%d)", err);
		return err;
	}

	if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		const int err = -errno;

		LOG_ERR("connect %s:%s failed (%d)", url->host, url->port, err);
		zsock_close(sock);
		return err;
	}

	req.method = HTTP_GET;
	req.url = url->path;
	req.protocol = "HTTP/1.1";
	req.host = url->host;
	req.port = url->port;
	req.recv_buf = http_recv_buf;
	req.recv_buf_len = sizeof(http_recv_buf);
	req.response = cb;

	/* Milliseconds, not k_timeout_t: http_client_req() owns the socket poll
	 * budget and must outlast a full image transfer. */
	ret = http_client_req(sock, &req, timeout_ms, ctx);
	zsock_close(sock);
	if (ret < 0) {
		LOG_ERR("GET %s failed (%d)", url->path, ret);
	}

	return ret;
}

static void trim_version(char *s, size_t *len)
{
	size_t n = *len;

	while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
			 s[n - 1] == ' ' || s[n - 1] == '\t')) {
		n--;
	}
	s[n] = '\0';
	*len = n;
}

static int source_get_version(struct ota_client_source *src, char *version,
			      size_t size)
{
	struct ota_client_http *ctx = CONTAINER_OF(src, struct ota_client_http,
						   source);
	int ret;

	ctx->version_len = 0;
	ctx->version[0] = '\0';
	ctx->error = 0;

	ret = http_get(ctx, &ctx->version_url, on_version, ctx->version_timeout_ms);
	if (ctx->error != 0) {
		return ctx->error;
	}
	if (ret < 0) {
		LOG_ERR("version request failed (%d)", ret);
		return ret;
	}

	trim_version(ctx->version, &ctx->version_len);
	if (ctx->version_len == 0) {
		LOG_ERR("version file was empty");
		return -EBADMSG;
	}
	if (ctx->version_len + 1 > size) {
		return -EMSGSIZE;
	}

	memcpy(version, ctx->version, ctx->version_len + 1);
	return 0;
}

static int source_fetch(struct ota_client_source *src, struct ota_client *client,
			struct ota_client_sink *sink)
{
	struct ota_client_http *ctx = CONTAINER_OF(src, struct ota_client_http,
						   source);
	int ret;

	ctx->client = client;
	ctx->sink = sink;
	ctx->written = 0;
	ctx->expected = -1;
	ctx->error = 0;

	ret = http_get(ctx, &ctx->image_url, on_image, ctx->image_timeout_ms);
	if (ctx->error != 0) {
		return ctx->error;
	}
	if (ret < 0) {
		LOG_ERR("image request failed (%d)", ret);
		return ret;
	}

	if (ctx->written == 0) {
		LOG_ERR("server sent an empty image");
		return -EBADMSG;
	}

	if (ctx->expected > 0 && (int64_t)ctx->written != ctx->expected) {
		/* A short body means the connection died quietly; staging it
		 * would only waste the slot. */
		LOG_ERR("image truncated: %u of %lld bytes", ctx->written,
			(long long)ctx->expected);
		return -EBADMSG;
	}

	return 0;
}

static const struct ota_client_source_api ota_client_http_api = {
	.get_version = source_get_version,
	.get_size = NULL, /* known only once the response starts */
	.fetch = source_fetch,
};

int ota_client_http_init(struct ota_client_http *ctx, const char *version_url,
			 const char *image_url)
{
	int ret;

	if (ctx == NULL || version_url == NULL || image_url == NULL) {
		return -EINVAL;
	}

	memset(ctx, 0, sizeof(*ctx));

	ret = ota_client_url_parse(version_url, &ctx->version_url);
	if (ret != 0) {
		return ret;
	}

	ret = ota_client_url_parse(image_url, &ctx->image_url);
	if (ret != 0) {
		return ret;
	}

	if (ctx->version_url.secure || ctx->image_url.secure) {
		/* No TLS here: pretending to support it would mean certificates, a
		 * much bigger image and a false sense of security. Authenticity is the
		 * bootloader's job (it validates the image signature), so an https://
		 * URL is refused rather than silently downgraded. */
		return -ENOTSUP;
	}

	ctx->version_timeout_ms = CONFIG_OTA_CLIENT_HTTP_TIMEOUT_MS;
	ctx->image_timeout_ms = CONFIG_OTA_CLIENT_HTTP_IMAGE_TIMEOUT_MS;

	ctx->source.api = &ota_client_http_api;
	ctx->source.user_data = ctx;
	ctx->expected = -1;

	return 0;
}
