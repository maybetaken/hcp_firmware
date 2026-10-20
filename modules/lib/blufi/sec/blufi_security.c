/*
 * BluFi security handshake (ESP-compatible): classic finite-field DH via the
 * vendored mbedtls-2.4 DHM -> MD5(shared) = 16-byte PSK -> AES-128-CFB128.
 * Mirrors esp-idf examples/bluetooth/blufi/main/blufi_security.c. MD5/AES use
 * the renamed vendored primitives (blufi_md5 / blufi_aes_*) to avoid clashing
 * with the app-wide TF-PSA-Crypto 1.0 (which removed those legacy APIs).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "blufi/blufi_security.h"

#if defined(CONFIG_BLUFI_SECURITY)

#include <stdlib.h>
#include <string.h>

#ifdef __ZEPHYR__
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#else
/* Host (unit-test) build: stub the few Zephyr facilities used here. */
#include <stdio.h>
#ifndef ARG_UNUSED
#define ARG_UNUSED(x) (void)(x)
#endif
#define LOG_ERR(fmt, ...) printf("E: " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) printf("I: " fmt "\n", ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) do { } while (0)
static int sys_csrand_get(void *dst, size_t len)
{
	unsigned char *d = (unsigned char *)dst;
	for (size_t i = 0; i < len; i++) {
		d[i] = (unsigned char)(rand() & 0xff);
	}
	return 0;
}
#endif

#include "mbedtls/dhm.h"
#include "mbedtls/bignum.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"

#ifdef __ZEPHYR__
LOG_MODULE_DECLARE(blufi, CONFIG_LOG_DEFAULT_LEVEL);
#endif

#define DH_SELF_PUB_KEY_LEN 256
#define SHARE_KEY_LEN       256
#define DH_PARAM_MAX        1024
#define PSK_LEN             16

/* mbedtls AES direction constants (values fixed by the vendored aes.h). */
#define BLUFI_AES_ENCRYPT   1
#define BLUFI_AES_DECRYPT   0

struct blufi_security {
	uint8_t self_public_key[DH_SELF_PUB_KEY_LEN];
	uint8_t share_key[SHARE_KEY_LEN];
	size_t share_len;
	uint8_t psk[PSK_LEN];
	uint8_t dh_param[DH_PARAM_MAX];
	int dh_param_len;
	uint8_t iv[16];
	mbedtls_dhm_context dhm;
	blufimbed_aes_context aes;
};

static struct blufi_security *sec;

static int myrand(void *rng_state, unsigned char *output, size_t len)
{
	int rc;

	ARG_UNUSED(rng_state);
	rc = sys_csrand_get(output, len);
	LOG_DBG("blufi sec: rng rc=%d len=%u", rc, (unsigned int)len);
	return rc == 0 ? 0 : -1;
}

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
	blufimbed_aes_init(&sec->aes);
	memset(sec->iv, 0x0, sizeof(sec->iv));
	LOG_INF("blufi sec: dhm/aes init ok");
	return 0;
}

void blufi_security_deinit(void)
{
	if (!sec) {
		return;
	}
	mbedtls_dhm_free(&sec->dhm);
	blufimbed_aes_free(&sec->aes);
	free(sec);
	sec = NULL;
}

int blufi_negotiate(const uint8_t *data, uint16_t len, const uint8_t **out,
		    uint16_t *out_len)
{
	int ret;
	uint8_t type;

	if (data == NULL || len < 3 || !sec) {
		LOG_ERR("blufi sec: invalid negotiate data");
		return -1;
	}
	type = data[0];

	switch (type) {
	case SEC_TYPE_DH_PARAM_LEN:
		sec->dh_param_len = ((data[1] << 8) | data[2]);
		if (sec->dh_param_len < 0 || sec->dh_param_len > DH_PARAM_MAX) {
			LOG_ERR("blufi sec: bad dh param len %d", sec->dh_param_len);
			sec->dh_param_len = 0;
			return -1;
		}
		return 0;

	case SEC_TYPE_DH_PARAM_DATA: {
		uint8_t *param = sec->dh_param;
		int dhm_len;

		if (len < sec->dh_param_len + 1) {
			LOG_ERR("blufi sec: short dh data");
			return -1;
		}
		memcpy(sec->dh_param, &data[1], sec->dh_param_len);

		ret = mbedtls_dhm_read_params(&sec->dhm, &param,
					      &param[sec->dh_param_len]);
		if (ret) {
			LOG_ERR("blufi sec: read_params failed %d", ret);
			return -1;
		}

		dhm_len = (mbedtls_mpi_bitlen(&sec->dhm.P) + 7) / 8;
		if (dhm_len > DH_SELF_PUB_KEY_LEN) {
			LOG_ERR("blufi sec: dhm len %d too big", dhm_len);
			return -1;
		}

		/* make_public requires 1 <= olen <= ctx->len (group size). Pass dhm_len
		 * (not the max buffer size) so we don't trip BAD_INPUT_DATA for
		 * groups smaller than DH_SELF_PUB_KEY_LEN (e.g. 1024-bit => len 128). */
		ret = mbedtls_dhm_make_public(&sec->dhm, dhm_len,
					      sec->self_public_key,
					      (size_t)dhm_len, myrand, NULL);
		if (ret) {
			LOG_ERR("blufi sec: make_public failed %d", ret);
			return -1;
		}
		/* The vendored mbedtls 2.4 DHM ignores a failing RNG callback and
		 * can leave GX == 1. Never hand a degenerate key to the phone. */
		if (mbedtls_mpi_cmp_int(&sec->dhm.GX, 2) < 0) {
			LOG_ERR("blufi sec: degenerate DH public key");
			return -1;
		}
		ret = mbedtls_dhm_calc_secret(&sec->dhm, sec->share_key,
					      SHARE_KEY_LEN, &sec->share_len,
					      myrand, NULL);
		if (ret) {
			LOG_ERR("blufi sec: calc_secret failed %d", ret);
			return -1;
		}
		blufimbed_md5(sec->share_key, sec->share_len, sec->psk);
		blufimbed_aes_setkey_enc(&sec->aes, sec->psk, PSK_LEN * 8);

		*out = sec->self_public_key;
		*out_len = (uint16_t)dhm_len;
		LOG_DBG("blufi sec: DH done, pub key %d bytes", dhm_len);
		return 0;
	}

	default:
		return 0;
	}
}

int blufi_aes_encrypt(uint8_t iv8, uint8_t *data, int len)
{
	uint8_t iv0[16];
	size_t iv_offset = 0;

	if (!sec) {
		return -1;
	}
	memcpy(iv0, sec->iv, sizeof(sec->iv));
	iv0[0] = iv8;
	if (blufimbed_aes_crypt_cfb128(&sec->aes, BLUFI_AES_ENCRYPT, len, &iv_offset,
					iv0, data, data) != 0) {
		return -1;
	}
	return len;
}

int blufi_aes_decrypt(uint8_t iv8, uint8_t *data, int len)
{
	uint8_t iv0[16];
	size_t iv_offset = 0;

	if (!sec) {
		return -1;
	}
	memcpy(iv0, sec->iv, sizeof(sec->iv));
	iv0[0] = iv8;
	if (blufimbed_aes_crypt_cfb128(&sec->aes, BLUFI_AES_DECRYPT, len, &iv_offset,
					iv0, data, data) != 0) {
		return -1;
	}
	return len;
}

#endif /* CONFIG_BLUFI_SECURITY */
