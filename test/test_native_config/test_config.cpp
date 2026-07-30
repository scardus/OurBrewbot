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

#include "../../OurBrewbot/Config.h"

// ---- millis(), settable per test (Config.cpp never calls it, but Arduino.h
// ---- declares it and other suites share the stub) ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}

// The code under test.
#include "../../OurBrewbot/Config.cpp"

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
  memset(&g_wifiConfig, 0, sizeof(g_wifiConfig));
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
    TEST_ASSERT_EQUAL_FLOAT(10.0f + (float)i, g_probes[i].temperature);
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

void test_oversized_string_is_truncated_to_the_member_size(void) {
  // SyslogConfig::host is char[64]; strlcpy must clamp rather than overflow
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

void test_legacy_brewservicesend_integer_migrates_to_the_bitmask(void) {
  // Pre-bitmask configs had a single per-fermenter flag; it becomes bit 0.
  // Stored as 0/1, the migration works.
  fsTestWrite(FILE_FERMENTER, "{\"BrewServiceSend\":[1,0,1,0]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[0].brewServices);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[1].brewServices);
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[2].brewServices);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[3].brewServices);
}

void test_legacy_brewservicesend_bool_migrates_to_the_bitmask(void) {
  // Regression guard for the v0.4.4 fix. BrewServiceSend was a bool member, so
  // it can also be on disk as a JSON boolean rather than 0/1. The original
  // `(doc[...][i] | 0)` read it as an int, and ArduinoJson requires the stored
  // type to BE an integer for that - a JSON boolean failed the check, took the
  // 0 default, and migrated to "not subscribed", silently dropping the user's
  // brew-service reporting on upgrade. Both encodings must give the same result.
  fsTestWrite(FILE_FERMENTER, "{\"BrewServiceSend\":[true,false,true,false]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[0].brewServices);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[1].brewServices);
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[2].brewServices);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[3].brewServices);
}

void test_legacy_brewservicesend_non_numeric_still_reads_as_unsubscribed(void) {
  // The fix must not widen what counts as "subscribed": only booleans take the
  // boolean path, so a hand-edited string keeps the original integer behaviour
  // (fails the int check, falls back to 0). Guards against reaching for
  // .as<bool>() alone, whose asBoolean() returns true for any non-null string.
  fsTestWrite(FILE_FERMENTER, "{\"BrewServiceSend\":[\"1\",\"0\",\"yes\",\"\"]}");
  TEST_ASSERT_TRUE(loadFermenterConfig());
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[i].brewServices);
  }
}

void test_brewservices_bitmask_wins_when_the_key_is_present(void) {
  // Both keys present: the new one must not be clobbered by the migration.
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

void test_brewsvc_legacy_global_brewers_friend_migrates_to_slot_zero(void) {
  g_globalConfig.brewService = BREW_SERVICE_BREWERS_FRIEND;   // 1
  strlcpy(g_globalConfig.brewServiceId, "legacy-bf", sizeof(g_globalConfig.brewServiceId));
  TEST_ASSERT_FALSE(loadBrewServiceConfig());
  TEST_ASSERT_TRUE(g_brewServices[0].enabled);
  TEST_ASSERT_EQUAL_STRING("legacy-bf", g_brewServices[0].serviceId);
  TEST_ASSERT_FALSE(g_brewServices[1].enabled);
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_BREWSVC));   // migration was persisted
}

void test_brewsvc_legacy_global_brewfather_migrates_to_slot_one(void) {
  g_globalConfig.brewService = 3;    // old "Brewfather" code, now index 1
  strlcpy(g_globalConfig.brewServiceId, "legacy-bfather", sizeof(g_globalConfig.brewServiceId));
  TEST_ASSERT_FALSE(loadBrewServiceConfig());
  TEST_ASSERT_FALSE(g_brewServices[0].enabled);
  TEST_ASSERT_TRUE(g_brewServices[1].enabled);
  TEST_ASSERT_EQUAL_STRING("legacy-bfather", g_brewServices[1].serviceId);
}

void test_brewsvc_legacy_global_removed_service_migrates_nothing(void) {
  g_globalConfig.brewService = 2;    // old "Monitor Beer", no new home
  strlcpy(g_globalConfig.brewServiceId, "legacy-mb", sizeof(g_globalConfig.brewServiceId));
  TEST_ASSERT_FALSE(loadBrewServiceConfig());
  TEST_ASSERT_FALSE(g_brewServices[0].enabled);
  TEST_ASSERT_FALSE(g_brewServices[1].enabled);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_BREWSVC));
}

void test_brewsvc_legacy_migration_is_skipped_when_a_file_exists_but_is_corrupt(void) {
  // The migration is first-boot-after-upgrade only. A parse failure with the
  // file present means the user HAS a config - re-running the migration would
  // resurrect a service they had since turned off.
  fsTestWrite(FILE_BREWSVC, "{corrupt");
  g_globalConfig.brewService = BREW_SERVICE_BREWERS_FRIEND;
  strlcpy(g_globalConfig.brewServiceId, "legacy-bf", sizeof(g_globalConfig.brewServiceId));
  TEST_ASSERT_FALSE(loadBrewServiceConfig());
  TEST_ASSERT_FALSE(g_brewServices[0].enabled);
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

void test_probe_load_mirrors_temperature_into_the_raw_reading(void) {
  fsTestWrite(FILE_PROBE, "{\"Temperature\":[19.5,0,0,0,0,0,0,0]}");
  TEST_ASSERT_TRUE(loadProbeConfig());
  TEST_ASSERT_EQUAL_FLOAT(19.5f, g_probes[0].temperature);
  TEST_ASSERT_EQUAL_FLOAT(19.5f, g_probes[0].rawTemperature);
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
  g_wifiConfig.magic = 0x627B4DAB;

  resetWiFiConfig();

  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG_BKP));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_DRD));
  TEST_ASSERT_EQUAL_UINT32(0, g_wifiConfig.magic);
  // Only the WiFi artifacts - the rest of the config is untouched.
  TEST_ASSERT_TRUE(LittleFS.exists(FILE_MQTT));
}

void test_reset_all_config_restores_defaults_and_writes_every_file(void) {
  fsTestWrite(FILE_CONFIG, "{\"ssid\":\"home\"}");
  g_fermenters[0].ceilingTemp = 40.0f;
  g_mqttConfig.enabled        = true;

  resetAllConfig();

  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_FALSE(g_mqttConfig.enabled);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot", g_mqttConfig.baseTopic);
  const char* files[] = {
    FILE_GLOBAL, FILE_FERMENTER, FILE_PROBE, FILE_SMARTPLUGS, FILE_PROFILE,
    FILE_STEPS, FILE_ISPINDEL, FILE_BREWSVC, FILE_MQTT, FILE_SYSLOG,
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    TEST_ASSERT_TRUE_MESSAGE(LittleFS.exists(files[i]), files[i]);
  }
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG));   // portal re-runs on boot
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

  // descriptor engine
  RUN_TEST(test_scalar_round_trip_preserves_every_field);
  RUN_TEST(test_array_round_trip_preserves_every_slot);
  RUN_TEST(test_missing_keys_fall_back_to_the_declared_defaults);
  RUN_TEST(test_wrong_json_type_falls_back_to_the_declared_default);
  RUN_TEST(test_null_json_value_falls_back_to_the_declared_default);
  RUN_TEST(test_oversized_string_is_truncated_to_the_member_size);
  RUN_TEST(test_saved_key_order_matches_the_declared_table);
  RUN_TEST(test_array_files_hold_one_array_per_field_with_one_element_per_slot);

  // fermenter migrations + trio validation
  RUN_TEST(test_legacy_integer_gravities_are_scaled_to_sg);
  RUN_TEST(test_legacy_brewservicesend_integer_migrates_to_the_bitmask);
  RUN_TEST(test_legacy_brewservicesend_bool_migrates_to_the_bitmask);
  RUN_TEST(test_legacy_brewservicesend_non_numeric_still_reads_as_unsubscribed);
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
  RUN_TEST(test_brewsvc_legacy_global_brewers_friend_migrates_to_slot_zero);
  RUN_TEST(test_brewsvc_legacy_global_brewfather_migrates_to_slot_one);
  RUN_TEST(test_brewsvc_legacy_global_removed_service_migrates_nothing);
  RUN_TEST(test_brewsvc_legacy_migration_is_skipped_when_a_file_exists_but_is_corrupt);

  // index-dependent and post-load defaults
  RUN_TEST(test_smartplug_plugno_defaults_to_the_slot_index);
  RUN_TEST(test_smartplug_stored_plugno_is_respected);
  RUN_TEST(test_probe_load_mirrors_temperature_into_the_raw_reading);
  RUN_TEST(test_ispindel_legacy_function_values_collapse_to_unassigned);
  RUN_TEST(test_ispindel_absent_function_defaults_to_beer_for_legacy_configs);
  RUN_TEST(test_profile_steps_load_returns_false_without_touching_state);
  RUN_TEST(test_profile_steps_round_trip_across_all_slots);

  // loadAllConfig one-shot migration
  RUN_TEST(test_alarm_tolerance_zero_is_bumped_to_three_on_first_load);
  RUN_TEST(test_alarm_tolerance_migration_does_not_run_twice);

  // resets
  RUN_TEST(test_reset_wifi_config_clears_the_provisioning_artifacts);
  RUN_TEST(test_reset_all_config_restores_defaults_and_writes_every_file);
  RUN_TEST(test_reset_all_config_survives_a_read_only_filesystem);

  // reboot log
  RUN_TEST(test_reboot_log_appends_an_entry);
  RUN_TEST(test_reboot_log_keeps_appending_across_calls);
  RUN_TEST(test_reboot_log_is_trimmed_to_the_last_ten_entries);
  RUN_TEST(test_reboot_log_records_register_detail_only_for_exception_resets);
  RUN_TEST(test_reboot_log_recovers_from_a_corrupt_log_file);

  return UNITY_END();
}
