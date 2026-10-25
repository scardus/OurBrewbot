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

// ---- log lines are discarded; the conversion report is checked instead ----
void logMsgImpl(uint8_t, PGM_P, ...) {}

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
  RUN_TEST(test_a_conversion_cut_short_runs_again_from_the_copies);
  RUN_TEST(test_a_finished_conversion_removes_its_marker);
  RUN_TEST(test_a_marker_left_by_a_power_cut_is_tidied_up);

  RUN_TEST(test_jsonConfig_is_deleted_and_not_copied);
  RUN_TEST(test_the_blynk_token_is_cleared);

  RUN_TEST(test_celsius_stays_celsius);
  RUN_TEST(test_the_original_fahrenheit_code_is_converted);
  RUN_TEST(test_the_report_starts_afresh_on_each_attempt);

  return UNITY_END();
}
