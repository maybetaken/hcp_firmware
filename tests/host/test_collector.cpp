/*
 * Host unit tests for hcp::collector's topic mapping - the contract with Home
 * Assistant, which otherwise lives as a switch inside a thread loop where a typo
 * is only visible on a device across the room.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include "hcp/collector/collector.h"

#include <string.h>

using hcp::collector::Classify;
using hcp::collector::Command;

namespace {

/* The MQTT packet gives a length-bounded, non-NUL-terminated suffix, so that is
 * the shape the tests hand over too. */
Command Of(const char* suffix) {
  return Classify(suffix, strlen(suffix));
}

}  // namespace

TEST(CollectorTopics, MapWhatThePluginsPublish) {
  /* Each of these is a _build_topic("...") call in
   * custom_components/solar_manager/plugins. */
  EXPECT_EQ(Of("config"), Command::kConfig);
  EXPECT_EQ(Of("control/cmd"), Command::kModbusWrite);
  EXPECT_EQ(Of("control/restart"), Command::kRestart);
  EXPECT_EQ(Of("control/reconfig"), Command::kReconfig);
  EXPECT_EQ(Of("control/led"), Command::kLed);
  EXPECT_EQ(Of("host/heartbeat"), Command::kHeartbeat);
  /* The previous firmware's OTA trigger, registered in solar_manager.cpp as
   * <sn>/ota/start; without it Home Assistant cannot drive an update. */
  EXPECT_EQ(Of("ota/start"), Command::kOtaStart);
}

TEST(CollectorTopics, NothingElseIsClaimed) {
  // <sn>/cmd belongs to hcp::ota's text commands and must not be grabbed here.
  EXPECT_EQ(Of("cmd"), Command::kNone);
  EXPECT_EQ(Of("state"), Command::kNone);
  EXPECT_EQ(Of("ota"), Command::kNone);
  EXPECT_EQ(Of("ota/stop"), Command::kNone);
  EXPECT_EQ(Of("online"), Command::kNone);
  EXPECT_EQ(Of(""), Command::kNone);
  EXPECT_EQ(Classify(nullptr, 0), Command::kNone);
}

TEST(CollectorTopics, NearMissesAreNotMatched) {
  EXPECT_EQ(Of("configs"), Command::kNone);
  EXPECT_EQ(Of("control/cmds"), Command::kNone);
  EXPECT_EQ(Of("control/comm"), Command::kNone);
  EXPECT_EQ(Of("/config"), Command::kNone);
  /* A prefix of a real suffix is not a match - the length is checked first. */
  EXPECT_EQ(Of("control"), Command::kNone);
}

TEST(CollectorTopics, AcceptsAnUnterminatedSlice) {
  const char* topic = "config and then some other bytes";
  EXPECT_EQ(Classify(topic, 6), Command::kConfig);
  EXPECT_EQ(Classify(topic, 7), Command::kNone); /* "config " */
}
