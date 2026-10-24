/*
 * HCP application - internal split of what used to be main.cpp.
 *
 * The application is one object (hcp::App, instantiated exactly once in
 * main.cpp). What it does on the way up lives in one file per concern, all
 * declared here:
 *
 *   app_bringup.cpp   the kernel self-test that proves the boot is alive
 *   app_demos.cpp     optional bring-up aids, each behind its own CONFIG_*_DEMO
 *   app_blufi.cpp     the standalone BluFi provisioning path (no hcp::provision)
 *   app_services.cpp  the running product: MQTT commands, SSDP, collector, OTA
 *
 * Nothing here is public API: this header belongs to src/ only. The order the
 * pieces are called in is documented in hcp-app.cpp, because it is the reason
 * several of them exist at all (identity before the network, trial watchdog
 * before anything that can block, and so on).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

namespace hcp {
namespace app {

/* ---- bring-up self test (app_bringup.cpp) ---- */

// Exercises hcp::kernel (Thread, MessageQueue, Timer) and the log domain setup.
// Returns once the producer has drained its queue, which is also the point where
// "the console is really working" is established.
void RunSelfTest();

// Never returns: the 1 s liveness line that makes the boot observable after a
// reset, without a debugger attached.
void AliveLoop();

/* ---- optional demos (app_demos.cpp) ---- */

// Starts whichever bring-up threads the build turned on (LED, Modbus polling,
// serial loopback, raw Wi-Fi). Also binds the button when hcp::provision is off -
// with provisioning on, that module owns the button and the LED.
void StartDemos();

/* ---- standalone BluFi provisioning (app_blufi.cpp) ---- */

#ifdef CONFIG_BLUFI

// Registers the Wi-Fi callback, the scan/connect work items and the BluFi data
// handler. Called before the provisioning state machine starts, because the state
// machine drives the same reports through these hooks.
void BlufiInit();

// No hcp::provision in this build: advertise BluFi and wait for a phone, with the
// hand-rolled connect/report sequence this file carries.
void BlufiStartStandalone();

#endif  // CONFIG_BLUFI

/* ---- the running product (app_services.cpp) ---- */

#ifdef CONFIG_HCP_PROVISION
// provisioning state machine, then MQTT session, SSDP discovery, the collector
// queue/worker and the OTA path - in that order, for the reasons noted there.
void StartServices();
#endif  // CONFIG_HCP_PROVISION

}  // namespace app
}  // namespace hcp
