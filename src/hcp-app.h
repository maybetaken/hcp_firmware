/*
 * Helios Collector Platform (HCP) — the application object.
 *
 * One instance exists, in main.cpp. It owns the start-up order (hcp-app.cpp) and
 * one instance of each collaborator the order needs; the collaborators themselves
 * are in app.h next to their implementations.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "app.h"

namespace hcp {

class App {
 public:
  /* Brings the device up and does not return: the bring-up aids, provisioning,
   * the network services, then the liveness loop. */
  void Start();

 private:
  /* The 1 s liveness line, at debug level. Kept because it is the output that
   * continues after everything else has failed - and nothing more than that: the
   * P0 self-test that used to sit here (queue, timer, "got 1..5") is gone, see
   * the note in hcp-app.cpp about what it was accidentally hiding. */
  void AliveLoop();

  app::BringupAids aids_;
  app::Services services_;
#ifdef CONFIG_BLUFI
  app::PhoneProvisioner phone_;
#endif
};

}  // namespace hcp
