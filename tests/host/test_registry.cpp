/*
 * Host unit tests for hcp::registry: the wire shape (payloads taken from the real
 * device_protocol JSONs Home Assistant sends), and the precedence / persistence
 * rules the user asked for - a re-download wins, only a change is written, a
 * payload with no segments array is ignored.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include "hcp/registry/table.h"

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

using namespace hcp::registry;

namespace {

/* Storage double that counts writes - the "no change, no flash" rule is about the
 * number of writes, not about the return value. */
class FakeStore : public KeyValue {
 public:
  bool GetString(const char* key, char* out, size_t cap) override {
    auto it = data.find(key);
    if (it == data.end()) {
      return false;
    }
    if (it->second.size() + 1 > cap) {
      return false;
    }
    strcpy(out, it->second.c_str());
    return true;
  }
  bool SetString(const char* key, const char* val) override {
    if (refuse_writes) {
      return false;
    }
    data[key] = val;
    writes++;
    return true;
  }
  bool Erase(const char* key) override {
    writes_erase++;
    return data.erase(key) > 0;
  }

  int writes = 0;
  int writes_erase = 0;
  bool refuse_writes = false;
  std::map<std::string, std::string> data;
};

/* Exactly what DDSU666.py publishes on <sn>/config (slave_id injected per plugin). */
const char kDdsu666[] =
    "{\"segments\": [{\"slave_id\": 1, \"start_address\": 8192, \"length\": 16, "
    "\"read_command\": 3}, {\"slave_id\": 1, \"start_address\": 16384, "
    "\"length\": 2, \"read_command\": 3}]}";

/* JKBMS.py: five segments, one of them 100 registers wide. */
const char kJkbms[] =
    "{\"segments\": [{\"slave_id\": 15, \"start_address\": 5120, \"length\": 20, "
    "\"read_command\": 3}, {\"slave_id\": 15, \"start_address\": 4096, "
    "\"length\": 100, \"read_command\": 3}, {\"slave_id\": 15, "
    "\"start_address\": 4296, \"length\": 42, \"read_command\": 3}, "
    "{\"slave_id\": 15, \"start_address\": 4416, \"length\": 100, "
    "\"read_command\": 3}, {\"slave_id\": 15, \"start_address\": 4196, "
    "\"length\": 35, \"read_command\": 3}]}";

Parsed Wire(const char* text, Segment* out, int* n) {
  return ParseWire(text, strlen(text), out, kMaxSegments, n);
}

Parsed Wire(const char* text, size_t len, Segment* out, int max, int* n) {
  return ParseWire(text, len, out, max, n);
}

}  // namespace

/* ---- the wire shape ---- */

TEST(RegistryWire, ParsesRealDdsu666Payload) {
  Segment segs[kMaxSegments];
  int n = 0;
  ASSERT_EQ(Wire(kDdsu666, segs, &n), Parsed::kParsed);
  ASSERT_EQ(n, 2);
  EXPECT_EQ(segs[0].slave_id, 1);
  EXPECT_EQ(segs[0].start_address, 8192);
  EXPECT_EQ(segs[0].length, 16);
  EXPECT_EQ(segs[0].read_command, 3);
  EXPECT_EQ(segs[1].start_address, 16384);
  EXPECT_EQ(segs[1].length, 2);
}

TEST(RegistryWire, ParsesWidestFieldTableInUse) {
  Segment segs[kMaxSegments];
  int n = 0;
  ASSERT_EQ(Wire(kJkbms, segs, &n), Parsed::kParsed);
  EXPECT_EQ(n, 5);
  EXPECT_EQ(segs[1].length, 100); /* under the 123 register ADU limit */
}

TEST(RegistryWire, EmptyObjectIsNotAnEmptyTable) {
  // Switches.py publishes "{}" on the same topic for handshake parity. Treating it
  // as a table would erase a working meter configuration.
  Segment segs[kMaxSegments];
  int n = 0;
  EXPECT_EQ(Wire("{}", segs, &n), Parsed::kNoSegments);
  EXPECT_EQ(Wire("{ }", segs, &n), Parsed::kNoSegments);
}

TEST(RegistryWire, ExplicitEmptyArrayIsATable) {
  Segment segs[kMaxSegments];
  int n = 99;
  ASSERT_EQ(Wire("{\"segments\": []}", segs, &n), Parsed::kParsed);
  EXPECT_EQ(n, 0);
}

TEST(RegistryWire, UnknownMembersAreSkipped) {
  // HA adds fields (name, unit, scale...) and orders them freely; nesting must not
  // leak a "length" from an inner object into the segment.
  Segment segs[kMaxSegments];
  int n = 0;
  const char* text =
      "{\"vendor\":\"x\",\"segments\":[{\"note\":{\"length\":9999},"
      "\"slave_id\":1,\"start_address\":0,\"length\":10,\"read_command\":4,"
      "\"scale\":1.0}],\"registers\":{}}";
  ASSERT_EQ(Wire(text, strlen(text), segs, kMaxSegments, &n), Parsed::kParsed);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(segs[0].length, 10);
  EXPECT_EQ(segs[0].read_command, 4);
  /* the nested note.length must not have been read as the segment's own */
  EXPECT_EQ(segs[0].start_address, 0);
}

TEST(RegistryWire, RejectsWhatItCannotRepresent) {
  Segment segs[kMaxSegments];
  int n = 0;
  EXPECT_EQ(Wire("{\"segments\":[{\"slave_id\":1,\"start_address\":0,"
                "\"length\":200,\"read_command\":3}]}", segs, &n),
            Parsed::kMalformed);                       /* over the register limit */
  EXPECT_EQ(Wire("{\"segments\":[{\"slave_id\":300,\"start_address\":0,"
                "\"length\":2,\"read_command\":3}]}", segs, &n),
            Parsed::kMalformed);                       /* unit address out of range */
  EXPECT_EQ(Wire("{\"segments\":[{\"slave_id\":1,\"start_address\":0,"
                "\"read_command\":3}]}", segs, &n),
            Parsed::kMalformed);                       /* missing length */
  EXPECT_EQ(Wire("{\"segments\":[{\"slave_id\":1,\"start_address\":0,"
                "\"length\":2.5,\"read_command\":3}]}", segs, &n),
            Parsed::kMalformed);                       /* not an integer */
  EXPECT_EQ(Wire("not json", segs, &n), Parsed::kMalformed);
  EXPECT_EQ(Wire("{\"segments\": \"nope\"}", segs, &n), Parsed::kMalformed);
}

TEST(RegistryWire, RefusesToTruncateAnOverflowedTable) {
  std::string text = "{\"segments\":[";
  for (int i = 0; i <= kMaxSegments; i++) {
    char piece[96];
    snprintf(piece, sizeof(piece), "%s{\"slave_id\":1,\"start_address\":%d,"
             "\"length\":2,\"read_command\":3}", i ? "," : "", i);
    text += piece;
  }
  text += "]}";
  Segment segs[kMaxSegments];
  int n = 0;
  EXPECT_EQ(Wire(text.c_str(), segs, &n), Parsed::kMalformed);
}

/* ---- the rules the user asked for ---- */

TEST(RegistryApply, FirstDownloadInstallsAndPersistsOnce) {
  Table t;
  FakeStore kv;
  EXPECT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv), Apply::kUpdated);
  EXPECT_EQ(t.count(), 2);
  EXPECT_EQ(t.source(), Source::kDownloaded);
  EXPECT_EQ(kv.writes, 1);
}

TEST(RegistryApply, RedownloadWinsButDoesNotRewriteWhenUnchanged) {
  Table t;
  FakeStore kv;
  ASSERT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv), Apply::kUpdated);
  EXPECT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv), Apply::kUnchanged);
  EXPECT_EQ(kv.writes, 1); /* HA re-sends on every start: no extra flash wear */
}

TEST(RegistryApply, LaterDownloadOverridesEarlierOne) {
  // The old firmware latched the first table it saw and needed a reboot to change.
  Table t;
  FakeStore kv;
  ASSERT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv), Apply::kUpdated);
  ASSERT_EQ(t.ApplyConfig(kJkbms, strlen(kJkbms), kv), Apply::kUpdated);
  EXPECT_EQ(t.count(), 5);
  EXPECT_EQ(t.at(0).slave_id, 15);
  EXPECT_EQ(kv.writes, 2);
  char blob[kMaxSegments * 24];
  ASSERT_TRUE(kv.GetString(kTableKey, blob, sizeof(blob)));
  Table reloaded;
  EXPECT_TRUE(reloaded.Deserialize(blob));
  EXPECT_TRUE(reloaded == t); /* storage holds what is running */
}

TEST(RegistryApply, EmptyObjectLeavesTheTableAlone) {
  Table t;
  FakeStore kv;
  ASSERT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv), Apply::kUpdated);
  EXPECT_EQ(t.ApplyConfig("{}", 2, kv), Apply::kIgnored);
  EXPECT_EQ(t.count(), 2);
  EXPECT_EQ(kv.writes, 1);
}

TEST(RegistryApply, ExplicitEmptyTableIsHonoured) {
  Table t;
  FakeStore kv;
  ASSERT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv), Apply::kUpdated);
  const char* empt = "{\"segments\":[]}";
  ASSERT_EQ(t.ApplyConfig(empt, strlen(empt), kv), Apply::kUpdated);
  EXPECT_EQ(t.count(), 0);
  EXPECT_FALSE(t.pollable());
  EXPECT_EQ(t.source(), Source::kDownloaded);
}

TEST(RegistryApply, MalformedKeepsCurrentTableAndWritesNothing) {
  Table t;
  FakeStore kv;
  ASSERT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv), Apply::kUpdated);
  EXPECT_EQ(t.ApplyConfig("{\"segments\":[{}]}", 15, kv), Apply::kRejected);
  EXPECT_EQ(t.count(), 2);
  EXPECT_EQ(kv.writes, 1);
}

TEST(RegistryApply, StorageRefusalIsReportedAsVolatile) {
  Table t;
  FakeStore kv;
  kv.refuse_writes = true;
  EXPECT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv),
            Apply::kUpdatedVolatile);
  EXPECT_EQ(t.count(), 2);  /* live now, gone after a reboot - and said so */
}

/* ---- boot-time precedence ---- */

TEST(RegistryLoad, PersistedTableWinsOverBuiltIn) {
  FakeStore kv;
  Table t;
  ASSERT_EQ(t.ApplyConfig(kDdsu666, strlen(kDdsu666), kv), Apply::kUpdated);

  Table after_reboot;
  EXPECT_EQ(after_reboot.Load(kv), Source::kDownloaded);
  EXPECT_TRUE(after_reboot == t);
  EXPECT_EQ(kv.writes, 1); /* Load writes nothing */
}

TEST(RegistryLoad, CorruptBlobIsErasedNotUsed) {
  FakeStore kv;
  ASSERT_TRUE(kv.SetString(kTableKey, "1:2:3;garbage"));
  Table t;
  EXPECT_EQ(t.Load(kv), Source::kEmpty); /* no built-in defaults in this build */
  EXPECT_EQ(kv.writes_erase, 1);
}

TEST(RegistryLoad, NothingStoredGivesBuiltInOrEmpty) {
  FakeStore kv;
  Table t;
  EXPECT_EQ(t.Load(kv), Source::kEmpty);
  EXPECT_FALSE(t.pollable());
}

/* ---- the storage text form ---- */

TEST(RegistrySerialize, RoundTripIsStable) {
  Table t;
  FakeStore kv;
  ASSERT_EQ(t.ApplyConfig(kJkbms, strlen(kJkbms), kv), Apply::kUpdated);
  char a[kMaxSegments * 24];
  char b[kMaxSegments * 24];
  ASSERT_GT(t.Serialize(a, sizeof(a)), 0U);
  Table u;
  ASSERT_TRUE(u.Deserialize(a));
  ASSERT_GT(u.Serialize(b, sizeof(b)), 0U);
  EXPECT_STREQ(a, b); /* stable text means a re-download compares cleanly */
  EXPECT_TRUE(u == t);
}

TEST(RegistrySerialize, RejectsTornOrOutOfRangeText) {
  Table t;
  EXPECT_FALSE(t.Deserialize("1:0:10:4;"));   /* trailing separator: torn write */
  EXPECT_FALSE(t.Deserialize("1:0:10"));      /* short field */
  EXPECT_FALSE(t.Deserialize("1:0:0:4"));     /* zero length */
  EXPECT_FALSE(t.Deserialize("248:0:10:4"));  /* unit address */
  EXPECT_FALSE(t.Deserialize(""));            /* "nothing stored" is not a table */
  EXPECT_EQ(t.count(), 0);
}
