// Host tests for hcp::ssdp parsing (the wire format Home Assistant emits).
#include "hcp/ssdp/discovery.h"

#include <string.h>

#include <gtest/gtest.h>

using namespace hcp::ssdp;

namespace {

bool Parse(const char* text, char* url, size_t cap = kUrlMax) {
  return ParseAnnouncement(text, strlen(text), url, cap);
}

}  // namespace

TEST(SsdpParse, RealAnnouncementFromHomeAssistant) {
  char url[kUrlMax];
  ASSERT_TRUE(Parse("maybetaken: mqtt://192.168.31.20", url));
  EXPECT_STREQ(url, "mqtt://192.168.31.20");
}

TEST(SsdpParse, TrimsTrailingCarriageReturns) {
  char url[kUrlMax];
  ASSERT_TRUE(Parse("maybetaken: mqtt://10.0.0.5\r\n", url));
  EXPECT_STREQ(url, "mqtt://10.0.0.5");
}

TEST(SsdpParse, IgnoresForeignTraffic) {
  char url[kUrlMax];
  EXPECT_FALSE(Parse("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n", url));
  EXPECT_EQ(url[0], '\0');
  EXPECT_FALSE(Parse("short", url));
  EXPECT_FALSE(ParseAnnouncement(nullptr, 5, url, sizeof(url)));
  EXPECT_FALSE(ParseAnnouncement("", 0, url, sizeof(url)));
}

TEST(SsdpParse, EmptyPayloadAfterMarkerIsRejected) {
  char url[kUrlMax];
  EXPECT_FALSE(Parse("maybetaken:    ", url));
}

TEST(SsdpParse, OverlongUrlIsRejectedNotTruncated) {
  char url[16];
  EXPECT_FALSE(Parse("maybetaken: mqtt://this-host-name-is-far-too-long-for-me", url,
                     sizeof(url)));
}

TEST(MqttUrl, HostOnlyUsesDefaultPort) {
  char host[32];
  uint16_t port = 0;
  ASSERT_TRUE(ParseMqttUrl("mqtt://192.168.31.20", host, sizeof(host), &port));
  EXPECT_STREQ(host, "192.168.31.20");
  EXPECT_EQ(port, 1883);
}

TEST(MqttUrl, ExplicitPortIsHonoured) {
  char host[32];
  uint16_t port = 0;
  ASSERT_TRUE(ParseMqttUrl("mqtt://broker.lan:1884", host, sizeof(host), &port));
  EXPECT_STREQ(host, "broker.lan");
  EXPECT_EQ(port, 1884);
}

TEST(MqttUrl, RejectsOtherSchemesAndJunk) {
  char host[32];
  uint16_t port = 0;
  EXPECT_FALSE(ParseMqttUrl("http://1.2.3.4", host, sizeof(host), &port));
  EXPECT_FALSE(ParseMqttUrl("mqtt://", host, sizeof(host), &port));
  EXPECT_FALSE(ParseMqttUrl("mqtt://:1883", host, sizeof(host), &port));
  EXPECT_FALSE(ParseMqttUrl("mqtt://host:notaport", host, sizeof(host), &port));
  EXPECT_FALSE(ParseMqttUrl("mqtt://host/0", host, sizeof(host), &port));
}
