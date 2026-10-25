/*
 * hcp::registry - the KeyValue seam implemented over hcp::storage.
 *
 * The point table logic is pure (see table.h); this is the one piece that touches
 * flash, kept separate so the rules can still be tested with a fake on the host.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "hcp/registry/table.h"

#ifdef __ZEPHYR__

#include "hcp/storage/store.h"

namespace hcp {
namespace registry {

/* settings-backed key/value store, rooted at hcp::storage's default root.
 * One instance per process is enough; it holds no state of its own. */
class StorageKeyValue : public KeyValue {
 public:
  bool GetString(const char* key, char* out, size_t cap) override;
  bool SetString(const char* key, const char* val) override;
  bool Erase(const char* key) override;

 private:
  storage::Store store_;
};

}  // namespace registry
}  // namespace hcp

#endif /* __ZEPHYR__ */
