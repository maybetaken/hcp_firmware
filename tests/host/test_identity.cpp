/*
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// Host tests for hcp::identity's pure formatters: the serial number string and
// the RFC 1123 hostname derived from it.
#include "hcp/identity/identity.h"

#include <string.h>

#include <vector>

#include <gtest/gtest.h>

using hcp::identity::ComposeSerial;
using hcp::identity::FormatFromMac;
using hcp::identity::FormatHostname;
using hcp::identity::ParseProdinfo;
using hcp::identity::kSerialMax;

namespace {

const uint8_t kMac[6] = {0x40, 0x4c, 0xca, 0x57, 0x44, 0x6c};

// The factory blob as tools/make_sn_bin.py writes it (0xFF = erased flash).
std::vector<uint8_t> Blob(const char* code, const char* magic = "HCP1",
                          size_t size = 64) {
  std::vector<uint8_t> blob(size, 0xFF);
  memcpy(blob.data(), magic, 4);
  blob[4] = static_cast<uint8_t>(strlen(code) & 0xFF);
  blob[5] = static_cast<uint8_t>((strlen(code) >> 8) & 0xFF);
  memcpy(blob.data() + 6, code, strlen(code));
  return blob;
}

}  // namespace

// ---- FormatFromMac ----

TEST(IdentitySerial, MatchesTheEspBlufiStyleName) {
  char sn[kSerialMax];
  ASSERT_TRUE(FormatFromMac(kMac, "hcp-", sn, sizeof(sn)));
  EXPECT_STREQ(sn, "hcp-404cca57446c");
}

TEST(IdentitySerial, HonoursAnEmptyPrefix) {
  char sn[kSerialMax];
  ASSERT_TRUE(FormatFromMac(kMac, "", sn, sizeof(sn)));
  EXPECT_STREQ(sn, "404cca57446c");
}

TEST(IdentitySerial, RejectsTooSmallBufferWithoutWriting) {
  char sn[16];
  strcpy(sn, "untouched");
  EXPECT_FALSE(FormatFromMac(kMac, "hcp-", sn, sizeof(sn)));
  EXPECT_STREQ(sn, "untouched");
}

TEST(IdentitySerial, RejectsNullArguments) {
  char sn[kSerialMax];
  EXPECT_FALSE(FormatFromMac(nullptr, "hcp-", sn, sizeof(sn)));
  EXPECT_FALSE(FormatFromMac(kMac, nullptr, sn, sizeof(sn)));
  EXPECT_FALSE(FormatFromMac(kMac, "hcp-", nullptr, kSerialMax));
}

// ---- ComposeSerial ----

TEST(IdentityCompose, PrefixesAFactoryCode) {
  char sn[kSerialMax];
  ASSERT_TRUE(ComposeSerial("M1230", "hcp-", sn, sizeof(sn)));
  EXPECT_STREQ(sn, "hcp-M1230");
}

TEST(IdentityCompose, NeverDoublesThePrefix) {
  char sn[kSerialMax];
  ASSERT_TRUE(ComposeSerial("hcp-M1230", "hcp-", sn, sizeof(sn)));
  EXPECT_STREQ(sn, "hcp-M1230");
}

TEST(IdentityCompose, RejectsEmptyAndOversized) {
  char sn[kSerialMax];
  EXPECT_FALSE(ComposeSerial("", "hcp-", sn, sizeof(sn)));
  EXPECT_FALSE(ComposeSerial(nullptr, "hcp-", sn, sizeof(sn)));
  EXPECT_FALSE(ComposeSerial("012345678901234567890123456789012345", "hcp-", sn,
                             sizeof(sn)));
  char tiny[4];
  EXPECT_FALSE(ComposeSerial("M1230", "hcp-", tiny, sizeof(tiny)));
  char exact[8];  // "hcp-12" needs 6 characters plus the NUL
  EXPECT_FALSE(ComposeSerial("M1230", "hcp-", exact, sizeof(exact)));
  ASSERT_TRUE(ComposeSerial("12", "hcp-", exact, sizeof(exact)));
  EXPECT_STREQ(exact, "hcp-12");
}

// ---- ParseProdinfo ----

TEST(IdentityProdinfo, ReadsABurnedCode) {
  char sn[kSerialMax];
  const auto blob = Blob("M1230");
  ASSERT_TRUE(ParseProdinfo(blob.data(), blob.size(), "hcp-", sn, sizeof(sn)));
  EXPECT_STREQ(sn, "hcp-M1230");
}

TEST(IdentityProdinfo, ErasedPartitionIsRejected) {
  char sn[kSerialMax];
  const std::vector<uint8_t> blank(64, 0xFF);
  EXPECT_FALSE(ParseProdinfo(blank.data(), blank.size(), "hcp-", sn, sizeof(sn)));
}

TEST(IdentityProdinfo, MalformedBlobsAreRejected) {
  char sn[kSerialMax];
  // Wrong magic (e.g. some other vendor's blob at this offset).
  EXPECT_FALSE(ParseProdinfo(Blob("M1", "XXXX").data(), 64, "hcp-", sn,
                             sizeof(sn)));
  // Zero length.
  EXPECT_FALSE(ParseProdinfo(Blob("").data(), 64, "hcp-", sn, sizeof(sn)));
  // Code claims to run past the read buffer (torn/short read).
  EXPECT_FALSE(ParseProdinfo(Blob("M1230", "HCP1", 10).data(), 10, "hcp-", sn,
                             sizeof(sn)));
  // NUL inside the code.
  std::vector<uint8_t> hole = Blob("M1230");
  hole[8] = 0x00;
  EXPECT_FALSE(ParseProdinfo(hole.data(), hole.size(), "hcp-", sn, sizeof(sn)));
  // Too long for the caller (never a partial value).
  char tiny[8];
  EXPECT_FALSE(ParseProdinfo(Blob("MJUDD9WE6").data(), 64, "hcp-", tiny,
                             sizeof(tiny)));
  EXPECT_FALSE(ParseProdinfo(nullptr, 64, "hcp-", sn, sizeof(sn)));
}

TEST(IdentityProdinfo, LongCodeStillFitsTheSerial) {
  char sn[kSerialMax];
  // 8 characters like the reference firmware's generate_sn.py output.
  ASSERT_TRUE(ParseProdinfo(Blob("MJUDD9WE6").data(), 64, "hcp-", sn,
                            sizeof(sn)));
  EXPECT_STREQ(sn, "hcp-MJUDD9WE6");
}

// ---- FormatHostname ----

TEST(IdentityHostname, DerivedSerialPassesThroughUnchanged) {
  char host[kSerialMax];
  ASSERT_TRUE(FormatHostname("hcp-404cca57446c", host, sizeof(host)));
  EXPECT_STREQ(host, "hcp-404cca57446c");
}

TEST(IdentityHostname, LowercasesAndReplacesIllegalCharacters) {
  char host[64];
  ASSERT_TRUE(FormatHostname("HCP_Jiang#7", host, sizeof(host)));
  EXPECT_STREQ(host, "hcp-jiang-7");
}

TEST(IdentityHostname, DropsHyphensAtBothEnds) {
  char host[64];
  ASSERT_TRUE(FormatHostname("__SN-01__", host, sizeof(host)));
  EXPECT_STREQ(host, "sn-01");
  // Nothing but separators: no label can be built.
  EXPECT_FALSE(FormatHostname("---", host, sizeof(host)));
  EXPECT_FALSE(FormatHostname("", host, sizeof(host)));
}

TEST(IdentityHostname, TruncatesToOneDnsLabel) {
  char host[80];
  // 70 characters, i.e. longer than a single label may be.
  const char* long_sn =
      "0123456789012345678901234567890123456789012345678901234567890123456"
      "789";
  ASSERT_EQ(strlen(long_sn), 70u);
  ASSERT_TRUE(FormatHostname(long_sn, host, sizeof(host)));
  EXPECT_EQ(strlen(host), 63u);  // RFC 1035 label limit
}

TEST(IdentityHostname, RespectsTheCallerBuffer) {
  char host[6];
  ASSERT_TRUE(FormatHostname("hcp-404cca57446c", host, sizeof(host)));
  EXPECT_STREQ(host, "hcp-4");
  char tiny[1];
  EXPECT_FALSE(FormatHostname("hcp", tiny, sizeof(tiny)));
  EXPECT_FALSE(FormatHostname(nullptr, host, sizeof(host)));
}
