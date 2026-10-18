/*
 * Helios Collector Platform (HCP) — P0 bring-up app.
 * Exercises hcp::kernel (Thread + MessageQueue + Timer + time), hcp::log, and
 * hcp::input on the Zephyr backend, logging over the console (USB-Serial-JTAG)
 * for the real-hardware ignition loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/kernel/kernel.h"
#include "hcp/log/log.h"
#include "hcp/input/input.h"

using namespace hcp::kernel;

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

static void OnButton(bool pressed, void* /*arg*/) {
  HCP_LOGI(hcp::log::kDomainDevice, "button %s",
           pressed ? "pressed" : "released");
}

int main(void) {
  HCP_LOGI(hcp::log::kDomainDevice, "*** HCP (Helios Collector Platform) P0 bring-up ***");
  HCP_LOGI(hcp::log::kDomainDevice, "build %s %s", __DATE__, __TIME__);

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

  // Bind the board user button (devicetree alias sw0) via hcp::input.
  if (!hcp::input::Button::Init(OnButton, nullptr)) {
    HCP_LOGW(hcp::log::kDomainDevice, "button init failed (no sw0?)");
  }

  // Continuous heartbeat so the ignition loop is observable at any time
  // after a reset.
  while (true) {
    HCP_LOGI(hcp::log::kDomainDevice, "alive, uptime %llu ms",
             static_cast<unsigned long long>(time::UptimeMs()));
    time::DelayMs(1000);
  }
  return 0;
}
