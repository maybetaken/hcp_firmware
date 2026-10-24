/*
 * The boot self-test: start a thread, push a few values through a message queue,
 * tick a timer, and then say "alive" once a second forever. It is the first
 * thing that runs on a board and the last thing still running when everything
 * else has failed, which is exactly why it stayed in the application after the
 * P0 phase: on a bench with no debugger, this is what answers "did it boot at
 * all".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"

#include "hcp/kernel/kernel.h"
#include "hcp/log/log.h"

using namespace hcp::kernel;

namespace hcp {
namespace app {
static MessageQueue<uint32_t> g_q(8);

static void ProducerWorker(void* /*arg*/) {
  for (uint32_t i = 1; i <= 5; ++i) {
    g_q.Send(i, 1000);
    time::DelayMs(100);
  }
  g_q.Send(0xFFFFFFFFu, 1000);  // sentinel
}

static void Heartbeat(void* /*arg*/) {
  HCP_LOGD(hcp::log::kDomainDevice, "tick");
}

void RunSelfTest() {
  Thread th(ProducerWorker, nullptr, "producer");
  if (!th.Start()) {
    HCP_LOGE(hcp::log::kDomainDevice, "failed to start producer");
  }

  Timer hb(Heartbeat);
  hb.StartPeriodic(1000);

  uint32_t v = 0;
  while (g_q.Receive(v, kInfiniteTimeoutMs)) {
    HCP_LOGI(hcp::log::kDomainDevice, "got %u", static_cast<unsigned>(v));
    if (v == 0xFFFFFFFFu) {
      break;
    }
  }

  th.Join();
  hb.Stop();
  HCP_LOGI(hcp::log::kDomainDevice, "bring-up done");
}

void AliveLoop() {
  while (true) {
    HCP_LOGI(hcp::log::kDomainDevice, "alive, uptime %llu ms",
             static_cast<unsigned long long>(time::UptimeMs()));
    time::DelayMs(1000);
  }
}

}  // namespace app
}  // namespace hcp
