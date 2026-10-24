/* Host unit tests for hcp::switchctl: the posix relay fake, and the JSON shapes of
 * the "<sn>-K" device - the contract with Home Assistant's Switches plugin, which is
 * otherwise only visible as "the switch in the UI did not move". */
#include <gtest/gtest.h>

#include <string.h>

#include "hcp/switch/command.h"
#include "hcp/switch/relay.h"

using hcp::switchctl::kRelayCount;
using hcp::switchctl::Relay;
using hcp::switchctl::SwitchCommand;

TEST(Switch, OpenAndToggle) {
  Relay r;
  ASSERT_TRUE(r.Open(0));
  EXPECT_EQ(r.Index(), 0);
  EXPECT_FALSE(r.Get());
  r.Set(true);
  EXPECT_TRUE(r.Get());
  r.Set(false);
  EXPECT_FALSE(r.Get());
}

TEST(Switch, IndependentInstances) {
  Relay a, b;
  ASSERT_TRUE(a.Open(0));
  ASSERT_TRUE(b.Open(1));
  a.Set(true);
  EXPECT_TRUE(a.Get());
  EXPECT_FALSE(b.Get());
}

TEST(Switch, OpenOutOfRangeFails) {
  Relay r;
  EXPECT_FALSE(r.Open(-1));
  EXPECT_FALSE(r.Open(kRelayCount));
  EXPECT_FALSE(r.Open(99));
}

namespace {

int Parse(const char* text, SwitchCommand* out, int max) {
  return hcp::switchctl::ParseCommands(
      reinterpret_cast<const uint8_t*>(text), strlen(text), out, max);
}

}  // namespace

TEST(SwitchCommand, TakesTheShapeHomeAssistantSends) {
  /* plugins/Switches.py switch_name(): one-based, zero-padded to two digits. */
  SwitchCommand cmd{};
  ASSERT_EQ(Parse("{\"switch_01\": \"on\"}", &cmd, 1), 1);
  EXPECT_EQ(cmd.index, 1);
  EXPECT_TRUE(cmd.on);

  ASSERT_EQ(Parse("{\"switch_02\":\"off\"}", &cmd, 1), 1);
  EXPECT_EQ(cmd.index, 2);
  EXPECT_FALSE(cmd.on);
}

TEST(SwitchCommand, ReadsEveryKeyPresent) {
  /* An object may name several switches; acting on only the first would leave the
   * UI showing one relay changed and the other not, with no way to tell the owner
   * their command was half ignored. */
  SwitchCommand cmds[4] = {};
  ASSERT_EQ(Parse("{\"switch_01\":\"on\",\"switch_02\":\"off\"}", cmds, 4), 2);
  EXPECT_EQ(cmds[0].index, 1);
  EXPECT_TRUE(cmds[0].on);
  EXPECT_EQ(cmds[1].index, 2);
  EXPECT_FALSE(cmds[1].on);
}

TEST(SwitchCommand, AcceptsAnUnpaddedIndex) {
  /* The previous firmware parsed the key with sscanf("switch_%d"), so "switch_1" and
   * "switch_01" both named relay one. A hand-written automation will use either. */
  SwitchCommand cmd{};
  EXPECT_EQ(Parse("{\"switch_1\":\"on\"}", &cmd, 1), 1);
  EXPECT_EQ(cmd.index, 1);

  ASSERT_EQ(Parse("{\"switch_10\":\"off\"}", &cmd, 1), 1);
  EXPECT_EQ(cmd.index, 10);
}

TEST(SwitchCommand, IgnoresWhatIsNotASwitchState) {
  SwitchCommand cmd{};
  // other keys of the same object
  EXPECT_EQ(Parse("{\"mode\":\"on\"}", &cmd, 1), 0);
  // states the integration itself would skip (handle_notify accepts only on/off)
  EXPECT_EQ(Parse("{\"switch_01\":\"true\"}", &cmd, 1), 0);
  EXPECT_EQ(Parse("{\"switch_01\":\"ON\"}", &cmd, 1), 0);
  EXPECT_EQ(Parse("{\"switch_01\":1}", &cmd, 1), 0);
  // not an object at all - a bare word is not a command
  EXPECT_EQ(Parse("on", &cmd, 1), 0);
  // zero is not addressable: switch_name() is one-based
  EXPECT_EQ(Parse("{\"switch_00\":\"on\"}", &cmd, 1), 0);
  EXPECT_EQ(Parse("{\"switch_0\":\"on\"}", &cmd, 1), 0);
}

TEST(SwitchCommand, NeverReadsPastThePayload) {
  /* The buffer is an MQTT payload and is not NUL terminated, so a truncated object
   * must simply yield nothing rather than run off the end. */
  const char text[] = "{\"switch_01\": \"on\"}";
  SwitchCommand cmd{};
  for (size_t n = 0; n < sizeof(text) - 1; n++) {
    ASSERT_EQ(hcp::switchctl::ParseCommands(
                  reinterpret_cast<const uint8_t*>(text), n, &cmd, 1),
              0)
        << "truncated at " << n;
  }
  ASSERT_EQ(hcp::switchctl::ParseCommands(
                reinterpret_cast<const uint8_t*>(text), sizeof(text) - 1, &cmd, 1),
            1);
}

TEST(SwitchCommand, ReportsTruncationRatherThanDroppingQuietly) {
  SwitchCommand cmds[1] = {};
  /* One slot: the first command is real, and the count reaching max is how the caller
   * learns there might have been more. */
  EXPECT_EQ(Parse("{\"switch_01\":\"on\",\"switch_02\":\"on\"}", cmds, 1), 1);
  EXPECT_EQ(cmds[0].index, 1);
}

TEST(SwitchNotify, FormatsWhatHomeAssistantSubscribes) {
  const bool states[2] = {true, false};
  char buf[64];
  const int n = hcp::switchctl::FormatNotify(states, 2, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_STREQ(buf, "{\"switch_01\":\"on\",\"switch_02\":\"off\"}");
}

TEST(SwitchNotify, RoundTripsThroughTheParser) {
  /* The two halves of one contract: what this device reports must be readable by the
   * same code that reads what Home Assistant sends, or the UI and the relay drift
   * apart and each side blames the other. */
  const bool states[2] = {false, true};
  char buf[64];
  ASSERT_GT(hcp::switchctl::FormatNotify(states, 2, buf, sizeof(buf)), 0);

  SwitchCommand cmds[2] = {};
  ASSERT_EQ(Parse(buf, cmds, 2), 2);
  EXPECT_EQ(cmds[0].index, 1);
  EXPECT_FALSE(cmds[0].on);
  EXPECT_EQ(cmds[1].index, 2);
  EXPECT_TRUE(cmds[1].on);
}

TEST(SwitchNotify, RefusesATruncatedDocument) {
  const bool states[2] = {true, true};
  char buf[30];  // the document is 36 bytes: {"switch_01":"on","switch_02":"on"}
  EXPECT_EQ(hcp::switchctl::FormatNotify(states, 2, buf, sizeof(buf)), 0);
}

TEST(SwitchOnline, CarriesSwitchCountAsAnInteger) {
  /* Switches.handle_online: json.loads, then "switch_count" must be an int >= 2. */
  char buf[64];
  const int n = hcp::switchctl::FormatOnline(2, "2.0.3.14", buf, sizeof(buf));
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_STREQ(buf, "{\"switch_count\": 2, \"version\": \"2.0.3.14\"}");
  // quoted would be a string, and handle_online ignores a string count
  EXPECT_EQ(strstr(buf, "\"switch_count\": \"2\""), nullptr);
}

TEST(SwitchOnline, RefusesToClaimFewerThanTheMinimum) {
  /* Announcing a count below Home Assistant's minimum gets the payload rejected with
   * a warning, which is worse than not announcing at all: the device would look
   * online with no entities, on every connect. */
  char buf[64];
  EXPECT_EQ(hcp::switchctl::FormatOnline(1, "2.0.3.14", buf, sizeof(buf)), 0);
  EXPECT_EQ(hcp::switchctl::FormatOnline(0, "2.0.3.14", buf, sizeof(buf)), 0);
}
