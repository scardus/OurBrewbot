/*
 * Copyright 2026 Sean Cardus
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Native (host) tests for OurBrewbot/Config.cpp - the persistence layer.
//
// Config.cpp is #included directly so the real production source is what runs,
// against the in-memory filesystem in test/stubs/LittleFS.h. That stub is what
// makes this suite possible: every branch below either reads a config file the
// test staged by hand, or asserts on the bytes a save wrote back.
//
// The emphasis is on the code that is effectively untestable any other way -
// the boot-time migrations. Each one runs once, on the first load after an
// upgrade, and silently rewrites persisted state; getting one wrong corrupts a
// user's config with no error output. Reproducing them manually would mean
// hand-editing files on a live device's LittleFS between reboots.
//
// Unlike the other suites this file does NOT define the g_* config globals -
// Config.cpp owns them.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#include "../../OurBrewbot/Config.h"

// ---- millis(), settable per test (Config.cpp never calls it, but Arduino.h
// ---- declares it and other suites share the stub) ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- the uptime clock (Uptime.cpp), set directly by the config import tests ----
static uint32_t s_uptimeSec = 0;
uint32_t uptimeSeconds() { return s_uptimeSec; }
uint32_t uptimeStamp()   { return s_uptimeSec ? s_uptimeSec : 1; }
uint32_t uptimeSince(uint32_t stamp) { return (s_uptimeSec > stamp) ? s_uptimeSec - stamp : 0; }

// ---- log capture: the last line, for the config import messages ----
static char s_lastLog[192];
void logMsgImpl(uint8_t, PGM_P fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_lastLog, sizeof(s_lastLog), fmt, args);
  va_end(args);
}

// The code under test.
#include "../../OurBrewbot/TextSafe.cpp"
#include "../../OurBrewbot/Config.cpp"
#include "../../OurBrewbot/OriginalConfig.cpp"

// ---- test fixture ----

void setUp(void) {
  fsTestReset();
  espTestSetResetReason(REASON_DEFAULT_RST);
  g_espFreeHeap = 24000;
  memset(&g_globalConfig, 0, sizeof(g_globalConfig));
  for (int i = 0; i < MAX_FERMENTERS; i++)   g_fermenters[i]   = FermenterConfig{};
  for (int i = 0; i < MAX_PROBES; i++)       g_probes[i]       = ProbeConfig{};
  for (int i = 0; i < MAX_SMART_PLUGS; i++)  g_smartPlugs[i]   = SmartPlugConfig{};
  for (int i = 0; i < MAX_PROFILES; i++)     g_profiles[i]     = ProfileConfig{};
  memset(g_profileSteps, 0, sizeof(g_profileSteps));
  // NOT TiltConfig{}: "unconfigured slot" is colour == PROBE_UNASSIGNED (99),
  // not 0, and 0 is a real colour (TILT_RED). Zero-initialising would leave all
  // eight colours looking configured, so saveTiltConfig() would pack four
  // phantom red slots. initDefaultTiltConfig() sets the sentinel, which is also
  // the state the firmware actually boots into.
  initDefaultTiltConfig();
  for (int i = 0; i < MAX_ISPINDELS; i++)    g_iSpindels[i]    = iSpindelConfig{};
  for (int i = 0; i < MAX_BREW_SERVICES; i++) g_brewServices[i] = BrewServiceConfig{};
  memset(&g_mqttConfig, 0, sizeof(g_mqttConfig));
  memset(&g_syslogConfig, 0, sizeof(g_syslogConfig));
  s_importActive    = false;
  s_importLastWrite = 0;
  s_unsavedFiles    = 0;
  s_fsMountAttempts = 0;
  s_fsFormatted     = false;
  s_uptimeSec       = 1000;
  s_lastLog[0]      = '\0';
}

void tearDown(void) {}

// Count occurrences of a substring - used to check array lengths and how many
// entries a log file ended up with.
static int countOf(const char* haystack, const char* needle) {
  int n = 0;
  for (const char* p = strstr(haystack, needle); p; p = strstr(p + 1, needle)) n++;
  return n;
}

static bool fileHas(const char* path, const char* fragment) {
  return strstr(fsTestRead(path), fragment) != nullptr;
}

// A fermenter config file with one slot's temp trio set explicitly and the
// other three left at their (valid) table defaults.
static void stageFermenterTrio(float ceiling, float floorT, float hyst) {
  char json[256];
  snprintf(json, sizeof(json),
           "{\"CeilingTemp\":[%.2f,22,22,22],"
           "\"FloorTemp\":[%.2f,18,18,18],"
           "\"Hysteresis\":[%.2f,0.5,0.5,0.5],"
           "\"BrewServices\":[0,0,0,0]}",
           ceiling, floorT, hyst);
  fsTestWrite(FILE_FERMENTER, json);
}

// ============================================================
// loadJsonDocSafe / saveJsonDocSafe - the primary/backup pair
// ============================================================

void test_load_uses_the_primary_when_it_is_valid(void) {
  fsTestWrite(FILE_SYSLOG,     "{\"port\":1000}");
  fsTestWrite(FILE_SYSLOG_BKP, "{\"port\":2000}");
  TEST_ASSERT_TRUE(loadSyslogConfig());
  TEST_ASSERT_EQUAL_UINT16(1000, g_syslogConfig.port);
}

void test_load_falls_back_to_the_backup_when_the_primary_is_missing(void) {
  fsTestWrite(FILE_SYSLOG_BKP, "{\"port\":2000}");
  TEST_ASSERT_TRUE(loadSyslogConfig());
  TEST_ASSERT_EQUAL_UINT16(2000, g_syslogConfig.port);
}

void test_load_falls_back_to_the_backup_when_the_primary_is_corrupt(void) {
  // The reason the backup exists at all: a power cut mid-write leaves a
  // truncated primary that parses as garbage, not as a missing file.
  fsTestWrite(FILE_SYSLOG,     "{\"port\":10");
  fsTestWrite(FILE_SYSLOG_BKP, "{\"port\":2000}");
  TEST_ASSERT_TRUE(loadSyslogConfig());
  TEST_ASSERT_EQUAL_UINT16(2000, g_syslogConfig.port);
}

void test_load_partial_read_of_the_primary_does_not_leak_into_the_backup(void) {
  // loadJsonDocSafe must doc.clear() before retrying, or keys parsed out of the
  // corrupt primary would survive alongside the backup's.
  fsTestWrite(FILE_SYSLOG,     "{\"facility\":9,\"port\":");
  fsTestWrite(FILE_SYSLOG_BKP, "{\"port\":2000}");
  TEST_ASSERT_TRUE(loadSyslogConfig());
  TEST_ASSERT_EQUAL_UINT16(2000, g_syslogConfig.port);
  TEST_ASSERT_EQUAL_UINT8(16, g_syslogConfig.facility);   // table default, not 9
}

void test_load_falls_back_to_defaults_when_both_copies_are_bad(void) {
  fsTestWrite(FILE_SYSLOG,     "not json");
  fsTestWrite(FILE_SYSLOG_BKP, "also not json");
  TEST_ASSERT_FALSE(loadSyslogConfig());
  TEST_ASSERT_EQUAL_UINT16(514, g_syslogConfig.port);     // initDefaultSyslogConfig
  TEST_ASSERT_EQUAL_UINT8(16, g_syslogConfig.facility);
  TEST_ASSERT_EQUAL_UINT8(7, g_syslogConfig.minLevel);
}

void test_save_writes_both_the_primary_and_the_backup(void) {
  g_syslogConfig.port = 1234;
  TEST_ASSERT_TRUE(saveSyslogConfig());
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_SYSLOG));
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_SYSLOG_BKP));
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG,     "\"port\":1234"));
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":1234"));
}

void test_save_fails_and_leaves_the_primary_untouched_when_the_fs_cannot_open(void) {
  // Ordering matters: the backup is written first, so a failure there must not
  // go on to overwrite the last-good primary.
  fsTestWrite(FILE_SYSLOG, "{\"port\":1000}");
  fsTestSetFull(true);
  g_syslogConfig.port = 1234;
  TEST_ASSERT_FALSE(saveSyslogConfig());
  fsTestSetFull(false);
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG, "\"port\":1000"));   // still the old copy
}

void test_save_detects_a_truncated_write(void) {
  // Filesystem full mid-serialize: the file lands on disk but short, which
  // saveJsonDocToFile catches by comparing written against measureJson.
  g_syslogConfig.port = 1234;
  fsTestSetWriteLimit(10);
  TEST_ASSERT_FALSE(saveSyslogConfig());
  fsTestSetWriteLimit(-1);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_SYSLOG));           // primary never attempted
  TEST_ASSERT_EQUAL_INT(10, (int)strlen(fsTestRead(FILE_SYSLOG_BKP)));
}

// Gives ArduinoJson no memory at all, so nothing can be added to a document
struct NoMemoryAllocator : ArduinoJson::Allocator {
  void* allocate(size_t) override { return nullptr; }
  void  deallocate(void*) override {}
  void* reallocate(void*, size_t) override { return nullptr; }
};

void test_save_refuses_a_document_that_ran_out_of_memory(void) {
  // Part of the settings would be missing, so the copies on flash are kept
  fsTestWrite(FILE_SYSLOG,     "{\"port\":1000}");
  fsTestWrite(FILE_SYSLOG_BKP, "{\"port\":1000}");
  NoMemoryAllocator noMemory;
  JsonDocument doc(&noMemory);
  doc["port"] = 1234;
  TEST_ASSERT_FALSE(saveJsonDocSafe(doc, FILE_SYSLOG, FILE_SYSLOG_BKP));
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG,     "\"port\":1000"));
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":1000"));
}

// ============================================================
// Descriptor-driven field I/O (CfgField tables)
// ============================================================

void test_scalar_round_trip_preserves_every_field(void) {
  g_mqttConfig.enabled      = true;
  g_mqttConfig.haDiscovery  = true;
  g_mqttConfig.allowControl = false;
  g_mqttConfig.logEnabled   = true;
  g_mqttConfig.port         = 8883;
  strlcpy(g_mqttConfig.host,      "broker.local",  sizeof(g_mqttConfig.host));
  strlcpy(g_mqttConfig.username,  "brewer",        sizeof(g_mqttConfig.username));
  strlcpy(g_mqttConfig.password,  "s3cret",        sizeof(g_mqttConfig.password));
  strlcpy(g_mqttConfig.baseTopic, "shed/brewbot",  sizeof(g_mqttConfig.baseTopic));
  TEST_ASSERT_TRUE(saveMqttConfig());

  memset(&g_mqttConfig, 0, sizeof(g_mqttConfig));
  TEST_ASSERT_TRUE(loadMqttConfig());

  TEST_ASSERT_TRUE(g_mqttConfig.enabled);
  TEST_ASSERT_TRUE(g_mqttConfig.haDiscovery);
  TEST_ASSERT_FALSE(g_mqttConfig.allowControl);
  TEST_ASSERT_TRUE(g_mqttConfig.logEnabled);
  TEST_ASSERT_EQUAL_UINT16(8883, g_mqttConfig.port);
  TEST_ASSERT_EQUAL_STRING("broker.local",  g_mqttConfig.host);
  TEST_ASSERT_EQUAL_STRING("brewer",        g_mqttConfig.username);
  TEST_ASSERT_EQUAL_STRING("s3cret",        g_mqttConfig.password);
  TEST_ASSERT_EQUAL_STRING("shed/brewbot",  g_mqttConfig.baseTopic);
}

void test_array_round_trip_preserves_every_slot(void) {
  for (int i = 0; i < MAX_PROBES; i++) {
    snprintf(g_probes[i].probeName, sizeof(g_probes[i].probeName), "Probe %d", i);
    snprintf(g_probes[i].address,   sizeof(g_probes[i].address),   "28FF%012d", i);
    g_probes[i].function    = (uint8_t)(PROBE_FN_BEER + (i % 3));
    g_probes[i].fermenter   = (uint8_t)(i % MAX_FERMENTERS);
    g_probes[i].temperature = 10.0f + (float)i;
    g_probes[i].tempAdjust  = 0.25f * (float)i;
  }
  TEST_ASSERT_TRUE(saveProbeConfig());
  // The reading is still saved, so the file format is unchanged...
  TEST_ASSERT_TRUE(fileHas(FILE_PROBE, "\"Temperature\":[10,11,12,"));

  for (int i = 0; i < MAX_PROBES; i++) g_probes[i] = ProbeConfig{};
  TEST_ASSERT_TRUE(loadProbeConfig());

  for (int i = 0; i < MAX_PROBES; i++) {
    char name[24], addr[20];
    snprintf(name, sizeof(name), "Probe %d", i);
    snprintf(addr, sizeof(addr), "28FF%012d", i);
    TEST_ASSERT_EQUAL_STRING(name, g_probes[i].probeName);
    TEST_ASSERT_EQUAL_STRING(addr, g_probes[i].address);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(PROBE_FN_BEER + (i % 3)), g_probes[i].function);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(i % MAX_FERMENTERS), g_probes[i].fermenter);
    // ...but never loaded back as a reading (see the probe load tests below)
    TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_probes[i].temperature);
    TEST_ASSERT_EQUAL_FLOAT(0.25f * (float)i, g_probes[i].tempAdjust);
  }
}

void test_missing_keys_fall_back_to_the_declared_defaults(void) {
  fsTestWrite(FILE_MQTT, "{\"host\":\"broker.local\"}");
  TEST_ASSERT_TRUE(loadMqttConfig());
  TEST_ASSERT_EQUAL_STRING("broker.local", g_mqttConfig.host);
  TEST_ASSERT_EQUAL_UINT16(1883, g_mqttConfig.port);            // declared default
  TEST_ASSERT_EQUAL_STRING("ourbrewbot", g_mqttConfig.baseTopic);
  TEST_ASSERT_FALSE(g_mqttConfig.enabled);
}

void test_wrong_json_type_falls_back_to_the_declared_default(void) {
  // A hand-edited config (POST /fs/save lets the user do exactly this) can put
  // a string where a number belongs - that must not land as 0.
  fsTestWrite(FILE_MQTT, "{\"port\":\"not-a-number\",\"baseTopic\":42}");
  TEST_ASSERT_TRUE(loadMqttConfig());
  TEST_ASSERT_EQUAL_UINT16(1883, g_mqttConfig.port);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot", g_mqttConfig.baseTopic);
}

void test_null_json_value_falls_back_to_the_declared_default(void) {
  fsTestWrite(FILE_MQTT, "{\"port\":null,\"baseTopic\":null}");
  TEST_ASSERT_TRUE(loadMqttConfig());
  TEST_ASSERT_EQUAL_UINT16(1883, g_mqttConfig.port);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot", g_mqttConfig.baseTopic);
}

void test_unusable_stored_base_topic_is_repaired_on_load(void) {
  // Saved before POST /mqtt checked it, or imported from a file: every
  // publish under it would fail, so the characters MQTT can't use become '_'.
  fsTestWrite(FILE_MQTT, "{\"baseTopic\":\"$brew/+/#\"}");
  TEST_ASSERT_TRUE(loadMqttConfig());
  TEST_ASSERT_EQUAL_STRING("_brew/_/_", g_mqttConfig.baseTopic);
}

void test_empty_stored_base_topic_falls_back_to_the_default(void) {
  fsTestWrite(FILE_MQTT, "{\"baseTopic\":\"\"}");
  TEST_ASSERT_TRUE(loadMqttConfig());
  TEST_ASSERT_EQUAL_STRING("ourbrewbot", g_mqttConfig.baseTopic);
}

void test_working_stored_base_topic_is_loaded_unchanged(void) {
  fsTestWrite(FILE_MQTT, "{\"baseTopic\":\"home/brew bot\"}");
  TEST_ASSERT_TRUE(loadMqttConfig());
  TEST_ASSERT_EQUAL_STRING("home/brew bot", g_mqttConfig.baseTopic);
}

void test_oversized_string_is_truncated_to_the_member_size(void) {
  // SyslogConfig::host is char[64]; the copy must clamp rather than overflow
  // into the adjacent members.
  char json[256];
  char host[200];
  memset(host, 'h', sizeof(host) - 1);
  host[sizeof(host) - 1] = '\0';
  snprintf(json, sizeof(json), "{\"host\":\"%.190s\",\"port\":515}", host);
  fsTestWrite(FILE_SYSLOG, json);

  TEST_ASSERT_TRUE(loadSyslogConfig());
  TEST_ASSERT_EQUAL_INT(63, (int)strlen(g_syslogConfig.host));  // sizeof - 1
  TEST_ASSERT_EQUAL_UINT16(515, g_syslogConfig.port);           // next field intact
}

void test_stored_text_is_made_valid_utf8_on_load(void) {
  // Older firmware cut long names with strlcpy(), which could stop half way
  // through an accented character, and an imported file can hold any bytes.
  // Either way the name went out raw in the API's JSON and broke strict
  // readers, so it is made valid UTF-8 as it loads: a broken byte becomes '?'.
  fsTestWrite(FILE_FERMENTER,
              "{\"FermenterName\":[\"F\xFF\",\"b\",\"c\",\"d\"],"
              "\"BeerName\":[\"Weisse\xC3\",\"Bi\xC3\xA8re\",\"y\",\"z\"]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_STRING("F?",              g_fermenters[0].fermenterName);
  TEST_ASSERT_EQUAL_STRING("Weisse?",         g_fermenters[0].beerName);
  TEST_ASSERT_EQUAL_STRING("Bi\xC3\xA8re",    g_fermenters[1].beerName);  // valid: kept
}

void test_saved_key_order_matches_the_declared_table(void) {
  // Table order defines on-disk key order, and Config.cpp documents that it
  // must stay byte-identical to previous releases. Pin it.
  g_syslogConfig.enabled  = true;
  strlcpy(g_syslogConfig.host, "10.0.0.5", sizeof(g_syslogConfig.host));
  g_syslogConfig.port     = 514;
  g_syslogConfig.facility = 16;
  g_syslogConfig.minLevel = 4;
  TEST_ASSERT_TRUE(saveSyslogConfig());
  TEST_ASSERT_EQUAL_STRING(
      "{\"enabled\":true,\"host\":\"10.0.0.5\",\"port\":514,"
      "\"facility\":16,\"minLevel\":4}",
      fsTestRead(FILE_SYSLOG));
}

void test_array_files_hold_one_array_per_field_with_one_element_per_slot(void) {
  TEST_ASSERT_TRUE(saveProfileConfig());
  const char* json = fsTestRead(FILE_PROFILE);
  // One key, MAX_PROFILES elements - the array shape the original firmware's
  // files use, and what the WebUI's per-slot editors expect.
  TEST_ASSERT_EQUAL_INT(1, countOf(json, "\"ProfileName\""));
  TEST_ASSERT_EQUAL_INT(MAX_PROFILES - 1, countOf(json, ","));
}

// ============================================================
// Fermenter migrations + trio validation
// ============================================================

void test_legacy_integer_gravities_are_scaled_to_sg(void) {
  fsTestWrite(FILE_FERMENTER,
              "{\"OG\":[1050,1.047,3000,2],\"TG\":[1010,1.010,1.5,2.0001],"
              "\"BrewServices\":[0,0,0,0]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_FLOAT(1.050f, g_fermenters[0].og);   // scaled
  TEST_ASSERT_EQUAL_FLOAT(1.047f, g_fermenters[1].og);   // already SG, untouched
  TEST_ASSERT_EQUAL_FLOAT(3.0f,   g_fermenters[2].og);   // scaled
  TEST_ASSERT_EQUAL_FLOAT(2.0f,   g_fermenters[3].og);   // boundary: > 2.0 is false
  TEST_ASSERT_EQUAL_FLOAT(1.010f, g_fermenters[0].tg);
  TEST_ASSERT_EQUAL_FLOAT(1.5f,   g_fermenters[2].tg);
  TEST_ASSERT_EQUAL_FLOAT(0.0020001f, g_fermenters[3].tg);
}

void test_brewservices_bitmask_wins_when_the_key_is_present(void) {
  // BrewServiceSend is the original firmware's key. The loader ignores it:
  // OriginalConfig.cpp converts it before any loader runs.
  fsTestWrite(FILE_FERMENTER,
              "{\"BrewServices\":[9,0,0,0],\"BrewServiceSend\":[false,true,true,true]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_UINT8(9, g_fermenters[0].brewServices);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[1].brewServices);
}

void test_valid_temp_trio_is_preserved(void) {
  stageFermenterTrio(24.0f, 16.0f, 1.0f);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_FLOAT(24.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(16.0f, g_fermenters[0].floorTemp);
  TEST_ASSERT_EQUAL_FLOAT(1.0f,  g_fermenters[0].hysteresis);
}

void test_trio_at_the_exact_two_hysteresis_boundary_is_kept(void) {
  // Rejection is (ceiling - floor) < 2 * hysteresis, so an exact 2x span passes.
  stageFermenterTrio(22.0f, 18.0f, 2.0f);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_FLOAT(2.0f, g_fermenters[0].hysteresis);
}

void test_ceiling_out_of_range_resets_the_trio(void) {
  stageFermenterTrio(60.0f, 18.0f, 0.5f);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, g_fermenters[0].floorTemp);
  TEST_ASSERT_EQUAL_FLOAT(0.5f,  g_fermenters[0].hysteresis);
}

void test_floor_at_or_above_ceiling_resets_the_trio(void) {
  // Inverted band: the control loop would heat and cool at once.
  stageFermenterTrio(18.0f, 22.0f, 0.5f);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, g_fermenters[0].floorTemp);
}

void test_hysteresis_out_of_range_resets_the_trio(void) {
  stageFermenterTrio(22.0f, 18.0f, 25.0f);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_FLOAT(0.5f, g_fermenters[0].hysteresis);
}

void test_safe_zone_narrower_than_two_hysteresis_resets_the_trio(void) {
  // Each in range on its own, but the band can't hold both switch points -
  // the heater and the fridge would fight.
  stageFermenterTrio(20.0f, 19.0f, 3.0f);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, g_fermenters[0].floorTemp);
  TEST_ASSERT_EQUAL_FLOAT(0.5f,  g_fermenters[0].hysteresis);
}

void test_only_the_offending_fermenter_is_reset(void) {
  fsTestWrite(FILE_FERMENTER,
              "{\"CeilingTemp\":[99,24,22,22],\"FloorTemp\":[18,16,18,18],"
              "\"Hysteresis\":[0.5,1,0.5,0.5],\"BeerName\":[\"Stout\",\"IPA\",\"\",\"\"],"
              "\"BrewServices\":[0,0,0,0]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  // Slot 0 corrected...
  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
  // ...but only its trio: unrelated fields survive the correction.
  TEST_ASSERT_EQUAL_STRING("Stout", g_fermenters[0].beerName);
  // Slot 1 was valid and is left exactly as stored.
  TEST_ASSERT_EQUAL_FLOAT(24.0f, g_fermenters[1].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(16.0f, g_fermenters[1].floorTemp);
  TEST_ASSERT_EQUAL_FLOAT(1.0f,  g_fermenters[1].hysteresis);
}

void test_trio_correction_is_written_back_to_disk(void) {
  // Without the re-save, every boot would re-correct in RAM only and the admin
  // POST validation would keep rejecting the stored config.
  stageFermenterTrio(99.0f, 18.0f, 0.5f);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER,     "\"CeilingTemp\":[22,"));
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER_BKP, "\"CeilingTemp\":[22,"));
}

void test_a_valid_config_is_not_rewritten_on_load(void) {
  stageFermenterTrio(24.0f, 16.0f, 1.0f);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  // The backup is only written by a save; if load left it alone, it's absent.
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_FERMENTER_BKP));
}

void test_missing_fermenter_file_falls_back_to_defaults(void) {
  TEST_ASSERT_FALSE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_STRING("Fermenter 1", g_fermenters[0].fermenterName);
  TEST_ASSERT_EQUAL_STRING("Fermenter 4", g_fermenters[3].fermenterName);
  TEST_ASSERT_EQUAL_FLOAT(22.0f,  g_fermenters[0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(1.050f, g_fermenters[0].og);
  TEST_ASSERT_EQUAL_FLOAT(3.0f,   g_fermenters[0].alarmTolerance);
}

// ---- checkTempBand() (Config.h): the one set of rules the WebUI save, the
// MQTT commands and the boot check above all use. The WebAPI and MQTT suites
// check the same messages through their own entry points.

void test_temp_band_accepts_a_normal_setting(void) {
  const char* err = "not cleared";
  TEST_ASSERT_TRUE(checkTempBand(22.0f, 18.0f, 0.5f, &err));
  TEST_ASSERT_NULL(err);
}

void test_temp_band_accepts_the_exact_limits(void) {
  TEST_ASSERT_TRUE(checkTempBand(TEMP_SETTING_MAX_C, TEMP_SETTING_MIN_C, HYSTERESIS_MAX_C, nullptr));
  TEST_ASSERT_TRUE(checkTempBand(20.0f, 19.0f, 0.5f, nullptr));   // gap exactly 2x hysteresis
  TEST_ASSERT_TRUE(checkTempBand(20.0f, 19.0f, 0.0f, nullptr));   // no hysteresis
}

void test_temp_band_rejects_a_ceiling_out_of_range(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(checkTempBand(50.1f, 18.0f, 0.5f, &err));
  TEST_ASSERT_EQUAL_STRING("ceiling temperature out of range (-20 to 50)", err);
  TEST_ASSERT_FALSE(checkTempBand(-20.1f, -25.0f, 0.5f, &err));
  TEST_ASSERT_EQUAL_STRING("ceiling temperature out of range (-20 to 50)", err);
}

void test_temp_band_rejects_a_floor_out_of_range(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(checkTempBand(22.0f, -20.1f, 0.5f, &err));
  TEST_ASSERT_EQUAL_STRING("floor temperature out of range (-20 to 50)", err);
}

void test_temp_band_rejects_a_hysteresis_out_of_range(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(checkTempBand(40.0f, 10.0f, 10.1f, &err));
  TEST_ASSERT_EQUAL_STRING("hysteresis out of range (0 to 10)", err);
  TEST_ASSERT_FALSE(checkTempBand(22.0f, 18.0f, -0.1f, &err));
  TEST_ASSERT_EQUAL_STRING("hysteresis out of range (0 to 10)", err);
}

void test_temp_band_rejects_a_floor_not_below_the_ceiling(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(checkTempBand(20.0f, 20.0f, 0.0f, &err));
  TEST_ASSERT_EQUAL_STRING("floor must be below ceiling", err);
}

void test_temp_band_rejects_a_safe_zone_under_twice_the_hysteresis(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(checkTempBand(20.0f, 19.0f, 0.6f, &err));
  TEST_ASSERT_EQUAL_STRING("safe zone must be at least 2x hysteresis", err);
}

// ============================================================
// Tilt config - slot-to-colour mapping (custom load/save)
// ============================================================

void test_tilt_slots_load_into_colour_indexed_entries(void) {
  // Slot order in the file is arbitrary; the colour in "Address" decides which
  // g_tilts entry the row belongs to.
  fsTestWrite(FILE_TILT,
              "{\"Address\":[5,2,99,99],\"Function\":[2,2,99,99],"
              "\"Fermenter\":[1,0,99,99],\"Temp_Adjust\":[-0.5,0.25,0,0],"
              "\"SG_Adjust\":[0.002,-0.001,0,0],\"MBB\":[0,0,0,0]}");
  TEST_ASSERT_TRUE(loadTiltConfig());

  TEST_ASSERT_EQUAL_UINT8(TILT_BLUE, g_tilts[TILT_BLUE].colour);
  TEST_ASSERT_EQUAL_UINT8(1, g_tilts[TILT_BLUE].fermenter);
  TEST_ASSERT_EQUAL_FLOAT(-0.5f,  g_tilts[TILT_BLUE].tempAdjust);
  TEST_ASSERT_EQUAL_FLOAT(0.002f, g_tilts[TILT_BLUE].sgAdjust);

  TEST_ASSERT_EQUAL_UINT8(TILT_BLACK, g_tilts[TILT_BLACK].colour);
  TEST_ASSERT_EQUAL_UINT8(0, g_tilts[TILT_BLACK].fermenter);
  TEST_ASSERT_EQUAL_FLOAT(0.25f,   g_tilts[TILT_BLACK].tempAdjust);
  TEST_ASSERT_EQUAL_FLOAT(-0.001f, g_tilts[TILT_BLACK].sgAdjust);

  // Every other colour stays at the default sentinel.
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_RED].colour);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_PINK].colour);
}

void test_tilt_unassigned_and_out_of_range_slots_are_skipped(void) {
  // 99 is the unassigned sentinel and 12 is past MAX_TILTS - writing either
  // into g_tilts would run off the end of the array.
  fsTestWrite(FILE_TILT,
              "{\"Address\":[99,12,3,99],\"Function\":[2,2,2,2],"
              "\"Fermenter\":[0,0,2,0],\"Temp_Adjust\":[0,0,0,0],"
              "\"SG_Adjust\":[0,0,0,0],\"MBB\":[0,0,0,0]}");
  TEST_ASSERT_TRUE(loadTiltConfig());
  TEST_ASSERT_EQUAL_UINT8(TILT_PURPLE, g_tilts[TILT_PURPLE].colour);
  TEST_ASSERT_EQUAL_UINT8(2, g_tilts[TILT_PURPLE].fermenter);
  for (int c = 0; c < MAX_TILTS; c++) {
    if (c == TILT_PURPLE) continue;
    TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[c].colour);
  }
}

void test_tilt_legacy_function_values_collapse_to_unassigned(void) {
  // Only PROBE_FN_BEER still means "provide beer temp"; Ambient/Tilt-only and
  // friends now mean "reading not used for temperature".
  fsTestWrite(FILE_TILT,
              "{\"Address\":[0,1,2,3],\"Function\":[2,3,4,7],"
              "\"Fermenter\":[0,0,0,0],\"Temp_Adjust\":[0,0,0,0],"
              "\"SG_Adjust\":[0,0,0,0],\"MBB\":[0,0,0,0]}");
  TEST_ASSERT_TRUE(loadTiltConfig());
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER,    g_tilts[TILT_RED].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_GREEN].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_BLACK].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_PURPLE].function);
}

void test_tilt_save_packs_configured_colours_first_and_pads_the_rest(void) {
  g_tilts[TILT_YELLOW].colour     = TILT_YELLOW;
  g_tilts[TILT_YELLOW].function   = PROBE_FN_BEER;
  g_tilts[TILT_YELLOW].fermenter  = 3;
  g_tilts[TILT_BLACK].colour      = TILT_BLACK;
  g_tilts[TILT_BLACK].function    = PROBE_UNASSIGNED;
  g_tilts[TILT_BLACK].fermenter   = 1;
  TEST_ASSERT_TRUE(saveTiltConfig());

  // Colours are emitted in ascending order (black=2 before yellow=6), then the
  // remaining slots are padded to MAX_TILT_SLOTS with the sentinel.
  TEST_ASSERT_TRUE(fileHas(FILE_TILT, "\"Address\":[2,6,99,99]"));
  TEST_ASSERT_TRUE(fileHas(FILE_TILT, "\"Fermenter\":[1,3,99,99]"));
  TEST_ASSERT_TRUE(fileHas(FILE_TILT, "\"Function\":[99,2,99,99]"));
}

void test_tilt_round_trip_survives_the_slot_repacking(void) {
  // Saving renumbers slots, so a round trip is the only way to know the colour
  // keyed data still lands on the right colour.
  g_tilts[TILT_PINK].colour     = TILT_PINK;
  g_tilts[TILT_PINK].function   = PROBE_FN_BEER;
  g_tilts[TILT_PINK].fermenter  = 2;
  g_tilts[TILT_PINK].tempAdjust = 1.5f;
  g_tilts[TILT_PINK].sgAdjust   = -0.003f;
  g_tilts[TILT_RED].colour      = TILT_RED;
  g_tilts[TILT_RED].function    = PROBE_UNASSIGNED;
  g_tilts[TILT_RED].fermenter   = 0;
  g_tilts[TILT_RED].tempAdjust  = -0.25f;
  TEST_ASSERT_TRUE(saveTiltConfig());

  initDefaultTiltConfig();   // prove the values below came from the file, not RAM
  TEST_ASSERT_TRUE(loadTiltConfig());

  TEST_ASSERT_EQUAL_UINT8(TILT_PINK, g_tilts[TILT_PINK].colour);
  TEST_ASSERT_EQUAL_UINT8(2, g_tilts[TILT_PINK].fermenter);
  TEST_ASSERT_EQUAL_FLOAT(1.5f,    g_tilts[TILT_PINK].tempAdjust);
  TEST_ASSERT_EQUAL_FLOAT(-0.003f, g_tilts[TILT_PINK].sgAdjust);
  TEST_ASSERT_EQUAL_UINT8(TILT_RED, g_tilts[TILT_RED].colour);
  TEST_ASSERT_EQUAL_FLOAT(-0.25f, g_tilts[TILT_RED].tempAdjust);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_GREEN].colour);
}

void test_tilt_save_stops_at_max_slots_when_more_colours_are_configured(void) {
  // g_tilts holds 8 colours but the file format only has 4 slots - the extra
  // colours must be dropped, not written past the end of the arrays.
  for (int c = 0; c < MAX_TILTS; c++) {
    g_tilts[c].colour    = (uint8_t)c;
    g_tilts[c].function  = PROBE_FN_BEER;
    g_tilts[c].fermenter = 0;
  }
  TEST_ASSERT_TRUE(saveTiltConfig());
  TEST_ASSERT_TRUE(fileHas(FILE_TILT, "\"Address\":[0,1,2,3]"));
}

void test_tilt_load_starts_from_defaults_when_the_file_is_missing(void) {
  g_tilts[TILT_RED].colour = TILT_RED;    // stale in-memory state
  TEST_ASSERT_FALSE(loadTiltConfig());
  for (int c = 0; c < MAX_TILTS; c++) {
    TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[c].colour);
  }
}

// ============================================================
// Brew service migrations
// ============================================================

void test_brewsvc_three_slot_config_remaps_to_two(void) {
  // Old layout was [Brewer's Friend, Monitor Beer, Brewfather]; Monitor Beer
  // was removed, so old slot 2 becomes new slot 1 and old slot 1 is dropped.
  fsTestWrite(FILE_BREWSVC,
              "{\"Enabled\":[true,true,true],"
              "\"ServiceId\":[\"bf-key\",\"mb-key\",\"bfather-key\"],"
              "\"DeviceName\":[\"D0\",\"D1\",\"D2\"]}");
  TEST_ASSERT_TRUE(loadBrewServiceConfig());

  TEST_ASSERT_TRUE(g_brewServices[0].enabled);
  TEST_ASSERT_EQUAL_STRING("bf-key", g_brewServices[0].serviceId);
  TEST_ASSERT_EQUAL_STRING("D0",     g_brewServices[0].deviceName);
  TEST_ASSERT_TRUE(g_brewServices[1].enabled);
  TEST_ASSERT_EQUAL_STRING("bfather-key", g_brewServices[1].serviceId);
  TEST_ASSERT_EQUAL_STRING("D2",          g_brewServices[1].deviceName);
  // Rewritten in the 2-slot format so the next boot takes the normal path.
  TEST_ASSERT_TRUE(fileHas(FILE_BREWSVC, "\"Enabled\":[true,true]"));
}

void test_brewsvc_two_slot_config_loads_through_the_table(void) {
  fsTestWrite(FILE_BREWSVC,
              "{\"Enabled\":[false,true],\"ServiceId\":[\"\",\"stream-id\"],"
              "\"DeviceName\":[\"OurBrewbot\",\"Shed\"]}");
  TEST_ASSERT_TRUE(loadBrewServiceConfig());
  TEST_ASSERT_FALSE(g_brewServices[0].enabled);
  TEST_ASSERT_TRUE(g_brewServices[1].enabled);
  TEST_ASSERT_EQUAL_STRING("stream-id", g_brewServices[1].serviceId);
  TEST_ASSERT_EQUAL_STRING("Shed",      g_brewServices[1].deviceName);
}

void test_brewsvc_without_a_file_does_not_use_the_global_service(void) {
  // The original's single global service is moved into this file by
  // OriginalConfig.cpp; the loader itself only falls back to the defaults.
  g_globalConfig.brewService = BREW_SERVICE_BREWERS_FRIEND;
  strlcpy(g_globalConfig.brewServiceId, "legacy-bf", sizeof(g_globalConfig.brewServiceId));
  TEST_ASSERT_FALSE(loadBrewServiceConfig());
  TEST_ASSERT_FALSE(g_brewServices[0].enabled);
  TEST_ASSERT_FALSE(g_brewServices[1].enabled);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_BREWSVC));
}

// ============================================================
// Index-dependent and post-load defaults
// ============================================================

void test_smartplug_plugno_defaults_to_the_slot_index(void) {
  fsTestWrite(FILE_SMARTPLUGS, "{\"Type\":[1,1,1,1,1,1,1,1,1,1]}");
  TEST_ASSERT_TRUE(loadSmartPlugConfig());
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)i, g_smartPlugs[i].plugNo);
  }
}

void test_smartplug_stored_plugno_is_respected(void) {
  fsTestWrite(FILE_SMARTPLUGS, "{\"PlugNo\":[9,8,7,6,5,4,3,2,1,0]}");
  TEST_ASSERT_TRUE(loadSmartPlugConfig());
  TEST_ASSERT_EQUAL_UINT8(9, g_smartPlugs[0].plugNo);
  TEST_ASSERT_EQUAL_UINT8(0, g_smartPlugs[9].plugNo);
}

// A saved reading can be hours old. Loading it as live meant a probe missing at
// boot kept it for its first 6 failed reads (~30 s), and control and alarms
// ran on it - e.g. a heater switched on for a beer that was never measured.
void test_probe_load_starts_with_no_reading(void) {
  fsTestWrite(FILE_PROBE, "{\"Address\":[\"28ff641f8b2c0011\"],\"Temperature\":[19.5,0,0,0,0,0,0,0]}");
  TEST_ASSERT_TRUE(loadProbeConfig());
  TEST_ASSERT_EQUAL_STRING("28ff641f8b2c0011", g_probes[0].address);   // the rest still loads
  for (int i = 0; i < MAX_PROBES; i++) {
    TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_probes[i].temperature);
    TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_probes[i].rawTemperature);
  }
}

// With no file the defaults apply, and a default slot has no reading either
// (it used to be 0.0, which looks like a real 0 C)
void test_probe_defaults_have_no_reading(void) {
  TEST_ASSERT_FALSE(loadProbeConfig());
  for (int i = 0; i < MAX_PROBES; i++) {
    TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_probes[i].temperature);
    TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_probes[i].rawTemperature);
  }
}

void test_ispindel_legacy_function_values_collapse_to_unassigned(void) {
  fsTestWrite(FILE_ISPINDEL, "{\"Function\":[2,3,5,99]}");
  TEST_ASSERT_TRUE(loadiSpindelConfig());
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER,    g_iSpindels[0].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_iSpindels[1].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_iSpindels[2].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_iSpindels[3].function);
}

void test_ispindel_absent_function_defaults_to_beer_for_legacy_configs(void) {
  // Old configs had no Function key, and their temperature did flow into the
  // beer-temp chain - the table default preserves that.
  fsTestWrite(FILE_ISPINDEL, "{\"ID\":[\"9b5c5e\",\"\",\"\",\"\"]}");
  TEST_ASSERT_TRUE(loadiSpindelConfig());
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER, g_iSpindels[0].function);
}

void test_ispindel_stored_ids_are_made_topic_safe_on_load(void) {
  // An ID stored before 0.5.1, or imported, can hold characters that break
  // MQTT topics. The same rule as for incoming readings is applied, so a
  // normal ID is untouched and the device's next reading still matches.
  fsTestWrite(FILE_ISPINDEL, "{\"ID\":[\"a+b#c/d\",\"9b5c5e\",\"\",\"C2cc-7C_01\"]}");
  TEST_ASSERT_TRUE(loadiSpindelConfig());
  TEST_ASSERT_EQUAL_STRING("a_b_c_d",    g_iSpindels[0].id);
  TEST_ASSERT_EQUAL_STRING("9b5c5e",     g_iSpindels[1].id);
  TEST_ASSERT_EQUAL_STRING("",           g_iSpindels[2].id);
  TEST_ASSERT_EQUAL_STRING("C2cc-7C_01", g_iSpindels[3].id);
}

// An iSpindel has no reading until it reports. Before 0.6.4 its temperature
// started at 0.0, which the controller took as a real 0 C after every reboot.
void test_ispindel_temperature_starts_as_no_reading(void) {
  fsTestWrite(FILE_ISPINDEL, "{\"ID\":[\"9b5c5e\",\"\",\"\",\"\"]}");
  TEST_ASSERT_TRUE(loadiSpindelConfig());
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_iSpindels[i].temperature);
  }
  initDefaultiSpindelConfig();
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_iSpindels[i].temperature);
  }
}

// A fresh install no longer links slot 0 to Fermenter 1 (see iSpindel.cpp)
void test_ispindel_defaults_are_all_unassigned(void) {
  initDefaultiSpindelConfig();
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_iSpindels[i].fermenter);
    TEST_ASSERT_FALSE(g_iSpindels[i].collectData);
  }
}

// ---- range checks on load (0.6.4) ----
// A bad or imported file used to load out-of-range values unchecked.

void test_load_corrects_an_out_of_range_probe_resolution(void) {
  fsTestWrite(FILE_GLOBAL, "{\"resolution\":0}");
  TEST_ASSERT_TRUE(loadGlobalConfig());
  TEST_ASSERT_EQUAL_UINT8(11, g_globalConfig.resolution);
  fsTestWrite(FILE_GLOBAL, "{\"resolution\":13}");
  TEST_ASSERT_TRUE(loadGlobalConfig());
  TEST_ASSERT_EQUAL_UINT8(11, g_globalConfig.resolution);
}

void test_load_keeps_a_valid_probe_resolution(void) {
  fsTestWrite(FILE_GLOBAL, "{\"resolution\":9}");
  TEST_ASSERT_TRUE(loadGlobalConfig());
  TEST_ASSERT_EQUAL_UINT8(9, g_globalConfig.resolution);
}

void test_load_stops_a_profile_that_does_not_exist(void) {
  fsTestWrite(FILE_FERMENTER,
    "{\"ProfileNo\":[7,2,0,0],\"ProfileRunning\":[true,true,false,false],"
    "\"CurrentStep\":[3,4,0,0],\"CurrentHour\":[10,10,0,0]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[0].profileNo);
  TEST_ASSERT_FALSE(g_fermenters[0].profileRunning);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[0].currentStep);
  // A valid profile is left alone
  TEST_ASSERT_EQUAL_UINT8(2, g_fermenters[1].profileNo);
  TEST_ASSERT_TRUE(g_fermenters[1].profileRunning);
  TEST_ASSERT_EQUAL_UINT8(4, g_fermenters[1].currentStep);
  TEST_ASSERT_EQUAL_UINT16(10, g_fermenters[1].currentHour);
}

void test_load_corrects_an_out_of_range_profile_step(void) {
  fsTestWrite(FILE_FERMENTER,
    "{\"ProfileNo\":[1,0,0,0],\"ProfileRunning\":[true,false,false,false],"
    "\"CurrentStep\":[15,0,0,0],\"CurrentHour\":[5,0,0,0]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[0].profileNo);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[0].currentStep);
  TEST_ASSERT_EQUAL_UINT16(0, g_fermenters[0].currentHour);
}

void test_load_corrects_an_out_of_range_plug_bit_count(void) {
  fsTestWrite(FILE_SMARTPLUGS, "{\"Bits\":[0,33,12,32,24,24,24,24,24,24]}");
  TEST_ASSERT_TRUE(loadSmartPlugConfig());
  TEST_ASSERT_EQUAL_UINT8(24, g_smartPlugs[0].bits);
  TEST_ASSERT_EQUAL_UINT8(24, g_smartPlugs[1].bits);
  TEST_ASSERT_EQUAL_UINT8(12, g_smartPlugs[2].bits);
  TEST_ASSERT_EQUAL_UINT8(32, g_smartPlugs[3].bits);
}

// The profile part-hour is saved, so it survives a reboot (0.6.4)
void test_hour_ticks_are_saved_and_loaded(void) {
  g_fermenters[0].hourTicks = 4;
  TEST_ASSERT_TRUE(saveFermenterConfig());
  g_fermenters[0].hourTicks = 0;
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_UINT8(4, g_fermenters[0].hourTicks);
}

void test_load_corrects_out_of_range_hour_ticks(void) {
  fsTestWrite(FILE_FERMENTER, "{\"HourTicks\":[9,5,0,0]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[0].hourTicks);
  TEST_ASSERT_EQUAL_UINT8(5, g_fermenters[1].hourTicks);
}

void test_profile_steps_load_returns_false_without_touching_state(void) {
  // loadProfileSteps has no default initialiser - a missing file must leave the
  // in-memory steps alone rather than half-clearing them.
  g_profileSteps[0].endTemp = 21.0f;
  TEST_ASSERT_FALSE(loadProfileSteps());
  TEST_ASSERT_EQUAL_FLOAT(21.0f, g_profileSteps[0].endTemp);
}

void test_profile_steps_round_trip_across_all_slots(void) {
  for (int s = 0; s < MAX_PROFILE_STEPS; s++) {
    g_profileSteps[s].stepNo    = (uint8_t)(s % MAX_STEPS_PER_PROFILE);
    g_profileSteps[s].stepType  = (uint8_t)(s % 10);
    g_profileSteps[s].startTemp = 10.0f + 0.5f * (float)s;
    g_profileSteps[s].endTemp   = 12.0f + 0.5f * (float)s;
    g_profileSteps[s].days      = 0.25f * (float)s;
  }
  TEST_ASSERT_TRUE(saveProfileSteps());
  memset(g_profileSteps, 0, sizeof(g_profileSteps));
  TEST_ASSERT_TRUE(loadProfileSteps());

  for (int s = 0; s < MAX_PROFILE_STEPS; s++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(s % MAX_STEPS_PER_PROFILE), g_profileSteps[s].stepNo);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(s % 10), g_profileSteps[s].stepType);
    TEST_ASSERT_EQUAL_FLOAT(10.0f + 0.5f * (float)s, g_profileSteps[s].startTemp);
    TEST_ASSERT_EQUAL_FLOAT(12.0f + 0.5f * (float)s, g_profileSteps[s].endTemp);
    TEST_ASSERT_EQUAL_FLOAT(0.25f * (float)s, g_profileSteps[s].days);
  }
}

// ============================================================
// loadAllConfig - the one-shot AlarmTolerance migration
// ============================================================

void test_alarm_tolerance_zero_is_bumped_to_three_on_first_load(void) {
  fsTestWrite(FILE_FERMENTER,
              "{\"AlarmTolerance\":[0,0,2.5,0],\"BrewServices\":[0,0,0,0]}");
  loadAllConfig();
  TEST_ASSERT_EQUAL_FLOAT(3.0f, g_fermenters[0].alarmTolerance);
  TEST_ASSERT_EQUAL_FLOAT(3.0f, g_fermenters[1].alarmTolerance);
  TEST_ASSERT_EQUAL_FLOAT(2.5f, g_fermenters[2].alarmTolerance);   // deliberate value kept
  TEST_ASSERT_TRUE(g_globalConfig.migrated);
  TEST_ASSERT_TRUE(fileHas(FILE_GLOBAL, "\"migrated\":true"));
}

void test_alarm_tolerance_migration_does_not_run_twice(void) {
  // Once migrated, a user's deliberate 0 must survive - that's what the flag
  // is for.
  fsTestWrite(FILE_GLOBAL,    "{\"migrated\":true}");
  fsTestWrite(FILE_FERMENTER, "{\"AlarmTolerance\":[0,0,0,0],\"BrewServices\":[0,0,0,0]}");
  loadAllConfig();
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_fermenters[0].alarmTolerance);
}

// ============================================================
// Resets
// ============================================================

void test_reset_wifi_config_clears_the_provisioning_artifacts(void) {
  fsTestWrite(FILE_CONFIG,     "{\"ssid\":\"home\"}");
  fsTestWrite(FILE_CONFIG_BKP, "{\"ssid\":\"home\"}");
  fsTestWrite(FILE_DRD,        "x");
  fsTestWrite(FILE_MQTT,       "{\"port\":8883}");
  fsTestWrite(FILE_CRASH_QUEUE, "{\"n\":1}\n");

  resetWiFiConfig();

  TEST_ASSERT_TRUE(LittleFS.exists(FILE_CRASH_QUEUE));   // a WiFi reset keeps waiting crash reports

  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG_BKP));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_DRD));
  // Only the WiFi artifacts - the rest of the config is untouched.
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_MQTT));
}

void test_reset_all_config_restores_defaults_and_writes_every_file(void) {
  fsTestWrite(FILE_CONFIG, "{\"ssid\":\"home\"}");
  fsTestWrite(FILE_CRASH_QUEUE, "{\"n\":1}\n");
  g_fermenters[0].ceilingTemp = 40.0f;
  g_mqttConfig.enabled        = true;

  resetAllConfig();

  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));   // crash reports waiting are deleted too

  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_FALSE(g_mqttConfig.enabled);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot", g_mqttConfig.baseTopic);
  const char* files[] = {
    FILE_GLOBAL, FILE_FERMENTER, FILE_PROBE, FILE_SMARTPLUGS, FILE_PROFILE,
    FILE_STEPS, FILE_ISPINDEL, FILE_BREWSVC, FILE_MQTT, FILE_SYSLOG,
    FILE_TILT,
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.exists(files[i]), files[i]);
  }
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG));   // portal re-runs on boot
}

void test_reset_all_config_clears_the_saved_tilt_settings(void) {
  // A Tilt assigned and calibrated before the reset
  g_tilts[TILT_PINK].colour    = TILT_PINK;
  g_tilts[TILT_PINK].fermenter = 0;
  g_tilts[TILT_PINK].sgAdjust  = 0.004f;
  TEST_ASSERT_TRUE(saveTiltConfig());

  resetAllConfig();
  loadTiltConfig();   // what the restart after a reset does

  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_PINK].colour);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_PINK].fermenter);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_tilts[TILT_PINK].sgAdjust);
}

void test_reset_all_config_survives_a_read_only_filesystem(void) {
  // Nothing can be written, but the in-memory defaults must still be applied -
  // otherwise a failing flash would leave the controller running old setpoints.
  fsTestSetFull(true);
  g_fermenters[0].ceilingTemp = 40.0f;
  resetAllConfig();
  fsTestSetFull(false);
  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
}

// ============================================================
// recordReboot - the reboot log
// ============================================================

void test_reboot_log_appends_an_entry(void) {
  g_globalConfig.lastUptime = 4242;
  g_espFreeHeap             = 21504;
  espTestSetResetReason(REASON_SOFT_RESTART);

  recordReboot(String("OTA_UPGRADE"));

  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"reason\":\"OTA_UPGRADE\""));
  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"uptime\":4242"));
  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"heap\":21504"));
  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"rsn_code\":4"));
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_REBOOT_BKP));
}

void test_reboot_log_keeps_appending_across_calls(void) {
  recordReboot(String("first"));
  recordReboot(String("second"));
  const char* json = fsTestRead(FILE_REBOOT);
  TEST_ASSERT_EQUAL_INT(2, countOf(json, "\"reason\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"first\""));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"second\""));
}

void test_reboot_log_is_trimmed_to_the_last_ten_entries(void) {
  // Unbounded growth here would eventually fill the filesystem and take the
  // config files' backups down with it.
  for (int i = 1; i <= 14; i++) {
    char reason[16];
    snprintf(reason, sizeof(reason), "boot-%02d", i);
    recordReboot(String(reason));
  }
  const char* json = fsTestRead(FILE_REBOOT);
  TEST_ASSERT_EQUAL_INT(10, countOf(json, "\"reason\""));
  TEST_ASSERT_NULL(strstr(json, "boot-04"));        // oldest dropped
  TEST_ASSERT_NOT_NULL(strstr(json, "boot-05"));    // oldest kept
  TEST_ASSERT_NOT_NULL(strstr(json, "boot-14"));    // newest
}

void test_reboot_log_records_register_detail_only_for_exception_resets(void) {
  // Exception 28 = LoadProhibited, the usual null-deref signature.
  espTestSetExceptionReset(28, 4096, 0);
  recordReboot(String("Exception"));
  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"exccause\":28"));
  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"epc1\":4096"));
  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"excvaddr\":0"));

  fsTestReset();
  espTestSetResetReason(REASON_WDT_RST);
  recordReboot(String("Hardware Watchdog"));
  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"rsn_code\":1"));
  TEST_ASSERT_FALSE(fileHas(FILE_REBOOT, "\"exccause\""));
  TEST_ASSERT_FALSE(fileHas(FILE_REBOOT, "\"epc1\""));
}

void test_reboot_log_recovers_from_a_corrupt_log_file(void) {
  // A corrupt log must not stop the reboot reason being recorded - this is the
  // only breadcrumb for a crash loop.
  fsTestWrite(FILE_REBOOT,     "{\"log\":[{\"reason\"");
  fsTestWrite(FILE_REBOOT_BKP, "garbage");
  recordReboot(String("Power on"));
  TEST_ASSERT_TRUE(fileHas(FILE_REBOOT, "\"reason\":\"Power on\""));
  TEST_ASSERT_EQUAL_INT(1, countOf(fsTestRead(FILE_REBOOT), "\"reason\""));
}

// ============================================================
// CONFIG IMPORT - back up first, hold off the 10-minute save, put back on failure
// ============================================================

static void setName(const char* name) {
  strlcpy(g_fermenters[0].fermenterName, name, sizeof(g_fermenters[0].fermenterName));
}

// What a POST /fs/save of the fermenter file puts on flash
static void writeImportedFermenterFile() {
  fsTestWrite(FILE_FERMENTER, "{\"FermenterName\":[\"Imported\"]}");
}

// "Back up before importing": the first imported file saves the settings in
// use to every file AND its backup before anything is replaced
static void test_an_import_backs_up_the_current_settings_first(void) {
  setName("Before");
  configImportBeforeWrite();
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER,     "\"Before\""));
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER_BKP, "\"Before\""));
  // ...including the Tilt file, which saveAllConfig() leaves out
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_TILT_BKP));
}

// Only the first file: later ones must not save memory over the files already
// imported, and the backups keep the settings from before the import
static void test_only_the_first_file_of_an_import_backs_up(void) {
  setName("Before");
  configImportBeforeWrite();
  writeImportedFermenterFile();
  setName("Changed");
  configImportBeforeWrite();
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER,     "\"Imported\""));
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER_BKP, "\"Before\""));
}

// The bug: the 10-minute save wrote memory back over the imported files while
// the WebUI's "Import complete" message was open, so the device restarted on
// its old settings
static void test_the_periodic_save_is_held_off_during_an_import(void) {
  setName("Before");
  configImportBeforeWrite();
  writeImportedFermenterFile();
  setName("Changed");
  configMarkUnsaved(CFG_FILE_FERMENTER);
  s_uptimeSec += 60;
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"Imported\""));
}

// No file for 2 minutes and no restart: the import was abandoned, so the next
// 10-minute save puts the previous settings back, then saving is normal again
static void test_an_abandoned_import_is_put_back(void) {
  setName("Before");
  configImportBeforeWrite();
  writeImportedFermenterFile();
  s_uptimeSec += 121;
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"Before\""));
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "was not finished - the previous settings have been put back"));

  setName("Later");
  configMarkUnsaved(CFG_FILE_FERMENTER);
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"Later\""));
}

// POST /fs/import/abort, when a file of the import was refused
static void test_restore_puts_the_previous_settings_back_and_ends_the_import(void) {
  setName("Before");
  configImportBeforeWrite();
  writeImportedFermenterFile();
  configImportRestore();
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER,     "\"Before\""));
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER_BKP, "\"Before\""));

  setName("Later");
  configMarkUnsaved(CFG_FILE_FERMENTER);
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"Later\""));
}

// ============================================================
// THE 10-MINUTE SAVE - only the files marked unsaved are written
// ============================================================

// It used to rewrite all ten files and their backups every time
static void test_the_periodic_save_leaves_unchanged_files_alone(void) {
  configPeriodicSave();
  TEST_ASSERT_EQUAL_INT(0, fsTestWriteCount());
}

static void test_the_periodic_save_writes_a_file_marked_unsaved(void) {
  setName("Changed");
  configMarkUnsaved(CFG_FILE_FERMENTER);
  configPeriodicSave();
  TEST_ASSERT_EQUAL_INT(2, fsTestWriteCount());   // the file and its backup, nothing else
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER,     "\"Changed\""));
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER_BKP, "\"Changed\""));

  // Saved now, so the next 10-minute save has nothing to do
  fsTestClearWriteCount();
  configPeriodicSave();
  TEST_ASSERT_EQUAL_INT(0, fsTestWriteCount());
}

// A WebUI change is saved straight away, so it isn't saved again
static void test_a_file_saved_straight_away_is_not_saved_again(void) {
  configMarkUnsaved(CFG_FILE_FERMENTER);
  saveFermenterConfig();
  fsTestClearWriteCount();
  configPeriodicSave();
  TEST_ASSERT_EQUAL_INT(0, fsTestWriteCount());
}

// Whoever made the save, a failure is tried again every 10 minutes until it works
static void test_a_failed_save_is_tried_again_by_the_periodic_save(void) {
  g_syslogConfig.port = 1234;
  fsTestSetFull(true);
  TEST_ASSERT_FALSE(saveSyslogConfig());
  configPeriodicSave();
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "trying again in 10 minutes"));
  fsTestSetFull(false);
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG,     "\"port\":1234"));
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":1234"));
}

// The backup is written but the primary is cut short, so the two copies differ.
// The file stays marked, and the next 10-minute save writes both again.
static void test_a_save_that_fails_after_the_backup_is_tried_again(void) {
  g_syslogConfig.port = 1234;
  TEST_ASSERT_TRUE(saveSyslogConfig());
  long backupLength = (long)strlen(fsTestRead(FILE_SYSLOG_BKP));

  g_syslogConfig.port = 4321;   // same length, so the budget covers just the backup
  fsTestSetWriteLimit(backupLength);
  TEST_ASSERT_FALSE(saveSyslogConfig());
  fsTestSetWriteLimit(-1);
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":4321"));
  TEST_ASSERT_FALSE(fileHas(FILE_SYSLOG, "\"port\":4321"));

  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG,     "\"port\":4321"));
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":4321"));
}

// saveAllConfig() leaves the Tilt file out, but a failed Tilt save is still tried again
static void test_a_failed_tilt_save_is_tried_again_by_the_periodic_save(void) {
  g_tilts[TILT_RED].colour = TILT_RED;
  fsTestSetFull(true);
  TEST_ASSERT_FALSE(saveTiltConfig());
  fsTestSetFull(false);
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_TILT, "\"Address\":[0,"));
}

// ============================================================
// LOADING - a missing or damaged copy is marked unsaved, so it is written again
// ============================================================

static void test_loading_a_good_pair_leaves_the_file_alone(void) {
  fsTestWrite(FILE_SYSLOG,     "{\"port\":1000}");
  fsTestWrite(FILE_SYSLOG_BKP, "{\"port\":1000}");
  TEST_ASSERT_TRUE(loadSyslogConfig());
  fsTestClearWriteCount();
  configPeriodicSave();
  TEST_ASSERT_EQUAL_INT(0, fsTestWriteCount());
}

static void test_a_missing_backup_is_saved_again(void) {
  fsTestWrite(FILE_SYSLOG, "{\"port\":1000}");
  TEST_ASSERT_TRUE(loadSyslogConfig());
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":1000"));
}

static void test_a_damaged_backup_is_saved_again(void) {
  fsTestWrite(FILE_SYSLOG,     "{\"port\":1000}");
  fsTestWrite(FILE_SYSLOG_BKP, "{\"port\":10");
  TEST_ASSERT_TRUE(loadSyslogConfig());
  TEST_ASSERT_EQUAL_UINT16(1000, g_syslogConfig.port);
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":1000"));
}

// The backup is read in full. A quicker check that skips over each value (an
// ArduinoJson filter) would pass both of these, and they would only be found
// when the primary failed and the backup was needed.
static void test_a_backup_with_a_bad_number_or_escape_counts_as_damaged(void) {
  const char* damaged[] = { "{\"port\":12.3.4}", "{\"host\":\"a\\qb\"}" };
  for (const char* json : damaged) {
    setUp();
    fsTestWrite(FILE_SYSLOG,     "{\"port\":1000}");
    fsTestWrite(FILE_SYSLOG_BKP, json);
    TEST_ASSERT_TRUE(loadSyslogConfig());
    configPeriodicSave();
    TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":1000"));
  }
}

static void test_a_damaged_primary_is_saved_again_from_the_backup(void) {
  fsTestWrite(FILE_SYSLOG,     "{\"port\":10");
  fsTestWrite(FILE_SYSLOG_BKP, "{\"port\":2000}");
  TEST_ASSERT_TRUE(loadSyslogConfig());
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG, "\"port\":2000"));
}

// First boot, or both copies lost: the defaults are saved, as they always were
static void test_a_missing_file_is_saved_with_the_defaults(void) {
  TEST_ASSERT_FALSE(loadSyslogConfig());
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG,     "\"port\":514"));
  TEST_ASSERT_TRUE(fileHas(FILE_SYSLOG_BKP, "\"port\":514"));
}

// The MQTT base topic is repaired in memory only, and saved by the 10-minute save
static void test_a_correction_made_while_loading_is_saved_by_the_periodic_save(void) {
  fsTestWrite(FILE_MQTT,     "{\"baseTopic\":\"\"}");
  fsTestWrite(FILE_MQTT_BKP, "{\"baseTopic\":\"\"}");
  TEST_ASSERT_TRUE(loadMqttConfig());
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_MQTT,     "\"baseTopic\":\"ourbrewbot\""));
  TEST_ASSERT_TRUE(fileHas(FILE_MQTT_BKP, "\"baseTopic\":\"ourbrewbot\""));
}

// A correction saved while loading that fails is not forgotten
static void test_a_failed_correction_save_while_loading_is_tried_again(void) {
  stageFermenterTrio(99.0f, 18.0f, 0.5f);   // ceiling out of range
  char staged[256];
  strlcpy(staged, fsTestRead(FILE_FERMENTER), sizeof(staged));
  fsTestWrite(FILE_FERMENTER_BKP, staged);  // a good pair, so only the failed save marks it
  fsTestSetFull(true);
  TEST_ASSERT_TRUE(loadFermenterConfig());
  fsTestSetFull(false);
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"CeilingTemp\":[99"));
  configPeriodicSave();
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER,     "\"CeilingTemp\":[22,"));
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER_BKP, "\"CeilingTemp\":[22,"));
}

// ============================================================
// MOUNTING THE FILESYSTEM - retried before it is formatted
// ============================================================

static void test_a_normal_mount_keeps_the_settings_and_never_formats(void) {
  fsTestWrite(FILE_GLOBAL, "{\"Unit\":1}");
  TEST_ASSERT_TRUE(mountFileSystem());
  TEST_ASSERT_FALSE(g_fsAutoFormat);         // the library may never format by itself
  TEST_ASSERT_EQUAL_INT(1, g_fsBeginCalls);
  TEST_ASSERT_EQUAL_INT(0, g_fsFormats);
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_GLOBAL));
}

// One or two bad reads at boot: the settings survive
static void test_a_failed_mount_is_tried_again_before_formatting(void) {
  fsTestWrite(FILE_GLOBAL, "{\"Unit\":1}");
  fsTestSetMountFailures(2);
  TEST_ASSERT_TRUE(mountFileSystem());
  TEST_ASSERT_EQUAL_INT(3, g_fsBeginCalls);
  TEST_ASSERT_EQUAL_INT(0, g_fsFormats);
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_GLOBAL));
  logFileSystemBootProblems();
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "mounted at boot on attempt 3 of 3"));
}

// A new device with blank flash, or a filesystem that really is damaged
static void test_a_filesystem_that_never_mounts_is_formatted(void) {
  fsTestWrite(FILE_GLOBAL, "{\"Unit\":1}");
  fsTestSetMountFailures(3);
  TEST_ASSERT_TRUE(mountFileSystem());
  TEST_ASSERT_EQUAL_INT(4, g_fsBeginCalls);   // 3 failures, then once after the format
  TEST_ASSERT_EQUAL_INT(1, g_fsFormats);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_GLOBAL));
  logFileSystemBootProblems();
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "was formatted"));
}

static void test_a_filesystem_that_cannot_be_formatted_reports_failure(void) {
  fsTestSetMountFailures(3);
  fsTestSetFormatFails(true);
  TEST_ASSERT_FALSE(mountFileSystem());
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "unusable"));
}

// A plain first-time mount has nothing to report
static void test_a_normal_mount_logs_no_boot_problem(void) {
  TEST_ASSERT_TRUE(mountFileSystem());
  s_lastLog[0] = '\0';
  logFileSystemBootProblems();
  TEST_ASSERT_EQUAL_STRING("", s_lastLog);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  // primary/backup pair
  RUN_TEST(test_load_uses_the_primary_when_it_is_valid);
  RUN_TEST(test_load_falls_back_to_the_backup_when_the_primary_is_missing);
  RUN_TEST(test_load_falls_back_to_the_backup_when_the_primary_is_corrupt);
  RUN_TEST(test_load_partial_read_of_the_primary_does_not_leak_into_the_backup);
  RUN_TEST(test_load_falls_back_to_defaults_when_both_copies_are_bad);
  RUN_TEST(test_save_writes_both_the_primary_and_the_backup);
  RUN_TEST(test_save_fails_and_leaves_the_primary_untouched_when_the_fs_cannot_open);
  RUN_TEST(test_save_detects_a_truncated_write);
  RUN_TEST(test_save_refuses_a_document_that_ran_out_of_memory);

  // descriptor engine
  RUN_TEST(test_scalar_round_trip_preserves_every_field);
  RUN_TEST(test_array_round_trip_preserves_every_slot);
  RUN_TEST(test_missing_keys_fall_back_to_the_declared_defaults);
  RUN_TEST(test_wrong_json_type_falls_back_to_the_declared_default);
  RUN_TEST(test_null_json_value_falls_back_to_the_declared_default);
  RUN_TEST(test_unusable_stored_base_topic_is_repaired_on_load);
  RUN_TEST(test_empty_stored_base_topic_falls_back_to_the_default);
  RUN_TEST(test_working_stored_base_topic_is_loaded_unchanged);
  RUN_TEST(test_oversized_string_is_truncated_to_the_member_size);
  RUN_TEST(test_stored_text_is_made_valid_utf8_on_load);
  RUN_TEST(test_saved_key_order_matches_the_declared_table);
  RUN_TEST(test_array_files_hold_one_array_per_field_with_one_element_per_slot);

  // fermenter migrations + trio validation
  RUN_TEST(test_legacy_integer_gravities_are_scaled_to_sg);
  RUN_TEST(test_brewservices_bitmask_wins_when_the_key_is_present);
  RUN_TEST(test_valid_temp_trio_is_preserved);
  RUN_TEST(test_trio_at_the_exact_two_hysteresis_boundary_is_kept);
  RUN_TEST(test_ceiling_out_of_range_resets_the_trio);
  RUN_TEST(test_floor_at_or_above_ceiling_resets_the_trio);
  RUN_TEST(test_hysteresis_out_of_range_resets_the_trio);
  RUN_TEST(test_safe_zone_narrower_than_two_hysteresis_resets_the_trio);
  RUN_TEST(test_only_the_offending_fermenter_is_reset);
  RUN_TEST(test_trio_correction_is_written_back_to_disk);
  RUN_TEST(test_a_valid_config_is_not_rewritten_on_load);
  RUN_TEST(test_missing_fermenter_file_falls_back_to_defaults);
  RUN_TEST(test_temp_band_accepts_a_normal_setting);
  RUN_TEST(test_temp_band_accepts_the_exact_limits);
  RUN_TEST(test_temp_band_rejects_a_ceiling_out_of_range);
  RUN_TEST(test_temp_band_rejects_a_floor_out_of_range);
  RUN_TEST(test_temp_band_rejects_a_hysteresis_out_of_range);
  RUN_TEST(test_temp_band_rejects_a_floor_not_below_the_ceiling);
  RUN_TEST(test_temp_band_rejects_a_safe_zone_under_twice_the_hysteresis);

  // Tilt slot/colour mapping
  RUN_TEST(test_tilt_slots_load_into_colour_indexed_entries);
  RUN_TEST(test_tilt_unassigned_and_out_of_range_slots_are_skipped);
  RUN_TEST(test_tilt_legacy_function_values_collapse_to_unassigned);
  RUN_TEST(test_tilt_save_packs_configured_colours_first_and_pads_the_rest);
  RUN_TEST(test_tilt_round_trip_survives_the_slot_repacking);
  RUN_TEST(test_tilt_save_stops_at_max_slots_when_more_colours_are_configured);
  RUN_TEST(test_tilt_load_starts_from_defaults_when_the_file_is_missing);

  // brew service migrations
  RUN_TEST(test_brewsvc_three_slot_config_remaps_to_two);
  RUN_TEST(test_brewsvc_two_slot_config_loads_through_the_table);
  RUN_TEST(test_brewsvc_without_a_file_does_not_use_the_global_service);

  // index-dependent and post-load defaults
  RUN_TEST(test_smartplug_plugno_defaults_to_the_slot_index);
  RUN_TEST(test_smartplug_stored_plugno_is_respected);
  RUN_TEST(test_probe_load_starts_with_no_reading);
  RUN_TEST(test_probe_defaults_have_no_reading);
  RUN_TEST(test_ispindel_legacy_function_values_collapse_to_unassigned);
  RUN_TEST(test_ispindel_absent_function_defaults_to_beer_for_legacy_configs);
  RUN_TEST(test_ispindel_stored_ids_are_made_topic_safe_on_load);
  RUN_TEST(test_ispindel_temperature_starts_as_no_reading);
  RUN_TEST(test_ispindel_defaults_are_all_unassigned);
  RUN_TEST(test_load_corrects_an_out_of_range_probe_resolution);
  RUN_TEST(test_load_keeps_a_valid_probe_resolution);
  RUN_TEST(test_load_stops_a_profile_that_does_not_exist);
  RUN_TEST(test_load_corrects_an_out_of_range_profile_step);
  RUN_TEST(test_load_corrects_an_out_of_range_plug_bit_count);
  RUN_TEST(test_hour_ticks_are_saved_and_loaded);
  RUN_TEST(test_load_corrects_out_of_range_hour_ticks);
  RUN_TEST(test_profile_steps_load_returns_false_without_touching_state);
  RUN_TEST(test_profile_steps_round_trip_across_all_slots);

  // loadAllConfig one-shot migration
  RUN_TEST(test_alarm_tolerance_zero_is_bumped_to_three_on_first_load);
  RUN_TEST(test_alarm_tolerance_migration_does_not_run_twice);

  // resets
  RUN_TEST(test_reset_wifi_config_clears_the_provisioning_artifacts);
  RUN_TEST(test_reset_all_config_restores_defaults_and_writes_every_file);
  RUN_TEST(test_reset_all_config_clears_the_saved_tilt_settings);
  RUN_TEST(test_reset_all_config_survives_a_read_only_filesystem);

  // reboot log
  RUN_TEST(test_reboot_log_appends_an_entry);
  RUN_TEST(test_reboot_log_keeps_appending_across_calls);
  RUN_TEST(test_reboot_log_is_trimmed_to_the_last_ten_entries);
  RUN_TEST(test_reboot_log_records_register_detail_only_for_exception_resets);
  RUN_TEST(test_reboot_log_recovers_from_a_corrupt_log_file);

  // config import
  RUN_TEST(test_an_import_backs_up_the_current_settings_first);
  RUN_TEST(test_only_the_first_file_of_an_import_backs_up);
  RUN_TEST(test_the_periodic_save_is_held_off_during_an_import);
  RUN_TEST(test_an_abandoned_import_is_put_back);
  RUN_TEST(test_restore_puts_the_previous_settings_back_and_ends_the_import);

  // the 10-minute save
  RUN_TEST(test_the_periodic_save_leaves_unchanged_files_alone);
  RUN_TEST(test_the_periodic_save_writes_a_file_marked_unsaved);
  RUN_TEST(test_a_file_saved_straight_away_is_not_saved_again);
  RUN_TEST(test_a_failed_save_is_tried_again_by_the_periodic_save);
  RUN_TEST(test_a_save_that_fails_after_the_backup_is_tried_again);
  RUN_TEST(test_a_failed_tilt_save_is_tried_again_by_the_periodic_save);

  // mounting the filesystem
  RUN_TEST(test_a_normal_mount_keeps_the_settings_and_never_formats);
  RUN_TEST(test_a_failed_mount_is_tried_again_before_formatting);
  RUN_TEST(test_a_filesystem_that_never_mounts_is_formatted);
  RUN_TEST(test_a_filesystem_that_cannot_be_formatted_reports_failure);
  RUN_TEST(test_a_normal_mount_logs_no_boot_problem);

  // missing or damaged copies at load
  RUN_TEST(test_loading_a_good_pair_leaves_the_file_alone);
  RUN_TEST(test_a_missing_backup_is_saved_again);
  RUN_TEST(test_a_damaged_backup_is_saved_again);
  RUN_TEST(test_a_backup_with_a_bad_number_or_escape_counts_as_damaged);
  RUN_TEST(test_a_damaged_primary_is_saved_again_from_the_backup);
  RUN_TEST(test_a_missing_file_is_saved_with_the_defaults);
  RUN_TEST(test_a_correction_made_while_loading_is_saved_by_the_periodic_save);
  RUN_TEST(test_a_failed_correction_save_while_loading_is_tried_again);

  return UNITY_END();
}
