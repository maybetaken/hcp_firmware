/*
 * hcp::registry - KeyValue over hcp::storage (settings -> NVS on target).
 * See storage_kv.h for why this is a separate file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/registry/storage_kv.h"

#ifdef __ZEPHYR__

namespace hcp {
namespace registry {

bool StorageKeyValue::GetString(const char* key, char* out, size_t cap) {
  if (!store_.Open()) {
    return false;
  }
  size_t len = 0;
  /* An empty value means "no table" (see Table::Deserialize) so it is reported as
   * absent here rather than handing the caller a zero length read to interpret. */
  return store_.GetString(key, out, cap, &len) && len > 0;
}

bool StorageKeyValue::SetString(const char* key, const char* val) {
  if (!store_.Open()) {
    return false;
  }
  return store_.SetString(key, val);
}

bool StorageKeyValue::Erase(const char* key) {
  if (!store_.Open()) {
    return false;
  }
  return store_.Erase(key);
}

}  // namespace registry
}  // namespace hcp

#endif /* __ZEPHYR__ */
