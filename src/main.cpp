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
#ifdef CONFIG_HCP_LED
#include "hcp/led/strip.h"
#endif
#ifdef CONFIG_HCP_MODBUS_DEMO
#include "hcp/modbus/master.h"
#endif

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

#ifdef CONFIG_HCP_LED
// Optional addressable-LED demo; only built when CONFIG_HCP_LED is enabled AND
// the ws2812-spi overlay is applied (see lib/led/ws2812-spi.{conf,overlay}).
static void LedWorker(void* /*arg*/) {
  hcp::led::Strip s(1);
  if (!s.Open()) {
    HCP_LOGW(hcp::log::kDomainDevice,
             "led_strip open failed (apply ws2812-spi.overlay?)");
    return;
  }
  static const hcp::led::Color seq[] = {
      {40, 0, 0}, {0, 40, 0}, {0, 0, 40}, {20, 20, 0}, {0, 0, 0}};
  unsigned i = 0;
  while (true) {
    s.SetAll(seq[i]);
    s.Show();
    i = (i + 1) % (sizeof(seq) / sizeof(seq[0]));
    time::DelayMs(1200);
  }
}
#endif  // CONFIG_HCP_LED

#ifdef CONFIG_HCP_MODBUS_DEMO
// Bring-up aid: periodically poll a Modbus slave over uart1 to exercise the
// RTU path on real hardware. With no slave attached this logs a timeout,
// which still proves the uart/RS485/nmbs integration is wired up.
static void ModbusWorker(void* /*arg*/) {
  hcp::modbus::Master bus;
  hcp::modbus::Config cfg;
  cfg.baud = CONFIG_HCP_MODBUS_DEFAULT_BAUD;
  cfg.response_timeout_ms = 500;
  if (!bus.Open(cfg)) {
    HCP_LOGE(hcp::log::kDomainModbus, "modbus open failed");
    return;
  }
  HCP_LOGI(hcp::log::kDomainModbus, "modbus master polling slave %d",
           CONFIG_HCP_MODBUS_DEMO_SLAVE);
  while (true) {
    uint16_t regs[4] = {};
    bool ok = bus.ReadHoldingRegisters(CONFIG_HCP_MODBUS_DEMO_SLAVE, 0, 4, regs);
    if (ok) {
      HCP_LOGI(hcp::log::kDomainModbus, "regs %u %u %u %u", regs[0], regs[1],
               regs[2], regs[3]);
    } else {
      HCP_LOGW(hcp::log::kDomainModbus, "poll failed (err=%d)", bus.last_error());
    }
    time::DelayMs(2000);
  }
}
#endif  // CONFIG_HCP_MODBUS_DEMO

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

#ifdef CONFIG_HCP_LED
  Thread led(LedWorker, nullptr, "led");
  led.Start();
#endif

#ifdef CONFIG_HCP_MODBUS_DEMO
  Thread mb(ModbusWorker, nullptr, "modbus");
  mb.Start();
#endif

  // Continuous heartbeat so the ignition loop is observable at any time
  // after a reset.
  while (true) {
    HCP_LOGI(hcp::log::kDomainDevice, "alive, uptime %llu ms",
             static_cast<unsigned long long>(time::UptimeMs()));
    time::DelayMs(1000);
  }
  return 0;
}
