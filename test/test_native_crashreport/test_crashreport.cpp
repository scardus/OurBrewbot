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
// from g_lastCrash, the queue file it is saved to, and the scheduler that
// sends the saved reports once WiFi has been up for 2 minutes, with retries.
//
// CrashReport.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test, and its file-static scheduler state
// is in scope for setUp() to reset. test/stubs/ESP8266HTTPClient.h records
// the POST instead of sending it, and test/stubs/LittleFS.h keeps the queue
// file in RAM. The JSON checks mirror the website Worker's
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
  g_lastCrash.uptimeSec     = 7260;
  g_lastCrash.freeHeap      = 17480;
  g_lastCrash.rssi          = -63;
}

static void stageWatchdog() {
  memset(&g_lastCrash, 0, sizeof(g_lastCrash));
  g_lastCrash.valid      = true;
  g_lastCrash.resetCode  = 1;                 // REASON_WDT_RST
  g_lastCrash.lastModule = CP_MDNS;
}

// A restart: CrashReport.cpp's RAM state goes back to how it is at boot, the
// clock starts again and there is no new crash - but the queue file is kept,
// as it is on flash
static void reboot() {
  s_started       = false;
  s_queued        = 0;
  s_failures      = 0;
  s_lastAttemptMs = 0;
  s_waitMs        = 0;
  s_wifiWasUp     = true;
  s_wifiUpSinceMs = 0;
  s_finished      = false;
  s_millis = 0;
  memset(&g_lastCrash, 0, sizeof(g_lastCrash));
}

void setUp(void) {
  s_lastLog[0] = '\0';
  s_lastCheckpoint = 0xFF;
  memset(&g_globalConfig, 0, sizeof(g_globalConfig));
  g_globalConfig.crashReports = true;
  WiFi.connected = true;
  httpTestReset();
  g_httpTest.nextStatus = 204;
  fsTestReset();
  reboot();
}

void tearDown(void) {}

// Run the loop at a given time and report whether a POST was made
static bool loopAt(uint32_t ms) {
  int before = g_httpTest.postCount;
  s_millis = ms;
  crashReportLoop();
  return g_httpTest.postCount != before;
}

// Reports in the queue file (one per line)
static int queuedLines() {
  int lines = 0;
  for (const char* p = fsTestRead(FILE_CRASH_QUEUE); *p; p++) {
    if (*p == '\n') lines++;
  }
  return lines;
}

static const uint32_t SEC = 1000UL;
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
  TEST_ASSERT_EQUAL_INT(7260,  doc["uptime"].as<int>());
  TEST_ASSERT_EQUAL_INT(17480, doc["heap"].as<int>());
  TEST_ASSERT_EQUAL_INT(-63,   doc["rssi"].as<int>());
}

// The exception that started the crash, when FirstFault.cpp caught it
static void test_report_carries_the_first_fault(void) {
  stageException();
  g_lastCrash.haveFirstFault = true;
  g_lastCrash.firstFault = { 0, 0x40216A42, 0x30, 0x4020B182, 0x3FFFFF50, 0 };
  char out[768];
  TEST_ASSERT_TRUE(buildCrashReportJson(g_lastCrash, out, sizeof(out)));

  JsonDocument doc;
  deserializeJson(doc, out);
  TEST_ASSERT_EQUAL_INT(0, doc["ff_cause"].as<int>());
  TEST_ASSERT_EQUAL_STRING("40216a42", doc["ff_epc"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("30",       doc["ff_ps"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("4020b182", doc["ff_a0"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("3fffff50", doc["ff_sp"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("0",        doc["ff_excvaddr"].as<const char*>());
}

static void test_report_without_a_first_fault_leaves_it_out(void) {
  stageException();
  char out[768];
  TEST_ASSERT_TRUE(buildCrashReportJson(g_lastCrash, out, sizeof(out)));

  JsonDocument doc;
  deserializeJson(doc, out);
  TEST_ASSERT_TRUE(doc["ff_cause"].isNull());
  TEST_ASSERT_TRUE(doc["ff_epc"].isNull());
}

// 0 means loop() never passed a signal strength in - leave it out rather
// than report a perfect 0 dBm
static void test_unknown_rssi_is_left_out(void) {
  stageException();
  g_lastCrash.rssi = 0;
  char out[768];
  TEST_ASSERT_TRUE(buildCrashReportJson(g_lastCrash, out, sizeof(out)));

  JsonDocument doc;
  deserializeJson(doc, out);
  TEST_ASSERT_TRUE(doc["rssi"].isNull());
  TEST_ASSERT_EQUAL_INT(7260, doc["uptime"].as<int>());   // the rest still sent
}

// The longest report there can be - every number at its widest, the longest
// checkpoint name and stack_at, and a first fault - must fit the buffer
// sendCrashReport() builds it in, and the website's 2048-byte limit.
static void test_the_longest_report_fits_the_buffer(void) {
  stageException();
  g_lastCrash.resetCode  = 255;
  g_lastCrash.lastModule = CP_PROBE_SCAN;                   // longest name
  g_lastCrash.reason     = 0xFFFFFFFF;
  g_lastCrash.exccause   = 0xFFFFFFFF;
  g_lastCrash.epc1 = g_lastCrash.epc2 = g_lastCrash.epc3 = 0xFFFFFFFF;
  g_lastCrash.excvaddr = g_lastCrash.depc = 0xFFFFFFFF;
  g_lastCrash.sp = g_lastCrash.spEnd = 0xFFFFFFFF;
  for (int i = 0; i < CRASH_STACK_WORDS; i++) g_lastCrash.stack[i] = 0xFFFFFFFF;
  g_lastCrash.stackFree = 0xFFFFFFFF;
  memset(g_lastCrash.stackAt, 'x', sizeof(g_lastCrash.stackAt) - 1);   // longest possible
  g_lastCrash.uptimeSec = 0xFFFFFFFF;
  g_lastCrash.freeHeap  = 0xFFFFFFFF;
  g_lastCrash.rssi      = -2147483647 - 1;                  // widest negative number
  g_lastCrash.haveFirstFault = true;
  g_lastCrash.firstFault = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
  char out[CRASH_REPORT_MAX_BYTES];
  TEST_ASSERT_TRUE(buildCrashReportJson(g_lastCrash, out, sizeof(out)));
  TEST_ASSERT_LESS_THAN(2048, (int)strlen(out));
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
  TEST_ASSERT_TRUE(doc["uptime"].isNull());
}

static void test_report_that_does_not_fit_is_refused(void) {
  stageException();
  char tiny[64];
  TEST_ASSERT_FALSE(buildCrashReportJson(g_lastCrash, tiny, sizeof(tiny)));
}

// ============================================================
// Saving to the queue
// ============================================================

static void test_nothing_is_saved_or_sent_after_a_normal_boot(void) {
  TEST_ASSERT_FALSE(loopAt(1 * MIN));
  TEST_ASSERT_FALSE(loopAt(60 * MIN));
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));
}

// Saved on the first loop pass, not 2 minutes later, so a crash loop that
// never reaches 2 minutes of uptime still leaves its reports behind
static void test_crash_is_saved_at_once(void) {
  stageException();
  TEST_ASSERT_FALSE(loopAt(0));
  TEST_ASSERT_EQUAL_INT(1, queuedLines());
  TEST_ASSERT_NOT_NULL(strstr(fsTestRead(FILE_CRASH_QUEUE), "\"last\":\"MQTT_PEND\""));
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "saved"));
}

// This boot's crash goes on the end, behind the ones still waiting
static void test_new_crash_goes_after_the_waiting_ones(void) {
  fsTestWrite(FILE_CRASH_QUEUE, "{\"n\":1}\n{\"n\":2}\n");
  stageException();
  loopAt(0);
  TEST_ASSERT_EQUAL_INT(3, queuedLines());
  const char* queue = fsTestRead(FILE_CRASH_QUEUE);
  TEST_ASSERT_EQUAL_INT(0, strncmp(queue, "{\"n\":1}\n{\"n\":2}\n{\"id\":", 22));
}

static void test_a_sixth_report_deletes_the_oldest(void) {
  fsTestWrite(FILE_CRASH_QUEUE, "{\"n\":1}\n{\"n\":2}\n{\"n\":3}\n{\"n\":4}\n{\"n\":5}\n");
  stageException();
  loopAt(0);
  TEST_ASSERT_EQUAL_INT(5, queuedLines());
  const char* queue = fsTestRead(FILE_CRASH_QUEUE);
  TEST_ASSERT_EQUAL_INT(0, strncmp(queue, "{\"n\":2}\n", 8));            // 1 is gone
  TEST_ASSERT_NOT_NULL(strstr(queue, "{\"n\":5}\n{\"id\":"));             // newest last
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE_TMP));               // temp file cleaned up
}

static void test_report_that_cannot_be_saved_is_logged(void) {
  stageException();
  fsTestSetFull(true);
  loopAt(0);
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "Cannot save"));
  TEST_ASSERT_FALSE(loopAt(2 * MIN));
}

// ============================================================
// Sending
// ============================================================

static void test_report_is_sent_two_minutes_after_boot(void) {
  stageException();
  loopAt(0);
  TEST_ASSERT_FALSE(loopAt(2 * MIN - 1));
  TEST_ASSERT_TRUE (loopAt(2 * MIN));
  TEST_ASSERT_EQUAL_STRING(CRASH_REPORT_URL, g_httpTest.url);
  TEST_ASSERT_NOT_NULL(strstr(g_httpTest.body, "\"last\":\"MQTT_PEND\""));
  TEST_ASSERT_EQUAL_UINT8(CP_CRASH_RPT, s_lastCheckpoint);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));   // sent, so off the queue
}

static void test_report_is_sent_only_once(void) {
  stageException();
  loopAt(0);
  loopAt(2 * MIN);
  TEST_ASSERT_FALSE(loopAt(30 * MIN));
  TEST_ASSERT_FALSE(loopAt(24 * 60 * MIN));
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
}

static void test_saved_reports_are_sent_oldest_first_30_seconds_apart(void) {
  fsTestWrite(FILE_CRASH_QUEUE, "{\"n\":1}\n{\"n\":2}\n{\"n\":3}\n");
  loopAt(0);
  TEST_ASSERT_TRUE(loopAt(2 * MIN));
  TEST_ASSERT_EQUAL_STRING("{\"n\":1}", g_httpTest.body);
  TEST_ASSERT_FALSE(loopAt(2 * MIN + 30 * SEC - 1));
  TEST_ASSERT_TRUE(loopAt(2 * MIN + 30 * SEC));
  TEST_ASSERT_EQUAL_STRING("{\"n\":2}", g_httpTest.body);
  TEST_ASSERT_TRUE(loopAt(2 * MIN + 60 * SEC));
  TEST_ASSERT_EQUAL_STRING("{\"n\":3}", g_httpTest.body);
  TEST_ASSERT_FALSE(loopAt(2 * MIN + 90 * SEC));
  TEST_ASSERT_EQUAL_INT(3, g_httpTest.postCount);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));
}

static void test_failed_send_is_retried_every_ten_minutes_up_to_four_tries(void) {
  stageException();
  g_httpTest.nextStatus = 503;
  loopAt(0);
  uint32_t t = 2 * MIN;
  TEST_ASSERT_TRUE(loopAt(t));                  // try 1
  for (int i = 2; i <= 4; i++) {
    TEST_ASSERT_FALSE(loopAt(t + 10 * MIN - 1));
    t += 10 * MIN;
    TEST_ASSERT_TRUE(loopAt(t));                // tries 2, 3, 4
  }
  TEST_ASSERT_FALSE(loopAt(t + 10 * MIN));      // stopped for this boot
  TEST_ASSERT_EQUAL_INT(4, g_httpTest.postCount);
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "next boot"));
  TEST_ASSERT_EQUAL_INT(1, queuedLines());      // but still saved
}

// The point of the queue: a report that couldn't be sent survives a restart
static void test_report_still_waiting_is_sent_after_the_next_boot(void) {
  stageException();
  g_httpTest.nextStatus = 503;
  loopAt(0);
  for (int i = 0; i < 4; i++) loopAt(2 * MIN + i * 10 * MIN);

  reboot();                                     // a normal boot - no new crash
  g_httpTest.nextStatus = 204;
  loopAt(0);
  TEST_ASSERT_TRUE(loopAt(2 * MIN));
  TEST_ASSERT_NOT_NULL(strstr(g_httpTest.body, "\"last\":\"MQTT_PEND\""));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));
}

static void test_retry_succeeds_then_stops(void) {
  stageException();
  g_httpTest.nextStatus = -1;                   // no connection
  loopAt(0);
  loopAt(2 * MIN);
  g_httpTest.nextStatus = 204;
  TEST_ASSERT_TRUE (loopAt(12 * MIN));
  TEST_ASSERT_FALSE(loopAt(22 * MIN));
  TEST_ASSERT_EQUAL_INT(2, g_httpTest.postCount);
}

// 400 / 413 won't change by retrying, so the report is deleted
static void test_refused_report_is_deleted(void) {
  const int codes[] = { 400, 413 };
  for (int code : codes) {
    setUp();
    stageException();
    g_httpTest.nextStatus = code;
    loopAt(0);
    loopAt(2 * MIN);
    TEST_ASSERT_FALSE_MESSAGE(loopAt(12 * MIN), "retried a refused report");
    TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
    TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));
  }
}

// 429 = the website's daily limit for this controller - keep it, try in an hour
static void test_daily_limit_is_retried_in_an_hour(void) {
  stageException();
  g_httpTest.nextStatus = 429;
  loopAt(0);
  TEST_ASSERT_TRUE(loopAt(2 * MIN));
  g_httpTest.nextStatus = 204;
  TEST_ASSERT_FALSE(loopAt(62 * MIN - 1));
  TEST_ASSERT_TRUE (loopAt(62 * MIN));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));
}

// No WiFi doesn't use up the tries - the report waits for as long as it takes,
// then goes 2 minutes after WiFi comes back
static void test_no_wifi_waits_for_wifi_to_come_back(void) {
  stageException();
  WiFi.connected = false;
  loopAt(0);
  TEST_ASSERT_FALSE(loopAt(2 * MIN));
  TEST_ASSERT_FALSE(loopAt(60 * MIN));
  TEST_ASSERT_FALSE(loopAt(24 * 60 * MIN));
  WiFi.connected = true;
  const uint32_t back = 24 * 60 * MIN + 1 * MIN;
  TEST_ASSERT_FALSE(loopAt(back));                 // connected, settling
  TEST_ASSERT_FALSE(loopAt(back + 2 * MIN - 1));
  TEST_ASSERT_TRUE (loopAt(back + 2 * MIN));
}

// A report cut short (power lost while saving it) is deleted unsent, and the
// next one still goes
static void test_damaged_report_is_deleted_unsent(void) {
  fsTestWrite(FILE_CRASH_QUEUE, "{\"n\":1\n{\"n\":2}\n{\"n\":");
  loopAt(0);
  TEST_ASSERT_FALSE(loopAt(2 * MIN));           // the damaged one - not posted
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "damaged"));
  TEST_ASSERT_TRUE(loopAt(2 * MIN + 30 * SEC));
  TEST_ASSERT_EQUAL_STRING("{\"n\":2}", g_httpTest.body);
  TEST_ASSERT_FALSE(loopAt(2 * MIN + 60 * SEC));  // the cut-short last one
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));
}

// If a sent report can't be taken off the queue, stop - rather than send the
// same report every 30 seconds
static void test_queue_that_cannot_be_updated_stops_sending(void) {
  fsTestWrite(FILE_CRASH_QUEUE, "{\"n\":1}\n{\"n\":2}\n");
  fsTestSetFull(true);
  loopAt(0);
  TEST_ASSERT_TRUE(loopAt(2 * MIN));
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "next boot"));
  TEST_ASSERT_FALSE(loopAt(2 * MIN + 30 * SEC));
  TEST_ASSERT_FALSE(loopAt(60 * MIN));
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
}

// ============================================================
// Switched off
// ============================================================

static void test_switched_off_saves_and_sends_nothing(void) {
  stageException();
  g_globalConfig.crashReports = false;
  TEST_ASSERT_FALSE(loopAt(0));
  TEST_ASSERT_FALSE(loopAt(2 * MIN));
  TEST_ASSERT_FALSE(loopAt(60 * MIN));
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "switched off"));
}

// Switching off while reports are waiting deletes them
static void test_switching_off_deletes_the_saved_reports(void) {
  fsTestWrite(FILE_CRASH_QUEUE, "{\"n\":1}\n{\"n\":2}\n");
  WiFi.connected = false;
  loopAt(0);
  g_globalConfig.crashReports = false;
  loopAt(1 * MIN);
  TEST_ASSERT_FALSE(LittleFS.exists(FILE_CRASH_QUEUE));
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "deleted 2 saved reports"));
  WiFi.connected = true;
  TEST_ASSERT_FALSE(loopAt(10 * MIN));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_report_carries_the_full_register_frame);
  RUN_TEST(test_unknown_rssi_is_left_out);
  RUN_TEST(test_report_carries_the_first_fault);
  RUN_TEST(test_report_without_a_first_fault_leaves_it_out);
  RUN_TEST(test_the_longest_report_fits_the_buffer);
  RUN_TEST(test_watchdog_report_has_no_registers_or_stack);
  RUN_TEST(test_report_that_does_not_fit_is_refused);

  RUN_TEST(test_nothing_is_saved_or_sent_after_a_normal_boot);
  RUN_TEST(test_crash_is_saved_at_once);
  RUN_TEST(test_new_crash_goes_after_the_waiting_ones);
  RUN_TEST(test_a_sixth_report_deletes_the_oldest);
  RUN_TEST(test_report_that_cannot_be_saved_is_logged);

  RUN_TEST(test_report_is_sent_two_minutes_after_boot);
  RUN_TEST(test_report_is_sent_only_once);
  RUN_TEST(test_saved_reports_are_sent_oldest_first_30_seconds_apart);
  RUN_TEST(test_failed_send_is_retried_every_ten_minutes_up_to_four_tries);
  RUN_TEST(test_report_still_waiting_is_sent_after_the_next_boot);
  RUN_TEST(test_retry_succeeds_then_stops);
  RUN_TEST(test_refused_report_is_deleted);
  RUN_TEST(test_daily_limit_is_retried_in_an_hour);
  RUN_TEST(test_no_wifi_waits_for_wifi_to_come_back);
  RUN_TEST(test_damaged_report_is_deleted_unsent);
  RUN_TEST(test_queue_that_cannot_be_updated_stops_sending);

  RUN_TEST(test_switched_off_saves_and_sends_nothing);
  RUN_TEST(test_switching_off_deletes_the_saved_reports);

  return UNITY_END();
}
