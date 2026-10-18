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
