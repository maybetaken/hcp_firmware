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
  struct net_mgmt_event_callback wifi_cb;
  struct net_mgmt_event_callback ipv4_cb;
  struct net_mgmt_event_callback scan_cb;
  ScanCallback scan_user_cb = nullptr;
  void* scan_user = nullptr;
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

void WifiHandler(struct net_mgmt_event_callback* cb, uint64_t event,
                 struct net_if* iface) {
  ARG_UNUSED(cb);
  ARG_UNUSED(iface);
  if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
    const struct wifi_status* st =
        static_cast<const struct wifi_status*>(cb->info);
    int status = st ? st->status : -1;
    if (status == 0) {
      LOG_INF("Wi-Fi associated, waiting for DHCP");
      Report(WifiEvent::kConnecting);
    } else {
      LOG_WRN("Wi-Fi connect failed: status=%d", status);
      Report(WifiEvent::kConnectFailed);
    }
  } else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
    const struct wifi_status* st =
        static_cast<const struct wifi_status*>(cb->info);
    g.connected = false;
    LOG_INF("Wi-Fi disconnected: reason=%d", st ? st->disconn_reason : -1);
    Report(WifiEvent::kDisconnected);
  }
}

void Ipv4Handler(struct net_mgmt_event_callback* cb, uint64_t event,
                 struct net_if* iface) {
  ARG_UNUSED(cb);
  ARG_UNUSED(iface);
  if (event == NET_EVENT_IPV4_ADDR_ADD) {
    g.connected = true;
    LOG_INF("IPv4 address assigned (DHCP bound)");
    Report(WifiEvent::kConnected);
  }
}

void ScanHandler(struct net_mgmt_event_callback* cb, uint64_t event,
                 struct net_if* iface) {
  ARG_UNUSED(iface);
  if (event == NET_EVENT_WIFI_SCAN_RESULT && g.scan_user_cb != nullptr) {
    const struct wifi_scan_result* r =
        static_cast<const struct wifi_scan_result*>(cb->info);
    ScanResult out{};
    uint8_t n = r->ssid_length < 32 ? r->ssid_length : 32;
    memcpy(out.ssid, r->ssid, n);
    out.ssid[n] = '\0';
    out.rssi = r->rssi;
    out.channel = r->channel;
    memcpy(out.bssid, r->mac, sizeof(out.bssid));
    g.scan_user_cb(&out, g.scan_user);
  } else if (event == NET_EVENT_WIFI_SCAN_DONE && g.scan_user_cb != nullptr) {
    g.scan_user_cb(nullptr, g.scan_user);
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
      &g.wifi_cb, WifiHandler,
      NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
  net_mgmt_add_event_callback(&g.wifi_cb);
  net_mgmt_init_event_callback(&g.ipv4_cb, Ipv4Handler, NET_EVENT_IPV4_ADDR_ADD);
  net_mgmt_add_event_callback(&g.ipv4_cb);
  net_mgmt_init_event_callback(
      &g.scan_cb, ScanHandler,
      NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
  net_mgmt_add_event_callback(&g.scan_cb);
  return true;
}

bool Scan(ScanCallback cb, void* user) {
  if (!Init()) {
    return false;
  }
  g.scan_user_cb = cb;
  g.scan_user = user;
  struct wifi_scan_params params = {};
  // The ESP32 Wi-Fi driver forwards scan_type verbatim to esp_wifi, whose values are
  // 0=WIFI_FAST_SCAN (stops early, few APs) / 1=WIFI_ALL_CHANNEL_SCAN. Zephyr names
  // these ACTIVE(0)/PASSIVE(1). Pick the second so esp does an all-channel scan ->
  // complete AP list for the BluFi wifi list.
  params.scan_type = WIFI_SCAN_TYPE_PASSIVE;
  int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, g.iface, &params, sizeof(params));
  return ret == 0;
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
    const uint8_t* secret = reinterpret_cast<const uint8_t*>(cred.psk);
    size_t slen = strlen(cred.psk);
#if defined(CONFIG_HCP_NET_SECURITY_SAE)
    param.security = WIFI_SECURITY_TYPE_SAE;
    param.sae_password = secret;
    param.sae_password_length = static_cast<uint8_t>(slen);
#elif defined(CONFIG_HCP_NET_SECURITY_PSK)
    param.security = WIFI_SECURITY_TYPE_PSK;
    param.psk = secret;
    param.psk_length = static_cast<uint8_t>(slen);
#else /* AUTO */
    /* The ESP32 Wi-Fi supplicant has no WPA-Personal transition security type;
     * WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL falls through to "Authentication
     * method not supported" (-EIO). Use PSK: the driver maps it to a WPA2_PSK
     * threshold, which also associates with WPA2/WPA3-transition APs. Opt into
     * SAE explicitly (CONFIG_HCP_NET_SECURITY_SAE) for WPA3-only networks. */
    param.security = WIFI_SECURITY_TYPE_PSK;
    param.psk = secret;
    param.psk_length = static_cast<uint8_t>(slen);
#endif
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
