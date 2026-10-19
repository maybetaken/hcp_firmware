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

void blufi_reasm_reset(blufi_reasm *r)
{
	r->total = 0;
	r->off = 0;
	r->type = 0;
}

int blufi_reasm_feed(blufi_reasm *r, uint8_t type_subtype, uint8_t fc,
		     const uint8_t *data, uint16_t data_len, uint8_t *out_type,
		     uint8_t **out_data, uint16_t *out_len)
{
	if (fc & BLUFI_FC_FRAG) {
		if (r->off == 0) {
			if (data_len < 2) {
				return -EINVAL;
			}
			r->total = (uint16_t)(data[0] | (data[1] << 8));
			r->type = type_subtype;
			if (r->total > sizeof(r->buf)) {
				return -EMSGSIZE;
			}
		}
		if (data_len < 2 ||
		    r->off + (uint16_t)(data_len - 2) > r->total) {
			r->off = 0;
			return -EINVAL;
		}
		memcpy(r->buf + r->off, data + 2, data_len - 2);
		r->off += data_len - 2;
		return 0; /* more fragments expected */
	}

	if (r->off > 0) {
		/* Final (unflagged) chunk of a fragmented message. */
		if (r->off + data_len > r->total) {
			r->off = 0;
			return -EINVAL;
		}
		memcpy(r->buf + r->off, data, data_len);
		r->off += data_len;
		if (out_type) {
			*out_type = r->type;
		}
		if (out_data) {
			*out_data = r->buf;
		}
		if (out_len) {
			*out_len = r->total;
		}
		r->off = 0;
		return 1; /* assembled */
	}

	/* Single unfragmented frame. */
	if (out_type) {
		*out_type = type_subtype;
	}
	if (out_data) {
		*out_data = (uint8_t *)data;
	}
	if (out_len) {
		*out_len = data_len;
	}
	return 1;
}

int blufi_next_fragment(const uint8_t *in, uint16_t in_len, uint16_t *off,
			uint16_t frag_cap, uint8_t *out, uint16_t *out_len,
			bool *is_frag)
{
	uint16_t remain = (uint16_t)(in_len - *off);

	if (frag_cap < 3 || frag_cap > BLUFI_MAX_DATA_LEN) {
		return -EINVAL;
	}
	if (in_len <= BLUFI_MAX_DATA_LEN) {
		/* Whole payload in one frame. */
		memcpy(out, in + *off, remain);
		*out_len = remain;
		*is_frag = false;
		*off = in_len;
		return 1;
	}
	if (remain > (uint16_t)(frag_cap - 2)) {
		uint16_t chunk = (uint16_t)(frag_cap - 2);
		out[0] = (uint8_t)(remain & 0xff);
		out[1] = (uint8_t)(remain >> 8);
		memcpy(out + 2, in + *off, chunk);
		*out_len = (uint16_t)(chunk + 2);
		*is_frag = true;
		*off = (uint16_t)(*off + chunk);
		return 0; /* more */
	}
	memcpy(out, in + *off, remain);
	*out_len = remain;
	*is_frag = false;
	*off = in_len;
	return 1;
}
