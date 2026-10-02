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

// Native (host) tests for OurBrewbot/CrashReport.cpp: the JSON report built
// from g_lastCrash, and the scheduler that sends it once, 2 minutes after
// boot, with retries.
//
// CrashReport.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test, and its file-static scheduler state
// is in scope for setUp() to reset. test/stubs/ESP8266HTTPClient.h records
// the POST instead of sending it. The JSON checks mirror the website Worker's
// validation (worker/index.js in the OurBrewbot-Website repo), which rejects
// anything that doesn't match.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#include "../../OurBrewbot/Config.h"
#include "../../OurBrewbot/Crash.h"

// ---- storage normally defined elsewhere ----
GlobalConfig g_globalConfig;
CrashInfo    g_lastCrash;

// ---- millis(), settable per test ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }

// ---- log capture ----
static char s_lastLog[192];
void logMsgImpl(uint8_t, PGM_P fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_lastLog, sizeof(s_lastLog), fmt, args);
  va_end(args);
}

// ---- Crash.cpp stand-ins ----
static uint8_t s_lastCheckpoint = 0xFF;
void checkpoint(uint8_t module) { s_lastCheckpoint = module; }
const char* checkpointName(uint32_t module) {
  if (module == CP_MQTT_PEND) return "MQTT_PEND";
  if (module == CP_MDNS) return "MDNS";
  return "?";
}

// The code under test
#include "../../OurBrewbot/CrashReport.cpp"

// ---- fixture ----

static void stageException() {
  memset(&g_lastCrash, 0, sizeof(g_lastCrash));
  g_lastCrash.valid         = true;
  g_lastCrash.haveRegisters = true;
  g_lastCrash.resetCode     = 2;              // REASON_EXCEPTION_RST
  g_lastCrash.lastModule    = CP_MQTT_PEND;
  g_lastCrash.reason        = 254;            // panic / heap-check hit
  g_lastCrash.exccause      = 28;
  g_lastCrash.epc1          = 0x4021A3B0;
  g_lastCrash.excvaddr      = 0x3FFF0000;
  g_lastCrash.sp            = 0x3FFFFD80;
  g_lastCrash.spEnd         = 0x3FFFFFB0;
  for (int i = 0; i < CRASH_STACK_WORDS; i++) g_lastCrash.stack[i] = 0x40201000 + i;
  g_lastCrash.stackFree     = 924;
  strcpy(g_lastCrash.stackAt, "WEB /iSpindel");
}

static void stageWatchdog() {
  memset(&g_lastCrash, 0, sizeof(g_lastCrash));
  g_lastCrash.valid      = true;
  g_lastCrash.resetCode  = 1;                 // REASON_WDT_RST
  g_lastCrash.lastModule = CP_MDNS;
}

void setUp(void) {
  s_millis = 0;
  s_lastLog[0] = '\0';
  s_lastCheckpoint = 0xFF;
  memset(&g_globalConfig, 0, sizeof(g_globalConfig));
  g_globalConfig.crashReports = true;
  memset(&g_lastCrash, 0, sizeof(g_lastCrash));
  WiFi.connected = true;
  httpTestReset();
  g_httpTest.nextStatus = 204;

  // CrashReport.cpp's state as it is at boot
  s_attempts = 0;
  s_lastAttemptMs = 0;
  s_finished = false;
}

void tearDown(void) {}

// Run the loop at a given time and report whether a POST was made
static bool loopAt(uint32_t ms) {
  int before = g_httpTest.postCount;
  s_millis = ms;
  crashReportLoop();
  return g_httpTest.postCount != before;
}

static const uint32_t MIN = 60000UL;

// ============================================================
// The JSON report
// ============================================================

static void test_report_carries_the_full_register_frame(void) {
  stageException();
  char out[768];
  TEST_ASSERT_TRUE(buildCrashReportJson(g_lastCrash, out, sizeof(out)));

  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, out) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("2924fa",      doc["id"].as<const char*>());   // stub chip ID
  TEST_ASSERT_EQUAL_STRING(FW_VERSION,    doc["v"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(FW_BUILD_DATE, doc["build"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(2,   doc["reset"].as<int>());
  TEST_ASSERT_EQUAL_STRING("MQTT_PEND", doc["last"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(254, doc["reason"].as<int>());
  TEST_ASSERT_EQUAL_INT(28,  doc["exccause"].as<int>());
  // Registers are lowercase hex strings without "0x", as the website requires
  TEST_ASSERT_EQUAL_STRING("4021a3b0", doc["epc1"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("0",        doc["epc2"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("3ffffd80", doc["sp"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("3fffffb0", doc["sp_end"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(CRASH_STACK_WORDS, doc["stack"].size());
  TEST_ASSERT_EQUAL_STRING("40201000", doc["stack"][0].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("40201017", doc["stack"][23].as<const char*>());
  TEST_ASSERT_EQUAL_INT(924, doc["stack_free"].as<int>());
  TEST_ASSERT_EQUAL_STRING("WEB /iSpindel", doc["stack_at"].as<const char*>());
}

// The website accepts at most 2048 bytes; keep well clear of it
static void test_a_full_report_is_well_under_the_website_limit(void) {
  stageException();
  for (int i = 0; i < CRASH_STACK_WORDS; i++) g_lastCrash.stack[i] = 0xFFFFFFFF;
  g_lastCrash.stackFree = 4096;
  memset(g_lastCrash.stackAt, 'x', sizeof(g_lastCrash.stackAt) - 1);   // longest possible
  char out[768];
  TEST_ASSERT_TRUE(buildCrashReportJson(g_lastCrash, out, sizeof(out)));
  TEST_ASSERT_LESS_THAN(700, (int)strlen(out));
}

static void test_watchdog_report_has_no_registers_or_stack(void) {
  stageWatchdog();
  char out[768];
  TEST_ASSERT_TRUE(buildCrashReportJson(g_lastCrash, out, sizeof(out)));

  JsonDocument doc;
  deserializeJson(doc, out);
  TEST_ASSERT_EQUAL_INT(1, doc["reset"].as<int>());
  TEST_ASSERT_EQUAL_STRING("MDNS", doc["last"].as<const char*>());
  TEST_ASSERT_TRUE(doc["reason"].isNull());
  TEST_ASSERT_TRUE(doc["epc1"].isNull());
  TEST_ASSERT_TRUE(doc["stack"].isNull());
  TEST_ASSERT_TRUE(doc["stack_free"].isNull());
}

static void test_report_that_does_not_fit_is_refused(void) {
  stageException();
  char tiny[64];
  TEST_ASSERT_FALSE(buildCrashReportJson(g_lastCrash, tiny, sizeof(tiny)));
}

// ============================================================
// Sending
// ============================================================

static void test_nothing_is_sent_after_a_normal_boot(void) {
  TEST_ASSERT_FALSE(loopAt(1 * MIN));
  TEST_ASSERT_FALSE(loopAt(60 * MIN));
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
}

static void test_report_is_sent_two_minutes_after_boot(void) {
  stageException();
  TEST_ASSERT_FALSE(loopAt(2 * MIN - 1));
  TEST_ASSERT_TRUE (loopAt(2 * MIN));
  TEST_ASSERT_EQUAL_STRING(CRASH_REPORT_URL, g_httpTest.url);
  TEST_ASSERT_NOT_NULL(strstr(g_httpTest.body, "\"last\":\"MQTT_PEND\""));
  TEST_ASSERT_EQUAL_UINT8(CP_CRASH_RPT, s_lastCheckpoint);
}

static void test_report_is_sent_only_once(void) {
  stageException();
  loopAt(2 * MIN);
  TEST_ASSERT_FALSE(loopAt(30 * MIN));
  TEST_ASSERT_FALSE(loopAt(24 * 60 * MIN));
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
}

static void test_failed_send_is_retried_every_ten_minutes_up_to_four_tries(void) {
  stageException();
  g_httpTest.nextStatus = 503;
  uint32_t t = 2 * MIN;
  TEST_ASSERT_TRUE(loopAt(t));                  // try 1
  for (int i = 2; i <= 4; i++) {
    TEST_ASSERT_FALSE(loopAt(t + 10 * MIN - 1));
    t += 10 * MIN;
    TEST_ASSERT_TRUE(loopAt(t));                // tries 2, 3, 4
  }
  TEST_ASSERT_FALSE(loopAt(t + 10 * MIN));      // gave up
  TEST_ASSERT_EQUAL_INT(4, g_httpTest.postCount);
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "giving up"));
}

static void test_retry_succeeds_then_stops(void) {
  stageException();
  g_httpTest.nextStatus = -1;                   // no connection
  loopAt(2 * MIN);
  g_httpTest.nextStatus = 204;
  TEST_ASSERT_TRUE (loopAt(12 * MIN));
  TEST_ASSERT_FALSE(loopAt(22 * MIN));
  TEST_ASSERT_EQUAL_INT(2, g_httpTest.postCount);
}

// 400 / 413 / 429 won't change by retrying
static void test_refused_report_is_not_retried(void) {
  const int codes[] = { 400, 413, 429 };
  for (int code : codes) {
    setUp();
    stageException();
    g_httpTest.nextStatus = code;
    loopAt(2 * MIN);
    TEST_ASSERT_FALSE_MESSAGE(loopAt(12 * MIN), "retried a refused report");
    TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  }
}

static void test_switched_off_sends_nothing(void) {
  stageException();
  g_globalConfig.crashReports = false;
  TEST_ASSERT_FALSE(loopAt(2 * MIN));
  TEST_ASSERT_FALSE(loopAt(60 * MIN));
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "switched off"));
}

static void test_no_wifi_counts_as_a_failed_try(void) {
  stageException();
  WiFi.connected = false;
  loopAt(2 * MIN);
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
  WiFi.connected = true;
  TEST_ASSERT_TRUE(loopAt(12 * MIN));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_report_carries_the_full_register_frame);
  RUN_TEST(test_a_full_report_is_well_under_the_website_limit);
  RUN_TEST(test_watchdog_report_has_no_registers_or_stack);
  RUN_TEST(test_report_that_does_not_fit_is_refused);

  RUN_TEST(test_nothing_is_sent_after_a_normal_boot);
  RUN_TEST(test_report_is_sent_two_minutes_after_boot);
  RUN_TEST(test_report_is_sent_only_once);
  RUN_TEST(test_failed_send_is_retried_every_ten_minutes_up_to_four_tries);
  RUN_TEST(test_retry_succeeds_then_stops);
  RUN_TEST(test_refused_report_is_not_retried);
  RUN_TEST(test_switched_off_sends_nothing);
  RUN_TEST(test_no_wifi_counts_as_a_failed_try);

  return UNITY_END();
}
