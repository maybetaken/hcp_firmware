/*
 * Helios Collector Platform (HCP) — entry point.
 *
 * One instance of the application, started, and nothing else. The sequence is in
 * hcp-app.cpp; the pieces of it live in app_bringup.cpp, app_demos.cpp,
 * app_blufi.cpp and app_services.cpp. This file stays four lines of code on
 * purpose: an entry point that cannot grow is one that cannot rot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp-app.h"

static hcp::App g_app;

int main(void) {
  g_app.Start();
  return 0; /* not reached: Start() ends in the liveness loop */
}
