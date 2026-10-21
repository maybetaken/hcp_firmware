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
#ifdef CONFIG_HCP_PROVISION
#include "hcp/provision/provision.h"
#endif
#ifdef CONFIG_HCP_MQTT
#include "hcp/mqtt/client.h"
#endif
#ifdef CONFIG_HCP_SSDP
#include "hcp/ssdp/discovery.h"
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

#ifndef CONFIG_HCP_PROVISION  // hcp::provision owns the button when enabled
static void OnButton(bool pressed, void* /*arg*/) {
  HCP_LOGI(hcp::log::kDomainDevice, "button %s",
           pressed ? "pressed" : "released");
}
#endif

#ifdef CONFIG_HCP_LED
// Optional addressable-LED demo. hcp::provision owns the LED when it is enabled
// (it reflects the connectivity state instead of cycling colors).
#ifndef CONFIG_HCP_PROVISION
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
#endif  // !CONFIG_HCP_PROVISION
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
  const char* s = (e == E::kConnected)      ? "connected (IP up)"
                  : (e == E::kConnecting)   ? "connecting"
                  : (e == E::kDisconnected) ? "disconnected"
                  : (e == E::kConnectFailed) ? "connect failed"
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

/* Signalled by the Wi-Fi disconnect event so a re-provision can wait for the
 * old connection to go down before issuing a new connect request. */
static K_SEM_DEFINE(g_disconn_sem, 0, 1);
static struct k_work_delayable g_prov_timeout;
static atomic_t g_prov_active;
/* Set once a SUCCESS report has actually gone out for the current provisioning
 * attempt; lets the timeout recover the case where the got-IP net-mgmt event was
 * dropped and the phone would otherwise hang below 100%. */
static atomic_t g_succ_sent;

// esp_blufi WIFI_REP encodes station connection state (esp_blufi_api.h).
#define BLUFI_STA_CONN_SUCCESS 0x00
#define BLUFI_STA_CONN_FAIL    0x01
#define BLUFI_STA_CONNECTING   0x02
#define BLUFI_OPMODE_STA       0x01  // wifi_mode_t WIFI_MODE_STA

// Send a 3-byte WIFI_REP [opmode][sta_state][softap_conn_num] like esp.
static void blufi_report_wifi_state(uint8_t sta_state) {
  if (!blufi_is_connected()) {
    return;  // no phone on the link: building a report would be pointless
  }
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
  int8_t rssi;
};
#define BLUFI_MAX_APS 32
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
  if (!blufi_is_connected()) {
    return;  // boot-time connect with provisioning closed: nothing to report
  }
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
  atomic_set(&g_succ_sent, 1);
  blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_REP, rep, n);
}

// Final failure report, including the ESP-compatible disconnect-reason TLV.
static void blufi_report_wifi_fail(uint8_t reason) {
  if (!blufi_is_connected()) {
    return;
  }
  uint8_t rep[6] = {BLUFI_OPMODE_STA, BLUFI_STA_CONN_FAIL, 0x00,
                    BLUFI_DATA_STA_CONN_END_REASON, 1, reason};
  HCP_LOGW(hcp::log::kDomainWifi, "blufi: report FAIL (reason=%u)", reason);
  blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_REP, rep, sizeof(rep));
}

/* The got-IP SUCCESS report is emitted from a work item instead of the net-mgmt
 * event thread.  It used to sleep 500 ms inline, which stalled *every* later
 * Wi-Fi/IP event (scan results included) for that whole delay.  Keeping the
 * delay but applying it before the notification preserves the original intent -
 * give the phone's BluFi stack time to finish the frame it is still processing,
 * otherwise the trailing SUCCESS can be missed - without blocking events.
 * If the link is already gone, blufi_send() returns -ENOTCONN. */
static void succ_report_fn(struct k_work*) {
  blufi_report_wifi_success();
}
static K_WORK_DELAYABLE_DEFINE(g_succ_work, succ_report_fn);

// If the driver never produces an association/DHCP result, still send a final
// FAIL so the phone UI does not sit in "connecting" forever.
static void prov_timeout_fn(struct k_work*) {
  if (!atomic_get(&g_prov_active)) {
    return;
  }
  atomic_set(&g_prov_active, 0);
  if (!hcp::net::wifi::IsConnected()) {
    HCP_LOGW(hcp::log::kDomainWifi, "blufi: provisioning timeout");
    blufi_report_wifi_fail(0x01);
  } else if (!atomic_get(&g_succ_sent)) {
    /* Connected but the phone was never told: the got-IP event was likely lost
     * (net-mgmt event queue overflow) - deliver the final report now so the
     * client does not sit at 85% while the link is actually up. */
    HCP_LOGW(hcp::log::kDomainWifi,
             "blufi: connected but no SUCCESS sent, recovering");
    blufi_report_wifi_success();
  }
}

// Wi-Fi result -> report the final BluFi conn state so the app confirms success
// instead of timing out waiting for it (then disconnects, reason 19).
static void on_wifi_for_blufi(hcp::net::WifiEvent e, void*) {
  using E = hcp::net::WifiEvent;
#ifdef CONFIG_HCP_PROVISION
  // The state machine drives retry / BLE fallback / LED from the same events.
  hcp::provision::NotifyWifiEvent(e);
#endif
  if (e == E::kConnected) {
    atomic_set(&g_prov_active, 0);
    k_work_cancel_delayable(&g_prov_timeout);
    k_work_reschedule(&g_succ_work, K_MSEC(500));
  } else if (e == E::kConnectFailed) {
#ifdef CONFIG_HCP_PROVISION
    /* The esp32 driver reports "dropped while CONNECTING" as an association
     * failure, which our own pre-connect Disconnect() also triggers. Do not tell
     * the phone it failed when we are the ones who just tore the link down. */
    if (hcp::provision::InOwnDisconnectWindow()) {
      HCP_LOGD(hcp::log::kDomainWifi, "blufi: self-inflicted drop, not reporting FAIL");
      return;
    }
#endif
    atomic_set(&g_prov_active, 0);
    k_work_cancel_delayable(&g_prov_timeout);
    blufi_report_wifi_fail(0x01);
  } else if (e == E::kDisconnected) {
    k_sem_give(&g_disconn_sem);
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
  /* Wait for the disconnect event instead of racing it: re-provisioning an
   * already-connected ESP32 with only a fixed 300 ms delay can leave the
   * driver in a transitional state where the next connect never completes.
   * Always done on the phone path (it can re-provision back-to-back while the
   * driver is still CONNECTING); unlike the boot path this is not latency
   * critical. */
  k_sem_reset(&g_disconn_sem);
#ifdef CONFIG_HCP_PROVISION
  hcp::provision::NoteOwnDisconnect();
#endif
  hcp::net::wifi::Disconnect();
  if (k_sem_take(&g_disconn_sem, K_SECONDS(1)) != 0) {
    HCP_LOGW(hcp::log::kDomainWifi,
             "blufi: disconnect wait timed out, connecting anyway");
  }
  hcp::net::WifiCredential c{g_ssid, g_pass[0] ? g_pass : nullptr};
  HCP_LOGI(hcp::log::kDomainWifi, "blufi: (re)connecting to \"%s\"", g_ssid);
  bool ok = hcp::net::wifi::Connect(c);
  if (!ok && !hcp::net::wifi::IsConnected()) {
    atomic_set(&g_prov_active, 0);
    k_work_cancel_delayable(&g_prov_timeout);
    HCP_LOGW(hcp::log::kDomainWifi, "blufi: connect failed, report FAIL");
    blufi_report_wifi_fail(0x01);
  }
}

// Accumulate a Wi-Fi scan into a BluFi WIFI_LIST payload.
//
// A single ESP32-C6 scan is a snapshot: weak APs flicker in and out between
// scans while the BLE link and Wi-Fi radio share the antenna.  If the first
// pass finds only a couple of APs (the common "why does it only show 2" case),
// run one more pass and union the results by BSSID before replying.
#define BLUFI_SCAN_PASSES 2
#define BLUFI_SCAN_MIN_APS 4
static uint8_t g_wlist[BLUFI_MAX_DATA_LEN];
static uint16_t g_wlist_len;
static int g_scan_pass;
static void on_scan(const hcp::net::ScanResult* r, void*) {
  if (r == nullptr) {
    HCP_LOGI(hcp::log::kDomainWifi, "blufi: scan pass %d found %d APs",
             g_scan_pass, g_ap_count);
    if (g_ap_count < BLUFI_SCAN_MIN_APS &&
        g_scan_pass + 1 < BLUFI_SCAN_PASSES) {
      g_scan_pass++;
      HCP_LOGI(hcp::log::kDomainWifi,
               "blufi: only %d APs, scanning again (pass %d/%d)", g_ap_count,
               g_scan_pass + 1, BLUFI_SCAN_PASSES);
      if (hcp::net::wifi::Scan(on_scan, nullptr)) {
        return;
      }
    }

    // esp_wifi hands its records strongest-first; our union over scan passes is
    // in arrival order, so sort here - the phone should see the usable APs first.
    for (int i = 1; i < g_ap_count; i++) {
      ApRec key = g_aps[i];
      int j = i - 1;
      while (j >= 0 && g_aps[j].rssi < key.rssi) {
        g_aps[j + 1] = g_aps[j];
        j--;
      }
      g_aps[j + 1] = key;
    }

    // Rebuild the BluFi list from the deduplicated union of all passes, keeping it
    // inside ONE BluFi frame (255 B). Fragmenting it is legal per the protocol, but
    // a client that does not reassemble inbound FC_FRAG shows a truncated list -
    // which reads as "the device only found a few networks".
    g_wlist_len = 0;
    for (int i = 0; i < g_ap_count; i++) {
      uint8_t slen = (uint8_t)strlen(g_aps[i].ssid);
      if (g_wlist_len + 2u + slen > sizeof(g_wlist)) {
        HCP_LOGI(hcp::log::kDomainWifi,
                 "blufi: list full after %d of %d APs", i, g_ap_count);
        break;
      }
      // esp btc_blufi_send_wifi_list record layout:
      // [slen+1][rssi(int8)][ssid...]
      g_wlist[g_wlist_len++] = slen + 1;  // length of (rssi + ssid)
      g_wlist[g_wlist_len++] = (uint8_t)(int8_t)g_aps[i].rssi;
      memcpy(&g_wlist[g_wlist_len], g_aps[i].ssid, slen);
      g_wlist_len += slen;
    }

    HCP_LOGI(hcp::log::kDomainWifi,
             "blufi: sending wifi list (%u bytes, %d APs)",
             (unsigned)g_wlist_len, g_ap_count);
    /* Per-AP detail is debug-level on purpose: with LOG_MODE_IMMEDIATE every line
     * costs several ms of 115200 UART on the net-mgmt event thread - the very
     * thread that is still delivering scan results, which then overflow the
     * event queue and silently lose APs. */
    for (int i = 0; i < g_ap_count; i++) {
      HCP_LOGD(hcp::log::kDomainWifi,
               "  ap[%d] \"%s\" rssi=%d ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
               i, g_aps[i].ssid, g_aps[i].rssi, g_aps[i].channel,
               g_aps[i].bssid[0], g_aps[i].bssid[1], g_aps[i].bssid[2],
               g_aps[i].bssid[3], g_aps[i].bssid[4], g_aps[i].bssid[5]);
    }
    blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_LIST, g_wlist, g_wlist_len);
    g_scan_pass = 0;
    return;
  }

  // Same BSSID seen in an earlier pass: keep the strongest RSSI.
  for (int i = 0; i < g_ap_count; i++) {
    if (memcmp(g_aps[i].bssid, r->bssid, 6) == 0) {
      if (r->rssi > g_aps[i].rssi) {
        g_aps[i].rssi = r->rssi;
      }
      return;
    }
  }

  uint8_t slen = (uint8_t)strlen(r->ssid);
  // Remember the AP (ssid -> bssid/channel) for the success conn report and
  // for the deduplicated Wi-Fi list.
  if (g_ap_count < BLUFI_MAX_APS && slen < sizeof(g_aps[0].ssid)) {
    ApRec& a = g_aps[g_ap_count++];
    memcpy(a.ssid, r->ssid, slen);
    a.ssid[slen] = '\0';
    memcpy(a.bssid, r->bssid, 6);
    a.channel = r->channel;
    a.rssi = r->rssi;
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
        g_scan_pass = 0;
        g_ap_count = 0;
        hcp::net::wifi::Scan(on_scan, nullptr);
        break;
      case BLUFI_CTRL_CONN_TO_AP: {
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: connect to \"%s\"", g_ssid);
        /* Match the ESP-IDF example's REQ_CONNECT_TO_AP path: it does NOT send
         * a report there.  Only got-IP sends CONN_SUCCESS (and GET_WIFI_STATUS
         * can return CONNECTING when polled).  Sending an unsolicited
         * CONNECTING frame mid-write desyncs the official WeChat
         * ESP-Config-WeChat mini-program: it shows "Connected: STA" at 85% and
         * then never processes the real SUCCESS report. */
        atomic_set(&g_prov_active, 1);
        atomic_set(&g_succ_sent, 0);
        k_work_reschedule(&g_prov_timeout, K_SECONDS(30));
#ifdef CONFIG_HCP_PROVISION
        /* Persist what the phone gave us and arm "close BLE after success"; the
         * connect below stays in this path so the BluFi reports stay accurate. */
        hcp::provision::NotifyPhoneCredentials(g_ssid, g_pass);
#endif
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
      case BLUFI_CTRL_DISCONN_FROM_AP:
        // ESP-IDF example: esp_wifi_disconnect() and nothing else - the driver's
        // own disconnect event ends the flow, no conn report is invented here.
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: disconnect from AP");
        atomic_set(&g_prov_active, 0);
        k_work_cancel_delayable(&g_prov_timeout);
        k_work_cancel_delayable(&g_succ_work);
        hcp::net::wifi::Disconnect();
        break;
      case BLUFI_CTRL_DISCONNECT_BLE:
        // ESP-IDF example closes the GATT link here; our disconnected() callback
        // resets per-link state and re-advertises.
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: phone asked to close BLE link");
        blufi_disconnect();
        break;
      case BLUFI_CTRL_DEAUTHENTICATE:
        // SoftAP-only command (kick a associated station). HCP runs STA-only
        // BluFi, and the ESP-IDF example leaves this unimplemented as well.
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: deauthenticate ignored (no softap)");
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
    } else if (subtype == BLUFI_DATA_CUSTOM_DATA) {
      // App-level channel: the ESP-IDF example only logs it (no echo), so we keep
      // the bytes for the application instead of inventing a reply frame.
      HCP_LOGI(hcp::log::kDomainWifi, "blufi custom data (%u bytes): %.*s",
               (unsigned)len, (int)(len > 32 ? 32 : len), (const char*)data);
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
#ifndef CONFIG_HCP_PROVISION
  if (!hcp::input::Button::Init(OnButton, nullptr)) {
    HCP_LOGW(hcp::log::kDomainDevice, "button init failed (no sw0?)");
  }
#endif

#ifdef CONFIG_HCP_LED
#ifndef CONFIG_HCP_PROVISION
  Thread led(LedWorker, nullptr, "led");
  led.Start();
#endif
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
  k_work_init_delayable(&g_prov_timeout, prov_timeout_fn);
  blufi_set_callback(on_blufi_data, nullptr);
#ifdef CONFIG_HCP_PROVISION
  /* The state machine decides: credentials in storage -> join the network with
   * BLE off, otherwise (or after retries fail) -> open BluFi. It also owns the
   * status LED and the button from here on. */
  hcp::provision::Start();
#ifdef CONFIG_HCP_MQTT
  /* Service session on top of the link: it brings the LED from violet to green
   * once the broker accepts us, and backs off while it does not. */
  hcp::mqtt::Start();
#endif
#ifdef CONFIG_HCP_SSDP
  /* Home Assistant announces its broker every 5 s on 239.255.255.250:1900; this
   * feeds hcp::mqtt so no broker address has to be compiled in. */
  hcp::ssdp::Start();
#endif
#else
  if (blufi_start() == 0) {
    HCP_LOGI(hcp::log::kDomainWifi, "blufi advertising; waiting for phone app");
  } else {
    HCP_LOGE(hcp::log::kDomainWifi, "blufi start failed");
  }
#endif
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
