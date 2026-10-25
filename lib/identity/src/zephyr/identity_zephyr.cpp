/*
 * hcp::identity - Zephyr resolution: storage, station MAC, DHCP hostname.
 *
 * Runs once at boot, before provisioning touches Wi-Fi, so the very first DHCP
 * DISCOVER already carries the right name (a later net_hostname_set() would only
 * be picked up on a renew, which is how the device ends up showing as
 * "espressif" in a router that was never told otherwise).
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef CONFIG_HCP_IDENTITY

#include "hcp/identity/identity.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/hostname.h>
#include <zephyr/storage/flash_map.h>

#include "hcp/log/log.h"
#include "hcp/net/wifi.h"
#include "hcp/storage/store.h"

namespace hcp {
namespace identity {
namespace {

char g_sn[kSerialMax];
char g_hostname[kSerialMax];
bool g_resolved;

storage::Store g_store;

/* Read the read-only factory partition (prodinfo_partition in the board dts, burned
 * per device with tools/make_sn_bin.py). A board without that partition compiles
 * this out entirely instead of failing at boot. */
bool ReadProdinfo(char* out, size_t cap) {
#if DT_NODE_EXISTS(DT_NODELABEL(prodinfo_partition)) && \
    IS_ENABLED(CONFIG_FLASH_MAP)
  constexpr size_t kPartition = DT_REG_SIZE(DT_NODELABEL(prodinfo_partition));
  constexpr size_t kWant =
      MIN(kPartition, static_cast<size_t>(kProdinfoHeader + kSerialMax));
  uint8_t raw[kWant];
  const struct flash_area* fa = nullptr;
  if (flash_area_open(PARTITION_ID(prodinfo_partition), &fa) != 0) {
    HCP_LOGW(hcp::log::kDomainDevice, "identity: prodinfo area not mapped");
    return false;
  }
  const int rc = flash_area_read(fa, 0, raw, sizeof(raw));
  flash_area_close(fa);
  if (rc != 0) {
    HCP_LOGW(hcp::log::kDomainDevice, "identity: prodinfo read failed (%d)", rc);
    return false;
  }
  // Erased or malformed is the normal state before production writes a code, so
  // no logging here - the caller just falls through to storage/MAC.
  return ParseProdinfo(raw, sizeof(raw), CONFIG_HCP_IDENTITY_PREFIX, out, cap);
#else
  ARG_UNUSED(out);
  ARG_UNUSED(cap);
  return false;
#endif
}

/* Push the name to the network stack so DHCPv4 puts it in option 12. */
void AnnounceHostname() {
  if (!FormatHostname(g_sn, g_hostname, sizeof(g_hostname))) {
    HCP_LOGW(hcp::log::kDomainDevice,
             "identity: serial \"%s\" has no usable hostname form", g_sn);
    return;
  }
#if defined(CONFIG_NET_HOSTNAME_DYNAMIC)
  if (net_hostname_set(g_hostname, strlen(g_hostname)) != 0) {
    HCP_LOGW(hcp::log::kDomainDevice, "identity: net_hostname_set(\"%s\") failed",
             g_hostname);
    return;
  }
  HCP_LOGI(hcp::log::kDomainDevice, "identity: hostname \"%s\"", g_hostname);
#else
  HCP_LOGW(hcp::log::kDomainDevice,
           "identity: CONFIG_NET_HOSTNAME_DYNAMIC off, router will show the "
           "build-time name \"%s\"",
           CONFIG_NET_HOSTNAME);
#endif
}

}  // namespace

bool Init() {
  if (g_resolved) {
    return true;
  }

  const bool opened = g_store.Open();
  char stored[kSerialMax];
  const bool have_stored =
      opened && g_store.GetString(kSerialKey, stored, sizeof(stored)) &&
      stored[0] != '\0';

  // The burned code wins over everything: it is what production wrote into the
  // device, and a stale value left in storage by an earlier MAC-derived boot must
  // not shadow it (that mismatch is worth a warning, hence the read above).
  char burned[kSerialMax];
  if (ReadProdinfo(burned, sizeof(burned))) {
    strncpy(g_sn, burned, sizeof(g_sn) - 1);
    g_sn[sizeof(g_sn) - 1] = '\0';
    if (have_stored && strcmp(g_sn, stored) != 0) {
      HCP_LOGW(hcp::log::kDomainDevice,
               "identity: storage holds \"%s\", prodinfo wins with \"%s\"",
               stored, g_sn);
    }
    HCP_LOGI(hcp::log::kDomainDevice, "identity: serial %s (from prodinfo)",
             g_sn);
    g_resolved = true;
    AnnounceHostname();
    return true;
  }

  if (have_stored) {
    strncpy(g_sn, stored, sizeof(g_sn) - 1);
    g_sn[sizeof(g_sn) - 1] = '\0';
    HCP_LOGI(hcp::log::kDomainDevice, "identity: serial %s (from storage)",
             g_sn);
    g_resolved = true;
    AnnounceHostname();
    return true;
  }

  g_sn[0] = '\0';
  uint8_t mac[6];
  bool from_mac = net::wifi::GetHwAddr(mac) &&
                  FormatFromMac(mac, CONFIG_HCP_IDENTITY_PREFIX, g_sn,
                                sizeof(g_sn));
  if (from_mac) {
    HCP_LOGI(hcp::log::kDomainDevice, "identity: derived serial %s", g_sn);
    if (opened && !g_store.SetString(kSerialKey, g_sn)) {
      // Not fatal: the value is deterministic from the MAC, it just is not
      // fixed any more, so a factory write later could disagree with it.
      HCP_LOGW(hcp::log::kDomainDevice, "identity: serial not persisted");
    }
  } else {
    // Interface not up yet: keep topics non-empty with a placeholder that is
    // obviously temporary, and derive again on the next boot.
    strncpy(g_sn, CONFIG_HCP_IDENTITY_PREFIX "pending", sizeof(g_sn) - 1);
    g_sn[sizeof(g_sn) - 1] = '\0';
    HCP_LOGW(hcp::log::kDomainDevice, "identity: no serial available (%s)",
             opened ? "no station MAC" : "storage unavailable");
  }

  g_resolved = true;
  AnnounceHostname();
  return from_mac;
}

const char* serial_number() { return g_sn; }

}  // namespace identity
}  // namespace hcp

#endif /* CONFIG_HCP_IDENTITY */
