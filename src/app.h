/*
 * HCP application - internal split of what used to be main.cpp.
 *
 * The application is one object (hcp::App, instantiated exactly once in
 * main.cpp). What it does on the way up lives in one file per concern, all
 * declared here:
 *
 *   app_bringup.cpp   the liveness line that outlives every other subsystem
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

/* ---- boot liveness (app_bringup.cpp) ---- */

// Never returns: the 1 s liveness line at debug level, kept because it is the
// output that continues when everything else has stopped working. See the note in
// app_bringup.cpp about why the P0 self-test that used to sit here is gone.
void AliveLoop();

/* ---- optional bring-up aids (app_demos.cpp) ---- */

// Starts whichever bench aids the build turned on (Modbus poll, serial loopback,
// raw Wi-Fi), and binds the button when hcp::provision is absent - with
// provisioning on, that module owns the button and the status LED.
void StartBringupAids();

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
