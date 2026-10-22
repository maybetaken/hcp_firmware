/*
 * hcp::ota - firmware updates for the product.
 *
 * Thin application-side wiring around the reusable ota_client module
 * (modules/lib/ota_client): it turns "install an update" into a download on its
 * own thread, and decides when a trial image has earned the right to stay.
 * Everything transport specific or bootloader specific lives over there.
 *
 * Where the pieces come from:
 *   image + version file - the templates in Kconfig, with %s filled by the host
 *     hcp::ssdp last heard from Home Assistant (the same box that runs the
 *     broker), or a base URL given explicitly on the command;
 *   keep or discard - MCUboot's trial swap. The image becomes permanent only
 *     once this device has a live broker session, so a firmware that boots but
 *     cannot reach the network is rolled back instead of confirmed. That is the
 *     deliberate difference from "rebooted once, therefore good".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>

namespace hcp {
namespace ota {

// Start the update worker (idempotent). Returns false when the thread or the
// client could not be set up; every other entry point then reports false too.
bool Start();

// Queue an update.
//
// url == nullptr builds both endpoints from the configured templates and the
// discovered host. Otherwise url must be a *base* URL ending in '/', and
// "version.txt" plus "firmware.bin" are appended to it - an update is always
// version-gated, so a bare image URL is refused rather than installed blind.
//
// Returns false when the worker is busy, no host has been discovered yet, or the
// URL is unusable.
bool Trigger(const char* url = nullptr);

// Handle a command payload: "ota" or "ota <base-url>". Returns true when the
// payload was an OTA command (and therefore consumed), false when it belongs to
// somebody else.
bool HandleCommand(const char* payload, size_t len);

// Library state as one word: "idle", "checking", "updating",
// "pending-reboot", "error". "" before Start().
const char* StateStr();

// Status for the telemetry topic: empty string when no update has ever been
// attempted, otherwise "<state> <received>/<total> <version>" (total -1 when the
// server did not say). Returns the length written, excluding the NUL.
size_t StatusText(char* out, size_t cap);

}  // namespace ota
}  // namespace hcp
