/*
 * HCP application - the start-up sequence, in the order it has to happen.
 *
 * This file is deliberately only ordering. Each step is where it is for a reason
 * that is easy to lose and expensive to rediscover, so the reasons are written
 * next to the calls rather than in a design document:
 *
 *   watchdog before anything else   a firmware that dies on the way up still
 *                                   gets reverted by MCUboot, which is the whole
 *                                   point of the trial-swap scheme
 *   identity before the network     the serial number is the MQTT client-id, the
 *                                   topic prefix and the DHCP hostname; setting
 *                                   it after a connect waits for a renew
 *   self-test before the services   it is also the point where "the console
 *                                   really works" is established
 *   BLE hooks before provisioning   hcp::provision drives its reports through
 *                                   them
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp-app.h"

#include "app.h"

#include "hcp/log/log.h"
#include "hcp/net/wifi.h"
#ifdef CONFIG_HCP_IDENTITY
#include "hcp/identity/identity.h"
#endif
#ifdef CONFIG_HCP_OTA
#include "hcp/ota/ota.h"
#endif

namespace hcp {

void App::Start() {
  HCP_LOGI(hcp::log::kDomainDevice, "*** HCP (Helios Collector Platform) P0 bring-up ***");
  HCP_LOGI(hcp::log::kDomainDevice, "build %s %s", __DATE__, __TIME__);

#ifdef CONFIG_HCP_OTA
  /* First thing, before any subsystem that can block or fail: if MCUboot still
   * treats this image as a trial, put a hardware deadline on it. Nothing ever
   * feeds that deadline; hcp::ota cancels it when the image is confirmed. Being
   * here this early is the point - a new firmware that dies on the way up still
   * gets reverted instead of staying dead. */
  hcp::ota::GuardTrialImage();
#endif

#ifdef CONFIG_HCP_IDENTITY
  /* Resolve the device identity before anything touches the network: the serial
   * number is both the MQTT client-id/topic prefix and the DHCP hostname, and the
   * name travels in the very first DISCOVER - setting it after a connect would
   * wait for a renew to show up on the router. wifi::Init() only binds the
   * interface and registers callbacks (hcp::net::wifi::Init is idempotent, the
   * BluFi path calls it again). */
  hcp::net::wifi::Init();
  hcp::identity::Init();
#endif

  app::RunSelfTest();
  app::StartDemos();

#ifdef CONFIG_BLUFI
  app::BlufiInit();
#endif

#ifdef CONFIG_HCP_PROVISION
  app::StartServices();
#elif defined(CONFIG_BLUFI)
  /* No provisioning state machine in this build: advertise BluFi and wait for a
   * phone, using the hand-rolled path in app_blufi.cpp. */
  app::BlufiStartStandalone();
#endif

  app::AliveLoop();
}

}  // namespace hcp
