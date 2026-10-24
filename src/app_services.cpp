/*
 * The running product: what happens after the link is up.
 *
 * Split out of main.cpp, where the Home Assistant command handler sat *inside*
 * the #ifdef CONFIG_BLUFI block. That was not reachable as a bug while
 * hcp::provision was on (its Kconfig depends on BLUFI), but it meant the
 * services could not start at all in a build without BLE - so they live here now.
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

namespace hcp {
namespace app {

/* Everything here belongs to the provisioning build: without that state machine
 * there is no session to serve, and the two handlers below would be dead weight
 * (and the compiler rightly says so). */
#ifdef CONFIG_HCP_PROVISION

#ifdef CONFIG_HCP_MQTT
/* Commands arrive on <sn>/cmd. Nothing acts on them yet (relay/collection control
 * is the next phase), but the device logs them and echoes to <sn>/ack, so a
 * publish from Home Assistant is observable end to end even when the USB console
 * is not attached. */
static void on_mqtt_command(const uint8_t* payload, size_t len, void* /*user*/) {
  char text[65];
  size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
  memcpy(text, payload, n);
  text[n] = '\0';
  HCP_LOGI(hcp::log::kDomainMqtt, "command: %s", text);
#ifdef CONFIG_HCP_OTA
  /* "ota" / "ota <base-url>" belongs to hcp::ota, which reports how it went on the
   * state topic rather than echoing the command back. */
  if (hcp::ota::HandleCommand(text, n)) {
    return;
  }
#endif
  hcp::mqtt::PublishTopic("ack", payload, len);
}
#endif  // CONFIG_HCP_MQTT

#ifdef CONFIG_HCP_OTA
/* Adds "ota" to the periodic <sn>/state document while an update has been tried. */
static int ota_state_extra(char* buf, size_t cap) {
  char status[48];

  if (hcp::ota::StatusText(status, sizeof(status)) == 0) {
    return 0;
  }
  return snprintk(buf, cap, ",\"ota\":\"%s\"", status);
}
#endif  // CONFIG_HCP_OTA

void StartServices() {
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
  hcp::mqtt::SetCommandHandler(on_mqtt_command, nullptr);
#endif
#ifdef CONFIG_HCP_OTA
  /* Updates are version-gated downloads from the host Home Assistant announced,
   * and a trial swap until this session proves the new image can reach the
   * network. */
  hcp::mqtt::SetStateExtraHandler(ota_state_extra);
  hcp::ota::Start();
#endif
}

#endif  // CONFIG_HCP_PROVISION

}  // namespace app
}  // namespace hcp
