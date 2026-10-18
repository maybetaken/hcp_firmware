/*
 * BluFi frame codec — faithful to ESP-IDF esp_blufi (blufi_int.h):
 * a 4-byte header {type, fc, seq, data_len} + payload (no header CRC).
 * type byte = (subtype<<2)|type. Fragmentation (>255) handled by the caller
 * via a fragment header (adds u16 total_len). Pure C, host-testable.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BLUFI_FRAME_H_
#define BLUFI_FRAME_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLUFI_HDR_LEN        4
#define BLUFI_MAX_DATA_LEN   255

#define BLUFI_TYPE_MASK      0x03
#define BLUFI_SUBTYPE_SHIFT  2
#define BLUFI_BUILD_TYPE(t, sub) \
	((((t) & BLUFI_TYPE_MASK)) | (((sub) << BLUFI_SUBTYPE_SHIFT)))
#define BLUFI_GET_TYPE(ts)       ((ts) & BLUFI_TYPE_MASK)
#define BLUFI_GET_SUBTYPE(ts)    (((ts) >> BLUFI_SUBTYPE_SHIFT) & 0x3F)

enum blufi_frame_type {
	BLUFI_TYPE_CTRL = 0x0,
	BLUFI_TYPE_DATA = 0x1,
};

/* Control subtypes */
enum blufi_ctrl_subtype {
	BLUFI_CTRL_ACK              = 0x00,
	BLUFI_CTRL_SET_SEC_MODE     = 0x01,
	BLUFI_CTRL_SET_WIFI_OPMODE  = 0x02,
	BLUFI_CTRL_CONN_TO_AP       = 0x03,
	BLUFI_CTRL_DISCONN_FROM_AP  = 0x04,
	BLUFI_CTRL_GET_WIFI_STATUS  = 0x05,
	BLUFI_CTRL_DEAUTHENTICATE   = 0x06,
	BLUFI_CTRL_GET_VERSION      = 0x07,
	BLUFI_CTRL_DISCONNECT_BLE   = 0x08,
	BLUFI_CTRL_GET_WIFI_LIST    = 0x09,
};

/* Data subtypes */
enum blufi_data_subtype {
	BLUFI_DATA_NEG            = 0x00,
	BLUFI_DATA_STA_BSSID      = 0x01,
	BLUFI_DATA_STA_SSID       = 0x02,
	BLUFI_DATA_STA_PASSWD     = 0x03,
	BLUFI_DATA_WIFI_REP       = 0x0f,
	BLUFI_DATA_REPLY_VERSION  = 0x10,
	BLUFI_DATA_WIFI_LIST      = 0x11,
	BLUFI_DATA_ERROR_INFO     = 0x12,
	BLUFI_DATA_CUSTOM_DATA    = 0x13,
};

/* A parsed frame (data points into the caller's buffer). */
struct blufi_frame {
	uint8_t type_subtype;
	uint8_t fc;
	uint8_t seq;
	uint8_t len;
	const uint8_t *data;
};

/*
 * Build a frame into buf; returns encoded length, or negative errno.
 * Fails with -EMSGSIZE if len > BLUFI_MAX_DATA_LEN (caller must fragment).
 */
int blufi_frame_build(uint8_t type, uint8_t subtype, uint8_t seq,
	const uint8_t *data, uint16_t len, uint8_t *buf, size_t cap);

/* Parse buf into out; returns 0 on success, negative errno on malformed input. */
int blufi_frame_parse(const uint8_t *buf, size_t buf_len, struct blufi_frame *out);

#ifdef __cplusplus
}
#endif

#endif /* BLUFI_FRAME_H_ */
