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
#include "hcp/kernel/kernel.h"
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

#ifndef CONFIG_HCP_PROVISION_LED_BRIGHTNESS
#define CONFIG_HCP_PROVISION_LED_BRIGHTNESS 10
#endif

constexpr char kKeySsid[] = "prov/ssid";
constexpr char kKeyPass[] = "prov/pass";
constexpr char kKeyLed[] = "prov/led";

// The status light, as Home Assistant's light entity can switch it.
bool g_led_enabled = true;

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
  // Buffered logging may not have drained yet; give the log thread a moment so
  // the reason for the reset is visible on the console.
  k_sleep(K_MSEC(200));
  kernel::Reboot();
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
    return;
  }

  /* Pre-provisioning for the bench and for factory images: a network compiled into
   * the build is used and then written to storage, so from the next boot on this is
   * an ordinary stored-credentials device (provisioning, the button and Home
   * Assistant can all still change it). Empty - the default - keeps the shipped
   * behaviour: wait for a phone. */
  if (CONFIG_HCP_PROVISION_BAKEIN_SSID[0] != '\0') {
    strncpy(g_ssid, CONFIG_HCP_PROVISION_BAKEIN_SSID, sizeof(g_ssid) - 1);
    g_ssid[sizeof(g_ssid) - 1] = '\0';
    strncpy(g_pass, CONFIG_HCP_PROVISION_BAKEIN_PSK, sizeof(g_pass) - 1);
    g_pass[sizeof(g_pass) - 1] = '\0';
    g_have_creds = true;
    const bool saved =
        g_store.SetString(kKeySsid, g_ssid) && g_store.SetString(kKeyPass, g_pass);
    HCP_LOGI(hcp::log::kDomainWifi,
             "provision: pre-provisioned from the build for \"%s\" (%s)", g_ssid,
             saved ? "saved" : "NOT saved, asked again next boot");
    return;
  }

  g_ssid[0] = '\0';
  g_pass[0] = '\0';
  HCP_LOGI(hcp::log::kDomainWifi, "provision: no stored credentials");
}

void ConnectNow() {
  if (!g_have_creds) {
    return;
  }
  HCP_LOGI(hcp::log::kDomainWifi, "provision: connecting to \"%s\" (try %d)",
           g_ssid, g_fsm.attempts());
  /* Cold boot has no link to drop, so skip straight to connecting (that used to
   * cost a wasted second per attempt). Retries must still tear the previous
   * attempt down first: the ESP32 driver rejects Connect() with -EALREADY while
   * it is still CONNECTING, and without the Disconnect the retry bounces back
   * within milliseconds and burns the budget on no-ops (observed: attempt 2 -> 3
   * in 18 ms). */
  if (g_fsm.attempts() > 1 || net::wifi::LinkActive()) {
    NoteOwnDisconnect();
    k_sem_reset(&g_disconn_sem);
    net::wifi::Disconnect();
    if (k_sem_take(&g_disconn_sem, K_MSEC(kDisconnectWaitMs)) != 0) {
      HCP_LOGD(hcp::log::kDomainWifi, "provision: no disconnect event, connecting");
    }
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
    case Fsm::Pattern::kVioletSlow:
      return "violet-slow";
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
/* Palette authored at full 8-bit scale with soft hues; CONFIG_HCP_PROVISION_LED_BRIGHTNESS
 * dims everything with one knob. To match a reference device, change these values
 * only - the cadence and the state mapping live elsewhere on purpose.
 * (the driver applies the devicetree color-mapping, so these are logical RGB.) */
// Sky blue: waiting for provisioning, or an attempt is running.
constexpr Color kLedBlue{60, 150, 255};
// Purple: Wi-Fi is up, the service session (MQTT) is not. Keep the green channel
// near zero - a balanced R+G+B reads white on a WS2812 once dimmed, which is what
// {170,130,255} did here before.
constexpr Color kLedViolet{170, 0, 255};
// Pale green: everything the product needs is connected.
constexpr Color kLedGreen{80, 255, 120};
// Alerts: three blinks, then back to the steady pattern. The erase alert is
// white rather than violet so it still reads on top of the violet wait state.
constexpr Color kLedRed{255, 30, 20};
constexpr Color kLedWhite{255, 255, 255};

Color SteadyColor() {
  switch (g_pattern) {
    case Fsm::Pattern::kGreenSolid:
      return kLedGreen;
    case Fsm::Pattern::kVioletSlow:
      // 0.5 Hz violet, same cadence as the provisioning blink: slow means
      // "nothing is being attempted, this is the steady wait".
      return (g_tick_count % 40) < 20 ? kLedViolet : kLedOff;
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
  Color c = (g_alert == Fsm::Alert::kErased3) ? kLedWhite : kLedRed;
  return ((12 - g_alert_ticks) % 4) < 2 ? c : kLedOff;
}

/* One frame to the strip.
 *
 * There is no log line here on purpose: hcp::led logs the frame it actually sends
 * (with the brightness already applied), which is the wire-level truth, and printing
 * the requested colour as well doubles the lines for the same event - four a second
 * while the provisioning blink runs, on a board whose log ring is 1 KB. The one thing
 * this function had to establish - "is the strip being told to change at all" - is
 * answered by that flush line, and it was: the C3 alternates rgb(6,15,25) and
 * rgb(0,0,0) every 250 ms in blue-fast, so a steady light means the state's pattern
 * is steady (kGreenSolid), not a broken driver. */
void Paint(Color c) {
  g_strip.SetAll(c);
  g_strip.Show();
}

void PaintLed() {
  if (!g_led_enabled) {
    // Asked to be dark: no steady pattern and no alert. The state machine keeps
    // running underneath - this function is only its output device.
    Paint(kLedOff);
    return;
  }
  Color c = kLedOff;
  if (g_alert_ticks > 0) {
    c = AlertColor();
    if (--g_alert_ticks == 0) {
      g_alert = Fsm::Alert::kNone;
    }
  } else {
    c = SteadyColor();
  }
  Paint(c);
}

/* ---- 50 ms tick: button timing + LED ---- */

void TickFn(struct k_work*) {
  g_tick_count++;

  /* One line a second, at debug level. Two separate field reports - "the LED never
   * blinks" and "the button does nothing" - are both, if true, symptoms of this
   * tick not running: it is the only thing that repaints the strip and the only
   * thing that asks the button what it thinks. They cannot be told apart from
   * outside the box, so the log answers it: a count that advances means the repaint
   * and the button sampling are both alive and the cause is somewhere else (a steady
   * green, for instance, is the correct pattern when everything is connected). A
   * count that stops names the culprit directly - the system workqueue is a
   * cooperative thread here, so one work item that blocks takes this tick and the
   * button debounce down with it. */
  if ((g_tick_count % 20) == 0) {
    HCP_LOGD(hcp::log::kDomainDevice, "provision: tick %u led=%s button=%d",
             (unsigned)g_tick_count, PatternName(g_pattern),
             input::Button::Pressed() ? 1 : 0);
  }

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

  // Restore the LED setting before the first paint, so a device that was told to
  // stay dark does not flash once on the way up.
  {
    char val[8];
    if (g_store.GetString(kKeyLed, val, sizeof(val))) {
      g_led_enabled = (val[0] != '0');
    }
  }

  if (g_strip.Open()) {
    g_strip.SetBrightness(CONFIG_HCP_PROVISION_LED_BRIGHTNESS);
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

void NotifyServiceReady() {
  // Called by the MQTT client once its session is up; until then the LED stays
  // violet, because "online" for the product means Wi-Fi *and* the service.
  Apply(g_fsm.OnServiceReady());
}

void NotifyServiceLost() { Apply(g_fsm.OnServiceLost()); }

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

void RequestReset() {
  // The same action as a long press on the button, reached from the network instead
  // (Home Assistant's <sn>/control/reconfig). Going through the state machine and
  // not doing our own erase-and-advertise is the point: the FSM owns what "no
  // credentials" means for the LED, for BLE and for the retry counters, and a
  // second path that half-does it is how a device ends up advertising BLE while it
  // still thinks it is connected.
  HCP_LOGI(hcp::log::kDomainWifi,
           "provision: reset requested over the network");
  Apply(g_fsm.OnResetRequested());
}

void SetLedEnabled(bool on) {
  if (on == g_led_enabled) {
    return;  // same as before: do not spend a flash write
  }
  g_led_enabled = on;
  if (!g_store.SetString(kKeyLed, on ? "1" : "0")) {
    HCP_LOGW(hcp::log::kDomainDevice,
             "provision: LED setting changed but could not be stored, a reboot will "
             "undo it");
  }
  HCP_LOGI(hcp::log::kDomainDevice, "provision: status light %s",
           on ? "on" : "off");
}

bool LedEnabled() { return g_led_enabled; }

}  // namespace provision
}  // namespace hcp
