/*
 * Host unit tests for the BluFi frame codec (esp_blufi 4-byte header framing).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>

extern "C" {
#include "blufi/frame.h"
#include "blufi/blufi_security.h"
#include "mbedtls/dhm.h"
#include "mbedtls/bignum.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
}

#include <vector>

TEST(BlufiFrame, TypeSubtypeEncoding) {
  // DATA / STA_SSID(0x02) -> byte0 = (0x02<<2)|0x01 = 0x09
  EXPECT_EQ(BLUFI_BUILD_TYPE(BLUFI_TYPE_DATA, BLUFI_DATA_STA_SSID), 0x09);
  // CTRL / GET_VERSION(0x07) -> (0x07<<2)|0x00 = 0x1C
  EXPECT_EQ(BLUFI_BUILD_TYPE(BLUFI_TYPE_CTRL, BLUFI_CTRL_GET_VERSION), 0x1C);
  EXPECT_EQ(BLUFI_GET_TYPE(0x09), BLUFI_TYPE_DATA);
  EXPECT_EQ(BLUFI_GET_SUBTYPE(0x09), BLUFI_DATA_STA_SSID);
}

TEST(BlufiFrame, BuildSsid) {
  const char* ssid = "MyAP";
  uint8_t buf[64];
  int n = blufi_frame_build(BLUFI_TYPE_DATA, BLUFI_DATA_STA_SSID, 3,
                            reinterpret_cast<const uint8_t*>(ssid), 4, buf,
                            sizeof(buf));
  ASSERT_EQ(n, 8);            // 4-byte header + 4
  EXPECT_EQ(buf[0], 0x09);    // DATA|STA_SSID
  EXPECT_EQ(buf[1], 0x00);    // fc
  EXPECT_EQ(buf[2], 3);       // seq
  EXPECT_EQ(buf[3], 4);       // len
  EXPECT_EQ(memcmp(buf + 4, "MyAP", 4), 0);
}

TEST(BlufiFrame, Roundtrip) {
  uint8_t buf[64];
  const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  int n = blufi_frame_build(BLUFI_TYPE_DATA, BLUFI_DATA_CUSTOM_DATA, 7, payload,
                            sizeof(payload), buf, sizeof(buf));
  ASSERT_GT(n, 0);

  struct blufi_frame f;
  ASSERT_EQ(blufi_frame_parse(buf, n, &f), 0);
  EXPECT_EQ(BLUFI_GET_TYPE(f.type_subtype), BLUFI_TYPE_DATA);
  EXPECT_EQ(BLUFI_GET_SUBTYPE(f.type_subtype), BLUFI_DATA_CUSTOM_DATA);
  EXPECT_EQ(f.seq, 7);
  EXPECT_EQ(f.len, sizeof(payload));
  EXPECT_EQ(memcmp(f.data, payload, f.len), 0);
}

TEST(BlufiFrame, RejectsShortAndTruncated) {
  struct blufi_frame f;
  EXPECT_LT(blufi_frame_parse(nullptr, 4, &f), 0);
  uint8_t hdr[4] = {0x09, 0, 0, 10};  // claims 10 payload bytes but none present
  EXPECT_LT(blufi_frame_parse(hdr, 4, &f), 0);
}

TEST(BlufiFrame, OversizedRejected) {
  uint8_t buf[300];
  uint8_t big[300] = {0};
  int n = blufi_frame_build(BLUFI_TYPE_DATA, BLUFI_DATA_STA_PASSWD, 0, big, 256,
                            buf, sizeof(buf));
  EXPECT_LT(n, 0);  // > BLUFI_MAX_DATA_LEN -> caller must fragment
}

TEST(BlufiCrc16, CcittFalseVector) {
  // CRC-16/CCITT-FALSE (poly 0x1021, MSB-first, no reflect) over "123456789"
  // with init 0xFFFF is the well-known 0x29B1. Validates poly/bit-order.
  const uint8_t check[] = {'1','2','3','4','5','6','7','8','9'};
  EXPECT_EQ(blufi_crc16_be(0xFFFF, check, sizeof(check)), 0x29B1);
}

TEST(BlufiChecksum, AppendAndVerifyRoundtrip) {
  uint8_t buf[64];
  const uint8_t payload[] = {0x11, 0x22, 0x33};
  int n = blufi_frame_build(BLUFI_TYPE_CTRL, BLUFI_CTRL_CONN_TO_AP, 5, payload,
                            sizeof(payload), buf, sizeof(buf));
  ASSERT_EQ(n, 7);  // 4 hdr + 3
  int total = blufi_frame_append_checksum(buf, n, sizeof(buf));
  ASSERT_EQ(total, 9);  // + 2 checksum bytes
  EXPECT_EQ(buf[1] & BLUFI_FC_CHECK, BLUFI_FC_CHECK);
  EXPECT_EQ(blufi_frame_verify_checksum(buf, total), 0);
}

TEST(BlufiChecksum, DetectsTamper) {
  uint8_t buf[64];
  const uint8_t payload[] = {0xAA, 0xBB};
  int n = blufi_frame_build(BLUFI_TYPE_DATA, BLUFI_DATA_STA_SSID, 1, payload,
                            sizeof(payload), buf, sizeof(buf));
  int total = blufi_frame_append_checksum(buf, n, sizeof(buf));
  ASSERT_GT(total, 0);
  buf[4] ^= 0x01;  // flip a payload bit
  EXPECT_EQ(blufi_frame_verify_checksum(buf, total), -EBADMSG);
}

TEST(BlufiChecksum, EmptyPayload) {
  uint8_t buf[16];
  int n = blufi_frame_build(BLUFI_TYPE_CTRL, BLUFI_CTRL_GET_WIFI_LIST, 0, nullptr,
                            0, buf, sizeof(buf));
  ASSERT_EQ(n, 4);
  int total = blufi_frame_append_checksum(buf, n, sizeof(buf));
  ASSERT_EQ(total, 6);
  EXPECT_EQ(blufi_frame_verify_checksum(buf, total), 0);
}

// ---- vendored crypto correctness ----

TEST(BlufiSec, Md5KnownVector) {
  uint8_t out[16];
  blufimbed_md5(reinterpret_cast<const uint8_t*>("abc"), 3, out);
  const uint8_t expect[16] = {0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,
                              0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
  EXPECT_EQ(memcmp(out, expect, 16), 0);
}

TEST(BlufiSec, AesEcbFips197Vector) {
  const uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                           0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
  const uint8_t pt[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                          0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
  const uint8_t ct[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                          0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
  blufimbed_aes_context a;
  blufimbed_aes_init(&a);
  ASSERT_EQ(blufimbed_aes_setkey_enc(&a, key, 128), 0);
  uint8_t out[16];
  blufimbed_aes_encrypt(&a, pt, out);
  EXPECT_EQ(memcmp(out, ct, 16), 0);
  blufimbed_aes_free(&a);
}

TEST(BlufiSec, AesCfb128Roundtrip) {
  const uint8_t key[16] = {0};
  uint8_t iv[16] = {0};
  const uint8_t pt[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
  blufimbed_aes_context a;
  blufimbed_aes_init(&a);
  ASSERT_EQ(blufimbed_aes_setkey_enc(&a, key, 128), 0);
  uint8_t ct[20], back[20];
  size_t off = 0;
  blufimbed_aes_crypt_cfb128(&a, MBEDTLS_AES_ENCRYPT, sizeof(pt), &off, iv, pt, ct);
  off = 0;
  memset(iv, 0, 16);
  blufimbed_aes_crypt_cfb128(&a, MBEDTLS_AES_DECRYPT, sizeof(pt), &off, iv, ct, back);
  EXPECT_EQ(memcmp(back, pt, sizeof(pt)), 0);
  EXPECT_NE(memcmp(ct, pt, sizeof(pt)), 0);  // actually changed
  blufimbed_aes_free(&a);
}

// ---- fragment encode <-> reassemble round-trip (the DH-pubkey path) ----

TEST(BlufiFrag, EncodeReassembleRoundtrip) {
  std::vector<uint8_t> payload(300);
  for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(i * 7 + 1);

  blufi_reasm r;
  blufi_reasm_reset(&r);
  uint16_t off = 0;
  int fr;
  uint8_t seq = 0;
  bool got_complete = false;
  do {
    uint16_t blen; bool is_frag; uint8_t body[BLUFI_MAX_DATA_LEN];
    fr = blufi_next_fragment(payload.data(), (uint16_t)payload.size(), &off,
                             BLUFI_MAX_DATA_LEN, body, &blen, &is_frag);
    ASSERT_GE(fr, 0);
    uint8_t fc = BLUFI_FC_DIR_E2P | (is_frag ? BLUFI_FC_FRAG : 0);
    uint8_t ot; uint8_t *od; uint16_t ol;
    int rr = blufi_reasm_feed(&r, BLUFI_BUILD_TYPE(BLUFI_TYPE_DATA, BLUFI_DATA_NEG),
                              fc, body, blen, &ot, &od, &ol);
    if (rr == 1) {
      ASSERT_EQ(ol, payload.size());
      EXPECT_EQ(memcmp(od, payload.data(), ol), 0);
      EXPECT_EQ(ot, BLUFI_BUILD_TYPE(BLUFI_TYPE_DATA, BLUFI_DATA_NEG));
      got_complete = true;
    }
    seq++;
  } while (fr == 0);
  EXPECT_TRUE(got_complete);
}

// ---- full DH + AES-CFB interop: simulate the phone against blufi_security ----
TEST(BlufiHandshake, DhThenAesCfbInterop) {
  ASSERT_EQ(blufi_security_init(), 0);

  // Phone group: P = 2^31-1 (prime), g = 2, private x, public X = g^x mod P.
  mbedtls_mpi P, G, X, x, Y, shared;
  mbedtls_mpi_init(&P); mbedtls_mpi_init(&G); mbedtls_mpi_init(&X);
  mbedtls_mpi_init(&x);  mbedtls_mpi_init(&Y); mbedtls_mpi_init(&shared);
  ASSERT_EQ(mbedtls_mpi_lset(&P, 0x7FFFFFFF), 0);
  ASSERT_EQ(mbedtls_mpi_lset(&G, 2), 0);
  ASSERT_EQ(mbedtls_mpi_lset(&x, 0x12345678), 0);
  ASSERT_EQ(mbedtls_mpi_exp_mod(&X, &G, &x, &P, nullptr), 0);

  // dh_param = [2B len][be] for P, G, then the peer public value (GY=X).
  auto bignum = [](const mbedtls_mpi *m) {
    size_t n = mbedtls_mpi_size(m);
    uint8_t buf[64];
    mbedtls_mpi_write_binary(m, buf, n);
    std::vector<uint8_t> r;
    r.push_back((uint8_t)(n >> 8));
    r.push_back((uint8_t)(n & 0xff));
    r.insert(r.end(), buf, buf + n);
    return r;
  };
  std::vector<uint8_t> der;
  for (auto v : {bignum(&P), bignum(&G), bignum(&X)})
    der.insert(der.end(), v.begin(), v.end());

  uint8_t lenf[3] = {SEC_TYPE_DH_PARAM_LEN, (uint8_t)(der.size() >> 8),
                     (uint8_t)(der.size() & 0xff)};
  const uint8_t *tmp; uint16_t tl;
  ASSERT_EQ(blufi_negotiate(lenf, sizeof(lenf), &tmp, &tl), 0);

  std::vector<uint8_t> f; f.push_back(SEC_TYPE_DH_PARAM_DATA);
  f.insert(f.end(), der.begin(), der.end());
  const uint8_t *out = nullptr; uint16_t ylen = 0;
  ASSERT_EQ(blufi_negotiate(f.data(), (uint16_t)f.size(), &out, &ylen), 0);
  ASSERT_GT(ylen, 0);
  ASSERT_LE(ylen, sizeof(uint8_t) * 64);
  uint8_t ykey[64]; memcpy(ykey, out, ylen);

  // Phone computes shared = devicePub^x mod P, psk = MD5(shared_be).
  // Must match device calc_secret, which writes mbedtls_mpi_size(K) bytes (no
  // leading zeros) and MD5s that length.
  ASSERT_EQ(mbedtls_mpi_read_binary(&Y, ykey, ylen), 0);
  ASSERT_EQ(mbedtls_mpi_exp_mod(&shared, &Y, &x, &P, nullptr), 0);
  size_t slen = mbedtls_mpi_size(&shared);
  uint8_t share_be[64];
  ASSERT_EQ(mbedtls_mpi_write_binary(&shared, share_be, slen), 0);
  uint8_t psk[16];
  blufimbed_md5(share_be, slen, psk);

  // Phone AES-128-CFB128-encrypts a password (iv=0, iv[0]=seq); device decrypts.
  const uint8_t seq = 42;
  const char *plain = "secret-password-1234";
  size_t plen = strlen(plain);
  uint8_t cipher[32];
  uint8_t iv[16] = {0}; iv[0] = seq;
  blufimbed_aes_context a;
  blufimbed_aes_init(&a);
  ASSERT_EQ(blufimbed_aes_setkey_enc(&a, psk, 128), 0);
  size_t off = 0;
  blufimbed_aes_crypt_cfb128(&a, MBEDTLS_AES_ENCRYPT, plen, &off, iv,
                              (const uint8_t *)plain, cipher);
  blufimbed_aes_free(&a);
  EXPECT_NE(memcmp(cipher, plain, plen), 0);

  uint8_t recovered[32];
  memcpy(recovered, cipher, plen);
  ASSERT_EQ(blufi_aes_decrypt(seq, recovered, (int)plen), (int)plen);
  EXPECT_EQ(memcmp(recovered, plain, plen), 0);

  mbedtls_mpi_free(&P); mbedtls_mpi_free(&G); mbedtls_mpi_free(&X);
  mbedtls_mpi_free(&x);  mbedtls_mpi_free(&Y); mbedtls_mpi_free(&shared);
  blufi_security_deinit();
}
