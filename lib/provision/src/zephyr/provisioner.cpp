/*
 * hcp::provision — Zephyr executor for the provisioning state machine.
 *
 * Maps Fsm actions onto Wi-Fi / storage / BluFi, drives the status LED from a
 * 50 ms tick and classifies the user button (short press = reboot, long press =
 * forget credentials and go back to provisioning).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/provision/provision.h"

#include <string.h>

#include <zephyr/kernel.h>

#include "blufi/blufi.h"
#include "hcp/input/input.h"
#include "hcp/led/strip.h"
#include "hcp/log/log.h"
#include "hcp/storage/store.h"

namespace hcp {
namespace provision {

namespace {

/* How many times to try the stored credentials before giving up and opening
 * BluFi. One attempt already blocks until the driver gives up, so keep this
 * small - the phone is waiting for the fallback in the worst case. */
constexpr int kWifiAttempts = 3;
constexpr uint32_t kTickMs = 50;
/* Grace period between the final SUCCESS report and tearing BLE down: the
 * WeChat mini-program polls rather than reacts, so it needs to be able to
 * receive that last frame before the link disappears. */
constexpr uint32_t kBleCloseDelayMs = 2000;
constexpr uint32_t kLongPressMs = 3000;
constexpr uint32_t kShortPressMaxMs = 1000;
/* How long to wait for the disconnect event before forcing a new connect: the
 * ESP32 driver returns -EALREADY while it is still in CONNECTING/CONNECTED, and
 * IsConnected() (IP based) cannot see the transitional state. */
constexpr int kDisconnectWaitMs = 1000;
// Deadline for a single Wi-Fi attempt. The ESP32 driver never reports a failed
// association that happens after esp_wifi_connect() returns (no CONNECT_RESULT),
// so without a deadline a bad credential set leaves the machine in kWifiTry:
// LED blinking, BLE closed, device unreachable. Real failures now arrive as
// kConnectFailed and retry immediately - this is the backstop. The first attempt
// gets the long budget (healthy AP + DHCP measured ~12 s here).
constexpr uint32_t kFirstAttemptTimeoutMs = 20000;
constexpr uint32_t kRetryAttemptTimeoutMs = 10000;
/* Window during which a link drop is attributed to our own pre-connect
 * Disconnect() instead of a failed attempt. */
constexpr int kOwnDisconnectMs = 1500;

constexpr char kKeySsid[] = "prov/ssid";
constexpr char kKeyPass[] = "prov/pass";

/* ROM entry point, linked from the ESP32 ROM symbol table (verified present in
 * the produced ELF). hal_espressif's esp_restart() is not compiled into a
 * Zephyr build and Zephyr's sys_reboot() has no Espressif implementation, so
 * this is the working way to reboot on ESP32-C6/C5/C3. */
extern "C" void esp_rom_software_reset_system(void);

Fsm g_fsm(kWifiAttempts);
storage::Store g_store;
led::Strip g_strip(1);
input::PressClassifier g_press(kLongPressMs, kShortPressMaxMs);

char g_ssid[33];
char g_pass[65];
bool g_have_creds;

// Credentials handed over by the phone, waiting to be written by kActSaveCreds.
char g_pend_ssid[33];
char g_pend_pass[65];

Fsm::Pattern g_pattern = Fsm::Pattern::kOff;
Fsm::Alert g_alert = Fsm::Alert::kNone;
int g_alert_ticks;  // remaining ticks of the current blink sequence
uint32_t g_tick_count;
int64_t g_own_disconnect_until_ms;

/* Connecting blocks in net_mgmt until the driver settles, so it runs on its own
 * thread: the 50 ms LED tick and the BluFi rx work live on the system workqueue
 * and must keep ticking while we wait. */
K_THREAD_STACK_DEFINE(g_wifi_thread_stack, 4096);
struct k_thread g_wifi_thread;
static K_SEM_DEFINE(g_wifi_kick, 0, 1);

struct k_work_delayable g_tick_work;
struct k_work_delayable g_ble_close_work;
struct k_work_delayable g_attempt_work;
static K_SEM_DEFINE(g_disconn_sem, 0, 1);

void Apply(const Fsm::Output& out);  // defined below

void AttemptTimeoutFn(struct k_work*) {
  HCP_LOGW(hcp::log::kDomainWifi, "provision: attempt timed out");
  Apply(g_fsm.OnAttemptTimeout());
}

void Reboot() {
  HCP_LOGI(hcp::log::kDomainDevice, "provision: short press -> reboot");
  // Logging is synchronous (LOG_MODE_IMMEDIATE), so the line above has already
  // reached the console by the time we get here.
  (void)irq_lock();
  esp_rom_software_reset_system();
  while (true) {
  }
}

void SavePendingCreds() {
  if (g_pend_ssid[0] == '\0') {
    return;
  }
  memcpy(g_ssid, g_pend_ssid, sizeof(g_ssid));
  memcpy(g_pass, g_pend_pass, sizeof(g_pass));
  g_have_creds = true;
  bool ok = g_store.SetString(kKeySsid, g_ssid);
  ok = ok && g_store.SetString(kKeyPass, g_pass);
  HCP_LOGI(hcp::log::kDomainWifi, "provision: credentials for \"%s\" saved (%s)",
           g_ssid, ok ? "ok" : "FAILED");
}

void EraseCreds() {
  g_store.Erase(kKeySsid);
  g_store.Erase(kKeyPass);
  g_ssid[0] = '\0';
  g_pass[0] = '\0';
  g_have_creds = false;
  HCP_LOGI(hcp::log::kDomainWifi, "provision: credentials erased");
}

void LoadCreds() {
  g_have_creds = g_store.Exists(kKeySsid) &&
                 g_store.GetString(kKeySsid, g_ssid, sizeof(g_ssid)) &&
                 g_store.GetString(kKeyPass, g_pass, sizeof(g_pass));
  if (g_have_creds) {
    HCP_LOGI(hcp::log::kDomainWifi, "provision: stored credentials for \"%s\"",
             g_ssid);
  } else {
    g_ssid[0] = '\0';
    g_pass[0] = '\0';
    HCP_LOGI(hcp::log::kDomainWifi, "provision: no stored credentials");
  }
}

void ConnectNow() {
  if (!g_have_creds) {
    return;
  }
  HCP_LOGI(hcp::log::kDomainWifi, "provision: connecting to \"%s\" (try %d)",
           g_ssid, g_fsm.attempts());
  // Force a clean slate: re-provisioning an ESP32 whose state is still
  // CONNECTING returns -EALREADY, and IP-based IsConnected() cannot see it.
  NoteOwnDisconnect();
  k_sem_reset(&g_disconn_sem);
  net::wifi::Disconnect();
  if (k_sem_take(&g_disconn_sem, K_MSEC(kDisconnectWaitMs)) != 0) {
    HCP_LOGD(hcp::log::kDomainWifi, "provision: no disconnect event, connecting");
  }

  net::WifiCredential c{g_ssid, g_pass[0] ? g_pass : nullptr};
  if (!net::wifi::Connect(c) && !net::wifi::IsConnected()) {
    // The request could not even be submitted (driver down/busy). Count it as a
    // failed attempt so the retry budget and the BLE fallback still apply.
    Apply(g_fsm.OnWifiConnectFailed());
  }
}

// Serializes Wi-Fi connect requests off the system workqueue.
void WifiThread(void*, void*, void*) {
  while (true) {
    k_sem_take(&g_wifi_kick, K_FOREVER);
    ConnectNow();
  }
}

void BleCloseFn(struct k_work*) {
  HCP_LOGI(hcp::log::kDomainWifi, "provision: closing BLE after provisioning");
  blufi_stop();
}

const char* StateName(Fsm::State s) {
  switch (s) {
    case Fsm::State::kProvisioning:
      return "provisioning";
    case Fsm::State::kWifiTry:
      return "wifi-try";
    case Fsm::State::kOnline:
      return "online";
    default:
      return "idle";
  }
}

const char* PatternName(Fsm::Pattern p) {
  switch (p) {
    case Fsm::Pattern::kBlueSlow:
      return "blue-slow";
    case Fsm::Pattern::kBlueFast:
      return "blue-fast";
    case Fsm::Pattern::kGreenSolid:
      return "green";
    default:
      return "off";
  }
}

void Apply(const Fsm::Output& out) {
  g_pattern = out.pattern;
  g_alert = out.alert;
  // 3 blinks, 4 ticks (200 ms) per blink.
  g_alert_ticks = (out.alert == Fsm::Alert::kNone) ? 0 : 12;

  // One readable line per transition: this is what tells you on the console
  // which state the box is in when the LED alone is ambiguous.
  HCP_LOGI(hcp::log::kDomainWifi, "provision: state=%s led=%s attempt=%d",
           StateName(out.state), PatternName(out.pattern), g_fsm.attempts());

  // Every stay in kWifiTry gets a deadline; leaving it cancels the pending one.
  if (out.state == Fsm::State::kWifiTry) {
    uint32_t ms = (g_fsm.attempts() <= 1) ? kFirstAttemptTimeoutMs
                                          : kRetryAttemptTimeoutMs;
    k_work_reschedule(&g_attempt_work, K_MSEC(ms));
  } else {
    k_work_cancel_delayable(&g_attempt_work);
  }

  for (int i = 0; i < out.num_actions; i++) {
    switch (out.actions[i]) {
      case Fsm::Action::kEnableBle: {
        int err = blufi_start();
        HCP_LOGI(hcp::log::kDomainWifi, "provision: BLE opened (%d)", err);
        break;
      }
      case Fsm::Action::kDisableBle:
        blufi_stop();
        break;
      case Fsm::Action::kCloseBleDelayed:
        k_work_reschedule(&g_ble_close_work, K_MSEC(kBleCloseDelayMs));
        break;
      case Fsm::Action::kConnectWifi:
        k_sem_give(&g_wifi_kick);
        break;
      case Fsm::Action::kSaveCreds:
        SavePendingCreds();
        break;
      case Fsm::Action::kEraseCreds:
        EraseCreds();
        break;
      default:
        break;
    }
  }
}

/* ---- status LED ---- */

using led::Color;

constexpr Color kLedOff{0, 0, 0};
constexpr Color kLedBlue{0, 0, 60};
constexpr Color kLedGreen{0, 60, 0};
constexpr Color kLedRed{60, 0, 0};
constexpr Color kLedPurple{40, 0, 40};

Color SteadyColor() {
  switch (g_pattern) {
    case Fsm::Pattern::kGreenSolid:
      return kLedGreen;
    case Fsm::Pattern::kBlueFast:
      // 2 Hz: 250 ms on, 250 ms off (5 ticks each at 50 ms)
      return (g_tick_count % 10) < 5 ? kLedBlue : kLedOff;
    case Fsm::Pattern::kBlueSlow:
      // 0.5 Hz: 1 s on, 1 s off
      return (g_tick_count % 40) < 20 ? kLedBlue : kLedOff;
    case Fsm::Pattern::kOff:
    default:
      return kLedOff;
  }
}

Color AlertColor() {
  Color c = (g_alert == Fsm::Alert::kPurple3) ? kLedPurple : kLedRed;
  return ((12 - g_alert_ticks) % 4) < 2 ? c : kLedOff;
}

void PaintLed() {
  Color c = kLedOff;
  if (g_alert_ticks > 0) {
    c = AlertColor();
    if (--g_alert_ticks == 0) {
      g_alert = Fsm::Alert::kNone;
    }
  } else {
    c = SteadyColor();
  }
  g_strip.SetAll(c);
  g_strip.Show();
}

/* ---- 50 ms tick: button timing + LED ---- */

void TickFn(struct k_work*) {
  g_tick_count++;

  input::PressClassifier::Action act =
      g_press.Update(input::Button::Pressed(),
                     static_cast<uint64_t>(k_uptime_get()));
  switch (act) {
    case input::PressClassifier::kShort:
      Reboot();
      break;
    case input::PressClassifier::kLong:
      HCP_LOGI(hcp::log::kDomainDevice,
               "provision: long press -> reset Wi-Fi configuration");
      Apply(g_fsm.OnResetRequested());
      break;
    case input::PressClassifier::kAborted:
      HCP_LOGI(hcp::log::kDomainDevice,
               "provision: release ignored (neither short nor long)");
      break;
    default:
      break;
  }

  PaintLed();
  k_work_reschedule(&g_tick_work, K_MSEC(kTickMs));
}

}  // namespace

void Start() {
  k_work_init_delayable(&g_tick_work, TickFn);
  k_work_init_delayable(&g_ble_close_work, BleCloseFn);
  k_work_init_delayable(&g_attempt_work, AttemptTimeoutFn);
  k_thread_create(&g_wifi_thread, g_wifi_thread_stack,
                  K_THREAD_STACK_SIZEOF(g_wifi_thread_stack), WifiThread,
                  nullptr, nullptr, nullptr, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
                  K_NO_WAIT);
  k_thread_name_set(&g_wifi_thread, "prov_wifi");

  if (!g_store.Open()) {
    HCP_LOGW(hcp::log::kDomainWifi, "provision: storage open failed");
  }
  LoadCreds();

  if (g_strip.Open()) {
    g_strip.SetBrightness(35);
  } else {
    HCP_LOGW(hcp::log::kDomainDevice, "provision: LED unavailable");
  }

  if (!input::Button::Init(nullptr, nullptr)) {
    HCP_LOGW(hcp::log::kDomainDevice, "provision: button unavailable (no sw0?)");
  }

  Apply(g_fsm.Start(g_have_creds));
  k_work_reschedule(&g_tick_work, K_MSEC(kTickMs));
}

void NotifyWifiEvent(net::WifiEvent e) {
  // The deliberate disconnect in ConnectWifiFn waits on this.
  if (e == net::WifiEvent::kDisconnected) {
    k_sem_give(&g_disconn_sem);
  }
  // A link drop right after our own Disconnect() is not a failed attempt.
  if ((e == net::WifiEvent::kDisconnected || e == net::WifiEvent::kConnectFailed) &&
      InOwnDisconnectWindow()) {
    HCP_LOGD(hcp::log::kDomainWifi, "provision: ignoring self-inflicted drop");
    return;
  }
  switch (e) {
    case net::WifiEvent::kConnected:
      Apply(g_fsm.OnWifiConnected());
      break;
    case net::WifiEvent::kConnectFailed:
      Apply(g_fsm.OnWifiConnectFailed());
      break;
    case net::WifiEvent::kDisconnected:
      Apply(g_fsm.OnWifiDisconnected());
      break;
    default:
      break;
  }
}

void NoteOwnDisconnect() {
  g_own_disconnect_until_ms = k_uptime_get() + kOwnDisconnectMs;
}

bool InOwnDisconnectWindow() {
  return k_uptime_get() < g_own_disconnect_until_ms;
}

void NotifyPhoneCredentials(const char* ssid, const char* pass) {
  if (ssid == nullptr) {
    return;
  }
  strncpy(g_pend_ssid, ssid, sizeof(g_pend_ssid) - 1);
  g_pend_ssid[sizeof(g_pend_ssid) - 1] = '\0';
  if (pass != nullptr) {
    strncpy(g_pend_pass, pass, sizeof(g_pend_pass) - 1);
    g_pend_pass[sizeof(g_pend_pass) - 1] = '\0';
  } else {
    g_pend_pass[0] = '\0';
  }
  // Persist + move to "trying"; the BluFi layer itself issues the connect (it
  // also owns the Wi-Fi status reports back to the phone).
  Apply(g_fsm.OnPhoneCreds());
}

Fsm::State state() { return g_fsm.state(); }

}  // namespace provision
}  // namespace hcp
