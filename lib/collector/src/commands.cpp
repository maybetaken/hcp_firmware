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
};

const Entry kTable[] = {
    {"config", Command::kConfig},
    {"control/cmd", Command::kModbusWrite},
    {"control/restart", Command::kRestart},
    {"control/reconfig", Command::kReconfig},
    {"control/led", Command::kLed},
    {"host/heartbeat", Command::kHeartbeat},
    {"ota/start", Command::kOtaStart},
};

}  // namespace

Command Classify(const char* suffix, size_t len) {
  if (suffix == nullptr) {
    return Command::kNone;
  }
  for (const Entry& e : kTable) {
    const size_t want = strlen(e.suffix);
    if (want == len && memcmp(e.suffix, suffix, want) == 0) {
      return e.cmd;
    }
  }
  return Command::kNone;
}

}  // namespace collector
}  // namespace hcp
