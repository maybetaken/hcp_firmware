/*
 * Host unit tests for the BluFi frame codec (esp_blufi 4-byte header framing).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include "blufi/frame.h"
}

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
