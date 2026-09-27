// Native (host) tests for the daily firmware update check in
// OurBrewbot/UpdateCheck.cpp: version parsing and comparison, reading
// version.json, the check itself (reply handling, logging, MQTT hand-off) and
// the scheduler that runs it once a day with retries.
//
// UpdateCheck.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test. That also puts its file-static
// scheduler state (s_waitMs etc.) in scope, so setUp() can reset it between
// tests. test/stubs/ESP8266HTTPClient.h scripts the version.json reply.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
GlobalConfig g_globalConfig;

// ---- millis(), settable per test ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }

// ---- log capture: counts per severity, plus the last line ----
static int  s_logCount[8];
static char s_lastLog[192];
void logMsgImpl(uint8_t level, PGM_P fmt, ...) {
  if (level < 8) s_logCount[level]++;
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_lastLog, sizeof(s_lastLog), fmt, args);
  va_end(args);
}

// ---- stubs for the modules UpdateCheck.cpp calls into ----
static int s_mqttPublishes = 0;
void mqttPublishUpdateStatus() { s_mqttPublishes++; }

static uint8_t s_lastCheckpoint = 0xFF;
void checkpoint(uint8_t module) { s_lastCheckpoint = module; }

// The code under test
#include "../../OurBrewbot/UpdateCheck.cpp"

// ---- test fixture ----

static const char* NEWER = "99.0.0";   // always newer than FW_VERSION
static const char* OLDER = "0.0.1";    // always older than FW_VERSION

// A version.json body for the given version
static const char* replyFor(const char* version) {
  static char body[160];
  snprintf(body, sizeof(body),
           "{\"version\":\"%s\",\"notes\":\"https://github.com/scardus/OurBrewbot/releases/tag/v%s\"}",
           version, version);
  return body;
}

void setUp(void) {
  s_millis = 1000;
  memset(s_logCount, 0, sizeof(s_logCount));
  s_lastLog[0] = '\0';
  s_mqttPublishes = 0;
  s_lastCheckpoint = 0xFF;

  memset(&g_globalConfig, 0, sizeof(g_globalConfig));
  g_globalConfig.updateCheck = true;
  WiFi.connected = true;
  httpTestReset();

  // Reset UpdateCheck.cpp's state as it is at boot
  memset(&g_updateStatus, 0, sizeof(g_updateStatus));
  s_waitStartMs = 0;
  s_waitMs = 0;
  s_retriesLeft = UPDATE_MAX_RETRIES;
  s_announcedVersion[0] = '\0';
}

void tearDown(void) {}

// ============================================================
// parseVersion / isNewerVersion
// ============================================================

static void test_parse_accepts_three_numbers(void) {
  uint16_t p[3];
  TEST_ASSERT_TRUE(parseVersion("0.4.16", p));
  TEST_ASSERT_EQUAL_UINT16(0,  p[0]);
  TEST_ASSERT_EQUAL_UINT16(4,  p[1]);
  TEST_ASSERT_EQUAL_UINT16(16, p[2]);
}

static void test_parse_accepts_a_leading_v(void) {
  uint16_t p[3];
  TEST_ASSERT_TRUE(parseVersion("v1.2.3", p));
  TEST_ASSERT_EQUAL_UINT16(1, p[0]);
  TEST_ASSERT_EQUAL_UINT16(3, p[2]);
}

static void test_parse_rejects_anything_else(void) {
  uint16_t p[3];
  TEST_ASSERT_FALSE(parseVersion(nullptr, p));
  TEST_ASSERT_FALSE(parseVersion("", p));
  TEST_ASSERT_FALSE(parseVersion("1.2", p));
  TEST_ASSERT_FALSE(parseVersion("1.2.3.4", p));
  TEST_ASSERT_FALSE(parseVersion("1.2.3-beta", p));
  TEST_ASSERT_FALSE(parseVersion("1..3", p));
  TEST_ASSERT_FALSE(parseVersion("1.2.", p));
  TEST_ASSERT_FALSE(parseVersion("a.b.c", p));
  TEST_ASSERT_FALSE(parseVersion(" 1.2.3", p));
  TEST_ASSERT_FALSE(parseVersion("70000.0.0", p));   // too big for a part
}

static void test_newer_compares_each_part_numerically(void) {
  TEST_ASSERT_TRUE (isNewerVersion("1.0.0",  "0.9.9"));
  TEST_ASSERT_TRUE (isNewerVersion("0.5.0",  "0.4.99"));
  TEST_ASSERT_TRUE (isNewerVersion("0.4.17", "0.4.16"));
  TEST_ASSERT_TRUE (isNewerVersion("0.4.10", "0.4.9"));    // not a text comparison
  TEST_ASSERT_FALSE(isNewerVersion("0.4.9",  "0.4.10"));
  TEST_ASSERT_FALSE(isNewerVersion("0.4.16", "0.4.16"));   // same is not newer
  TEST_ASSERT_TRUE (isNewerVersion("v0.4.17", "0.4.16"));
}

static void test_newer_is_false_when_either_version_is_invalid(void) {
  TEST_ASSERT_FALSE(isNewerVersion("garbage", "0.4.16"));
  TEST_ASSERT_FALSE(isNewerVersion("0.4.17",  "garbage"));
  TEST_ASSERT_FALSE(isNewerVersion(nullptr,   "0.4.16"));
}

// ============================================================
// parseVersionJson
// ============================================================

static void test_json_reads_version_and_notes(void) {
  char v[16], n[96];
  TEST_ASSERT_TRUE(parseVersionJson(String("{\"version\":\"0.4.17\",\"notes\":\"https://example.com/n\"}"),
                                    v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("0.4.17", v);
  TEST_ASSERT_EQUAL_STRING("https://example.com/n", n);
}

static void test_json_strips_a_leading_v(void) {
  char v[16], n[96];
  TEST_ASSERT_TRUE(parseVersionJson(String("{\"version\":\"v0.4.17\"}"), v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("0.4.17", v);
}

static void test_json_notes_are_optional_and_must_be_a_whole_link(void) {
  char v[16], n[96];
  TEST_ASSERT_TRUE(parseVersionJson(String("{\"version\":\"0.4.17\"}"), v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("", n);

  TEST_ASSERT_TRUE(parseVersionJson(String("{\"version\":\"0.4.17\",\"notes\":\"javascript:alert(1)\"}"),
                                    v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("", n);

  // Too long for the buffer: dropped rather than cut short into a broken link
  char small[16];
  TEST_ASSERT_TRUE(parseVersionJson(String("{\"version\":\"0.4.17\",\"notes\":\"https://example.com/a/long/path\"}"),
                                    v, sizeof(v), small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("", small);
}

static void test_json_ignores_extra_fields(void) {
  char v[16], n[96];
  TEST_ASSERT_TRUE(parseVersionJson(String("{\"version\":\"0.4.17\",\"future\":[1,2,3]}"),
                                    v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_EQUAL_STRING("0.4.17", v);
}

static void test_json_rejects_a_missing_or_bad_version(void) {
  char v[16], n[96];
  TEST_ASSERT_FALSE(parseVersionJson(String("{\"notes\":\"https://example.com\"}"), v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_FALSE(parseVersionJson(String("{\"version\":417}"),           v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_FALSE(parseVersionJson(String("{\"version\":\"latest\"}"),    v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_FALSE(parseVersionJson(String("<html>Moved</html>"),          v, sizeof(v), n, sizeof(n)));
  TEST_ASSERT_FALSE(parseVersionJson(String(""),                            v, sizeof(v), n, sizeof(n)));
}

// ============================================================
// runUpdateCheck
// ============================================================

// The stub chip ID is 0x2924FA
static void test_check_reads_the_configured_url_with_id_and_version(void) {
  httpTestSetReply(200, replyFor(OLDER));
  runUpdateCheck();
  TEST_ASSERT_EQUAL_STRING(UPDATE_CHECK_URL "?id=2924fa&v=" FW_VERSION, g_httpTest.url);
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.getCount);
}

static void test_check_reports_a_newer_release(void) {
  httpTestSetReply(200, replyFor(NEWER));
  TEST_ASSERT_TRUE(runUpdateCheck());
  TEST_ASSERT_TRUE(g_updateStatus.checked);
  TEST_ASSERT_TRUE(g_updateStatus.updateAvailable);
  TEST_ASSERT_EQUAL_STRING(NEWER, g_updateStatus.latestVersion);
  TEST_ASSERT_EQUAL_STRING("https://github.com/scardus/OurBrewbot/releases/tag/v99.0.0", g_updateStatus.notesUrl);
  TEST_ASSERT_EQUAL_STRING("", g_updateStatus.lastError);
  TEST_ASSERT_EQUAL_INT(1, s_logCount[SYSLOG_NOTICE]);
  TEST_ASSERT_EQUAL_INT(1, s_mqttPublishes);
}

static void test_check_same_version_is_up_to_date(void) {
  httpTestSetReply(200, replyFor(FW_VERSION));
  TEST_ASSERT_TRUE(runUpdateCheck());
  TEST_ASSERT_FALSE(g_updateStatus.updateAvailable);
  TEST_ASSERT_EQUAL_STRING(FW_VERSION, g_updateStatus.latestVersion);
  TEST_ASSERT_EQUAL_INT(0, s_logCount[SYSLOG_NOTICE]);
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "up to date"));
}

// A development build is newer than the latest release - not an update
static void test_check_older_release_is_not_an_update(void) {
  httpTestSetReply(200, replyFor(OLDER));
  TEST_ASSERT_TRUE(runUpdateCheck());
  TEST_ASSERT_FALSE(g_updateStatus.updateAvailable);
  TEST_ASSERT_EQUAL_INT(0, s_logCount[SYSLOG_NOTICE]);
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "newer than the latest release"));
}

static void test_check_announces_each_new_release_once(void) {
  httpTestSetReply(200, replyFor("99.0.0"));
  runUpdateCheck();
  runUpdateCheck();   // the next day - same release
  TEST_ASSERT_EQUAL_INT(1, s_logCount[SYSLOG_NOTICE]);
  TEST_ASSERT_NOT_NULL(strstr(s_lastLog, "still available"));

  httpTestSetReply(200, replyFor("99.0.1"));
  runUpdateCheck();   // a newer release again
  TEST_ASSERT_EQUAL_INT(2, s_logCount[SYSLOG_NOTICE]);
}

static void test_check_http_error_fails_with_the_status(void) {
  httpTestSetReply(301, "");   // e.g. the site redirecting to https
  TEST_ASSERT_FALSE(runUpdateCheck());
  TEST_ASSERT_FALSE(g_updateStatus.checked);
  TEST_ASSERT_TRUE(g_updateStatus.attempted);
  TEST_ASSERT_EQUAL_STRING("HTTP 301", g_updateStatus.lastError);
  TEST_ASSERT_EQUAL_INT(1, s_logCount[SYSLOG_WARNING]);
  TEST_ASSERT_EQUAL_INT(0, s_mqttPublishes);
}

static void test_check_connection_error_fails_with_its_description(void) {
  httpTestSetReply(-1, "");    // HTTPClient's negative codes = no connection etc.
  TEST_ASSERT_FALSE(runUpdateCheck());
  TEST_ASSERT_EQUAL_STRING("stub error", g_updateStatus.lastError);
}

static void test_check_rejects_an_oversized_reply(void) {
  httpTestSetReply(200, replyFor(NEWER));
  g_httpTest.responseSize = 5000;   // server claims a big body
  TEST_ASSERT_FALSE(runUpdateCheck());
  TEST_ASSERT_EQUAL_STRING("reply too large", g_updateStatus.lastError);
  TEST_ASSERT_FALSE(g_updateStatus.updateAvailable);
}

static void test_check_rejects_an_unreadable_reply(void) {
  httpTestSetReply(200, "<html>not json</html>");
  TEST_ASSERT_FALSE(runUpdateCheck());
  TEST_ASSERT_EQUAL_STRING("unreadable reply", g_updateStatus.lastError);
}

static void test_check_without_wifi_does_not_try(void) {
  WiFi.connected = false;
  TEST_ASSERT_FALSE(runUpdateCheck());
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.getCount);
  TEST_ASSERT_EQUAL_STRING("WiFi not connected", g_updateStatus.lastError);
}

// A failure after a success keeps the last known release, and a later
// success clears the error again
static void test_failure_keeps_the_last_good_result(void) {
  httpTestSetReply(200, replyFor(NEWER));
  runUpdateCheck();
  httpTestSetReply(500, "");
  runUpdateCheck();
  TEST_ASSERT_TRUE(g_updateStatus.checked);
  TEST_ASSERT_EQUAL_STRING(NEWER, g_updateStatus.latestVersion);
  TEST_ASSERT_EQUAL_STRING("HTTP 500", g_updateStatus.lastError);

  httpTestSetReply(200, replyFor(NEWER));
  runUpdateCheck();
  TEST_ASSERT_EQUAL_STRING("", g_updateStatus.lastError);
}

// ============================================================
// updateCheckLoop (scheduler)
// ============================================================

// Run the loop at a given time and report whether a check happened
static bool loopAt(uint32_t ms) {
  int before = g_httpTest.getCount;
  s_millis = ms;
  updateCheckLoop();
  return g_httpTest.getCount != before;
}

static uint32_t firstCheckDelay() {
  return UPDATE_FIRST_CHECK_MS + (ESP.getChipId() % UPDATE_SPREAD_MIN) * 60000UL;
}

static void test_first_check_waits_ten_minutes_plus_the_spread(void) {
  httpTestSetReply(200, replyFor(OLDER));
  TEST_ASSERT_FALSE(loopAt(1000));                          // first call only schedules
  TEST_ASSERT_FALSE(loopAt(1000 + firstCheckDelay() - 1));
  TEST_ASSERT_TRUE (loopAt(1000 + firstCheckDelay()));
  TEST_ASSERT_EQUAL_UINT8(CP_UPDATE, s_lastCheckpoint);
}

static void test_after_a_success_the_next_check_is_a_day_later(void) {
  httpTestSetReply(200, replyFor(OLDER));
  uint32_t t = 1000;
  loopAt(t);
  t += firstCheckDelay();
  TEST_ASSERT_TRUE (loopAt(t));
  TEST_ASSERT_FALSE(loopAt(t + UPDATE_INTERVAL_MS - 1));
  TEST_ASSERT_TRUE (loopAt(t + UPDATE_INTERVAL_MS));
}

static void test_failures_retry_hourly_three_times_then_wait_a_day(void) {
  httpTestSetReply(503, "");
  uint32_t t = 1000;
  loopAt(t);
  t += firstCheckDelay();
  TEST_ASSERT_TRUE(loopAt(t));                          // fails
  for (int i = 0; i < UPDATE_MAX_RETRIES; i++) {
    TEST_ASSERT_FALSE(loopAt(t + UPDATE_RETRY_MS - 1));
    t += UPDATE_RETRY_MS;
    TEST_ASSERT_TRUE(loopAt(t));                        // retry, fails again
  }
  // Out of retries: nothing for a day
  TEST_ASSERT_FALSE(loopAt(t + UPDATE_RETRY_MS));
  TEST_ASSERT_TRUE (loopAt(t + UPDATE_INTERVAL_MS));
}

static void test_switched_off_skips_the_check(void) {
  g_globalConfig.updateCheck = false;
  httpTestSetReply(200, replyFor(NEWER));
  uint32_t t = 1000;
  loopAt(t);
  t += firstCheckDelay();
  TEST_ASSERT_FALSE(loopAt(t));
  TEST_ASSERT_FALSE(g_updateStatus.attempted);

  // Switched back on: it runs at the next daily slot
  g_globalConfig.updateCheck = true;
  TEST_ASSERT_TRUE(loopAt(t + UPDATE_INTERVAL_MS));
}

// millis() wraps every ~49.7 days; the unsigned subtraction must cope
static void test_schedule_survives_the_millis_wrap(void) {
  httpTestSetReply(200, replyFor(OLDER));
  uint32_t t = 0xFFFFFFFFUL - 1000;
  loopAt(t);
  TEST_ASSERT_FALSE(loopAt(t + 60000));                      // wrapped, not yet due
  TEST_ASSERT_TRUE (loopAt(t + firstCheckDelay()));          // wrapped, due
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_parse_accepts_three_numbers);
  RUN_TEST(test_parse_accepts_a_leading_v);
  RUN_TEST(test_parse_rejects_anything_else);
  RUN_TEST(test_newer_compares_each_part_numerically);
  RUN_TEST(test_newer_is_false_when_either_version_is_invalid);

  RUN_TEST(test_json_reads_version_and_notes);
  RUN_TEST(test_json_strips_a_leading_v);
  RUN_TEST(test_json_notes_are_optional_and_must_be_a_whole_link);
  RUN_TEST(test_json_ignores_extra_fields);
  RUN_TEST(test_json_rejects_a_missing_or_bad_version);

  RUN_TEST(test_check_reads_the_configured_url_with_id_and_version);
  RUN_TEST(test_check_reports_a_newer_release);
  RUN_TEST(test_check_same_version_is_up_to_date);
  RUN_TEST(test_check_older_release_is_not_an_update);
  RUN_TEST(test_check_announces_each_new_release_once);
  RUN_TEST(test_check_http_error_fails_with_the_status);
  RUN_TEST(test_check_connection_error_fails_with_its_description);
  RUN_TEST(test_check_rejects_an_oversized_reply);
  RUN_TEST(test_check_rejects_an_unreadable_reply);
  RUN_TEST(test_check_without_wifi_does_not_try);
  RUN_TEST(test_failure_keeps_the_last_good_result);

  RUN_TEST(test_first_check_waits_ten_minutes_plus_the_spread);
  RUN_TEST(test_after_a_success_the_next_check_is_a_day_later);
  RUN_TEST(test_failures_retry_hourly_three_times_then_wait_a_day);
  RUN_TEST(test_switched_off_skips_the_check);
  RUN_TEST(test_schedule_survives_the_millis_wrap);

  return UNITY_END();
}
