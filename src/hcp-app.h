/*
 * Helios Collector Platform (HCP) — the application object.
 *
 * One instance exists, in main.cpp. It owns the order in which the product
 * starts, which is the part worth keeping in one place and reading in one
 * screen; everything else lives in the app_*.cpp files next to it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

namespace hcp {

class App {
 public:
  /* Brings the device up and does not return: the boot self-test, the optional
   * demos, provisioning, the network services, then the liveness loop. */
  void Start();
};

}  // namespace hcp
