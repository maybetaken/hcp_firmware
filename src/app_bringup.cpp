/*
 * HCP boot liveness.
 *
 * This used to be the P0 self-test: a producer thread pushing 1..5 through a
 * message queue, a timer ticking, and a consumer logging each value - plus an
 * "alive, uptime" line every second. The queue/thread/timer paths it exercised
 * are now exercised daily by hcp::provision and hcp::collector, so the self-test
 * is gone: it cost six log lines on every boot to prove something that, if it
 * were broken, would stop the device from ever reaching the network.
 *
 * What remains is the liveness line at debug level. It is worth keeping because
 * it is the only output that continues when everything else has failed - but it
 * is not information anybody reads in a shipping device, and in immediate mode
 * each line costs milliseconds of the USB console. To see it, build with
 * -DCONFIG_HCP_LOG_LEVEL_DEVICE=4.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"

#include "hcp/kernel/kernel.h"
#include "hcp/log/log.h"

namespace hcp {
namespace app {

void AliveLoop() {
  while (true) {
    HCP_LOGD(hcp::log::kDomainDevice, "alive, uptime %llu ms",
             static_cast<unsigned long long>(kernel::time::UptimeMs()));
    kernel::time::DelayMs(1000);
  }
}

}  // namespace app
}  // namespace hcp
