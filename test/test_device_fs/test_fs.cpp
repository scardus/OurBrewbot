// On-device (hardware) tests for the persistence layer in
// OurBrewbot/Config.cpp against the real LittleFS partition.
//
// `pio test -e nodemcuv2_test --upload-port COM6` REPLACES the running
// firmware for the duration of the run. Only do this when the fermenter
// isn't mid-batch, and never without explicit go-ahead.
//
// WHY THIS CANNOT BE A NATIVE TEST
// test_native_config covers the same functions against the in-memory
// filesystem in test/stubs/LittleFS.h, whose failure modes are simulated:
// fsTestSetFull() makes open() fail and fsTestSetWriteLimit() makes a write
// come up short, both because the harness was told to. Neither proves that a
// genuinely full 2 MB partition behaves that way - the short return from
// serializeJson() is the ONLY thing standing between a full filesystem and a
// half-written config file that still parses, and on the stub that return
// value is fabricated. Nor can a host test show that a file written through
// close() is really on flash rather than in a buffer that a reset discards.
//
// SAFETY
// Every file this suite touches is a /test_* scratch name; the production
// jsonXxx.txt files are never opened. The fill test runs LAST, caps its own
// iterations, and removes its file in the same test rather than relying on
// tearDown - a fill left behind would stop the real firmware saving config
// on the next boot.

#include <Arduino.h>
#include <unity.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "../../OurBrewbot/Log.h"

// ---- log sink: these tests assert on return values, not log text ----
void logMsgImpl(uint8_t, PGM_P, ...) {}

// Config.cpp is #included directly (not linked) so the real production
// source is what runs. It also defines every g_* config global, so nothing
// else has to.
#include "../../OurBrewbot/Config.cpp"

static const char* const PRIMARY = "/test_cfg.txt";
static const char* const BACKUP  = "/test_cfgbkup.txt";
static const char* const FILLER  = "/test_fill.bin";

// ---- helpers ----

static void removeIfPresent(const char* path) {
  if (LittleFS.exists(path)) LittleFS.remove(path);
}

static bool writeRaw(const char* path, const char* contents) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  f.write(reinterpret_cast<const uint8_t*>(contents), strlen(contents));
  f.close();
  return true;
}

static size_t fileSize(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

static uint32_t freeBytes() {
  FSInfo info;
  LittleFS.info(info);
  return info.totalBytes - info.usedBytes;
}

void setUp(void) {
  removeIfPresent(PRIMARY);
  removeIfPresent(BACKUP);
}

void tearDown(void) {
  removeIfPresent(PRIMARY);
  removeIfPresent(BACKUP);
  removeIfPresent(FILLER);
}

// ============================================================
// Mount + basic persistence
// ============================================================

// Guards every other test here: without a mounted filesystem the rest would
// fail for the wrong reason.
void test_the_filesystem_mounts(void) {
  TEST_ASSERT_TRUE(LittleFS.begin());
  FSInfo info;
  TEST_ASSERT_TRUE(LittleFS.info(info));
  TEST_ASSERT_GREATER_THAN_UINT32(0, info.totalBytes);
}

void test_save_writes_both_primary_and_backup(void) {
  JsonDocument doc;
  doc["unit"]  = 1;
  doc["floor"] = 18.5;

  TEST_ASSERT_TRUE(saveJsonDocSafe(doc, PRIMARY, BACKUP));
  TEST_ASSERT_TRUE(LittleFS.exists(PRIMARY));
  TEST_ASSERT_TRUE(LittleFS.exists(BACKUP));
  TEST_ASSERT_EQUAL_size_t(fileSize(PRIMARY), fileSize(BACKUP));
}

void test_values_round_trip_through_flash(void) {
  JsonDocument out;
  out["unit"]  = 1;
  out["floor"] = 18.5;
  TEST_ASSERT_TRUE(saveJsonDocSafe(out, PRIMARY, BACKUP));

  JsonDocument in;
  TEST_ASSERT_TRUE(loadJsonDocSafe(in, PRIMARY, BACKUP));
  TEST_ASSERT_EQUAL_INT(1, in["unit"].as<int>());
  TEST_ASSERT_EQUAL_FLOAT(18.5f, in["floor"].as<float>());
}

// close() has to have committed the data to flash, not left it in a buffer
// that a reset would drop. Remounting is the closest thing to a power cycle
// available from inside a test.
void test_data_survives_a_remount(void) {
  JsonDocument out;
  out["unit"] = 1;
  TEST_ASSERT_TRUE(saveJsonDocSafe(out, PRIMARY, BACKUP));

  LittleFS.end();
  TEST_ASSERT_TRUE(LittleFS.begin());

  JsonDocument in;
  TEST_ASSERT_TRUE(loadJsonDocSafe(in, PRIMARY, BACKUP));
  TEST_ASSERT_EQUAL_INT(1, in["unit"].as<int>());
}

// ============================================================
// Primary/backup fallback
// ============================================================

void test_load_falls_back_when_the_primary_is_missing(void) {
  JsonDocument out;
  out["unit"] = 7;
  TEST_ASSERT_TRUE(saveJsonDocSafe(out, PRIMARY, BACKUP));
  LittleFS.remove(PRIMARY);

  JsonDocument in;
  TEST_ASSERT_TRUE(loadJsonDocSafe(in, PRIMARY, BACKUP));
  TEST_ASSERT_EQUAL_INT(7, in["unit"].as<int>());
}

void test_load_falls_back_when_the_primary_is_corrupt(void) {
  JsonDocument out;
  out["unit"] = 7;
  TEST_ASSERT_TRUE(saveJsonDocSafe(out, PRIMARY, BACKUP));
  TEST_ASSERT_TRUE(writeRaw(PRIMARY, "{\"unit\":"));   // truncated mid-object

  JsonDocument in;
  TEST_ASSERT_TRUE(loadJsonDocSafe(in, PRIMARY, BACKUP));
  TEST_ASSERT_EQUAL_INT(7, in["unit"].as<int>());
}

// A partly-parsed primary populates the document before it fails. Without
// the doc.clear() before the backup attempt, those keys would survive and be
// merged into whatever the backup provides - a config half from each file.
void test_keys_from_a_half_parsed_primary_do_not_leak(void) {
  JsonDocument out;
  out["unit"] = 7;
  TEST_ASSERT_TRUE(saveJsonDocSafe(out, PRIMARY, BACKUP));
  TEST_ASSERT_TRUE(writeRaw(PRIMARY, "{\"ghost\":42,\"unit\":"));

  JsonDocument in;
  TEST_ASSERT_TRUE(loadJsonDocSafe(in, PRIMARY, BACKUP));
  TEST_ASSERT_EQUAL_INT(7, in["unit"].as<int>());
  TEST_ASSERT_TRUE(in["ghost"].isNull());
}

void test_load_fails_when_both_copies_are_gone(void) {
  JsonDocument in;
  TEST_ASSERT_FALSE(loadJsonDocSafe(in, PRIMARY, BACKUP));
}

void test_load_fails_when_both_copies_are_corrupt(void) {
  TEST_ASSERT_TRUE(writeRaw(PRIMARY, "not json at all"));
  TEST_ASSERT_TRUE(writeRaw(BACKUP,  "also not json"));

  JsonDocument in;
  TEST_ASSERT_FALSE(loadJsonDocSafe(in, PRIMARY, BACKUP));
}

// ============================================================
// A genuinely full filesystem - runs last, cleans up in-test
// ============================================================

// Fill to within `headroom` bytes of full. Returns the number of blocks
// written; capped so a miscalculation can't loop forever.
//
// Two chunk sizes, and both are needed. LittleFS erases a whole 4 KB block
// per write, so filling 2 MB in 512 B pieces pays that erase eight times for
// the same data - it took over five minutes. Writing in 4 KB pieces is far
// faster but overshoots: the loop stops as soon as one write comes up short,
// which can leave several KB free, and the save under test would then
// succeed. So: 4 KB until the partition is nearly full, then 512 B to close
// the gap to `headroom`.
static int fillFilesystem(uint32_t headroom) {
  static uint8_t block[4096];
  memset(block, 'F', sizeof(block));

  File f = LittleFS.open(FILLER, "w");
  if (!f) return 0;

  const uint32_t COARSE_UNTIL = 64u * 1024u;
  const int      MAX_PASSES   = 8192;   // ceiling: more passes than can fit
  bool coarse  = true;
  int  written = 0;

  // Bounded by the loop counter, not by the write result: a `continue` on a
  // failed coarse write must not be able to spin forever and trip the
  // watchdog.
  for (int pass = 0; pass < MAX_PASSES; pass++) {
    const uint32_t remaining = freeBytes();
    if (remaining <= headroom) break;

    const size_t chunk = (coarse && remaining > COARSE_UNTIL) ? sizeof(block) : 512;
    if (f.write(block, chunk) != chunk) {
      if (chunk == 512) break;          // genuinely full
      coarse = false;                   // too big to fit; finish fine-grained
      continue;
    }
    written++;
    yield();
  }
  f.close();
  return written;
}

// Two things at once, on one fill: that the save FAILS against a genuinely
// full partition rather than silently truncating, and that the last good
// primary is still loadable afterwards. saveJsonDocSafe writes the BACKUP
// first and returns early if that fails, specifically so the primary
// survives - which only means anything if the failure is detected at all.
//
// Filling 2 MB takes a few seconds, so this is deliberately one test rather
// than two: a second fill would double the runtime for no extra coverage.
void test_a_full_filesystem_fails_the_save_and_spares_the_previous_copy(void) {
  JsonDocument good;
  good["unit"] = 3;
  TEST_ASSERT_TRUE(saveJsonDocSafe(good, PRIMARY, BACKUP));
  const uint32_t sizeBefore = (uint32_t)fileSize(PRIMARY);

  // Comfortably larger than any slack the fill can leave behind, so "the
  // save failed" can't be an artefact of the document happening to fit.
  JsonDocument big;
  for (int i = 0; i < 120; i++) {
    char key[8];
    snprintf(key, sizeof(key), "k%d", i);
    big[key] = 1234567890;
  }

  TEST_ASSERT_GREATER_THAN_INT(0, fillFilesystem(256));
  const bool saved = saveJsonDocSafe(big, PRIMARY, BACKUP);
  removeIfPresent(FILLER);

  const uint32_t sizeAfter = (uint32_t)fileSize(PRIMARY);
  JsonDocument in;
  const bool stillLoads = loadJsonDocSafe(in, PRIMARY, BACKUP);
  const int  unit       = in["unit"] | -1;

  // Clean up before asserting, so a failed assertion can't leave the
  // partition full for the production firmware that boots next.
  removeIfPresent(PRIMARY);
  removeIfPresent(BACKUP);

  TEST_ASSERT_FALSE(saved);
  TEST_ASSERT_EQUAL_UINT32(sizeBefore, sizeAfter);
  TEST_ASSERT_TRUE(stillLoads);
  TEST_ASSERT_EQUAL_INT(3, unit);
}

// Belt and braces: whatever happened above, the partition must be left with
// room for the real firmware to save its config on the next boot.
void test_the_partition_is_left_with_free_space(void) {
  removeIfPresent(FILLER);
  TEST_ASSERT_GREATER_THAN_UINT32(64u * 1024u, freeBytes());
}

void setup() {
  delay(2000);  // let the board settle after upload before tests start

  LittleFS.begin();

  UNITY_BEGIN();

  RUN_TEST(test_the_filesystem_mounts);
  RUN_TEST(test_save_writes_both_primary_and_backup);
  RUN_TEST(test_values_round_trip_through_flash);
  RUN_TEST(test_data_survives_a_remount);

  RUN_TEST(test_load_falls_back_when_the_primary_is_missing);
  RUN_TEST(test_load_falls_back_when_the_primary_is_corrupt);
  RUN_TEST(test_keys_from_a_half_parsed_primary_do_not_leak);
  RUN_TEST(test_load_fails_when_both_copies_are_gone);
  RUN_TEST(test_load_fails_when_both_copies_are_corrupt);

  RUN_TEST(test_a_full_filesystem_fails_the_save_and_spares_the_previous_copy);
  RUN_TEST(test_the_partition_is_left_with_free_space);

  UNITY_END();
}

void loop() {}
