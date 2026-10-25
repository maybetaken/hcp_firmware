/*
 * HCP application - the start-up sequence, in the order it has to happen. This file is
 * deliberately only ordering plus the liveness loop; each position is load-bearing:
 *
 *   printk banner              the 1024 B log ring fills before the log thread drains
 *   watchdog / trial guard     a firmware that dies on the way up still gets reverted
 *   identity before network    the serial number is the MQTT client-id, topic prefix,
 *                              DHCP hostname and the first DISCOVER payload
 *   BLE hooks before provisioning  the state machine drives its reports through them
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp-app.h"

#include <zephyr/sys/printk.h>

#include "hcp/log/log.h"
#include "hcp/kernel/kernel.h"
#include "hcp/net/wifi.h"
#ifdef CONFIG_HCP_IDENTITY
#include "hcp/identity/identity.h"
#endif
#ifdef CONFIG_HCP_OTA
#include "hcp/ota/ota.h"
#endif

namespace hcp {

void App::Start() {
  /* Not through hcp::log: the ring is 1024 B and the boot burst fills it before the log
   * thread runs ("N messages dropped"). The earliest lines can still be lost because
   * USB-Serial-JTAG is not host-attached yet, which is why the version is also logged at
   * INFO by hcp::ota and carried on <sn>/online. */
  printk("*** HCP (Helios Collector Platform) ***\n");
  printk("hcp: build %s %s\n", __DATE__, __TIME__);
#ifdef CONFIG_HCP_BENCH
  /* This image runs the bring-up aids with debug logging and is not what ships - hence
   * the noise. It keeps provisioning, MQTT and OTA so it can be upgraded over the
   * network instead of plugged into USB again. */
  printk("hcp: *** BENCH BUILD - not a deployment image ***\n");
#endif

#ifdef CONFIG_HCP_OTA
  /* Before anything that can block or fail: if MCUboot still treats this image as a
   * trial, put a hardware deadline on it. Nothing feeds it; hcp::ota cancels it on
   * confirmation. */
  hcp::ota::GuardTrialImage();
#endif

#ifdef CONFIG_HCP_IDENTITY
  /* Only binds the interface and registers callbacks; idempotent, and the BluFi path
   * calls it again below. */
  hcp::net::wifi::Init();
  hcp::identity::Init();
#endif

  aids_.Start();

#ifdef CONFIG_BLUFI
  phone_.Init();
#endif

#ifdef CONFIG_HCP_PROVISION
  services_.Start();
#elif defined(CONFIG_BLUFI)
  /* No provisioning state machine in this build: advertise BluFi and wait for a phone. */
  phone_.StartStandalone();
#endif

  AliveLoop();
}

void App::AliveLoop() {
  while (true) {
    HCP_LOGD(hcp::log::kDomainDevice, "alive, uptime %llu ms",
             static_cast<unsigned long long>(kernel::time::UptimeMs()));
    kernel::time::DelayMs(1000);
  }
}

}  // namespace hcp
