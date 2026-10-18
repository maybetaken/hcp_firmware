/*
 * BluFi frame codec implementation.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "blufi/frame.h"

#include <errno.h>
#include <string.h>

int blufi_frame_build(uint8_t type, uint8_t subtype, uint8_t seq,
	const uint8_t *data, uint16_t len, uint8_t *buf, size_t cap)
{
	if (buf == NULL) {
		return -EINVAL;
	}
	if (len > BLUFI_MAX_DATA_LEN) {
		return -EMSGSIZE;
	}
	if (cap < (size_t)BLUFI_HDR_LEN + len) {
		return -ENOSPC;
	}

	buf[0] = BLUFI_BUILD_TYPE(type, subtype);
	buf[1] = 0; /* fc: unsegmented, no ack */
	buf[2] = seq;
	buf[3] = (uint8_t)len;
	if (len > 0 && data != NULL) {
		memcpy(buf + BLUFI_HDR_LEN, data, len);
	}
	return BLUFI_HDR_LEN + len;
}

int blufi_frame_parse(const uint8_t *buf, size_t buf_len, struct blufi_frame *out)
{
	if (buf == NULL || out == NULL || buf_len < BLUFI_HDR_LEN) {
		return -EINVAL;
	}

	out->type_subtype = buf[0];
	out->fc = buf[1];
	out->seq = buf[2];
	out->len = buf[3];

	if (buf_len < (size_t)BLUFI_HDR_LEN + out->len) {
		return -EINVAL; /* truncated */
	}
	out->data = buf + BLUFI_HDR_LEN;
	return 0;
}

uint16_t blufi_crc16_be(uint16_t crc, const uint8_t *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		crc ^= (uint16_t)buf[i] << 8;
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x8000) {
				crc = (uint16_t)((crc << 1) ^ 0x1021);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}
	return crc;
}

int blufi_frame_append_checksum(uint8_t *buf, uint16_t len, size_t cap)
{
	if (buf == NULL || len < BLUFI_HDR_LEN) {
		return -EINVAL;
	}
	uint16_t data_len = buf[3];
	if ((size_t)len != (size_t)BLUFI_HDR_LEN + data_len) {
		return -EINVAL; /* len must be the exact encoded frame length */
	}
	if (cap < (size_t)len + 2) {
		return -ENOSPC;
	}
	/* checksum covers [seq, data_len, payload] = buf[2 .. 4+data_len) */
	uint16_t crc = blufi_crc16_be(0, &buf[2], data_len + 2);
	buf[len] = (uint8_t)(crc & 0xff);
	buf[len + 1] = (uint8_t)(crc >> 8);
	buf[1] |= BLUFI_FC_CHECK;
	return len + 2;
}

int blufi_frame_verify_checksum(const uint8_t *buf, uint16_t buf_len)
{
	if (buf == NULL || buf_len < BLUFI_HDR_LEN + 2) {
		return -EINVAL;
	}
	if (!(buf[1] & BLUFI_FC_CHECK)) {
		return -EINVAL; /* frame does not carry a checksum */
	}
	uint16_t data_len = buf[3];
	/* total must be 4 + data_len + 2 */
	if (buf_len != (uint16_t)(BLUFI_HDR_LEN + data_len + 2)) {
		return -EINVAL;
	}
	uint16_t crc = blufi_crc16_be(0, &buf[2], data_len + 2);
	uint16_t got = (uint16_t)buf[BLUFI_HDR_LEN + data_len] |
			   ((uint16_t)buf[BLUFI_HDR_LEN + data_len + 1] << 8);
	return crc == got ? 0 : -EBADMSG;
}
