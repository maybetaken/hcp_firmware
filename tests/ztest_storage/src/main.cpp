/*
 * ztest for the hcp::storage Zephyr (settings) backend, on native_sim with the
 * SETTINGS_NONE backend (KV/cache + settings API wiring). Google style naming.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/ztest.h>

#include <string.h>

#include "hcp/storage/store.h"

using hcp::storage::Store;

ZTEST(hcp_storage, string_roundtrip) {
  Store s;
  zassert_true(s.Open(), "open");
  zassert_true(s.SetString("sn", "HELIOS-001"), "set string");
  char buf[32] = {};
  zassert_true(s.GetString("sn", buf, sizeof(buf)), "get string");
  zassert_true(strcmp(buf, "HELIOS-001") == 0, "value match");
  zassert_true(s.Exists("sn"), "exists");
}

ZTEST(hcp_storage, int_roundtrip) {
  Store s;
  zassert_true(s.Open(), "open");
  zassert_true(s.SetInt("baud", 115200), "set int");
  int32_t v = 0;
  zassert_true(s.GetInt("baud", v), "get int");
  zassert_equal(v, 115200, "value match");
}

ZTEST(hcp_storage, get_missing_fails) {
  Store s;
  zassert_true(s.Open(), "open");
  s.Erase("absent");
  char buf[16];
  zassert_false(s.GetString("absent", buf, sizeof(buf)), "missing get");
  zassert_false(s.Exists("absent"), "missing exists");
}

ZTEST(hcp_storage, erase_removes_in_session) {
  Store s;
  zassert_true(s.Open(), "open");
  zassert_true(s.SetString("tmp", "x"), "set");
  zassert_true(s.Exists("tmp"), "exists after set");
  s.Erase("tmp");
  zassert_false(s.Exists("tmp"), "gone after erase");
}

ZTEST_SUITE(hcp_storage, NULL, NULL, NULL, NULL, NULL);
