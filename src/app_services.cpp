/*
 * HCP application - the running product's services, in the order they come up.
 *
 * Split out of what used to be main.cpp, where the Home Assistant command handler
 * sat *inside* the #ifdef CONFIG_BLUFI block. That was not reachable as a bug
 * while hcp::provision was on (its Kconfig depends on BLUFI), but it meant a build
 * without BLE could not start MQTT, SSDP, the collector or OTA at all - so the
 * services live here now, under their own guard.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"

#include <string.h>

#include <zephyr/kernel.h>

#include "hcp/log/log.h"
#ifdef CONFIG_HCP_COLLECTOR
#include "hcp/collector/collector.h"
#endif
#ifdef CONFIG_HCP_MQTT
#include "hcp/mqtt/client.h"
#endif
#ifdef CONFIG_HCP_OTA
#include "hcp/ota/ota.h"
#endif
#ifdef CONFIG_HCP_PROVISION
#include "hcp/provision/provision.h"
#endif
#ifdef CONFIG_HCP_SSDP
#include "hcp/ssdp/discovery.h"
#endif

#ifdef CONFIG_HCP_PROVISION

namespace hcp {
namespace app {

#ifdef CONFIG_HCP_MQTT
/* Commands arrive on <sn>/cmd, the topic this firmware owns for text commands
 * (the previous firmware had no such topic; Home Assistant's own commands arrive on
 * <sn>/config and under <sn>/control and are handled by hcp::collector). hcp::ota
 * owns the "ota" verb. Nothing is echoed back: an <sn>/ack topic was added here once
 * and removed again, because no subscriber exists on either side and it was only bus
 * traffic. The observable result of a command is the state change it causes. */
void Services::OnMqttCommand(const uint8_t* payload, size_t len, void* /*user*/) {
  char text[65];
  size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
  memcpy(text, payload, n);
  text[n] = '\0';
  HCP_LOGI(hcp::log::kDomainMqtt, "command: %s", text);
#ifdef CONFIG_HCP_OTA
  /* "ota" / "ota <base-url>" belongs to hcp::ota, which reports how it went on the
   * diagnostics topic rather than echoing the command back. */
  if (hcp::ota::HandleCommand(text, n)) {
    return;
  }
#endif
}
#endif  // CONFIG_HCP_MQTT

#if defined(CONFIG_HCP_OTA) || defined(CONFIG_HCP_PROVISION)
/* Two fields hcp::mqtt cannot know about, appended to the periodic
 * <sn>/diagnostics document:
 *   "led"  - Home Assistant's light entity reads its state from here
 *            (light.py: value == "on"), so a device that answers control/led has to
 *            report it back or the UI drifts from the device.
 *   "ota"  - present only while an update has been attempted.
 * Returns the number of bytes written (0 = nothing to add). */
int Services::StateExtra(char* buf, size_t cap) {
  size_t used = 0;
#ifdef CONFIG_HCP_PROVISION
  const int n = snprintk(buf, cap, ",\"led\":\"%s\"",
                         hcp::provision::LedEnabled() ? "on" : "off");
  if (n > 0 && static_cast<size_t>(n) < cap) {
    used += static_cast<size_t>(n);
  }
#endif
#ifdef CONFIG_HCP_OTA
  char status[48];
  if (hcp::ota::StatusText(status, sizeof(status)) != 0) {
    const int n = snprintk(buf + used, cap - used, ",\"ota\":\"%s\"", status);
    if (n > 0 && static_cast<size_t>(n) < cap - used) {
      used += static_cast<size_t>(n);
    }
  }
#endif
  return static_cast<int>(used);
}
#endif  // CONFIG_HCP_OTA || CONFIG_HCP_PROVISION

void Services::Start() {
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
#ifdef CONFIG_HCP_COLLECTOR
  /* Routes <sn>/config, <sn>/control/cmd and friends onto one queue, consumed by
   * one worker that owns the point table and the Modbus bus. Started after MQTT so
   * the routes exist before the session subscribes. */
  if (!hcp::collector::Start()) {
    HCP_LOGW(hcp::log::kDomainModbus, "collector not started");
  }
#endif
#ifdef CONFIG_HCP_MQTT
  hcp::mqtt::SetCommandHandler(OnMqttCommand, nullptr);
#endif
#if defined(CONFIG_HCP_OTA) || defined(CONFIG_HCP_PROVISION)
  hcp::mqtt::SetStateExtraHandler(StateExtra);
#endif
#ifdef CONFIG_HCP_OTA
  /* Updates are version-gated downloads from the host Home Assistant announced,
   * and a trial swap until this session proves the new image can reach the
   * network. */
  hcp::ota::Start();
#endif
}

}  // namespace app
}  // namespace hcp

#endif  // CONFIG_HCP_PROVISION
