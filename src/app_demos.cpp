/*
 * HCP application - bring-up aids.
 *
 * Bench helpers, each behind its own CONFIG_* switch and none of them part of
 * what a shipped device does: poll a Modbus slave with no point table, echo
 * bytes through the RS-485/UART pins, join a network from Kconfig credentials.
 * The button binding is the fallback for a build without hcp::provision - with
 * provisioning on, that module owns the button (and the status LED).
 *
 * The P0 LED colour-cycle demo used to live here too. It is gone: what it proved
 * (the ws2812-spi path lights the strip) is exercised continuously by the status
 * light in hcp::provision, which is the LED logic the product actually has.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"

#include "hcp/input/input.h"
#include "hcp/kernel/kernel.h"
#include "hcp/log/log.h"

#if defined(CONFIG_HCP_BENCH) && defined(CONFIG_HCP_LED)
#include "hcp/led/strip.h"
#endif
#ifdef CONFIG_HCP_MODBUS_DEMO
#include "hcp/modbus/master.h"
#endif
#ifdef CONFIG_HCP_SERIAL_LOOPBACK_TEST
#include "hcp/serial/port.h"
#endif
#ifdef CONFIG_HCP_RTU_PROBE
#include "hcp/serial/port.h"
#endif
#ifdef CONFIG_HCP_NET_DEMO
#include "hcp/net/wifi.h"
#endif

namespace hcp {
namespace app {

using namespace hcp::kernel;

#ifndef CONFIG_HCP_PROVISION
void BringupAids::OnButton(bool pressed, void* /*arg*/) {
  HCP_LOGI(hcp::log::kDomainDevice, "button %s",
           pressed ? "pressed" : "released");
}
#endif

#if defined(CONFIG_HCP_BENCH) && defined(CONFIG_HCP_LED) && !defined(CONFIG_HCP_PROVISION)
// The strip and its SPI timing, made visible: four colours and off, 1.2 s apart.
// Lives in the bench image because that is the build where nothing else is using
// the LED - hcp::provision owns the strip in a product build and reflects the
// connection state through it instead.
void BringupAids::LedWorker(void* /*arg*/) {
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
#endif  // HCP_BENCH && HCP_LED && !HCP_PROVISION

#ifdef CONFIG_HCP_MODBUS_DEMO
// Bring-up aid: periodically poll a Modbus slave over uart1 to exercise the RTU
// path on real hardware. With no slave attached this logs a timeout (-ENOTCONN),
// which still proves the devicetree interface resolved, the uart was configured
// and the RS485 DE line toggles.
void BringupAids::ModbusWorker(void* /*arg*/) {
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

#ifdef CONFIG_HCP_SERIAL_LOOPBACK_TEST
// Raw TX->RX loopback self-test on the modbus UART (hcp-uart alias = uart1).
// Short the port's TX and RX pins to see echo_ok turn true. c6 TTL = GPIO22<->23.
void BringupAids::LoopbackWorker(void* /*arg*/) {
  hcp::serial::Port p;
  if (!p.Open(CONFIG_HCP_SERIAL_UART_BAUD)) {
    HCP_LOGE(hcp::log::kDomainModbus, "loopback: uart open failed");
    return;
  }
  static const uint8_t pat[] = {0x55, 0xAA, 'H', 'C', 'P', '1', 0x0D, 0x0A};
  HCP_LOGI(hcp::log::kDomainModbus,
           "loopback start: short TX<->RX (c6 TTL GPIO22<->GPIO23)");
  while (true) {
    // Discard any stale bytes, then write and try to read the echo back.
    uint8_t dummy;
    while (p.Read(&dummy, 1, 0) > 0) {
    }
    p.Write(pat, sizeof(pat));
    uint8_t rb[sizeof(pat)] = {};
    int got = p.Read(rb, sizeof(pat), 100);
    bool ok = (got == (int)sizeof(pat)) &&
              __builtin_memcmp(rb, pat, sizeof(pat)) == 0;
    HCP_LOGI(hcp::log::kDomainModbus, "loopback got=%d/%u echo_ok=%d", got,
             (unsigned)sizeof(pat), ok ? 1 : 0);
    time::DelayMs(1000);
  }
}
#endif  // CONFIG_HCP_SERIAL_LOOPBACK_TEST

#ifdef CONFIG_HCP_RTU_PROBE
namespace {

// Modbus CRC-16 (poly 0xA001, init 0xFFFF) - what the subsys computes too, written
// here because the probe has to build a request without the subsys involved.
uint16_t Crc16(const uint8_t* p, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) {
      c = (c & 1U) ? static_cast<uint16_t>((c >> 1) ^ 0xA001U)
                   : static_cast<uint16_t>(c >> 1);
    }
  }
  return c;
}

int Nibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

/* One request, and the raw bytes that came back.
 *
 * This exists because Zephyr's modbus_serial refuses a frame whose CRC does not
 * match *before* it logs anything: with a real slave on the bus the result is
 * "err=-5, nothing received shown" and no way forward. Printing the reply turns a
 * guess into a measurement - and it also separates "the meter answered something
 * unexpected" from "the bytes are corrupted on the wire", which no log level in
 * the subsys can tell apart. */
void BringupAids::RtuProbeWorker(void* /*arg*/) {
  hcp::serial::Port p;
  if (!p.Open(CONFIG_HCP_SERIAL_UART_BAUD)) {
    HCP_LOGE(hcp::log::kDomainModbus, "rtu probe: uart open failed");
    return;
  }

  static constexpr char kReq[] = CONFIG_HCP_RTU_PROBE_REQUEST;
  uint8_t frame[40] = {};
  size_t n = 0;
  for (size_t i = 0; kReq[2 * i] != '\0' && n < sizeof(frame) - 2; i++) {
    const int hi = Nibble(kReq[2 * i]);
    const int lo = Nibble(kReq[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      HCP_LOGE(hcp::log::kDomainModbus, "rtu probe: bad request hex");
      return;
    }
    frame[n++] = static_cast<uint8_t>((hi << 4) | lo);
  }
  const uint16_t tx_crc = Crc16(frame, n);
  frame[n++] = static_cast<uint8_t>(tx_crc & 0xFF);  // Modbus sends CRC low first
  frame[n++] = static_cast<uint8_t>(tx_crc >> 8);

  uint8_t rb[80] = {};
  HCP_LOGI(hcp::log::kDomainModbus, "rtu probe: %u baud, %u byte request, every 2 s",
           (unsigned)CONFIG_HCP_SERIAL_UART_BAUD, (unsigned)n);
  while (true) {
    uint8_t d = 0;
    while (p.Read(&d, 1, 0) > 0) {  // start from a clean fifo
    }
    const int64_t t_tx = k_uptime_get();
    p.Write(frame, n);
    /* What is sitting in the receive FIFO the moment our write call returns. On a
     * half-duplex line the master can hear its own request: Zephyr's modbus_serial
     * has a rx_fifo_drain() that is only ever called from an RX interrupt that is
     * disabled at that point - effectively dead code - so such bytes stay queued
     * and shift the reply by 8 bytes, which shows up as a CRC error on a reply
     * that is otherwise perfectly good. Log it rather than assume it. */
    int echo = 0;
    uint8_t eb = 0;
    while (p.Read(&eb, 1, 0) > 0 && echo < (int)n + 4) {
      echo++;
    }
    /* How long until the slave's first byte, and how much of it survives: a slave
     * that starts answering within a millisecond or two of our last stop bit is
     * exactly what a master that closes its receiver during TX and reopens it in
     * the TX-complete interrupt will clip - and a clipped head byte looks like a
     * CRC error, which is the symptom being chased. */
    const int one = p.Read(rb, 1, 800);
    const int64_t t_first = k_uptime_get();
    int got = one > 0 ? 1 : 0;
    if (got > 0) {
      /* Byte-by-byte, with the arrival time of each gap. An RTU frame is defined by
       * silence on the wire, so a slave that answers in two bursts separated by
       * more than the receiver's inter-character timeout is torn into two frames -
       * and the first half fails its CRC while a whole-byte read still looks
       * perfect. Only the timing shows it. */
      int64_t prev = t_first;
      char gaps[120] = {};
      int used = 0;
      while (got < (int)sizeof(rb)) {
        const int r = p.Read(rb + got, 1, 60);
        if (r <= 0) {
          break;
        }
        const int64_t now = k_uptime_get();
        if (now - prev > 2 && used < (int)sizeof(gaps) - 12) {
          used += snprintk(gaps + used, sizeof(gaps) - used, " +%lldms", (long long)(now - prev));
        }
        prev = now;
        got++;
      }
      if (used > 0) {
        HCP_LOGI(hcp::log::kDomainModbus, "gaps inside the reply:%s", gaps);
      }
    }

    char text[sizeof(rb) * 3 + 1] = {};
    int used = 0;
    for (int i = 0; i < got && used < (int)sizeof(text) - 4; i++) {
      used += snprintk(text + used, sizeof(text) - used, "%02X ", rb[i]);
    }
    bool crc_ok = false;
    if (got >= 4) {
      const uint16_t want = Crc16(rb, static_cast<size_t>(got - 2));
      crc_ok = rb[got - 2] == static_cast<uint8_t>(want & 0xFF) &&
               rb[got - 1] == static_cast<uint8_t>(want >> 8);
    }
    HCP_LOGI(hcp::log::kDomainModbus,
             "first byte after %lld ms, echoed-own-tx %d, got %d, crc %s: %s",
             static_cast<long long>(t_first - t_tx), echo, got,
             crc_ok ? "GOOD" : "bad", text);
    time::DelayMs(2000);
  }
}
#endif  // CONFIG_HCP_RTU_PROBE

#ifdef CONFIG_HCP_NET_DEMO
void BringupAids::OnWifi(hcp::net::WifiEvent e, void* /*user*/) {
  using E = hcp::net::WifiEvent;
  const char* s = (e == E::kConnected)       ? "connected (IP up)"
                  : (e == E::kConnecting)   ? "connecting"
                  : (e == E::kDisconnected) ? "disconnected"
                  : (e == E::kConnectFailed) ? "connect failed"
                                             : "?";
  HCP_LOGI(hcp::log::kDomainWifi, "wifi %s", s);
}

void BringupAids::NetWorker(void* /*arg*/) {
  hcp::net::wifi::SetCallback(OnWifi);
  if (!hcp::net::wifi::Init()) {
    HCP_LOGE(hcp::log::kDomainWifi, "wifi init failed");
    return;
  }
  hcp::net::WifiCredential cred;
  cred.ssid = CONFIG_HCP_NET_SSID;
  cred.psk = CONFIG_HCP_NET_PSK[0] ? CONFIG_HCP_NET_PSK : nullptr;
  while (true) {
    if (!hcp::net::wifi::IsConnected()) {
      HCP_LOGI(hcp::log::kDomainWifi, "connecting to \"%s\"", CONFIG_HCP_NET_SSID);
      hcp::net::wifi::Connect(cred);
    }
    time::DelayMs(3000);
  }
}
#endif  // CONFIG_HCP_NET_DEMO

void BringupAids::Start() {
  // With hcp::provision on, that module owns the button and the status LED; the
  // binding below is for bench builds that run without it.
#ifndef CONFIG_HCP_PROVISION
  if (!hcp::input::Button::Init(OnButton, nullptr)) {
    HCP_LOGW(hcp::log::kDomainDevice, "button init failed (no sw0?)");
  }
#endif

#if defined(CONFIG_HCP_BENCH) && defined(CONFIG_HCP_LED) && !defined(CONFIG_HCP_PROVISION)
  Thread led(LedWorker, nullptr, "led");
  led.Start();
#endif

#ifdef CONFIG_HCP_MODBUS_DEMO
  Thread mb(ModbusWorker, nullptr, "modbus");
  mb.Start();
#endif

#ifdef CONFIG_HCP_SERIAL_LOOPBACK_TEST
  Thread lb(LoopbackWorker, nullptr, "loopback");
  lb.Start();
#endif

#ifdef CONFIG_HCP_RTU_PROBE
  Thread probe(RtuProbeWorker, nullptr, "rtuprobe");
  probe.Start();
#endif

#ifdef CONFIG_HCP_NET_DEMO
  Thread net(NetWorker, nullptr, "net");
  net.Start();
#endif
}

}  // namespace app
}  // namespace hcp
