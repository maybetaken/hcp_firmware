/*
 * HCP application - the running product's services, in the order they come up.
 *
 * Split out of the old main.cpp, where the Home Assistant command handler sat inside the
 * #ifdef CONFIG_BLUFI block: harmless while hcp::provision was on, but a build without
 * BLE then could not start MQTT, SSDP, the collector or OTA at all.
 *
 * Copyright (c) 2026 maybetaken
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
#include "hcp/net/wifi.h"  // diagnostics carries the live ssid/rssi
#include "hcp/provision/provision.h"
#endif
#ifdef CONFIG_HCP_SSDP
#include "hcp/ssdp/discovery.h"
#endif

#ifdef CONFIG_HCP_PROVISION

namespace hcp {
namespace app {

#ifdef CONFIG_HCP_MQTT
/* <sn>/cmd: the text-command topic this firmware owns ("ota" is handled by hcp::ota;
 * Home Assistant's own commands arrive on <sn>/config and <sn>/control/..., routed by
 * hcp::collector). Nothing is echoed back - there is no ack topic, because neither side
 * has a reader for one, and a command's observable result is the state change it causes. */
void Services::OnMqttCommand(const uint8_t* payload, size_t len, void* /*user*/) {
  char text[65];
  size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
  memcpy(text, payload, n);
  text[n] = '\0';
  HCP_LOGI(hcp::log::kDomainMqtt, "command: %s", text);
#ifdef CONFIG_HCP_OTA
  /* "ota" / "ota <base-url>": hcp::ota reports the outcome on diagnostics rather
   * than echoing the command. */
  if (hcp::ota::HandleCommand(text, n)) {
    return;
  }
#endif
}
#endif  // CONFIG_HCP_MQTT

#if defined(CONFIG_HCP_OTA) || defined(CONFIG_HCP_PROVISION)
/* Fields hcp::mqtt cannot know, appended to the periodic <sn>/diagnostics document.
 * Home Assistant's diagnostics card reads exactly these keys and nothing else
 * (plugins/base_device.py: update_diagnostics takes "ssid", "rssi", "led"=="on"), which
 * is the whole specification:
 *   "ssid", "rssi" - the AP as the driver sees it right now, so the card cannot show the
 *                    network we were asked to join instead of the one we are on;
 *   "led"          - the light entity's state source (light.py: value == "on"); not
 *                    reported, the UI drifts from the device;
 *   "ledf", "lederr", "ledrc", "ledp" - what hcp::provision has handed the strip and
 *                    what it got back: frames, refusals, the last code, and the steady
 *                    pattern the machine is in. Nothing in Home Assistant reads these;
 *                    they are here so a report about the light can be checked without
 *                    opening the box (see the note where they are appended);
 *   "ota"          - present only once an update has been attempted.
 * Returns the bytes written (0 = nothing to add). */
int Services::StateExtra(char* buf, size_t cap) {
  size_t used = 0;
#ifdef CONFIG_HCP_PROVISION
  /* An SSID is user data and a JSON string cannot carry a raw '"', '\\' or a control
   * byte; one of those would make the whole document unparseable, and Home Assistant
   * logs the JSONDecodeError and drops the update. Hostile bytes become '?'; UTF-8
   * passes through untouched. */
  net::LinkInfo link{};
  if (hcp::net::wifi::GetLinkInfo(&link)) {
    char safe[sizeof(link.ssid)];
    size_t i = 0;
    for (; i + 1 < sizeof(safe) && link.ssid[i] != '\0'; i++) {
      const unsigned char ch = static_cast<unsigned char>(link.ssid[i]);
      safe[i] = (ch < 0x20U || ch == '"' || ch == '\\') ? '?' : link.ssid[i];
    }
    safe[i] = '\0';
    const int n = snprintk(buf + used, cap - used, ",\"ssid\":\"%s\",\"rssi\":%d",
                           safe, static_cast<int>(link.rssi));
    if (n > 0 && static_cast<size_t>(n) < cap - used) {
      used += static_cast<size_t>(n);
    }
  }

  const int n = snprintk(buf + used, cap - used, ",\"led\":\"%s\"",
                         hcp::provision::LedEnabled() ? "on" : "off");
  if (n > 0 && static_cast<size_t>(n) < cap - used) {
    used += static_cast<size_t>(n);
  }

  /* The counters that let someone read a light problem from the other side of the
   * house. "the status light is doing something odd" used to have three answers - the
   * machine left its steady pattern, the light was switched back on, or bits reached
   * the strip that this firmware did not send - and no way to tell them apart without a
   * serial cable, because the flush line is a debug message on a board whose log ring is
   * 1 KB and the network carried no frame count at all.
   *
   * They separated on the first try, and not in the direction this comment expected.
   * The bench C3 reported led "off" with pattern "green", which is a silenced output on
   * an online device and explains nothing, but ledf was climbing ten times a second
   * where a dark light adds at most one. An unchanged framebuffer only reaches the
   * driver when the previous attempt was refused, so that rate IS the bug report - and
   * lederr and ledrc are here to name it instead of leaving it to be inferred, because
   * "failing" and "failing with EIO after the channel was started" are different bugs
   * with different fixes and only one of them can be told from a distance. */
  const int more = snprintk(buf + used, cap - used,
                            ",\"ledf\":%u,\"lederr\":%u,\"ledrc\":%d,\"ledp\":\"%s\"",
                            static_cast<unsigned>(hcp::provision::LedFrames()),
                            static_cast<unsigned>(hcp::provision::LedErrors()),
                            hcp::provision::LedLastError(),
                            hcp::provision::LedPattern());
  if (more > 0 && static_cast<size_t>(more) < cap - used) {
    used += static_cast<size_t>(more);
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
  /* The state machine decides: stored credentials -> join with BLE off, otherwise (or
   * after retries fail) -> open BluFi. It owns the status LED and the button from here. */
  hcp::provision::Start();
#ifdef CONFIG_HCP_MQTT
  /* Brings the LED from violet to green once the broker accepts us; backs off until it does. */
  hcp::mqtt::Start();
#endif
#ifdef CONFIG_HCP_SSDP
  /* Home Assistant announces its broker every 5 s on 239.255.255.250:1900, so no broker
   * address has to be compiled in. */
  hcp::ssdp::Start();
#endif
#ifdef CONFIG_HCP_COLLECTOR
  /* One queue, one worker owning the point table and the Modbus bus. Started after MQTT
   * so the routes exist before the session subscribes. */
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
  /* Version-gated downloads from the announced host, and a trial swap until this
   * session proves the new image can reach the network. */
  hcp::ota::Start();
#endif
}

}  // namespace app
}  // namespace hcp

#endif  // CONFIG_HCP_PROVISION
