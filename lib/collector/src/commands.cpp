/*
 * hcp::collector - the topic-to-command mapping, as data.
 *
 * These suffixes are what Home Assistant publishes, taken from the plugin modules
 * under custom_components/solar_manager/plugins (each calls
 * self._build_topic("<suffix>")). Keeping them here, in one table, is what lets
 * them be tested: the alternative is a switch inside a thread loop, where a typo
 * shows up as "nothing happens" on a device across the room.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/collector/collector.h"

#include <string.h>

namespace hcp {
namespace collector {
namespace {

struct Entry {
  const char* suffix;
  Command cmd;
  mqtt::Tree tree;
};

const Entry kTable[] = {
    {"config", Command::kConfig, mqtt::Tree::kMain},
    {"control/cmd", Command::kModbusWrite, mqtt::Tree::kMain},
    {"control/restart", Command::kRestart, mqtt::Tree::kMain},
    {"control/reconfig", Command::kReconfig, mqtt::Tree::kMain},
    {"control/led", Command::kLed, mqtt::Tree::kMain},
    {"host/heartbeat", Command::kHeartbeat, mqtt::Tree::kMain},
    {"ota/start", Command::kOtaStart, mqtt::Tree::kMain},
    /* The relay device's own tree. Only this one suffix is claimed: Home Assistant
     * publishes nothing else here, and the previous firmware subscribed exactly
     * "<sn>-K/control/cmd" (main/solar_manager.cpp:206). */
    {"control/cmd", Command::kSwitchWrite, mqtt::Tree::kSwitch},
};

}  // namespace

Command Classify(const char* suffix, size_t len, mqtt::Tree tree) {
  if (suffix == nullptr) {
    return Command::kNone;
  }
  for (const Entry& e : kTable) {
    const size_t want = strlen(e.suffix);
    if (e.tree == tree && want == len && memcmp(e.suffix, suffix, want) == 0) {
      return e.cmd;
    }
  }
  return Command::kNone;
}

}  // namespace collector
}  // namespace hcp
