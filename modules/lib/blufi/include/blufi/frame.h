/*
 * BluFi frame codec — faithful to ESP-IDF esp_blufi (blufi_int.h):
 * a 4-byte header {type, fc, seq, data_len} + payload (no header CRC).
 * type byte = (subtype<<2)|type. Fragmentation (>255) handled by the caller
 * via a fragment header (adds u16 total_len). Pure C, host-testable.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BLUFI_FRAME_H_
#define BLUFI_FRAME_H_

#include <stdbool.h>
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

/* Frame-control (fc) bit flags — faithful to esp blufi_int.h. */
#define BLUFI_FC_ENC      0x01 /* payload encrypted            */
#define BLUFI_FC_CHECK    0x02 /* crc16 checksum appended      */
#define BLUFI_FC_DIR_E2P  0x04 /* direction esp -> phone       */
#define BLUFI_FC_REQ_ACK  0x08 /* request acknowledgement      */
#define BLUFI_FC_FRAG     0x10 /* fragmented                   */

/* Security mode bits (from CTRL SET_SEC_MODE payload), per esp blufi_int.h. */
#define BLUFI_DATA_SEC_MODE_CHECK_MASK 0x01
#define BLUFI_DATA_SEC_MODE_ENC_MASK   0x02
#define BLUFI_CTRL_SEC_MODE_CHECK_MASK 0x10
#define BLUFI_CTRL_SEC_MODE_ENC_MASK   0x20

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
	/* esp btc_blufi_send_error_info(): payload is exactly 1 byte, the esp
	 * error state. It is an app-initiated push - the ESP-IDF blufi example
	 * never sends it on its own - so a device should not add it to the
	 * failure path unless the client asks for it. */
	BLUFI_DATA_ERROR_INFO     = 0x12,
	/* Opaque application channel; the ESP-IDF example only logs what arrives
	 * (no echo), so replies stay a product decision. */
	BLUFI_DATA_CUSTOM_DATA    = 0x13,
	BLUFI_DATA_STA_CONN_END_REASON = 0x15,
	BLUFI_DATA_STA_CONN_RSSI  = 0x16,
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

/*
 * esp_crc16_be: identical semantics to ESP-IDF's esp_rom_crc16_be() — poly 0x1021,
 * MSB-first, no reflection, with the seed/result inverted internally (the ROM
 * helper does `crc = ~crc` on entry and returns `~crc`). BluFi passes seed 0:
 * the inner loop is CRC-16/CCITT-FALSE and the returned value is its complement
 * (0xD64E for the "123456789" check string). This matches EspBlufiForiOS's
 * BlufiSecurity.crc and the ESP device checksum. Pass the previous result only
 * when chaining multiple calls.
 */
uint16_t blufi_crc16_be(uint16_t crc, const uint8_t *buf, uint32_t len);

/*
 * Finalize a frame buffer (as produced by blufi_frame_build: type,fc,seq,len,payload)
 * by appending the BluFi checksum: crc16 over [seq, data_len, payload] (len+2 bytes
 * starting at buf[2]), stored little-endian, and set the FC_CHECK bit.
 * `len` is the current encoded frame length (4 + payload). Requires cap >= len + 2.
 * Returns the new total length, or negative errno.
 */
int blufi_frame_append_checksum(uint8_t *buf, uint16_t len, size_t cap);

/*
 * Verify a received full frame buffer that carries FC_CHECK. buf_len is the total
 * received length. Returns 0 if the checksum matches, negative errno otherwise.
 */
int blufi_frame_verify_checksum(const uint8_t *buf, uint16_t buf_len);

/* ---- Inbound fragment reassembly (esp blufi_env.aggr_buf) ---- */

#define BLUFI_FRAG_MAX 1024

typedef struct {
	uint8_t buf[BLUFI_FRAG_MAX];
	uint16_t total; /* total assembled length (from first fragment prefix) */
	uint16_t off;   /* bytes accumulated so far */
	uint8_t type;   /* type_subtype of the fragmented message */
} blufi_reasm;

void blufi_reasm_reset(blufi_reasm *r);

/*
 * Feed one parsed frame. Returns:
 *   0  = more fragments expected (nothing to dispatch),
 *   1  = a complete message is ready -> out_type/out_data/out_len set
 *        (either a whole unfragmented frame, or the reassembled fragments),
 *  <0  = malformed.
 */
int blufi_reasm_feed(blufi_reasm *r, uint8_t type_subtype, uint8_t fc,
		     const uint8_t *data, uint16_t data_len, uint8_t *out_type,
		     uint8_t **out_data, uint16_t *out_len);

/* ---- Outbound fragment encoding (esp btc_blufi_send_encap loop) ---- */

/*
 * Produce the next on-wire frame from a logical payload. For a payload that fits
 * in one frame (len <= BLUFI_MAX_DATA_LEN) it is emitted whole. Otherwise a
 * leading 2-byte little-endian "remaining length" is prepended, up to
 * BLUFI_MAX_DATA_LEN-2 payload bytes are taken per fragment, FC_FRAG is set, and
 * the final remainder is sent unflagged. Drive it with an offset.
 *
 * in/in_len: full logical payload. *off: bytes consumed so far (updated).
 * frag_cap: max frame payload bytes per fragment (incl the 2-byte prefix), <= 255.
 * Writes the frame *body* (payload region only, WITHOUT the 4-byte header) to
 * out (cap >= frag_cap) and sets *out_len + *is_frag. Returns 0 while more
 * remains, 1 after the last fragment is emitted, negative on error.
 */
int blufi_next_fragment(const uint8_t *in, uint16_t in_len, uint16_t *off,
			uint16_t frag_cap, uint8_t *out, uint16_t *out_len,
			bool *is_frag);

#ifdef __cplusplus
}
#endif

#endif /* BLUFI_FRAME_H_ */
