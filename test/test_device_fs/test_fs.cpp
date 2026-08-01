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
// jsonXxx.txt files are never opened, and the last test asserts that nothing
// is left behind. See the note further down on why filling the partition is
// deliberately NOT done here.

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

// Returns whether the path is gone afterwards. The result is checked by the
// last test rather than ignored: the first run of this suite left all three
// scratch files behind and nothing failed, because every cleanup call
// discarded its result.
static bool removeIfPresent(const char* path) {
  if (!LittleFS.exists(path)) return true;
  LittleFS.remove(path);
  return !LittleFS.exists(path);
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

// ============================================================
// NOT TESTED HERE: the full-filesystem short write
//
// An earlier version of this suite filled the partition to 100% and checked
// that saveJsonDocSafe() detected the short write. It passed - and left
// three scratch files on the production controller, twice.
//
// The reason is worth recording, because it is a property of LittleFS rather
// than a bug in the test: a genuinely full filesystem cannot commit ANY
// metadata, and a delete is a metadata write. Once full, LittleFS.remove()
// cannot complete. Worse, LittleFS.exists() then answers from its in-RAM
// view and reports the file gone, so the cleanup asserted successfully while
// flash was never touched. After a reboot the whole partition reverted to
// its state part-way through the fill: the filler back at 0 bytes and the
// scratch config files back with their pre-cleanup contents.
//
// So the cleanup cannot be made reliable from inside the test - the test is
// what removed the ability to clean up. Filling a real partition to 100% is
// not a safe thing to do to a device that has to save its config on the next
// boot, so this belongs in test_native_config, where fsTestSetWriteLimit()
// reaches the same branch with no hardware at stake. What the native harness
// genuinely cannot prove - that flash persists, that close() commits, that
// the fallback works against real files - is covered above.
// ============================================================

// Belt and braces: the partition must be left with room for the real
// firmware to save its config on the next boot.
void test_the_partition_is_left_with_free_space(void) {
  TEST_ASSERT_GREATER_THAN_UINT32(64u * 1024u, freeBytes());
}

// The suite must leave the filesystem as it found it. An assertion rather
// than a tidy-up: when the earlier fill test left scratch files behind,
// every test still passed and nothing said so. FILLER is included even
// though nothing writes it any more, so a repeat run clears the residue the
// removed test left on this device.
void test_no_scratch_files_are_left_behind(void) {
  TEST_ASSERT_TRUE(removeIfPresent(PRIMARY));
  TEST_ASSERT_TRUE(removeIfPresent(BACKUP));
  TEST_ASSERT_TRUE(removeIfPresent(FILLER));
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

  RUN_TEST(test_the_partition_is_left_with_free_space);
  RUN_TEST(test_no_scratch_files_are_left_behind);

  UNITY_END();

  // Unmount cleanly so nothing is left half-committed when the board is
  // reset by the next upload.
  LittleFS.end();
}

void loop() {}
