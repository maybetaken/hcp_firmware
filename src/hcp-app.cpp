/*
 * HCP application - the start-up sequence, in the order it has to happen.
 *
 * This file is deliberately only ordering plus the liveness loop. Each step is
 * where it is for a reason that is easy to lose and expensive to rediscover:
 *
 *   banner via printk           the log ring is 1024 B and the boot burst fills it
 *                               before the log thread drains anything, which is
 *                               how the build identity used to be dropped
 *   watchdog before everything  a firmware that dies on the way up still gets
 *                               reverted by MCUboot instead of staying dead
 *   identity before the network  the serial number is the MQTT client-id, the
 *                               topic prefix and the DHCP hostname, and it
 *                               travels in the very first DISCOVER
 *   BLE hooks before provisioning  hcp::provision drives its reports through them
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
  /* Best-effort early identity, straight to the console. Two separate things can
   * swallow the first lines of a boot: the 1024 B log ring fills before the log
   * thread drains anything (Zephyr reports "N messages dropped" - measured, and
   * why these lines do not go through hcp::log), and the USB-Serial-JTAG console
   * is not host-attached yet when the earliest writes happen. The second one is
   * not fixable from here: two boots of the same image, captured with the port
   * opened before reset, gave 3/3 lines and then 0/3 - even Zephyr's own banner
   * went missing. So "which build is this" also has to be readable somewhere the
   * console cannot eat it: hcp::ota logs it at INFO, and <sn>/online carries it to
   * the broker. */
  printk("*** HCP (Helios Collector Platform) ***\n");
  printk("hcp: build %s %s\n", __DATE__, __TIME__);

#ifdef CONFIG_HCP_OTA
  /* First thing, before any subsystem that can block or fail: if MCUboot still
   * treats this image as a trial, put a hardware deadline on it. Nothing ever
   * feeds that deadline; hcp::ota cancels it when the image is confirmed. */
  hcp::ota::GuardTrialImage();
#endif

#ifdef CONFIG_HCP_IDENTITY
  /* wifi::Init() only binds the interface and registers callbacks; it is
   * idempotent, and the BluFi path calls it again below. */
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
  /* No provisioning state machine in this build: advertise BluFi and wait for a
   * phone, using the hand-rolled path in app_blufi.cpp. */
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
