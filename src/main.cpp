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
#ifdef CONFIG_HCP_SERIAL_LOOPBACK_TEST
#include "hcp/serial/port.h"
#endif
#ifdef CONFIG_HCP_NET
#include "hcp/net/wifi.h"
#endif
#ifdef CONFIG_BLUFI
#include <zephyr/kernel.h>
extern "C" {
#include "blufi/blufi.h"
}
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

#ifdef CONFIG_HCP_SERIAL_LOOPBACK_TEST
// Raw TX->RX loopback self-test on the modbus UART (hcp-uart alias = uart1).
// Short the port's TX and RX pins to see echo_ok turn true. c6 TTL = GPIO22<->23.
static void LoopbackWorker(void* /*arg*/) {
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

#ifdef CONFIG_HCP_NET_DEMO
static void OnWifi(hcp::net::WifiEvent e, void* /*user*/) {
  using E = hcp::net::WifiEvent;
  const char* s = (e == E::kConnected)     ? "connected (IP up)"
                  : (e == E::kConnecting)  ? "connecting"
                  : (e == E::kDisconnected) ? "disconnected"
                                            : "?";
  HCP_LOGI(hcp::log::kDomainWifi, "wifi %s", s);
}

static void NetWorker(void* /*arg*/) {
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

#ifdef CONFIG_BLUFI
#include <string.h>

static char g_ssid[33];
static char g_pass[64];

// esp_blufi WIFI_REP encodes station connection state (esp_blufi_api.h).
#define BLUFI_STA_CONN_SUCCESS 0x00
#define BLUFI_STA_CONN_FAIL    0x01
#define BLUFI_STA_CONNECTING   0x02
#define BLUFI_OPMODE_STA       0x01  // wifi_mode_t WIFI_MODE_STA

// Send a 3-byte WIFI_REP [opmode][sta_state][softap_conn_num] like esp.
static void blufi_report_wifi_state(uint8_t sta_state) {
  uint8_t rep[3] = {BLUFI_OPMODE_STA, sta_state, 0x00};
  blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_REP, rep, sizeof(rep));
}

// Remember scanned APs so the success report can carry the AP's BSSID + channel
// as esp's btc_blufi_send_wifi_conn_report extra_info (the official EspBlufi app
// relies on this; a bare 3-byte report is ignored by it).
struct ApRec {
  char ssid[33];
  uint8_t bssid[6];
  uint8_t channel;
};
#define BLUFI_MAX_APS 8
static ApRec g_aps[BLUFI_MAX_APS];
static int g_ap_count;

static const ApRec* find_ap(const char* ssid) {
  for (int i = 0; i < g_ap_count; i++) {
    if (strcmp(g_aps[i].ssid, ssid) == 0) return &g_aps[i];
  }
  return nullptr;
}

// CONN_SUCCESS report with esp-style extra_info TLVs: [opmode][state][softap]
// + BSSID(0x01,len=6) + SSID(0x02,len). Mirrors the ESP-IDF got-IP report.
static void blufi_report_wifi_success() {
  uint8_t rep[3 + 2 + 6 + 2 + 32];
  int n = 0;
  rep[n++] = BLUFI_OPMODE_STA;
  rep[n++] = BLUFI_STA_CONN_SUCCESS;
  rep[n++] = 0x00;  // softap_conn_num
  const ApRec* ap = find_ap(g_ssid);
  if (ap != nullptr) {
    rep[n++] = BLUFI_DATA_STA_BSSID;
    rep[n++] = 6;
    memcpy(&rep[n], ap->bssid, 6);
    n += 6;
    uint8_t slen = (uint8_t)strlen(ap->ssid);
    rep[n++] = BLUFI_DATA_STA_SSID;
    rep[n++] = slen;
    memcpy(&rep[n], ap->ssid, slen);
    n += slen;
  }
  HCP_LOGI(hcp::log::kDomainWifi, "blufi: report SUCCESS (%d bytes, bssid=%s)",
           n, ap ? "yes" : "no");
  blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_REP, rep, n);
}

// Wi-Fi result -> report the final BluFi conn state so the app confirms success
// instead of timing out waiting for it (then disconnects, reason 19).
static void on_wifi_for_blufi(hcp::net::WifiEvent e, void*) {
  using E = hcp::net::WifiEvent;
  if (e == E::kConnected) {
    blufi_report_wifi_success();
  }
}

// Provisioning connect runs on the system workqueue (NOT the BT RX thread:
// net_mgmt WIFI_CONNECT blocks up to `timeout`). It forcibly Disconnects first
// so re-provisioning works — the ESP32 driver returns -EALREADY whenever its
// state is already CONNECTING/CONNECTED, and IsConnected() (IP-based) can't see
// a lingering CONNECTING state. Final SUCCESS is reported via on_wifi_for_blufi.
static struct k_work g_prov_work;
static void prov_work_fn(struct k_work *work) {
  ARG_UNUSED(work);
  hcp::net::wifi::Disconnect();
  k_sleep(K_MSEC(300));
  hcp::net::WifiCredential c{g_ssid, g_pass[0] ? g_pass : nullptr};
  HCP_LOGI(hcp::log::kDomainWifi, "blufi: (re)connecting to \"%s\"", g_ssid);
  bool ok = hcp::net::wifi::Connect(c);
  if (!ok && !hcp::net::wifi::IsConnected()) {
    HCP_LOGW(hcp::log::kDomainWifi, "blufi: connect failed, report FAIL");
    blufi_report_wifi_state(BLUFI_STA_CONN_FAIL);
  }
}

// Accumulate a Wi-Fi scan into a BluFi WIFI_LIST payload.
static uint8_t g_wlist[512];
static uint16_t g_wlist_len;
static void on_scan(const hcp::net::ScanResult* r, void*) {
  if (r == nullptr) {
    HCP_LOGI(hcp::log::kDomainWifi, "blufi: sending wifi list (%u bytes)",
             (unsigned)g_wlist_len);
    blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_LIST, g_wlist, g_wlist_len);
    return;
  }
  uint8_t slen = (uint8_t)strlen(r->ssid);
  // esp btc_blufi_send_wifi_list record layout: [slen+1][rssi(int8)][ssid...]
  // i.e. the length byte counts ssid+rssi (rssi comes BEFORE the ssid).
  if (g_wlist_len + 2u + slen <= BLUFI_MAX_DATA_LEN) {
    g_wlist[g_wlist_len++] = slen + 1;          // length of (rssi + ssid)
    g_wlist[g_wlist_len++] = (uint8_t)(int8_t)r->rssi;
    memcpy(&g_wlist[g_wlist_len], r->ssid, slen);
    g_wlist_len += slen;
  }
  // Remember the AP (ssid -> bssid/channel) for the success conn report.
  if (g_ap_count < BLUFI_MAX_APS && slen < sizeof(g_aps[0].ssid)) {
    ApRec& a = g_aps[g_ap_count++];
    memcpy(a.ssid, r->ssid, slen);
    a.ssid[slen] = '\0';
    memcpy(a.bssid, r->bssid, 6);
    a.channel = r->channel;
  }
}

extern "C" void on_blufi_data(uint8_t type, uint8_t subtype,
                              const uint8_t *data, uint16_t len, void *user) {
  (void)user;
  if (type == BLUFI_TYPE_CTRL) {
    switch (subtype) {
      case BLUFI_CTRL_GET_VERSION: {
        // esp blufi replies exactly 2 bytes: [major, minor] (BTC_BLUFI_GREAT_VER=0x01,
        // BTC_BLUFI_SUB_VER=0x03). A longer/shorter payload makes the app reject it
        // ("receive device version error: 103").
        const uint8_t ver[2] = {0x01, 0x03};
        blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_REPLY_VERSION, ver, sizeof(ver));
        break;
      }
      case BLUFI_CTRL_GET_WIFI_LIST:
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: wifi scan requested");
        g_wlist_len = 0;
        g_ap_count = 0;
        hcp::net::wifi::Scan(on_scan, nullptr);
        break;
      case BLUFI_CTRL_CONN_TO_AP: {
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: connect to \"%s\"", g_ssid);
        // Match the ESP-IDF example: do NOT send an unsolicited CONNECTING report
        // here. Only the got-IP SUCCESS report (via on_wifi_for_blufi) is sent.
        // The WeChat mini-program doesn't expect a mid-write CONNECTING frame and
        // desyncs/misses the real result if we emit one.
        k_work_submit(&g_prov_work);  // connect off the BT RX thread
        break;
      }
      case BLUFI_CTRL_GET_WIFI_STATUS:
        // Some clients (incl. WeChat mini-programs) poll this instead of relying on
        // the spontaneous got-IP report -> answer with the current conn state,
        // exactly like the ESP-IDF example's ESP_BLUFI_EVENT_GET_WIFI_STATUS.
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: get wifi status");
        if (hcp::net::wifi::IsConnected()) {
          blufi_report_wifi_success();
        } else {
          blufi_report_wifi_state(BLUFI_STA_CONNECTING);
        }
        break;
      case BLUFI_CTRL_SET_WIFI_OPMODE:
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: set wifi opmode %u",
                 len ? data[0] : 0);
        break;
      default:
        HCP_LOGI(hcp::log::kDomainWifi, "blufi ctrl subtype=0x%02x len=%u",
                 subtype, (unsigned)len);
        break;
    }
  } else if (type == BLUFI_TYPE_DATA) {
    if (subtype == BLUFI_DATA_STA_SSID) {
      uint16_t n = len < sizeof(g_ssid) - 1 ? len : sizeof(g_ssid) - 1;
      memcpy(g_ssid, data, n);
      g_ssid[n] = '\0';
      HCP_LOGI(hcp::log::kDomainWifi, "blufi STA_SSID: %s", g_ssid);
    } else if (subtype == BLUFI_DATA_STA_PASSWD) {
      uint16_t n = len < sizeof(g_pass) - 1 ? len : sizeof(g_pass) - 1;
      memcpy(g_pass, data, n);
      g_pass[n] = '\0';
      HCP_LOGI(hcp::log::kDomainWifi, "blufi STA_PASSWD: (%u bytes)",
               (unsigned)len);
    } else {
      HCP_LOGI(hcp::log::kDomainWifi, "blufi data subtype=0x%02x len=%u", subtype,
               (unsigned)len);
    }
  }
}
#endif  // CONFIG_BLUFI

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

#ifdef CONFIG_HCP_SERIAL_LOOPBACK_TEST
  Thread lb(LoopbackWorker, nullptr, "loopback");
  lb.Start();
#endif

#ifdef CONFIG_HCP_NET_DEMO
  Thread net(NetWorker, nullptr, "net");
  net.Start();
#endif

#ifdef CONFIG_BLUFI
  hcp::log::SetLevel(hcp::log::kDomainWifi, hcp::log::kLevelDebug);
  hcp::net::wifi::SetCallback(on_wifi_for_blufi, nullptr);
  hcp::net::wifi::Init();
  k_work_init(&g_prov_work, prov_work_fn);
  blufi_set_callback(on_blufi_data, nullptr);
  if (blufi_start() == 0) {
    HCP_LOGI(hcp::log::kDomainWifi, "blufi advertising; waiting for phone app");
  } else {
    HCP_LOGE(hcp::log::kDomainWifi, "blufi start failed");
  }
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
