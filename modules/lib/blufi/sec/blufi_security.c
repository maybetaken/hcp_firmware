/*
 * BluFi security handshake (ESP-compatible). Phase 1: bring up the vendored
 * mbedtls-2.4 DHM/bignum and self-contained MD5 + AES-CFB128, proving the
 * isolated crypto build. Negotiation/encrypt/decrypt filled in incrementally.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "blufi/blufi_security.h"

#if defined(CONFIG_BLUFI_SECURITY)

#include <string.h>
#include <stdlib.h>

#include <zephyr/sys/util.h>

#include "mbedtls/dhm.h"
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(blufi, CONFIG_LOG_DEFAULT_LEVEL);

struct blufi_security {
	uint8_t self_public_key[128];
	uint8_t share_key[128];
	size_t share_len;
	uint8_t psk[16];
	uint8_t dh_param[1024];
	int dh_param_len;
	uint8_t iv[16];
	mbedtls_dhm_context dhm;
};
static struct blufi_security *sec;

int blufi_security_init(void)
{
	if (sec) {
		return 0;
	}
	sec = calloc(1, sizeof(*sec));
	if (!sec) {
		return -1;
	}
	mbedtls_dhm_init(&sec->dhm);
	LOG_INF("blufi sec: dhm init ok");
	return 0;
}

void blufi_security_deinit(void)
{
	if (!sec) {
		return;
	}
	mbedtls_dhm_free(&sec->dhm);
	free(sec);
	sec = NULL;
}

int blufi_negotiate(const uint8_t *data, uint16_t len, const uint8_t **out,
		    uint16_t *out_len)
{
	ARG_UNUSED(data); ARG_UNUSED(len);
	if (out) *out = NULL;
	if (out_len) *out_len = 0;
	return 0; /* TODO: DH param/data handling next */
}

int blufi_aes_encrypt(uint8_t iv8, uint8_t *data, int len)
{
	ARG_UNUSED(iv8); ARG_UNUSED(data);
	return len; /* TODO: AES-CFB128 next */
}

int blufi_aes_decrypt(uint8_t iv8, uint8_t *data, int len)
{
	ARG_UNUSED(iv8); ARG_UNUSED(data);
	return len; /* TODO: AES-CFB128 next */
}

#endif /* CONFIG_BLUFI_SECURITY */
