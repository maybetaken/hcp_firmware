/*
 * hcp::net — Zephyr Wi-Fi station backend (net_mgmt Wi-Fi MGMT + DHCP).
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef CONFIG_HCP_NET

#include "hcp/net/wifi.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(hcp_wifi, CONFIG_LOG_DEFAULT_LEVEL);

namespace hcp {
namespace net {
namespace wifi {

namespace {

struct State {
  struct net_if* iface = nullptr;
  struct net_mgmt_event_callback mgmt_cb;
  WifiCallback cb = nullptr;
  void* user = nullptr;
  volatile bool connected = false;
};
State g;

void Report(WifiEvent e) {
  if (g.cb) {
    g.cb(e, g.user);
  }
}

void MgmtHandler(struct net_mgmt_event_callback* cb, uint64_t event,
                 struct net_if* iface) {
  ARG_UNUSED(cb);
  ARG_UNUSED(iface);
  switch (event) {
    case NET_EVENT_WIFI_CONNECT_RESULT:
      Report(WifiEvent::kConnecting);
      break;
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
      g.connected = false;
      Report(WifiEvent::kDisconnected);
      break;
    case NET_EVENT_IPV4_DHCP_BOUND:
      g.connected = true;
      LOG_INF("DHCP bound (IPv4 acquired)");
      Report(WifiEvent::kConnected);
      break;
    default:
      LOG_DBG("mgmt event 0x%llx", (unsigned long long)event);
      break;
  }
}

}  // namespace

void SetCallback(WifiCallback cb, void* user) {
  g.cb = cb;
  g.user = user;
}

bool Init() {
  if (g.iface != nullptr) {
    return true;
  }
  g.iface = net_if_get_default();  // Wi-Fi is the board's only L2 iface here
  if (g.iface == nullptr) {
    return false;
  }
  net_mgmt_init_event_callback(
      &g.mgmt_cb, MgmtHandler,
      NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |
          NET_EVENT_IPV4_DHCP_BOUND);
  net_mgmt_add_event_callback(&g.mgmt_cb);
  return true;
}

bool Connect(const WifiCredential& cred) {
  if (!Init() || cred.ssid == nullptr) {
    return false;
  }
  struct wifi_connect_req_params param;
  memset(&param, 0, sizeof(param));
  param.ssid = reinterpret_cast<const uint8_t*>(cred.ssid);
  param.ssid_length = static_cast<uint8_t>(strlen(cred.ssid));
  if (cred.psk != nullptr && cred.psk[0] != '\0') {
    param.psk = reinterpret_cast<const uint8_t*>(cred.psk);
    param.psk_length = static_cast<uint8_t>(strlen(cred.psk));
    param.security = WIFI_SECURITY_TYPE_PSK; /* WPA2-PSK */
  } else {
    param.security = WIFI_SECURITY_TYPE_NONE;
  }
  param.timeout = 20;  /* seconds */

  Report(WifiEvent::kConnecting);
  int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, g.iface, &param, sizeof(param));
  if (ret != 0) {
    LOG_ERR("WIFI_CONNECT req failed: ret=%d errno=%d", ret, errno);
  }
  return ret == 0;
}

bool Disconnect() {
  if (g.iface == nullptr) {
    return false;
  }
  int ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, g.iface, NULL, 0);
  g.connected = false;
  return ret == 0;
}

bool IsConnected() { return g.connected; }

}  // namespace wifi
}  // namespace net
}  // namespace hcp

#endif  // CONFIG_HCP_NET
