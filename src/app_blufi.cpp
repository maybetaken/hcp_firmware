/*
 * HCP application - the BluFi provisioning path (PhoneProvisioner): the phone is on the
 * BLE link, this receives credentials, runs the Wi-Fi connect and reports back in the
 * exact shapes the Espressif apps and the WeChat mini-program expect. Every odd-looking
 * delay, order and report format in here is a measurement of what those clients do on the
 * wire - do not "simplify" one without re-testing against both.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"

#ifdef CONFIG_BLUFI

#include <string.h>

extern "C" {
#include "blufi/blufi.h"
}

#include "hcp/log/log.h"
#include "hcp/net/wifi.h"
#ifdef CONFIG_HCP_PROVISION
#include "hcp/provision/provision.h"
#endif

namespace hcp {
namespace app {
namespace {

// esp_blufi WIFI_REP encodes station connection state (esp_blufi_api.h).
constexpr uint8_t kStaConnSuccess = 0x00;
constexpr uint8_t kStaConnFail = 0x01;
constexpr uint8_t kStaConnecting = 0x02;
constexpr uint8_t kOpModeSta = 0x01;  // wifi_mode_t WIFI_MODE_STA

/* A single C6 scan is a snapshot: weak APs flicker in and out while the BLE link
 * and the Wi-Fi radio share the antenna. If the first pass finds only a couple of
 * APs (the common "why does it only show 2" case), run one more and union the
 * results by BSSID before replying. */
constexpr int kScanPasses = 2;
constexpr int kScanMinAps = 4;

/* The Wi-Fi APIs, the BluFi transport and Zephyr's work items take plain function
 * pointers, so the handlers are static thunks on the one PhoneProvisioner instance App
 * owns - a single pointer, rather than container_of across three work items for no gain. */
PhoneProvisioner* s_self;

}  // namespace

/* ------------------------------------------------------------------ reports -- */

void PhoneProvisioner::ReportState(uint8_t sta_state) {
  if (!blufi_is_connected()) {
    return;  // no phone on the link: building a report would be pointless
  }
  uint8_t rep[3] = {kOpModeSta, sta_state, 0x00};
  blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_REP, rep, sizeof(rep));
}

// The list of APs to show the phone is rebuilt inside one BluFi frame; the
// scan that produced it is unioned across passes (see OnScanResult).
void PhoneProvisioner::ReportSuccess() {
  uint8_t rep[3 + 2 + 6 + 2 + 32];
  if (!blufi_is_connected()) {
    return;  // boot-time connect with provisioning closed: nothing to report
  }
  int n = 0;
  rep[n++] = kOpModeSta;
  rep[n++] = kStaConnSuccess;
  rep[n++] = 0x00;  // softap_conn_num
  const Ap* ap = FindAp(ssid_);
  if (ap != nullptr) {
    // esp-style extra_info TLVs: BSSID(0x01,len=6) then SSID(0x02,len). A bare
    // 3-byte report is ignored by the official EspBlufi app.
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
  atomic_set(&succ_sent_, 1);
  blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_REP, rep, n);
}

// Final failure report, including the ESP-compatible disconnect-reason TLV.
void PhoneProvisioner::ReportFail(uint8_t reason) {
  if (!blufi_is_connected()) {
    return;
  }
  uint8_t rep[6] = {kOpModeSta, kStaConnFail, 0x00, BLUFI_DATA_STA_CONN_END_REASON,
                    1, reason};
  HCP_LOGW(hcp::log::kDomainWifi, "blufi: report FAIL (reason=%u)", reason);
  blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_REP, rep, sizeof(rep));
}

const PhoneProvisioner::Ap* PhoneProvisioner::FindAp(const char* ssid) const {
  for (int i = 0; i < ap_count_; i++) {
    if (strcmp(aps_[i].ssid, ssid) == 0) {
      return &aps_[i];
    }
  }
  return nullptr;
}

/* ------------------------------------------------------------------- events -- */

/* The got-IP SUCCESS report goes out from a work item, not the net-mgmt event thread:
 * sleeping 500 ms inline stalled every later Wi-Fi/IP event, scan results included. The
 * delay stays - the phone's BluFi stack needs time to finish the frame it is processing,
 * or the trailing SUCCESS is missed - it just no longer blocks events. If the link is
 * gone, blufi_send() returns -ENOTCONN. */
void PhoneProvisioner::OnSuccReportDue() { ReportSuccess(); }

// If the driver never produces an association/DHCP result, still send a final FAIL
// so the phone UI does not sit in "connecting" forever.
void PhoneProvisioner::OnProvTimeout() {
  if (!atomic_get(&prov_active_)) {
    return;
  }
  atomic_set(&prov_active_, 0);
  if (!hcp::net::wifi::IsConnected()) {
    HCP_LOGW(hcp::log::kDomainWifi, "blufi: provisioning timeout");
    ReportFail(0x01);
  } else if (!atomic_get(&succ_sent_)) {
    /* Connected but the phone was never told: the got-IP event was likely lost
     * (net-mgmt event queue overflow) - deliver the final report now so the
     * client does not sit at 85% while the link is actually up. */
    HCP_LOGW(hcp::log::kDomainWifi,
             "blufi: connected but no SUCCESS sent, recovering");
    ReportSuccess();
  }
}

// Wi-Fi result -> report the final BluFi conn state so the app confirms success
// instead of timing out waiting for it (then disconnects, reason 19).
void PhoneProvisioner::OnWifiEvent(hcp::net::WifiEvent e) {
  using E = hcp::net::WifiEvent;
#ifdef CONFIG_HCP_PROVISION
  // The state machine drives retry / BLE fallback / LED from the same events.
  hcp::provision::NotifyWifiEvent(e);
#endif
  if (e == E::kConnected) {
    atomic_set(&prov_active_, 0);
    k_work_cancel_delayable(&prov_timeout_);
    k_work_reschedule(&succ_work_, K_MSEC(500));
  } else if (e == E::kConnectFailed) {
#ifdef CONFIG_HCP_PROVISION
    /* The esp32 driver reports "dropped while CONNECTING" as an association
     * failure, which our own pre-connect Disconnect() also triggers. Do not tell
     * the phone it failed when we are the ones who just tore the link down. */
    if (hcp::provision::InOwnDisconnectWindow()) {
      HCP_LOGD(hcp::log::kDomainWifi,
               "blufi: self-inflicted drop, not reporting FAIL");
      return;
    }
#endif
    /* Not a straight FAIL report anymore: the WeChat mini-program locks its UI
     * at 85% on any report that is not the final SUCCESS (see
     * kPhoneConnAttempts), so a failed attempt is retried silently and only
     * the exhaustion of the budget is reported. */
    FailOrRetry();
  } else if (e == E::kDisconnected) {
    k_sem_give(&disconn_sem_);
  }
}

// Provisioning connect runs on the system workqueue (NOT the BT RX thread: net_mgmt
// WIFI_CONNECT blocks up to `timeout`). It disconnects first so re-provisioning works:
// the ESP32 driver returns -EALREADY while CONNECTING/CONNECTED, and the IP-based
// IsConnected() cannot see a lingering CONNECTING. SUCCESS is reported via OnWifiEvent.
void PhoneProvisioner::ConnectNow() {
  /* Wait for the disconnect event rather than racing it: re-provisioning with only a
   * fixed delay can leave the driver in a state where the next connect never completes.
   * The phone path can re-provision back-to-back while still CONNECTING; unlike the boot
   * path, this is not latency critical. */
  k_sem_reset(&disconn_sem_);
#ifdef CONFIG_HCP_PROVISION
  hcp::provision::NoteOwnDisconnect();
#endif
  hcp::net::wifi::Disconnect();
  if (k_sem_take(&disconn_sem_, K_SECONDS(1)) != 0) {
    HCP_LOGW(hcp::log::kDomainWifi,
             "blufi: disconnect wait timed out, connecting anyway");
  }
  hcp::net::WifiCredential c{ssid_, pass_[0] ? pass_ : nullptr};
  HCP_LOGI(hcp::log::kDomainWifi, "blufi: (re)connecting to \"%s\" (attempt %d/%d)",
           ssid_, phone_attempts_, kPhoneConnAttempts);
  phone_attempts_++;
  bool ok = hcp::net::wifi::Connect(c);
  if (!ok && !hcp::net::wifi::IsConnected()) {
    /* The request itself was refused (EALREADY while CONNECTING, a disconnect
     * still settling...): same policy as an association failure - retry, and
     * only report the final one. */
    FailOrRetry();
  }
}

// One phone-driven association attempt failed (the driver reported a failure or
// the connect request was refused). Retry silently while the budget lasts;
// tell the phone only once, at the end.
void PhoneProvisioner::FailOrRetry() {
  if (!atomic_get(&prov_active_)) {
    return;  // not a phone-driven attempt in flight: nothing to report
  }
  if (phone_attempts_ < kPhoneConnAttempts) {
    HCP_LOGW(hcp::log::kDomainWifi,
             "blufi: attempt %d/%d failed; retrying without telling the phone",
             phone_attempts_, kPhoneConnAttempts);
    k_work_reschedule(&retry_work_, K_MSEC(300));
    return;
  }
  atomic_set(&prov_active_, 0);
  k_work_cancel_delayable(&prov_timeout_);
  HCP_LOGW(hcp::log::kDomainWifi,
           "blufi: all %d attempts failed, reporting the final FAIL",
           kPhoneConnAttempts);
  ReportFail(0x01);
}

// Accumulate a Wi-Fi scan into a BluFi WIFI_LIST payload.
void PhoneProvisioner::OnScanResult(const hcp::net::ScanResult* r) {
  if (r == nullptr) {
    HCP_LOGI(hcp::log::kDomainWifi, "blufi: scan pass %d found %d APs", scan_pass_,
             ap_count_);
    if (ap_count_ < kScanMinAps && scan_pass_ + 1 < kScanPasses) {
      scan_pass_++;
      HCP_LOGI(hcp::log::kDomainWifi,
               "blufi: only %d APs, scanning again (pass %d/%d)", ap_count_,
               scan_pass_ + 1, kScanPasses);
      if (hcp::net::wifi::Scan(ScanThunk, this)) {
        return;
      }
    }

    // esp_wifi hands its records strongest-first; our union over scan passes is in
    // arrival order, so sort here - the phone should see the usable APs first.
    for (int i = 1; i < ap_count_; i++) {
      Ap key = aps_[i];
      int j = i - 1;
      while (j >= 0 && aps_[j].rssi < key.rssi) {
        aps_[j + 1] = aps_[j];
        j--;
      }
      aps_[j + 1] = key;
    }

    /* Rebuild the BluFi list from the deduplicated union of all passes, inside ONE BluFi
     * frame. Fragmenting it is legal per the protocol, but a client that does not
     * reassemble inbound FC_FRAG shows a truncated list - which reads as "the device
     * only found a few networks". */
    wlist_len_ = 0;
    for (int i = 0; i < ap_count_; i++) {
      uint8_t slen = (uint8_t)strlen(aps_[i].ssid);
      if (wlist_len_ + 2u + slen > sizeof(wlist_)) {
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: list full after %d of %d APs", i,
                 ap_count_);
        break;
      }
      // esp btc_blufi_send_wifi_list record layout:
      // [slen+1][rssi(int8)][ssid...]
      wlist_[wlist_len_++] = slen + 1;  // length of (rssi + ssid)
      wlist_[wlist_len_++] = (uint8_t)(int8_t)aps_[i].rssi;
      memcpy(&wlist_[wlist_len_], aps_[i].ssid, slen);
      wlist_len_ += slen;
    }

    HCP_LOGI(hcp::log::kDomainWifi, "blufi: sending wifi list (%u bytes, %d APs)",
             (unsigned)wlist_len_, ap_count_);
    /* Per-AP detail is debug-level on purpose: with LOG_MODE_IMMEDIATE each line costs
     * several ms of 115200 UART on the net-mgmt event thread - the thread still
     * delivering scan results, which then overflows the queue and silently loses APs. */
    for (int i = 0; i < ap_count_; i++) {
      HCP_LOGD(hcp::log::kDomainWifi,
               "  ap[%d] \"%s\" rssi=%d ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
               i, aps_[i].ssid, aps_[i].rssi, aps_[i].channel, aps_[i].bssid[0],
               aps_[i].bssid[1], aps_[i].bssid[2], aps_[i].bssid[3],
               aps_[i].bssid[4], aps_[i].bssid[5]);
    }
    blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_LIST, wlist_, wlist_len_);
    scan_pass_ = 0;
    return;
  }

  // Same BSSID seen in an earlier pass: keep the strongest RSSI.
  for (int i = 0; i < ap_count_; i++) {
    if (memcmp(aps_[i].bssid, r->bssid, 6) == 0) {
      if (r->rssi > aps_[i].rssi) {
        aps_[i].rssi = r->rssi;
      }
      return;
    }
  }

  uint8_t slen = (uint8_t)strlen(r->ssid);
  // Remember the AP (ssid -> bssid/channel) for the success conn report and for
  // the deduplicated Wi-Fi list.
  if (ap_count_ < kMaxAps && slen < sizeof(aps_[0].ssid)) {
    Ap& a = aps_[ap_count_++];
    memcpy(a.ssid, r->ssid, slen);
    a.ssid[slen] = '\0';
    memcpy(a.bssid, r->bssid, 6);
    a.channel = r->channel;
    a.rssi = r->rssi;
  }
}

void PhoneProvisioner::OnData(uint8_t type, uint8_t subtype, const uint8_t* data,
                              uint16_t len) {
  if (type == BLUFI_TYPE_CTRL) {
    switch (subtype) {
      case BLUFI_CTRL_GET_VERSION: {
        // esp blufi replies exactly 2 bytes: [major, minor] (BTC_BLUFI_GREAT_VER=
        // 0x01, BTC_BLUFI_SUB_VER=0x03). A longer/shorter payload makes the app
        // reject it ("receive device version error: 103").
        const uint8_t ver[2] = {0x01, 0x03};
        blufi_send(BLUFI_TYPE_DATA, BLUFI_DATA_REPLY_VERSION, ver, sizeof(ver));
        break;
      }
      case BLUFI_CTRL_GET_WIFI_LIST:
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: wifi scan requested");
        wlist_len_ = 0;
        scan_pass_ = 0;
        ap_count_ = 0;
        hcp::net::wifi::Scan(ScanThunk, this);
        break;
      case BLUFI_CTRL_CONN_TO_AP: {
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: connect to \"%s\"", ssid_);
        /* Match the ESP-IDF example's REQ_CONNECT_TO_AP path: it does NOT send a
         * report there. Only got-IP sends CONN_SUCCESS (and GET_WIFI_STATUS can
         * return CONNECTING when polled). Sending an unsolicited CONNECTING frame
         * mid-write desyncs the official WeChat ESP-Config-WeChat mini-program: it
         * shows "Connected: STA" at 85% and then never processes the real SUCCESS
         * report. */
        atomic_set(&prov_active_, 1);
        atomic_set(&succ_sent_, 0);
        phone_attempts_ = 0;  // a fresh request cancels any retry in flight
        k_work_cancel_delayable(&retry_work_);
        k_work_reschedule(&prov_timeout_, K_SECONDS(30));
#ifdef CONFIG_HCP_PROVISION
        /* Persist what the phone gave us and arm "close BLE after success"; the
         * connect below stays in this path so the BluFi reports stay accurate. */
        hcp::provision::NotifyPhoneCredentials(ssid_, pass_);
#endif
        k_work_submit(&prov_work_);  // connect off the BT RX thread
        break;
      }
      case BLUFI_CTRL_GET_WIFI_STATUS:
        // Some clients (incl. WeChat mini-programs) poll this instead of relying on
        // the spontaneous got-IP report -> answer with the current conn state,
        // exactly like the ESP-IDF example's ESP_BLUFI_EVENT_GET_WIFI_STATUS.
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: get wifi status");
        if (hcp::net::wifi::IsConnected()) {
          ReportSuccess();
        } else {
          ReportState(kStaConnecting);
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
        atomic_set(&prov_active_, 0);
        k_work_cancel_delayable(&prov_timeout_);
        k_work_cancel_delayable(&succ_work_);
        hcp::net::wifi::Disconnect();
        break;
      case BLUFI_CTRL_DISCONNECT_BLE:
        // ESP-IDF example closes the GATT link here; our disconnected() callback
        // resets per-link state and re-advertises.
        HCP_LOGI(hcp::log::kDomainWifi, "blufi: phone asked to close BLE link");
        blufi_disconnect();
        break;
      case BLUFI_CTRL_DEAUTHENTICATE:
        // SoftAP-only command (kick an associated station). HCP runs STA-only
        // BluFi, and the ESP-IDF example leaves this unimplemented as well.
        HCP_LOGI(hcp::log::kDomainWifi,
                 "blufi: deauthenticate ignored (no softap)");
        break;
      default:
        HCP_LOGI(hcp::log::kDomainWifi, "blufi ctrl subtype=0x%02x len=%u",
                 subtype, (unsigned)len);
        break;
    }
  } else if (type == BLUFI_TYPE_DATA) {
    if (subtype == BLUFI_DATA_STA_SSID) {
      uint16_t n = len < sizeof(ssid_) - 1 ? len : sizeof(ssid_) - 1;
      memcpy(ssid_, data, n);
      ssid_[n] = '\0';
      HCP_LOGI(hcp::log::kDomainWifi, "blufi STA_SSID: %s", ssid_);
    } else if (subtype == BLUFI_DATA_STA_PASSWD) {
      uint16_t n = len < sizeof(pass_) - 1 ? len : sizeof(pass_) - 1;
      memcpy(pass_, data, n);
      pass_[n] = '\0';
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

/* ------------------------------------------------------------------- thunks -- */

void PhoneProvisioner::WifiEventThunk(hcp::net::WifiEvent e, void* user) {
  static_cast<PhoneProvisioner*>(user)->OnWifiEvent(e);
}

void PhoneProvisioner::ScanThunk(const hcp::net::ScanResult* r, void* user) {
  static_cast<PhoneProvisioner*>(user)->OnScanResult(r);
}

void PhoneProvisioner::DataThunk(uint8_t type, uint8_t subtype,
                                 const uint8_t* data, uint16_t len, void* user) {
  static_cast<PhoneProvisioner*>(user)->OnData(type, subtype, data, len);
}

void PhoneProvisioner::ProvWorkThunk(struct k_work* /*work*/) {
  s_self->ConnectNow();
}

void PhoneProvisioner::ProvTimeoutThunk(struct k_work* /*work*/) {
  s_self->OnProvTimeout();
}

void PhoneProvisioner::SuccWorkThunk(struct k_work* /*work*/) {
  s_self->OnSuccReportDue();
}

void PhoneProvisioner::RetryThunk(struct k_work* /*work*/) {
  s_self->ConnectNow();
}

/* -------------------------------------------------------------- entry points -- */

void PhoneProvisioner::Init() {
  s_self = this;
  atomic_set(&prov_active_, 0);
  atomic_set(&succ_sent_, 0);
  k_sem_init(&disconn_sem_, 0, 1);
  k_work_init(&prov_work_, ProvWorkThunk);
  k_work_init_delayable(&prov_timeout_, ProvTimeoutThunk);
  k_work_init_delayable(&succ_work_, SuccWorkThunk);
  k_work_init_delayable(&retry_work_, RetryThunk);

  hcp::log::SetLevel(hcp::log::kDomainWifi, hcp::log::kLevelDebug);
  hcp::net::wifi::SetCallback(WifiEventThunk, this);
  hcp::net::wifi::Init();
  blufi_set_callback(DataThunk, this);
}

void PhoneProvisioner::StartStandalone() {
  if (blufi_start() == 0) {
    HCP_LOGI(hcp::log::kDomainWifi, "blufi advertising; waiting for phone app");
  } else {
    HCP_LOGE(hcp::log::kDomainWifi, "blufi start failed");
  }
}

}  // namespace app
}  // namespace hcp

#endif  // CONFIG_BLUFI
