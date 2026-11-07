/*
 * HCP application internals - the collaborators the App drives. Internal to src/.
 *
 *   PhoneProvisioner  real state with a lifecycle: the credentials a phone is feeding us,
 *                     the AP set accumulated across scan passes, and the timeout/success
 *                     work items that must move together with the "already reported"
 *                     flags - coupling that was 22 file-scope globals and a comment.
 *   Services          no state, but a startup sequence with a strict order; the handlers
 *                     are members rather than file-statics.
 *   BringupAids       bench threads and the button fallback without hcp::provision, kept
 *                     separate so the product path does not read like the workshop.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include <zephyr/kernel.h>

#include "hcp/net/wifi.h"

namespace hcp {
namespace app {

/* --------------------------------------------------------------------------
 * BringupAids: bench helpers, each behind its own CONFIG_* switch.
 * -------------------------------------------------------------------------- */
class BringupAids {
 public:
  // Binds the button when hcp::provision is absent and starts whichever aids the
  // build turned on. Does nothing at all in a normal product build.
  void Start();

 private:
#ifndef CONFIG_HCP_PROVISION
  static void OnButton(bool pressed, void* arg);
#endif
#if defined(CONFIG_HCP_BENCH) && defined(CONFIG_HCP_LED) && !defined(CONFIG_HCP_PROVISION)
  // Colour cycle, to show the strip is alive. Bench image only: a shipped device
  // shows state through the LED, not a demo.
  static void LedWorker(void* arg);
#endif
#ifdef CONFIG_HCP_MODBUS_DEMO
  static void ModbusWorker(void* arg);
#endif
#ifdef CONFIG_HCP_SERIAL_LOOPBACK_TEST
  static void LoopbackWorker(void* arg);
#endif
#ifdef CONFIG_HCP_RTU_PROBE
  // Raw RTU request/reply with the bytes printed - for when a real slave answers and
  // Zephyr's subsys refuses the frame before logging it.
  static void RtuProbeWorker(void* arg);
#endif
#ifdef CONFIG_HCP_NET_DEMO
  static void NetWorker(void* arg);
  static void OnWifi(hcp::net::WifiEvent e, void* user);
#endif
};

/* --------------------------------------------------------------------------
 * Services: the running product, in the order it must come up.
 * -------------------------------------------------------------------------- */
class Services {
 public:
  // hcp::provision, then the MQTT session, SSDP discovery, the collector queue and
  // the OTA path. Called once from App::Start().
  void Start();

 private:
#ifdef CONFIG_HCP_MQTT
  // <sn>/cmd: Home Assistant's text commands (hcp::ota owns "ota").
  static void OnMqttCommand(const uint8_t* payload, size_t len, void* user);
#endif
#if defined(CONFIG_HCP_OTA) || defined(CONFIG_HCP_PROVISION)
  /* The diagnostics fields hcp::mqtt cannot know: "led" (the light entity reads its
   * state from there), the two frame/pattern counters behind it, and "ota" once an
   * update has been tried. */
  static int StateExtra(char* buf, size_t cap);
#endif
};

/* --------------------------------------------------------------------------
 * PhoneProvisioner: the no-provisioning-state-machine BluFi path.
 *
 * One instance, owned by App. Espressif's apps and the WeChat mini-program drive
 * this through BLE; it owns the connect attempt, the AP list shown to the phone
 * and the reports that tell the phone how it went.
 * -------------------------------------------------------------------------- */
#ifdef CONFIG_BLUFI
class PhoneProvisioner {
 public:
  static constexpr int kMaxAps = 32;
  // One BluFi frame, so the phone shows the list without reassembling (see OnScan).
  static constexpr int kMaxListBytes = 255;
  /*
   * Association attempts the phone-driven connect gets before the phone is
   * told it failed. The official WeChat mini-program (ESP-Config-WeChat,
   * pages/blueConnect/blueConnect.js) can only render ONE report per
   * provisioning run: its UI moves to 85% on the first WIFI_REP notification
   * and to 100% only if that first report's second byte is 0x00 (SUCCESS).
   * A FAIL report freezes it at 85% until its own 20 s timeout - and because
   * it never clears its accumulated byte array between notifications, no
   * later SUCCESS can reach the byte position it checks either. So every
   * intermediate failure MUST be retried silently, and exactly one final
   * report (SUCCESS, or the FAIL after the last attempt) must be sent.
   */
  static constexpr int kPhoneConnAttempts = 3;

  // Registers the Wi-Fi callback and the BluFi data handler and arms the work items.
  // Called before the provisioning state machine starts, because that machine drives
  // the same reports through these hooks.
  void Init();

  // Nothing else in this build owns provisioning: advertise and wait for a phone.
  void StartStandalone();

 private:
  struct Ap {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
  };

  // ---- logic, all instance methods ----
  void OnWifiEvent(hcp::net::WifiEvent e);
  void OnData(uint8_t type, uint8_t subtype, const uint8_t* data, uint16_t len);
  void ReportState(uint8_t sta_state);
  void ReportSuccess();
  void ReportFail(uint8_t reason);
  void ConnectNow();          // on the system workqueue, never the BT RX thread
  void OnScanResult(const hcp::net::ScanResult* r);
  void OnProvTimeout();
  void OnSuccReportDue();
  // One phone-driven association attempt failed: retry it silently while the
  // budget lasts, and only tell the phone about the final failure (see
  // kPhoneConnAttempts for why an intermediate FAIL report is fatal to the
  // WeChat mini-program's UI). Returns true when a retry was scheduled.
  bool FailOrRetry();
  const Ap* FindAp(const char* ssid) const;

  // ---- thunks: the APIs below take plain function pointers ----
  static void WifiEventThunk(hcp::net::WifiEvent e, void* user);
  static void ScanThunk(const hcp::net::ScanResult* r, void* user);
  static void DataThunk(uint8_t type, uint8_t subtype, const uint8_t* data,
                        uint16_t len, void* user);
  static void ProvWorkThunk(struct k_work* work);
  static void ProvTimeoutThunk(struct k_work* work);
  static void SuccWorkThunk(struct k_work* work);
  static void RetryThunk(struct k_work* work);

  // ---- state ----
  char ssid_[33];
  char pass_[65];

  Ap aps_[kMaxAps];
  int ap_count_ = 0;
  int scan_pass_ = 0;
  uint8_t wlist_[kMaxListBytes];
  uint16_t wlist_len_ = 0;

  /* Which attempt is in flight, and whether the phone has been told: the timeout
   * handler reads these to choose a FAIL report over a late SUCCESS one. */
  atomic_t prov_active_;
  atomic_t succ_sent_;
  // Association attempts issued for the current phone-driven connect (see
  // kPhoneConnAttempts).
  int phone_attempts_ = 0;
  // True once any Wi-Fi driver event has been seen (or a connect issued):
  // until then the pre-connect teardown dance in ConnectNow() is skipped,
  // saving its full 1 s wait on a fresh device.
  bool wifi_was_active_ = false;

  struct k_sem disconn_sem_;
  struct k_work prov_work_;
  struct k_work_delayable prov_timeout_;
  struct k_work_delayable succ_work_;
  struct k_work_delayable retry_work_;
};
#endif  // CONFIG_BLUFI

}  // namespace app
}  // namespace hcp
