/*
 * hcp::ssdp - Zephyr listener: one UDP socket joined to the SSDP group.
 *
 * The announcement only arrives while the station link is up, and joining a
 * multicast group needs a configured interface, so opening the socket is
 * retried until it succeeds instead of failing once at boot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef CONFIG_HCP_SSDP

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>

#include "hcp/log/log.h"
#include "hcp/mqtt/client.h"
#include "hcp/provision/provision.h"
#include "hcp/ssdp/discovery.h"

namespace hcp {
namespace ssdp {
namespace {

#define SSDP_LOGI(...) HCP_LOGI(hcp::log::kDomainSsdp, __VA_ARGS__)
#define SSDP_LOGW(...) HCP_LOGW(hcp::log::kDomainSsdp, __VA_ARGS__)
#define SSDP_LOGD(...) HCP_LOGD(hcp::log::kDomainSsdp, __VA_ARGS__)

constexpr int kRetryMs = 2000;
constexpr int kRxBufLen = 256;

/* Only worth trying once the device is on the network. */
bool provision_up() {
  return provision::state() == provision::Fsm::State::kOnline;
}

char url[kUrlMax];
bool started;
bool socket_warned;
int last_bind_errno;     // diagnostic: why the reopen keeps failing
int join_pending_errno;

K_THREAD_STACK_DEFINE(listener_stack, CONFIG_HCP_SSDP_STACK_SIZE);
struct k_thread listener_thread;

/* Open the socket and join the group; -1 when the interface is not ready. */
int OpenSocket() {
  int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    SSDP_LOGW("socket failed (errno %d)", errno);
    return -1;
  }
  const int opt = 1;
  // Another HA instance or a restart of this one may find the socket still in
  // TIME_WAIT; without this the bind fails and discovery never starts.
  const int reuse_rc = zsock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt,
                                        sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kSsdpPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (zsock_bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    const int bind_errno = errno;
    zsock_close(sock);
    // Reported by the caller once per reopen so a stuck state is visible
    // without flooding the console every retry.
    last_bind_errno = bind_errno;
    return -1;
  }

  /* Zephyr does not implement the classic ip_mreq/IP_ADD_MEMBERSHIP pair: the
   * socket layer only handles ZSOCK_IP_ADD_MEMBERSHIP and rejects anything whose
   * optlen is not sizeof(struct net_ip_mreqn) (EINVAL). imr_address = ANY plus
   * ifindex 0 lets it fall back to the default interface. */
  struct net_ip_mreqn mreq = {};
  if (zsock_inet_pton(AF_INET, kSsdpGroup, &mreq.imr_multiaddr) != 1) {
    SSDP_LOGW("bad multicast group %s", kSsdpGroup);
    zsock_close(sock);
    return -1;
  }
  mreq.imr_address.s_addr = htonl(INADDR_ANY);
  mreq.imr_ifindex = 0;
  if (zsock_setsockopt(sock, IPPROTO_IP, ZSOCK_IP_ADD_MEMBERSHIP, &mreq,
                       sizeof(mreq)) < 0) {
    const int join_errno = errno;
    zsock_close(sock);  // do not leak the descriptor on every retry
    last_bind_errno = 0;
    // Normal before the station link has a usable interface; the caller logs it.
    join_pending_errno = join_errno;
    return -1;
  }
  SSDP_LOGI("SO_REUSEADDR rc=%d, bound :%u, joined %s", reuse_rc,
            static_cast<unsigned>(kSsdpPort), kSsdpGroup);
  return sock;
}

void HandToMqtt(const char* discovered) {
  char host[64];
  uint16_t port = 0;
  if (!ParseMqttUrl(discovered, host, sizeof(host), &port)) {
    SSDP_LOGW("ignoring announcement, not an mqtt url: %s", discovered);
    return;
  }
  if (url[0] != '\0' && strcmp(url, discovered) == 0) {
    return;  // the broadcaster sends every few seconds; only act on change
  }
  if (url[0] != '\0') {
    // The old firmware rebooted here. Re-pointing the client is enough and does
    // not interrupt the rest of the product.
    SSDP_LOGI("broker moved %s -> %s", url, discovered);
  }
  strncpy(url, discovered, sizeof(url) - 1);
  url[sizeof(url) - 1] = '\0';
  SSDP_LOGI("discovered broker %s -> %s:%u", url, host,
            static_cast<unsigned>(port));
  mqtt::SetBroker(host, port);
}

void ListenerThread(void*, void*, void*) {
  int sock = -1;
  while (true) {
    if (sock < 0) {
      sock = OpenSocket();
      if (sock < 0) {
        // Do not even try until the station link is up: the group join needs a
        // configured interface, and a bind failure before that is expected noise.
        if (provision_up()) {
          if (last_bind_errno != 0) {
            SSDP_LOGW("bind :%u failed (errno %d, SO_REUSEADDR applied first)",
                      static_cast<unsigned>(kSsdpPort), last_bind_errno);
            last_bind_errno = 0;
          } else if (join_pending_errno != 0) {
            SSDP_LOGW("group join failed (errno %d)", join_pending_errno);
            join_pending_errno = 0;
          } else if (!socket_warned) {
            SSDP_LOGD("no multicast group yet, retrying");
            socket_warned = true;
          }
        }
        k_sleep(K_MSEC(kRetryMs));
        continue;
      }
      socket_warned = false;
      SSDP_LOGI("listening on %s:%u", kSsdpGroup, static_cast<unsigned>(kSsdpPort));
    }

    char buf[kRxBufLen];
    const int len = zsock_recv(sock, buf, sizeof(buf) - 1, 0);
    if (len <= 0) {
      if (len < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        SSDP_LOGW("recv failed (errno %d), reopening", errno);
        zsock_close(sock);
        sock = -1;
      }
      continue;
    }
    buf[len] = '\0';

    char candidate[kUrlMax];
    if (!ParseAnnouncement(buf, static_cast<size_t>(len), candidate,
                           sizeof(candidate))) {
      continue;  // somebody else's SSDP traffic
    }
    HandToMqtt(candidate);
  }
}

}  // namespace

bool Start() {
  if (started) {
    return true;
  }
  started = true;
  k_thread_create(&listener_thread, listener_stack,
                  K_THREAD_STACK_SIZEOF(listener_stack), ListenerThread, nullptr,
                  nullptr, nullptr, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
  k_thread_name_set(&listener_thread, "ssdp");
  return true;
}

const char* last_url() {
  static const char* kEmpty = "";
  return url[0] != '\0' ? url : kEmpty;
}

}  // namespace ssdp
}  // namespace hcp

#endif /* CONFIG_HCP_SSDP */
