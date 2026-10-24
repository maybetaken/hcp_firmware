/*
 * HCP application internals - the collaborators the App drives.
 *
 * Internal to src/: none of this is public API. The application is one hcp::App
 * instance (in main.cpp) that owns one instance of each of these three, and the
 * reason each of them is a class rather than a file of free functions differs:
 *
 *   PhoneProvisioner  real state with a lifecycle - the credentials a phone is
 *                     feeding us, the AP set accumulated across scan passes, the
 *                     timeout/success work items that must move together with the
 *                     "did we already tell the phone" flag. That coupling was 22
 *                     file-scope globals and a comment; now it is a type.
 *   Services          no state, but a sequence with a strict order (provisioning,
 *                     MQTT, SSDP, collector, OTA). Named object so the order has
 *                     one obvious home and the handlers are not file-statics any
 *                     build can accidentally call.
 *   BringupAids       bench-only threads and the button fallback for builds
 *                     without hcp::provision. Kept separate so the product path
 *                     never reads like the workshop.
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
#ifdef CONFIG_HCP_MODBUS_DEMO
  static void ModbusWorker(void* arg);
#endif
#ifdef CONFIG_HCP_SERIAL_LOOPBACK_TEST
  static void LoopbackWorker(void* arg);
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
#ifdef CONFIG_HCP_OTA
  // Adds "ota" to the periodic state document while an update has been tried.
  static int OtaStateExtra(char* buf, size_t cap);
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
  // One BluFi frame, so the list never has to be fragmented for the phone to
  // show it (see the note in OnScan about clients that do not reassemble).
  static constexpr int kMaxListBytes = 255;

  // Registers the Wi-Fi callback and the BluFi data handler, and arms the work
  // items. Called before the provisioning state machine starts, because that
  // machine drives the same reports through these hooks.
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
  void ConnectNow();          // runs on the system workqueue, never the BT RX thread
  void OnScanResult(const hcp::net::ScanResult* r);
  void OnProvTimeout();
  void OnSuccReportDue();
  const Ap* FindAp(const char* ssid) const;

  // ---- thunks: the APIs below take plain function pointers ----
  static void WifiEventThunk(hcp::net::WifiEvent e, void* user);
  static void ScanThunk(const hcp::net::ScanResult* r, void* user);
  static void DataThunk(uint8_t type, uint8_t subtype, const uint8_t* data,
                        uint16_t len, void* user);
  static void ProvWorkThunk(struct k_work* work);
  static void ProvTimeoutThunk(struct k_work* work);
  static void SuccWorkThunk(struct k_work* work);

  // ---- state that used to be 22 file-scope globals ----
  char ssid_[33];
  char pass_[65];

  Ap aps_[kMaxAps];
  int ap_count_ = 0;
  int scan_pass_ = 0;
  uint8_t wlist_[kMaxListBytes];
  uint16_t wlist_len_ = 0;

  // "which attempt is in flight" and "has the phone been told yet" are read by the
  // timeout handler to decide between a FAIL report and a late SUCCESS one.
  atomic_t prov_active_;
  atomic_t succ_sent_;

  struct k_sem disconn_sem_;
  struct k_work prov_work_;
  struct k_work_delayable prov_timeout_;
  struct k_work_delayable succ_work_;
};
#endif  // CONFIG_BLUFI

}  // namespace app
}  // namespace hcp
