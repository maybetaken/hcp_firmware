// Host tests for ota_client's pure layer: version ordering and URL splitting.
//
// The core and the backends need the kernel, sockets and flash; these two are
// deliberately free of them so they can be checked here.
#include <ota_client/ota_util.h>

#include <cerrno>

#include <gtest/gtest.h>

namespace {

constexpr int kOlder = -1;
constexpr int kNewer = 1;

}  // namespace

// ---- version ordering ----

TEST(OtaVersion, EqualVersionsCompareZero) {
  EXPECT_EQ(ota_client_version_cmp("1.2.3", "1.2.3"), 0);
  EXPECT_EQ(ota_client_version_cmp("0.0.0", "0"), 0);
  EXPECT_EQ(ota_client_version_cmp("v1.2", "1.2.0"), 0);
}

TEST(OtaVersion, OrdersNumericFieldsNotText) {
  // A string compare would put "1.10.0" below "1.2.0", which silently blocks
  // every update from the tenth minor release on.
  EXPECT_EQ(ota_client_version_cmp("1.10.0", "1.2.0"), kNewer);
  EXPECT_EQ(ota_client_version_cmp("1.2.0", "1.10.0"), kOlder);
  EXPECT_EQ(ota_client_version_cmp("2.0.0", "10.0.0"), kOlder);
  EXPECT_EQ(ota_client_version_cmp("1.2.3", "1.2.4"), kOlder);
}

TEST(OtaVersion, ToleratesLeadingVAndAnyFieldCount) {
  EXPECT_EQ(ota_client_version_cmp("v1.2.3", "V1.2.3"), 0);
  EXPECT_EQ(ota_client_version_cmp("1.2", "1.2.1"), kOlder);
  EXPECT_EQ(ota_client_version_cmp("1.2.3.4", "1.2.3"), kNewer);
}

TEST(OtaVersion, RejectsAnythingThatIsNotAPlainVersion) {
  EXPECT_EQ(ota_client_version_cmp("", "1.0.0"), -EINVAL);
  EXPECT_EQ(ota_client_version_cmp("1.0.0", nullptr), -EINVAL);
  EXPECT_EQ(ota_client_version_cmp("1.0.0-beta", "1.0.0"), -EINVAL);
  EXPECT_EQ(ota_client_version_cmp("1.0.0 ", "1.0.0"), -EINVAL);
  EXPECT_EQ(ota_client_version_cmp("1..0", "1.0"), -EINVAL);
  EXPECT_EQ(ota_client_version_cmp("1.0.", "1.0"), -EINVAL);
  EXPECT_EQ(ota_client_version_cmp("x1.0", "1.0"), -EINVAL);
  // More than the four fields the library compares would be silently truncated.
  EXPECT_EQ(ota_client_version_cmp("1.0.0.0.1", "1.0.0.0.2"), -EINVAL);
}

TEST(OtaVersion, RefusesToWrapOnHugeFields) {
  EXPECT_EQ(ota_client_version_cmp("4294967296.0", "1.0"), -EINVAL);
  EXPECT_EQ(ota_client_version_cmp("4294967295.0", "4294967294.0"), kNewer);
}

// ---- URL splitting ----

TEST(OtaUrl, SplitsHostPortAndPath) {
  ota_client_url url;
  ASSERT_EQ(ota_client_url_parse("http://192.168.31.110:9998/firmware.bin", &url),
            0);
  EXPECT_STREQ(url.host, "192.168.31.110");
  EXPECT_STREQ(url.port, "9998");
  EXPECT_STREQ(url.path, "/firmware.bin");
  EXPECT_FALSE(url.secure);
}

TEST(OtaUrl, DefaultsPortAndPath) {
  ota_client_url url;
  ASSERT_EQ(ota_client_url_parse("http://ota.example", &url), 0);
  EXPECT_STREQ(url.host, "ota.example");
  EXPECT_STREQ(url.port, "80");
  EXPECT_STREQ(url.path, "/");

  ASSERT_EQ(ota_client_url_parse("https://ota.example/firmware.bin", &url), 0);
  EXPECT_STREQ(url.port, "443");
  EXPECT_TRUE(url.secure);
}

TEST(OtaUrl, KeepsTheWholePathIncludingQuery) {
  ota_client_url url;
  ASSERT_EQ(ota_client_url_parse("http://h:8080/a/b/c.bin?v=2", &url), 0);
  EXPECT_STREQ(url.host, "h");
  EXPECT_STREQ(url.port, "8080");
  EXPECT_STREQ(url.path, "/a/b/c.bin?v=2");
}

TEST(OtaUrl, RejectsMalformedInput) {
  ota_client_url url;
  EXPECT_EQ(ota_client_url_parse(nullptr, &url), -EINVAL);
  EXPECT_EQ(ota_client_url_parse("ftp://host/fw.bin", &url), -EINVAL);
  EXPECT_EQ(ota_client_url_parse("http://", &url), -EINVAL);
  EXPECT_EQ(ota_client_url_parse("http:///firmware.bin", &url), -EINVAL);
  EXPECT_EQ(ota_client_url_parse("http://host:0/", &url), -EINVAL);
  EXPECT_EQ(ota_client_url_parse("http://host:99999/", &url), -EINVAL);
  EXPECT_EQ(ota_client_url_parse("http://host:notaport/", &url), -EINVAL);
  // IPv6 literals would need their own delimiter rules; refused, not mangled.
  EXPECT_EQ(ota_client_url_parse("http://[::1]:80/x", &url), -EINVAL);
}

TEST(OtaUrl, SchemeIsCaseInsensitive) {
  ota_client_url url;
  ASSERT_EQ(ota_client_url_parse("HTTP://Host/Fw.BIN", &url), 0);
  EXPECT_STREQ(url.host, "Host");
  EXPECT_STREQ(url.path, "/Fw.BIN");
}
