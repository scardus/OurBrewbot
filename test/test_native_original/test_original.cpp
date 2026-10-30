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

// Native (host) tests for OurBrewbot/OriginalConfig.cpp - the one-time
// conversion of a config left on flash by the original MyBrewbot firmware.
//
// Most tests stage a real original config (original_fixtures.h), run the same
// loadAllConfig() the firmware runs at boot, and check what OurBrewbot ends up
// with. Settings the real config never used are staged by editing one key of
// a fixture, so every file stays in the original's own format.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>

#include "../../OurBrewbot/Config.h"

// ---- millis() and the uptime clock (Config.cpp's import code uses them) ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

static uint32_t s_uptimeSec = 0;
uint32_t uptimeSeconds() { return s_uptimeSec; }
uint32_t uptimeStamp()   { return s_uptimeSec ? s_uptimeSec : 1; }
uint32_t uptimeSince(uint32_t stamp) { return (s_uptimeSec > stamp) ? s_uptimeSec - stamp : 0; }

// ---- log capture: the last line, for the DEFERRED give-up messages. Most
// tests check the conversion report instead. ----
static char s_lastLog[256];
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

#include "original_fixtures.h"

// ---- test fixture ----

void setUp(void) {
  fsTestReset();
  espTestSetResetReason(REASON_DEFAULT_RST);
  memset(&g_globalConfig, 0, sizeof(g_globalConfig));
  for (int i = 0; i < MAX_FERMENTERS; i++)   g_fermenters[i]   = FermenterConfig{};
  for (int i = 0; i < MAX_PROBES; i++)       g_probes[i]       = ProbeConfig{};
  for (int i = 0; i < MAX_SMART_PLUGS; i++)  g_smartPlugs[i]   = SmartPlugConfig{};
  for (int i = 0; i < MAX_PROFILES; i++)     g_profiles[i]     = ProfileConfig{};
  memset(g_profileSteps, 0, sizeof(g_profileSteps));
  initDefaultTiltConfig();   // unconfigured Tilt = colour 99, not 0 (0 is red)
  for (int i = 0; i < MAX_ISPINDELS; i++)    g_iSpindels[i]    = iSpindelConfig{};
  for (int i = 0; i < MAX_BREW_SERVICES; i++) g_brewServices[i] = BrewServiceConfig{};
  memset(&g_mqttConfig, 0, sizeof(g_mqttConfig));
  memset(&g_syslogConfig, 0, sizeof(g_syslogConfig));
  s_importActive = false;
  s_uptimeSec    = 1000;
  WiFi = ESP8266WiFiStub{};
  espTestClearRtc();   // a cold boot: no failed conversion attempts counted
  g_espRestarts  = 0;
  s_gaveUp       = false;
  s_copyFailed   = false;
  s_lastLog[0]   = '\0';
}

void tearDown(void) {}

// Puts the whole original config on the filesystem, as the original left it
static void stageOriginal() {
  fsTestWrite("/jsonGlobal.txt",       ORIG_GLOBAL);
  fsTestWrite("/jsonFermenter.txt",    ORIG_FERMENTER);
  fsTestWrite("/jsonProbe.txt",        ORIG_PROBE);
  fsTestWrite("/jsonSmartPlugs.txt",   ORIG_SMARTPLUGS);
  fsTestWrite("/jsonProfile.txt",      ORIG_PROFILE);
  fsTestWrite("/jsonProfileSteps.txt", ORIG_PROFILESTEPS);
  fsTestWrite("/jsonTilt.txt",         ORIG_TILT);
  fsTestWrite("/jsoniSpindel.txt",     ORIG_ISPINDEL);
  fsTestWrite("/jsonReBoot.txt",       ORIG_REBOOT);
  fsTestWrite("/jsonPressure.txt",     ORIG_PRESSURE);
  fsTestWrite("/jsonPlaato.txt",       ORIG_PLAATO);
  fsTestWrite("/jsonRepeater.txt",     ORIG_REPEATER);
  fsTestWrite("/jsonchartNames.txt",   ORIG_CHARTNAMES);
  fsTestWrite("/jsonchartSeries.txt",  ORIG_CHARTSERIES);
  fsTestWrite("/jsonConfig.txt",       ORIG_CONFIG);
}

// A fixture with one piece of text replaced, so a test can stage a setting the
// real config never used while keeping the original's format
static std::string edited(const char* fixture, const char* from, const char* to) {
  std::string s(fixture);
  size_t at = s.find(from);
  TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, from);
  s.replace(at, strlen(from), to);
  return s;
}

static bool fileHas(const char* path, const char* fragment) {
  return strstr(fsTestRead(path), fragment) != nullptr;
}

static bool reportHas(const char* fragment) {
  return fileHas(ORIG_REPORT_FILE, fragment);
}

// ============================================================
// Detection
// ============================================================

void test_a_new_device_has_nothing_to_convert(void) {
  TEST_ASSERT_FALSE(convertOriginalConfig());
  TEST_ASSERT_FALSE(LittleFS.exists("/orig/jsonFermenter.txt"));
}

void test_an_ourbrewbot_config_is_not_converted(void) {
  // OurBrewbot keeps the original's keys, so only BrewServices tells them apart
  fsTestWrite(FILE_FERMENTER, R"({"Power":[true,false,false,false],"BrewServices":[0,0,0,0]})");
  fsTestWrite("/jsonPlaato.txt", ORIG_PLAATO);   // left over from the original
  TEST_ASSERT_FALSE(convertOriginalConfig());
  TEST_ASSERT_FALSE(LittleFS.exists("/orig/jsonPlaato.txt"));
}

void test_an_original_config_is_converted_at_boot(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_TRUE(originalConfigConverted());
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"BrewServices\""));
  TEST_ASSERT_FALSE(fileHas(FILE_FERMENTER, "\"BrewServiceSend\""));
}

void test_a_converted_config_is_not_converted_again(void) {
  stageOriginal();
  TEST_ASSERT_TRUE(convertOriginalConfig());
  TEST_ASSERT_FALSE(convertOriginalConfig());
}

// ============================================================
// The copy in /orig/
// ============================================================

void test_the_untouched_files_are_kept_in_orig(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_EQUAL_STRING(ORIG_FERMENTER,   fsTestRead("/orig/jsonFermenter.txt"));
  TEST_ASSERT_EQUAL_STRING(ORIG_GLOBAL,      fsTestRead("/orig/jsonGlobal.txt"));
  TEST_ASSERT_EQUAL_STRING(ORIG_SMARTPLUGS,  fsTestRead("/orig/jsonSmartPlugs.txt"));
  TEST_ASSERT_EQUAL_STRING(ORIG_CHARTSERIES, fsTestRead("/orig/jsonchartSeries.txt"));
}

void test_backups_are_copied_too(void) {
  stageOriginal();
  fsTestWrite("/jsonProbebkup.txt", ORIG_PROBE);
  convertOriginalConfig();
  TEST_ASSERT_EQUAL_STRING(ORIG_PROBE, fsTestRead("/orig/jsonProbebkup.txt"));
}

void test_a_copy_left_from_an_older_conversion_is_removed(void) {
  // /orig/ must hold only the config converted last
  stageOriginal();
  LittleFS.remove("/jsonPressure.txt");
  fsTestWrite("/orig/jsonPressure.txt", ORIG_PRESSURE);
  convertOriginalConfig();
  TEST_ASSERT_FALSE(LittleFS.exists("/orig/jsonPressure.txt"));
}

void test_nothing_is_converted_if_the_copy_fails(void) {
  stageOriginal();
  fsTestSetFull(true);
  TEST_ASSERT_FALSE(convertOriginalConfig());
  fsTestSetFull(false);
  TEST_ASSERT_EQUAL_STRING(ORIG_FERMENTER, fsTestRead(FILE_FERMENTER));
  TEST_ASSERT_EQUAL_INT(1, g_espRestarts);   // restarts to try again
}

// ============================================================
// A file that can't be saved
// ============================================================

// The copies are in /orig/ and the conversion has to run again: the state a
// power cut part way through leaves. Every save then fails.
static void stageConversionWithFailingSaves() {
  stageOriginal();
  TEST_ASSERT_TRUE(convertOriginalConfig());
  fsTestWrite(FILE_FERMENTER, ORIG_FERMENTER);
  fsTestWrite(ORIG_IN_PROGRESS, "");
  fsTestSetFull(true);
}

void test_a_file_that_cant_be_saved_restarts_to_try_again(void) {
  stageConversionWithFailingSaves();
  TEST_ASSERT_FALSE(convertOriginalConfig());
  fsTestSetFull(false);

  TEST_ASSERT_EQUAL_INT(1, g_espRestarts);
  TEST_ASSERT_FALSE(originalConversionGaveUp());
  TEST_ASSERT_EQUAL_UINT32(1, readFailedAttempts());
  // Not marked as done, so the next boot converts it again
  TEST_ASSERT_EQUAL_STRING(ORIG_FERMENTER, fsTestRead(FILE_FERMENTER));
  TEST_ASSERT_TRUE(LittleFS.exists(ORIG_IN_PROGRESS));
}

void test_the_fermenter_file_waits_for_every_other_file(void) {
  // Only the brew services file fails. The fermenter file must stay original,
  // or the next boot would think the conversion had finished.
  stageOriginal();
  fsTestFailWritesTo(FILE_BREWSVC);
  TEST_ASSERT_FALSE(convertOriginalConfig());
  fsTestFailWritesTo(nullptr);
  TEST_ASSERT_EQUAL_STRING(ORIG_FERMENTER, fsTestRead(FILE_FERMENTER));
  TEST_ASSERT_EQUAL_INT(1, g_espRestarts);
}

void test_the_conversion_gives_up_after_three_tries(void) {
  stageConversionWithFailingSaves();
  convertOriginalConfig();   // each call is one boot - RTC memory carries the count
  convertOriginalConfig();
  TEST_ASSERT_FALSE(originalConversionGaveUp());
  convertOriginalConfig();
  fsTestSetFull(false);

  TEST_ASSERT_TRUE(originalConversionGaveUp());
  TEST_ASSERT_EQUAL_INT(2, g_espRestarts);          // no restart after the last try
  TEST_ASSERT_EQUAL_UINT32(0, readFailedAttempts()); // the next boot starts counting again
}

void test_giving_up_starts_on_default_settings(void) {
  stageConversionWithFailingSaves();
  convertOriginalConfig();
  convertOriginalConfig();
  g_fermenters[0].ceilingTemp = 40.0f;   // anything left from before
  g_smartPlugs[0].function    = 0;
  loadAllConfig();                       // the third try
  fsTestSetFull(false);

  TEST_ASSERT_TRUE(originalConversionGaveUp());
  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_EQUAL_UINT8(PLUG_FN_UNASSIGNED, g_smartPlugs[0].function);
}

void test_giving_up_saves_the_default_settings(void) {
  // Only the brew services file can't be saved, so the defaults reach every
  // other file and the next boot loads the same settings
  stageOriginal();
  fsTestFailWritesTo(FILE_BREWSVC);
  convertOriginalConfig();
  convertOriginalConfig();
  loadAllConfig();   // the third try
  fsTestFailWritesTo(nullptr);

  TEST_ASSERT_TRUE(originalConversionGaveUp());
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"BrewServices\""));   // no longer the original's
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"CeilingTemp\":[22,22,22,22]"));
  TEST_ASSERT_FALSE(convertOriginalConfig());   // so it isn't converted again
}

void test_giving_up_after_a_save_failure_says_the_originals_are_in_orig(void) {
  stageConversionWithFailingSaves();
  convertOriginalConfig();
  convertOriginalConfig();
  convertOriginalConfig();
  fsTestSetFull(false);
  logOriginalConversionProblems();
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "the originals are in /orig"));
}

void test_giving_up_after_a_copy_failure_says_the_originals_were_not_kept(void) {
  // The copy to /orig/ never finished, so the defaults saved over the live
  // files leave no copy of the original settings anywhere
  stageOriginal();
  fsTestSetFull(true);
  convertOriginalConfig();
  convertOriginalConfig();
  convertOriginalConfig();
  fsTestSetFull(false);
  TEST_ASSERT_TRUE(originalConversionGaveUp());
  logOriginalConversionProblems();
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "the original settings were not kept"));
  TEST_ASSERT_NULL(strstr(s_lastLog, "/orig"));
}

void test_a_retry_that_works_clears_the_count(void) {
  stageConversionWithFailingSaves();
  convertOriginalConfig();
  fsTestSetFull(false);
  TEST_ASSERT_TRUE(convertOriginalConfig());
  TEST_ASSERT_EQUAL_UINT32(0, readFailedAttempts());
  TEST_ASSERT_FALSE(LittleFS.exists(ORIG_IN_PROGRESS));
}

void test_a_power_cut_starts_the_count_again(void) {
  stageConversionWithFailingSaves();
  convertOriginalConfig();
  convertOriginalConfig();
  espTestClearRtc();          // power cut: RTC memory is lost
  convertOriginalConfig();
  fsTestSetFull(false);
  TEST_ASSERT_FALSE(originalConversionGaveUp());
  TEST_ASSERT_EQUAL_INT(3, g_espRestarts);
}

void test_a_conversion_cut_short_runs_again_from_the_copies(void) {
  // Power lost after the global file was converted but before the fermenter
  // file was: the marker is still there and the fermenter file is original.
  stageOriginal();
  convertOriginalConfig();
  fsTestWrite(FILE_FERMENTER, ORIG_FERMENTER);
  fsTestWrite(ORIG_IN_PROGRESS, "");

  TEST_ASSERT_TRUE(convertOriginalConfig());
  // The copy still holds the original, not the half-converted live file
  TEST_ASSERT_EQUAL_STRING(ORIG_GLOBAL, fsTestRead("/orig/jsonGlobal.txt"));
  TEST_ASSERT_FALSE(LittleFS.exists(ORIG_IN_PROGRESS));
}

void test_a_finished_conversion_removes_its_marker(void) {
  stageOriginal();
  convertOriginalConfig();
  TEST_ASSERT_FALSE(LittleFS.exists(ORIG_IN_PROGRESS));
}

void test_a_marker_left_by_a_power_cut_is_tidied_up(void) {
  // Power lost after the fermenter file was saved, before the marker went
  fsTestWrite(FILE_FERMENTER, R"({"BrewServices":[0,0,0,0]})");
  fsTestWrite(ORIG_IN_PROGRESS, "");
  convertOriginalConfig();
  TEST_ASSERT_FALSE(LittleFS.exists(ORIG_IN_PROGRESS));
}

// ============================================================
// An original file that can't be read
// ============================================================

void test_an_unreadable_file_is_saved_with_default_settings(void) {
  stageOriginal();
  fsTestWrite("/jsonProbe.txt",     "{not json");
  fsTestWrite("/jsonProbebkup.txt", "{not json");
  loadAllConfig();

  TEST_ASSERT_TRUE(originalConfigConverted());
  TEST_ASSERT_TRUE(reportHas("jsonProbe.txt could not be read - its settings start on defaults"));
  // Saved in OurBrewbot's format, not left for the loader to misread
  TEST_ASSERT_TRUE(fileHas(FILE_PROBE, "\"Function\":[99,99"));
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[0].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[0].fermenter);
  // The other files are still converted
  TEST_ASSERT_EQUAL_UINT8(PLUG_FN_F1_HOT, g_smartPlugs[0].function);
}

void test_an_unreadable_file_read_twice_is_reported_once(void) {
  // jsonGlobal.txt is read for the global and the brew service settings
  stageOriginal();
  fsTestWrite("/jsonGlobal.txt", "{not json");
  loadAllConfig();

  const char* text  = fsTestRead(ORIG_REPORT_FILE);
  const char* first = strstr(text, "jsonGlobal.txt could not be read");
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_NULL(strstr(first + 1, "jsonGlobal.txt could not be read"));
  TEST_ASSERT_EQUAL_UINT8(UNIT_CELSIUS, g_globalConfig.unit);
  TEST_ASSERT_FALSE(g_brewServices[BREW_SERVICE_BREWFATHER - 1].enabled);
}

void test_a_copy_that_cant_be_read_still_finishes_the_conversion(void) {
  // A retry after a power cut, when the copy of the fermenter file can't be
  // read but the live file still can: the conversion must still finish
  stageOriginal();
  convertOriginalConfig();
  fsTestWrite(FILE_FERMENTER, ORIG_FERMENTER);
  fsTestWrite(ORIG_IN_PROGRESS, "");
  fsTestWrite("/orig/jsonFermenter.txt", "{not json");
  loadAllConfig();

  TEST_ASSERT_TRUE(originalConfigConverted());
  TEST_ASSERT_TRUE(reportHas("jsonFermenter.txt could not be read"));
  TEST_ASSERT_TRUE(fileHas(FILE_FERMENTER, "\"BrewServices\""));
  TEST_ASSERT_EQUAL_FLOAT(22.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_FALSE(convertOriginalConfig());   // done, so not converted again
}

// ============================================================
// Secrets
// ============================================================

void test_jsonConfig_is_deleted_and_not_copied(void) {
  stageOriginal();
  fsTestWrite("/jsonConfigbkup.txt", ORIG_CONFIG);
  loadAllConfig();
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG_BKP));
  TEST_ASSERT_FALSE(LittleFS.exists("/orig/jsonConfig.txt"));
  TEST_ASSERT_TRUE(reportHas("Deleted jsonConfig.txt"));
}

void test_the_wifi_details_are_copied_to_the_wifi_store(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_TRUE(WiFi.persistentCalled);
  TEST_ASSERT_EQUAL(WIFI_STA, WiFi.modeSet);
  TEST_ASSERT_EQUAL_STRING("DummySSID", WiFi.savedSsid);
  TEST_ASSERT_EQUAL_STRING("DummyWiFiPassword", WiFi.savedPassword);
  TEST_ASSERT_TRUE(reportHas("Copied the WiFi details for DummySSID into the WiFi store"));
  // Only the network name is reported, never the password
  TEST_ASSERT_FALSE(reportHas("DummyWiFiPassword"));
}

void test_the_wifi_details_are_copied_from_the_backup_file(void) {
  stageOriginal();
  LittleFS.remove(FILE_CONFIG);
  fsTestWrite(FILE_CONFIG_BKP, ORIG_CONFIG);
  loadAllConfig();
  TEST_ASSERT_EQUAL_STRING("DummySSID", WiFi.savedSsid);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG_BKP));
}

void test_empty_wifi_details_are_not_copied(void) {
  stageOriginal();
  fsTestWrite(FILE_CONFIG, R"({"wifiSSID":"","wifiPass":""})");
  loadAllConfig();
  TEST_ASSERT_FALSE(WiFi.persistentCalled);
  TEST_ASSERT_EQUAL_STRING("", WiFi.savedSsid);
  TEST_ASSERT_TRUE(reportHas("No usable WiFi details in jsonConfig.txt"));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CONFIG));
}

void test_a_network_name_too_long_for_the_wifi_store_is_not_copied(void) {
  stageOriginal();
  // 33 characters - one more than the WiFi store holds
  fsTestWrite(FILE_CONFIG, R"({"wifiSSID":"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456","wifiPass":"secret"})");
  loadAllConfig();
  TEST_ASSERT_EQUAL_STRING("", WiFi.savedSsid);
  TEST_ASSERT_TRUE(reportHas("No usable WiFi details in jsonConfig.txt"));
}

void test_no_jsonConfig_means_nothing_is_copied(void) {
  stageOriginal();
  LittleFS.remove(FILE_CONFIG);
  loadAllConfig();
  TEST_ASSERT_FALSE(WiFi.persistentCalled);
  TEST_ASSERT_FALSE(reportHas("WiFi details"));
}

void test_the_blynk_token_is_cleared(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_EQUAL_STRING("", g_globalConfig.authCode);
  TEST_ASSERT_FALSE(fileHas(FILE_GLOBAL, "DummyBlynkToken"));
  TEST_ASSERT_FALSE(fileHas(FILE_GLOBAL_BKP, "DummyBlynkToken"));
  TEST_ASSERT_TRUE(reportHas("Blynk token cleared"));
}

// ============================================================
// Global
// ============================================================

void test_celsius_stays_celsius(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(UNIT_CELSIUS, g_globalConfig.unit);
}

void test_the_original_fahrenheit_code_is_converted(void) {
  // Original: 0 = Fahrenheit. OurBrewbot: UNIT_FAHRENHEIT.
  stageOriginal();
  fsTestWrite("/jsonGlobal.txt", edited(ORIG_GLOBAL, "\"unit\":1", "\"unit\":0").c_str());
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(UNIT_FAHRENHEIT, g_globalConfig.unit);
}

void test_the_report_starts_afresh_on_each_attempt(void) {
  stageOriginal();
  convertOriginalConfig();
  fsTestWrite(FILE_FERMENTER, ORIG_FERMENTER);
  fsTestWrite(ORIG_IN_PROGRESS, "");
  convertOriginalConfig();
  const char* text = fsTestRead(ORIG_REPORT_FILE);
  const char* first = strstr(text, "Blynk token cleared");
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_NULL(strstr(first + 1, "Blynk token cleared"));
}

// ============================================================
// Probes
// ============================================================

// The fixture's probe file with slot 2 given a role and fermenter
static void stageProbeRole(const char* role, const char* fermenter) {
  std::string probes = edited(ORIG_PROBE, "\"Function\":[2,3,99", (std::string("\"Function\":[2,3,") + role).c_str());
  probes = edited(probes.c_str(), "\"Fermenter\":[0,0,99", (std::string("\"Fermenter\":[0,0,") + fermenter).c_str());
  probes = edited(probes.c_str(), "\"Address\":[\"28805555a0080\",\"28ce2555a008b\",\"\"",
                                  "\"Address\":[\"28805555a0080\",\"28ce2555a008b\",\"28aabbccdd0011\"");
  fsTestWrite("/jsonProbe.txt", probes.c_str());
}

void test_beer_and_ambient_probes_keep_their_roles(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER, g_probes[0].function);
  TEST_ASSERT_EQUAL_UINT8(0, g_probes[0].fermenter);
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_AMBIENT, g_probes[1].function);
  TEST_ASSERT_EQUAL_UINT8(0, g_probes[1].fermenter);
  TEST_ASSERT_EQUAL_STRING("28805555a0080", g_probes[0].address);   // fixed at the first scan
}

void test_an_air_probe_becomes_air(void) {
  stageOriginal();
  stageProbeRole("4", "1");
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_AIR, g_probes[2].function);
  TEST_ASSERT_EQUAL_UINT8(1, g_probes[2].fermenter);
  TEST_ASSERT_TRUE(reportHas("Probe 3 (28aabbccdd0011): role 4 -> 6"));
}

void test_a_common_air_probe_becomes_air(void) {
  stageOriginal();
  stageProbeRole("8", "2");
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_AIR, g_probes[2].function);
  TEST_ASSERT_EQUAL_UINT8(2, g_probes[2].fermenter);
}

void test_a_common_ambient_probe_becomes_ambient_for_its_fermenter(void) {
  stageOriginal();
  stageProbeRole("7", "3");
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_AMBIENT, g_probes[2].function);
  TEST_ASSERT_EQUAL_UINT8(3, g_probes[2].fermenter);
  TEST_ASSERT_TRUE(reportHas("now for fermenter 4 only"));
}

void test_a_common_probe_without_a_fermenter_goes_to_fermenter_1(void) {
  stageOriginal();
  stageProbeRole("7", "99");
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_AMBIENT, g_probes[2].function);
  TEST_ASSERT_EQUAL_UINT8(0, g_probes[2].fermenter);
}

void test_a_discovered_probe_becomes_unassigned(void) {
  stageOriginal();
  stageProbeRole("1", "0");
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[2].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[2].fermenter);
  TEST_ASSERT_EQUAL_STRING("28aabbccdd0011", g_probes[2].address);   // still registered
}

// ============================================================
// Tilts
// ============================================================

// Red (original colour 1) on fermenter 1 and yellow (7) on fermenter 2
static const char TILTS_RED_AND_YELLOW[] =
  R"({"Address":[1,7,99,99],"Function":[2,2,99,99],"Fermenter":[0,1,99,99],"Temp_Adjust":[0.5,0,0,0],"SG_Adjust":[2,0,0,0],"MBB":[0,0,0,0]})";

// Fermenter 1 controls on its Tilt (TempControl 2), the others on a probe
static void stageTiltControlOnFermenter1() {
  fsTestWrite("/jsonFermenter.txt",
              edited(ORIG_FERMENTER, "\"TempControl\":[1,1,1,1]", "\"TempControl\":[2,1,1,1]").c_str());
}

void test_tilt_colours_are_renumbered_from_zero(void) {
  stageOriginal();
  fsTestWrite("/jsonTilt.txt", TILTS_RED_AND_YELLOW);
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(TILT_RED, g_tilts[TILT_RED].colour);
  TEST_ASSERT_EQUAL_UINT8(TILT_YELLOW, g_tilts[TILT_YELLOW].colour);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_GREEN].colour);   // not original 1 + 1
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_PINK].colour);    // not original 7 + 1
}

void test_a_tilt_chosen_for_control_gets_the_beer_role(void) {
  stageOriginal();
  fsTestWrite("/jsonTilt.txt", TILTS_RED_AND_YELLOW);
  stageTiltControlOnFermenter1();
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(0, g_tilts[TILT_RED].fermenter);
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER, g_tilts[TILT_RED].function);
}

void test_a_tilt_not_chosen_for_control_gives_gravity_only(void) {
  stageOriginal();
  fsTestWrite("/jsonTilt.txt", TILTS_RED_AND_YELLOW);
  stageTiltControlOnFermenter1();
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(1, g_tilts[TILT_YELLOW].fermenter);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_YELLOW].function);
  TEST_ASSERT_TRUE(reportHas("Tilt yellow: fermenter 2, gravity only"));
}

void test_an_unassigned_tilt_has_no_fermenter(void) {
  stageOriginal();
  fsTestWrite("/jsonTilt.txt",
              R"({"Address":[3,99,99,99],"Function":[99,99,99,99],"Fermenter":[0,99,99,99],"Temp_Adjust":[0,0,0,0],"SG_Adjust":[0,0,0,0],"MBB":[0,0,0,0]})");
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(TILT_BLACK, g_tilts[TILT_BLACK].colour);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_tilts[TILT_BLACK].fermenter);
}

void test_tilt_calibration_changes_sign(void) {
  // Original: shown = reading - adjust. OurBrewbot: shown = reading + adjust.
  stageOriginal();
  fsTestWrite("/jsonTilt.txt", TILTS_RED_AND_YELLOW);
  loadAllConfig();
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -0.5f, g_tilts[TILT_RED].tempAdjust);
  TEST_ASSERT_FLOAT_WITHIN(0.00001f, -0.002f, g_tilts[TILT_RED].sgAdjust);
}

void test_a_fahrenheit_tilt_calibration_becomes_celsius(void) {
  stageOriginal();
  fsTestWrite("/jsonGlobal.txt", edited(ORIG_GLOBAL, "\"unit\":1", "\"unit\":0").c_str());
  fsTestWrite("/jsonTilt.txt", edited(TILTS_RED_AND_YELLOW, "\"Temp_Adjust\":[0.5", "\"Temp_Adjust\":[0.9").c_str());
  loadAllConfig();
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -0.5f, g_tilts[TILT_RED].tempAdjust);
}

// ============================================================
// iSpindels
// ============================================================

// Slot 0: a real iSpindel on fermenter 1 (SG); slot 1: one on fermenter 2 (Plato)
static const char ISPINDELS_TWO[] =
  R"({"iSpindelName":["iSpindel001","iSpindel002","None","None"],"ID":[1234567,7654321,0,0],"iSpindelCollectData":[1,1,0,0],"iSpindelFermenter":[0,1,99,99],"Unit":[1,2,0,0]})";

void test_the_empty_ispindel_slot_of_the_real_config_stays_empty(void) {
  // The real file has slot 0 on fermenter 1 with ID 0 - an empty slot
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_EQUAL_STRING("", g_iSpindels[0].id);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_iSpindels[0].fermenter);
  TEST_ASSERT_FALSE(g_iSpindels[0].collectData);
}

void test_the_real_configs_unit_1_is_sg_not_plato(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_SG, g_iSpindels[0].unit);
}

void test_a_numeric_ispindel_id_is_kept_as_text(void) {
  stageOriginal();
  fsTestWrite("/jsoniSpindel.txt", ISPINDELS_TWO);
  loadAllConfig();
  TEST_ASSERT_EQUAL_STRING("1234567", g_iSpindels[0].id);
  TEST_ASSERT_EQUAL_UINT8(0, g_iSpindels[0].fermenter);
  TEST_ASSERT_TRUE(g_iSpindels[0].collectData);
}

void test_original_ispindel_unit_2_is_plato(void) {
  stageOriginal();
  fsTestWrite("/jsoniSpindel.txt", ISPINDELS_TWO);
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_PLATO, g_iSpindels[1].unit);
}

void test_an_ispindel_not_chosen_for_control_gives_gravity_only(void) {
  stageOriginal();
  fsTestWrite("/jsoniSpindel.txt", ISPINDELS_TWO);
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_iSpindels[0].function);
  TEST_ASSERT_TRUE(reportHas("iSpindel iSpindel001 (ID 1234567): fermenter 1, SG, gravity only"));
}

void test_an_ispindel_chosen_for_control_gets_the_beer_role(void) {
  stageOriginal();
  fsTestWrite("/jsoniSpindel.txt", ISPINDELS_TWO);
  fsTestWrite("/jsonFermenter.txt",
              edited(ORIG_FERMENTER, "\"TempControl\":[1,1,1,1]", "\"TempControl\":[1,4,1,1]").c_str());
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER, g_iSpindels[1].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_iSpindels[0].function);
}

// ============================================================
// Profiles
// ============================================================

// OurBrewbot's empty step (the same test as Profile.cpp's isStepEmpty())
static bool stepIsEmpty(const ProfileStep& s) {
  return s.stepType == 0 && s.days == 0 && s.startTemp == 0.0f && s.endTemp == 0.0f && s.sgTrigger == 0.0f;
}

struct OrigStep { int type; float start; float end; float sgTrigger; float days; };

// An original jsonProfileSteps.txt (60 steps, 15 per profile) with the given
// steps at the start of profile 1 and the rest empty
static std::string origSteps(const OrigStep* steps, int n) {
  const char* keys[] = { "StepNo", "StepType", "StartTemp", "EndTemp", "SGTrigger", "Days" };
  std::string json = "{";
  for (int k = 0; k < 6; k++) {
    json += std::string(k ? "," : "") + "\"" + keys[k] + "\":[";
    for (int i = 0; i < MAX_PROFILE_STEPS; i++) {
      char v[24] = "0";
      if (i < n) {
        float values[] = { 0, (float)steps[i].type, steps[i].start, steps[i].end, steps[i].sgTrigger, steps[i].days };
        snprintf(v, sizeof(v), "%g", values[k]);
      }
      json += std::string(i ? "," : "") + v;
    }
    json += "]";
  }
  return json + "}";
}

void test_the_real_configs_empty_profiles_stay_empty(void) {
  stageOriginal();
  loadAllConfig();
  for (int k = 0; k < MAX_PROFILE_STEPS; k++) {
    TEST_ASSERT_TRUE(stepIsEmpty(g_profileSteps[k]));
  }
}

void test_step_types_are_renumbered(void) {
  const OrigStep steps[] = {
    {1, 18, 18, 0, 2}, {2, 18, 20, 0, 2}, {3, 18, 22, 0, 2}, {4, 20, 20, 1010, 2},
    {5, 20, 20, 75, 2}, {6, 20, 22, 0, 1}, {7, 22, 22, 1012, 1}, {8, 22, 22, 80, 1}
  };
  stageOriginal();
  fsTestWrite("/jsonProfileSteps.txt", origSteps(steps, 8).c_str());
  loadAllConfig();
  const uint8_t expected[] = { STEP_TIME_OVER_TEMP, STEP_TEMP_OVER_TIME, STEP_FREE_RISE,
                               STEP_SPECIFIC_GRAVITY, STEP_ATTENUATION, STEP_TEMP_REACHED,
                               STEP_SG_REACHED, STEP_ATTN_REACHED };
  for (int k = 0; k < 8; k++) {
    TEST_ASSERT_EQUAL_UINT8(expected[k], g_profileSteps[k].stepType);
  }
  TEST_ASSERT_TRUE(stepIsEmpty(g_profileSteps[8]));
  TEST_ASSERT_TRUE(reportHas("Profile 1: 8 step(s) converted"));
}

void test_sg_targets_are_divided_by_1000(void) {
  const OrigStep steps[] = { {4, 20, 20, 1010, 2}, {7, 22, 22, 1012, 1} };
  stageOriginal();
  fsTestWrite("/jsonProfileSteps.txt", origSteps(steps, 2).c_str());
  loadAllConfig();
  TEST_ASSERT_FLOAT_WITHIN(0.00001f, 1.010f, g_profileSteps[0].sgTrigger);
  TEST_ASSERT_FLOAT_WITHIN(0.00001f, 1.012f, g_profileSteps[1].sgTrigger);
}

void test_attenuation_targets_stay_percentages(void) {
  const OrigStep steps[] = { {5, 20, 20, 75, 2}, {8, 22, 22, 80, 1} };
  stageOriginal();
  fsTestWrite("/jsonProfileSteps.txt", origSteps(steps, 2).c_str());
  loadAllConfig();
  TEST_ASSERT_EQUAL_FLOAT(75.0f, g_profileSteps[0].sgTrigger);
  TEST_ASSERT_EQUAL_FLOAT(80.0f, g_profileSteps[1].sgTrigger);
}

void test_a_type_0_step_with_values_becomes_empty(void) {
  // Type 0 ended the original's profile whatever else the step held
  const OrigStep steps[] = { {1, 18, 18, 0, 2}, {0, 20, 20, 0, 3}, {1, 22, 22, 0, 1} };
  stageOriginal();
  fsTestWrite("/jsonProfileSteps.txt", origSteps(steps, 3).c_str());
  loadAllConfig();
  TEST_ASSERT_TRUE(stepIsEmpty(g_profileSteps[1]));
}

void test_fahrenheit_step_temperatures_become_celsius(void) {
  const OrigStep steps[] = { {2, 68, 50, 0, 2} };
  stageOriginal();
  fsTestWrite("/jsonGlobal.txt", edited(ORIG_GLOBAL, "\"unit\":1", "\"unit\":0").c_str());
  fsTestWrite("/jsonProfileSteps.txt", origSteps(steps, 1).c_str());
  loadAllConfig();
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, g_profileSteps[0].startTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, g_profileSteps[0].endTemp);
  TEST_ASSERT_TRUE(stepIsEmpty(g_profileSteps[1]));   // not -17.8 C
}

void test_a_running_profile_is_stopped(void) {
  stageOriginal();
  std::string ferm = edited(ORIG_FERMENTER, "\"ProfileRunning\":[0,0,0,0]", "\"ProfileRunning\":[1,0,0,0]");
  ferm = edited(ferm.c_str(), "\"ProfileNo\":[0,0,0,0]", "\"ProfileNo\":[2,0,0,0]");
  ferm = edited(ferm.c_str(), "\"CurrentHour\":[0,0,0,0]", "\"CurrentHour\":[30,0,0,0]");
  fsTestWrite("/jsonFermenter.txt", ferm.c_str());
  loadAllConfig();
  TEST_ASSERT_FALSE(g_fermenters[0].profileRunning);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[0].profileNo);
  TEST_ASSERT_EQUAL_UINT16(0, g_fermenters[0].currentHour);
  TEST_ASSERT_TRUE(reportHas("Fermenter 1: profile 2 was running - stopped"));
}

// ============================================================
// Brew services
// ============================================================

static void stageFermenter1Sends() {
  fsTestWrite("/jsonFermenter.txt",
              edited(ORIG_FERMENTER, "\"BrewServiceSend\":[1,1,1,1]", "\"BrewServiceSend\":[0,1,1,1]").c_str());
}

void test_the_real_configs_brewfather_service_is_kept(void) {
  stageOriginal();
  loadAllConfig();
  const int slot = BREW_SERVICE_BREWFATHER - 1;
  TEST_ASSERT_TRUE(g_brewServices[slot].enabled);
  TEST_ASSERT_EQUAL_STRING("DummyBrewId123", g_brewServices[slot].serviceId);
  TEST_ASSERT_FALSE(g_brewServices[BREW_SERVICE_BREWERS_FRIEND - 1].enabled);
  TEST_ASSERT_TRUE(reportHas("Brew service: Brewfather, ID kept"));
}

void test_the_real_configs_fermenters_send_nothing(void) {
  // Its BrewServiceSend is [1,1,1,1] - in the original, 1 means DON'T send
  stageOriginal();
  loadAllConfig();
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[i].brewServices);
  }
}

void test_a_sending_fermenter_is_subscribed_to_brewfather(void) {
  stageOriginal();
  stageFermenter1Sends();
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(1 << (BREW_SERVICE_BREWFATHER - 1), g_fermenters[0].brewServices);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[1].brewServices);
}

void test_original_service_2_is_brewers_friend(void) {
  stageOriginal();
  stageFermenter1Sends();
  fsTestWrite("/jsonGlobal.txt", edited(ORIG_GLOBAL, "\"brewservice\":3", "\"brewservice\":2").c_str());
  loadAllConfig();
  const int slot = BREW_SERVICE_BREWERS_FRIEND - 1;
  TEST_ASSERT_TRUE(g_brewServices[slot].enabled);
  TEST_ASSERT_EQUAL_STRING("DummyBrewId123", g_brewServices[slot].serviceId);
  TEST_ASSERT_EQUAL_UINT8(1 << slot, g_fermenters[0].brewServices);
}

void test_monitor_beer_is_reported_and_not_converted(void) {
  stageOriginal();
  stageFermenter1Sends();
  fsTestWrite("/jsonGlobal.txt", edited(ORIG_GLOBAL, "\"brewservice\":3", "\"brewservice\":4").c_str());
  loadAllConfig();
  TEST_ASSERT_FALSE(g_brewServices[0].enabled);
  TEST_ASSERT_FALSE(g_brewServices[1].enabled);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[0].brewServices);
  TEST_ASSERT_TRUE(reportHas("monitor.beer is not supported yet"));
}

void test_the_originals_default_service_id_is_not_a_service(void) {
  stageOriginal();
  stageFermenter1Sends();
  fsTestWrite("/jsonGlobal.txt", edited(ORIG_GLOBAL, "\"DummyBrewId123\"", "\"Brewservice\"").c_str());
  loadAllConfig();
  TEST_ASSERT_FALSE(g_brewServices[BREW_SERVICE_BREWFATHER - 1].enabled);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[0].brewServices);
}

// ============================================================
// Smart plugs
// ============================================================

// rc-switch's own getCodeWordB() + sendTriState() (RCSwitch.cpp), written out
// with strings, to check dialPlugCode() against
static uint32_t rcSwitchTypeB(int group, int button, bool on) {
  char word[13];
  int pos = 0;
  for (int i = 1; i <= 4; i++) word[pos++] = (group == i)  ? '0' : 'F';
  for (int i = 1; i <= 4; i++) word[pos++] = (button == i) ? '0' : 'F';
  word[pos++] = 'F';
  word[pos++] = 'F';
  word[pos++] = 'F';
  word[pos++] = on ? 'F' : '0';
  word[pos]   = '\0';
  uint32_t code = 0;
  for (const char* p = word; *p; p++) {
    code <<= 2;
    if (*p == 'F') code |= 1;
    if (*p == '1') code |= 3;
  }
  return code;
}

void test_dial_codes_match_rc_switch_for_every_group_and_button(void) {
  for (int group = 1; group <= 4; group++) {
    for (int button = 1; button <= 4; button++) {
      TEST_ASSERT_EQUAL_UINT32(rcSwitchTypeB(group, button, true),  dialPlugCode(group, button, true));
      TEST_ASSERT_EQUAL_UINT32(rcSwitchTypeB(group, button, false), dialPlugCode(group, button, false));
    }
  }
}

void test_dial_codes_out_of_range_are_zero(void) {
  TEST_ASSERT_EQUAL_UINT32(0, dialPlugCode(0, 1, true));
  TEST_ASSERT_EQUAL_UINT32(0, dialPlugCode(5, 1, true));
  TEST_ASSERT_EQUAL_UINT32(0, dialPlugCode(1, 0, true));
  TEST_ASSERT_EQUAL_UINT32(0, dialPlugCode(1, 5, true));
}

void test_the_real_configs_dial_plugs_get_their_real_codes(void) {
  // The same plugs, set up by hand on the same controller after the switch to
  // OurBrewbot, switch with exactly these codes
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT32(5313877, g_smartPlugs[0].onCode);
  TEST_ASSERT_EQUAL_UINT32(5313876, g_smartPlugs[0].offCode);
  TEST_ASSERT_EQUAL_UINT32(5326165, g_smartPlugs[1].onCode);
  TEST_ASSERT_EQUAL_UINT32(5326164, g_smartPlugs[1].offCode);
  TEST_ASSERT_EQUAL_UINT8(24, g_smartPlugs[0].bits);
  TEST_ASSERT_EQUAL_UINT16(427, g_smartPlugs[0].delayLength);
}

void test_the_real_configs_plugs_heat_and_cool_fermenter_1(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PLUG_FN_F1_HOT, g_smartPlugs[0].function);
  TEST_ASSERT_EQUAL_UINT8(0, g_smartPlugs[0].fermenter);
  TEST_ASSERT_EQUAL_UINT8(PLUG_FN_F1_COLD, g_smartPlugs[1].function);
  TEST_ASSERT_EQUAL_UINT8(0, g_smartPlugs[1].fermenter);
  TEST_ASSERT_TRUE(reportHas("Plug 2: fermenter 1 cooling"));
}

void test_a_slot_without_codes_has_no_role(void) {
  stageOriginal();
  loadAllConfig();
  for (int i = 2; i < MAX_SMART_PLUGS; i++) {
    TEST_ASSERT_EQUAL_UINT8(PLUG_FN_UNASSIGNED, g_smartPlugs[i].function);
    TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_smartPlugs[i].fermenter);
  }
}

void test_a_raw_code_plug_keeps_its_codes_and_gets_its_slots_role(void) {
  // Slot 3 (fermenter 2 cooling), a learned plug (type 11) with raw codes
  stageOriginal();
  std::string plugs = edited(ORIG_SMARTPLUGS, "\"Type\":[1,1,1,1", "\"Type\":[1,1,1,11");
  plugs = edited(plugs.c_str(), "\"OnCode\":[1,2,0,0", "\"OnCode\":[1,2,0,4542805");
  plugs = edited(plugs.c_str(), "\"OffCode\":[1,2,0,0", "\"OffCode\":[1,2,0,4542804");
  fsTestWrite("/jsonSmartPlugs.txt", plugs.c_str());
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT32(4542805, g_smartPlugs[3].onCode);
  TEST_ASSERT_EQUAL_UINT32(4542804, g_smartPlugs[3].offCode);
  TEST_ASSERT_EQUAL_UINT8(PLUG_FN_F2_COLD, g_smartPlugs[3].function);
  TEST_ASSERT_EQUAL_UINT8(1, g_smartPlugs[3].fermenter);
}

void test_an_aux_slot_becomes_an_aux_plug(void) {
  stageOriginal();
  std::string plugs = edited(ORIG_SMARTPLUGS, "\"Type\":[1,1,1,1,1,1,1,1,1,1]", "\"Type\":[1,1,1,1,1,1,1,1,10,1]");
  plugs = edited(plugs.c_str(), "\"OnCode\":[1,2,0,0,0,0,0,0,0,0]", "\"OnCode\":[1,2,0,0,0,0,0,0,1234567,0]");
  plugs = edited(plugs.c_str(), "\"OffCode\":[1,2,0,0,0,0,0,0,0,0]", "\"OffCode\":[1,2,0,0,0,0,0,0,1234566,0]");
  fsTestWrite("/jsonSmartPlugs.txt", plugs.c_str());
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT8(PLUG_FN_AUX1, g_smartPlugs[8].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_smartPlugs[8].fermenter);
  TEST_ASSERT_TRUE(reportHas("Plug 9: Aux 1"));
}

void test_a_dial_plug_with_an_unknown_group_is_reported(void) {
  stageOriginal();
  fsTestWrite("/jsonSmartPlugs.txt", edited(ORIG_SMARTPLUGS, "\"Codeset\":[3,3", "\"Codeset\":[7,3").c_str());
  loadAllConfig();
  TEST_ASSERT_EQUAL_UINT32(0, g_smartPlugs[0].onCode);
  TEST_ASSERT_EQUAL_UINT8(PLUG_FN_UNASSIGNED, g_smartPlugs[0].function);
  TEST_ASSERT_TRUE(reportHas("Plug 1: Dial settings not recognised"));
}

// ============================================================
// Fermenters
// ============================================================

void test_power_is_inverted(void) {
  // The real config: [0,1,1,1] - only fermenter 1 was in use
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_TRUE(g_fermenters[0].power);
  TEST_ASSERT_FALSE(g_fermenters[1].power);
  TEST_ASSERT_FALSE(g_fermenters[2].power);
  TEST_ASSERT_FALSE(g_fermenters[3].power);
}

void test_temperature_control_is_on_and_sg_control_off(void) {
  stageOriginal();
  loadAllConfig();
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    TEST_ASSERT_TRUE(g_fermenters[i].tempControl);
    TEST_ASSERT_FALSE(g_fermenters[i].sgControl);
  }
}

void test_a_percentage_alarm_tolerance_becomes_the_default(void) {
  stageOriginal();
  fsTestWrite("/jsonFermenter.txt",
              edited(ORIG_FERMENTER, "\"AlarmTolerance\":[0,0,0,0]", "\"AlarmTolerance\":[5,10,0,0]").c_str());
  loadAllConfig();
  TEST_ASSERT_EQUAL_FLOAT(3.0f, g_fermenters[0].alarmTolerance);
  TEST_ASSERT_EQUAL_FLOAT(3.0f, g_fermenters[1].alarmTolerance);
}

void test_the_band_widens_by_the_hysteresis(void) {
  // Fermenter 1: floor 1, ceiling 2, hysteresis 0.1. The original heated below
  // 0.9 and cooled above 2.1, so those become the floor and ceiling.
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, g_fermenters[0].floorTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.1f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, g_fermenters[0].hysteresis);
}

void test_floor_equal_to_ceiling_is_kept_not_reset(void) {
  // Fermenters 2-4: floor = ceiling = 20, no hysteresis. Unconverted,
  // OurBrewbot's validation would reset them to 18-22.
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.5f, g_fermenters[1].floorTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.5f, g_fermenters[1].ceilingTemp);
}

void test_fahrenheit_settings_become_celsius(void) {
  stageOriginal();
  fsTestWrite("/jsonGlobal.txt", edited(ORIG_GLOBAL, "\"unit\":1", "\"unit\":0").c_str());
  std::string ferm = edited(ORIG_FERMENTER, "\"CeilingTemp\":[2,", "\"CeilingTemp\":[68,");
  ferm = edited(ferm.c_str(), "\"FloorTemp\":[1,", "\"FloorTemp\":[59,");
  ferm = edited(ferm.c_str(), "\"Hysteresis\":[0.1,", "\"Hysteresis\":[0.9,");
  fsTestWrite("/jsonFermenter.txt", ferm.c_str());
  loadAllConfig();
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f,  g_fermenters[0].hysteresis);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.5f, g_fermenters[0].floorTemp);     // 15 C - 0.5
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.5f, g_fermenters[0].ceilingTemp);   // 20 C + 0.5
}

void test_control_on_the_ambient_probe_is_reported(void) {
  stageOriginal();
  fsTestWrite("/jsonFermenter.txt",
              edited(ORIG_FERMENTER, "\"TempControl\":[1,1,1,1]", "\"TempControl\":[3,1,1,1]").c_str());
  loadAllConfig();
  TEST_ASSERT_TRUE(reportHas("Fermenter 1: controlled on its ambient probe before"));
}

void test_gravities_are_scaled_by_the_loader(void) {
  stageOriginal();
  loadAllConfig();
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.047f, g_fermenters[0].og);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.010f, g_fermenters[0].tg);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_a_new_device_has_nothing_to_convert);
  RUN_TEST(test_an_ourbrewbot_config_is_not_converted);
  RUN_TEST(test_an_original_config_is_converted_at_boot);
  RUN_TEST(test_a_converted_config_is_not_converted_again);

  RUN_TEST(test_the_untouched_files_are_kept_in_orig);
  RUN_TEST(test_backups_are_copied_too);
  RUN_TEST(test_a_copy_left_from_an_older_conversion_is_removed);
  RUN_TEST(test_nothing_is_converted_if_the_copy_fails);
  RUN_TEST(test_a_file_that_cant_be_saved_restarts_to_try_again);
  RUN_TEST(test_the_fermenter_file_waits_for_every_other_file);
  RUN_TEST(test_the_conversion_gives_up_after_three_tries);
  RUN_TEST(test_giving_up_starts_on_default_settings);
  RUN_TEST(test_giving_up_saves_the_default_settings);
  RUN_TEST(test_giving_up_after_a_save_failure_says_the_originals_are_in_orig);
  RUN_TEST(test_giving_up_after_a_copy_failure_says_the_originals_were_not_kept);
  RUN_TEST(test_a_retry_that_works_clears_the_count);
  RUN_TEST(test_a_power_cut_starts_the_count_again);
  RUN_TEST(test_a_conversion_cut_short_runs_again_from_the_copies);
  RUN_TEST(test_a_finished_conversion_removes_its_marker);
  RUN_TEST(test_a_marker_left_by_a_power_cut_is_tidied_up);

  RUN_TEST(test_an_unreadable_file_is_saved_with_default_settings);
  RUN_TEST(test_an_unreadable_file_read_twice_is_reported_once);
  RUN_TEST(test_a_copy_that_cant_be_read_still_finishes_the_conversion);
  RUN_TEST(test_jsonConfig_is_deleted_and_not_copied);
  RUN_TEST(test_the_wifi_details_are_copied_to_the_wifi_store);
  RUN_TEST(test_the_wifi_details_are_copied_from_the_backup_file);
  RUN_TEST(test_empty_wifi_details_are_not_copied);
  RUN_TEST(test_a_network_name_too_long_for_the_wifi_store_is_not_copied);
  RUN_TEST(test_no_jsonConfig_means_nothing_is_copied);
  RUN_TEST(test_the_blynk_token_is_cleared);

  RUN_TEST(test_celsius_stays_celsius);
  RUN_TEST(test_the_original_fahrenheit_code_is_converted);
  RUN_TEST(test_the_report_starts_afresh_on_each_attempt);

  RUN_TEST(test_beer_and_ambient_probes_keep_their_roles);
  RUN_TEST(test_an_air_probe_becomes_air);
  RUN_TEST(test_a_common_air_probe_becomes_air);
  RUN_TEST(test_a_common_ambient_probe_becomes_ambient_for_its_fermenter);
  RUN_TEST(test_a_common_probe_without_a_fermenter_goes_to_fermenter_1);
  RUN_TEST(test_a_discovered_probe_becomes_unassigned);

  RUN_TEST(test_tilt_colours_are_renumbered_from_zero);
  RUN_TEST(test_a_tilt_chosen_for_control_gets_the_beer_role);
  RUN_TEST(test_a_tilt_not_chosen_for_control_gives_gravity_only);
  RUN_TEST(test_an_unassigned_tilt_has_no_fermenter);
  RUN_TEST(test_tilt_calibration_changes_sign);
  RUN_TEST(test_a_fahrenheit_tilt_calibration_becomes_celsius);

  RUN_TEST(test_the_empty_ispindel_slot_of_the_real_config_stays_empty);
  RUN_TEST(test_the_real_configs_unit_1_is_sg_not_plato);
  RUN_TEST(test_a_numeric_ispindel_id_is_kept_as_text);
  RUN_TEST(test_original_ispindel_unit_2_is_plato);
  RUN_TEST(test_an_ispindel_not_chosen_for_control_gives_gravity_only);
  RUN_TEST(test_an_ispindel_chosen_for_control_gets_the_beer_role);

  RUN_TEST(test_the_real_configs_empty_profiles_stay_empty);
  RUN_TEST(test_step_types_are_renumbered);
  RUN_TEST(test_sg_targets_are_divided_by_1000);
  RUN_TEST(test_attenuation_targets_stay_percentages);
  RUN_TEST(test_a_type_0_step_with_values_becomes_empty);
  RUN_TEST(test_fahrenheit_step_temperatures_become_celsius);
  RUN_TEST(test_a_running_profile_is_stopped);

  RUN_TEST(test_the_real_configs_brewfather_service_is_kept);
  RUN_TEST(test_the_real_configs_fermenters_send_nothing);
  RUN_TEST(test_a_sending_fermenter_is_subscribed_to_brewfather);
  RUN_TEST(test_original_service_2_is_brewers_friend);
  RUN_TEST(test_monitor_beer_is_reported_and_not_converted);
  RUN_TEST(test_the_originals_default_service_id_is_not_a_service);

  RUN_TEST(test_dial_codes_match_rc_switch_for_every_group_and_button);
  RUN_TEST(test_dial_codes_out_of_range_are_zero);
  RUN_TEST(test_the_real_configs_dial_plugs_get_their_real_codes);
  RUN_TEST(test_the_real_configs_plugs_heat_and_cool_fermenter_1);
  RUN_TEST(test_a_slot_without_codes_has_no_role);
  RUN_TEST(test_a_raw_code_plug_keeps_its_codes_and_gets_its_slots_role);
  RUN_TEST(test_an_aux_slot_becomes_an_aux_plug);
  RUN_TEST(test_a_dial_plug_with_an_unknown_group_is_reported);

  RUN_TEST(test_power_is_inverted);
  RUN_TEST(test_temperature_control_is_on_and_sg_control_off);
  RUN_TEST(test_a_percentage_alarm_tolerance_becomes_the_default);
  RUN_TEST(test_the_band_widens_by_the_hysteresis);
  RUN_TEST(test_floor_equal_to_ceiling_is_kept_not_reset);
  RUN_TEST(test_fahrenheit_settings_become_celsius);
  RUN_TEST(test_control_on_the_ambient_probe_is_reported);
  RUN_TEST(test_gravities_are_scaled_by_the_loader);

  return UNITY_END();
}
