// Native (host) tests for OurBrewbot/Log.cpp - the timestamp, the syslog
// packet and the three independent gates that decide whether a line leaves the
// device at all.
//
// Everything here is fire-and-forget: UDP has no acknowledgement and logMsg()
// returns void, so a misfiled or dropped line is invisible from the firmware
// side. The syslog PRI byte in particular is pure arithmetic that the receiving
// server acts on - get the facility wrong and every message lands in the wrong
// bucket, which is a silent failure of the main diagnostic channel this project
// relies on (see the DEFERRED crash-line workflow).
//
// Log.cpp is #included directly (not linked) so the real, unmodified production
// source is what's under test. Note this suite must NOT define logMsgImpl - it
// is the function under test, unlike every other native suite where it's a
// no-op double.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
// Only the two Log.cpp actually reads; the rest stay unresolved, which is fine
// because nothing in this translation unit references them.
SyslogConfig g_syslogConfig;
MqttConfig   g_mqttConfig;

// ---- millis(), settable so the timestamp can be driven ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- MQTT mirror double: records what would have been published ----
static int  s_mqttCalls = 0;
static int  s_mqttLastLevel = -1;
static char s_mqttLastLine[256];

void mqttPublishLog(uint8_t level, const char* line) {
  s_mqttCalls++;
  s_mqttLastLevel = level;
  snprintf(s_mqttLastLine, sizeof(s_mqttLastLine), "%s", line);
}

// The code under test. Also brings the file-static s_ipResolved/s_syslogIP
// into scope so the fixture can force a resolved state without a lookup.
#include "../../OurBrewbot/Log.cpp"

// ---- fixture ----

// A fully working syslog target: enabled, resolvable, link up. Individual
// tests then knock out one gate at a time.
static void enableSyslog(uint8_t facility = 16, uint8_t minLevel = SYSLOG_DEBUG) {
  g_syslogConfig.enabled  = true;
  snprintf(g_syslogConfig.host, sizeof(g_syslogConfig.host), "syslog.local");
  g_syslogConfig.port     = 514;
  g_syslogConfig.facility = facility;
  g_syslogConfig.minLevel = minLevel;
  WiFi.connected = true;
  WiFi.resolveOk = true;
  logInit();
}

void setUp(void) {
  g_syslogConfig = SyslogConfig{};
  g_mqttConfig   = MqttConfig{};
  s_millis       = 0;
  s_ipResolved   = false;
  s_syslogIP     = IPAddress(0, 0, 0, 0);
  s_mqttCalls    = 0;
  s_mqttLastLevel = -1;
  s_mqttLastLine[0] = '\0';
  WiFi.connected = true;
  WiFi.resolveOk = true;
  WiFi.resolveTo = IPAddress(192, 168, 0, 50);
  Serial.reset();
  udpTestReset();
}

void tearDown(void) {}

// ============================================================
// logInit() — host resolution
// ============================================================

static void test_disabled_syslog_never_resolves(void) {
  g_syslogConfig.enabled = false;
  snprintf(g_syslogConfig.host, sizeof(g_syslogConfig.host), "syslog.local");
  logInit();
  TEST_ASSERT_FALSE(s_ipResolved);
}

static void test_empty_host_never_resolves(void) {
  g_syslogConfig.enabled = true;
  g_syslogConfig.host[0] = '\0';
  logInit();
  TEST_ASSERT_FALSE(s_ipResolved);
}

// hostByName is synchronous, so logInit() deliberately skips it entirely when
// the link is down rather than blocking boot for the 2 s timeout.
static void test_no_resolution_attempt_while_wifi_is_down(void) {
  g_syslogConfig.enabled = true;
  snprintf(g_syslogConfig.host, sizeof(g_syslogConfig.host), "syslog.local");
  WiFi.connected = false;
  logInit();
  TEST_ASSERT_FALSE(s_ipResolved);
}

static void test_successful_lookup_caches_the_address(void) {
  WiFi.resolveTo = IPAddress(10, 1, 2, 3);
  enableSyslog();
  TEST_ASSERT_TRUE(s_ipResolved);
  TEST_ASSERT_TRUE(s_syslogIP == IPAddress(10, 1, 2, 3));
}

static void test_failed_lookup_leaves_the_address_unresolved(void) {
  g_syslogConfig.enabled = true;
  snprintf(g_syslogConfig.host, sizeof(g_syslogConfig.host), "nonexistent.local");
  WiFi.resolveOk = false;
  logInit();
  TEST_ASSERT_FALSE(s_ipResolved);
  TEST_ASSERT_TRUE(s_syslogIP == IPAddress(0, 0, 0, 0));
}

// A re-init after a failed lookup must clear a previously cached address,
// otherwise a config change to a bad host keeps sending to the old server.
static void test_reinit_clears_a_previously_resolved_address(void) {
  enableSyslog();
  TEST_ASSERT_TRUE(s_ipResolved);
  WiFi.resolveOk = false;
  logInit();
  TEST_ASSERT_FALSE(s_ipResolved);
}

// ============================================================
// THE SYSLOG PRI BYTE — facility * 8 + level (RFC 3164)
// ============================================================

static void test_pri_for_local0_and_info(void) {
  enableSyslog(16, SYSLOG_DEBUG);
  logMsgL(SYSLOG_INFO, "hello");
  TEST_ASSERT_EQUAL_INT(1, g_udpPacketCount);
  // 16 * 8 + 6 = 134
  TEST_ASSERT_EQUAL_STRING("<134>ourbrewbot ourbrewbot: hello", udpTestLastPayload());
}

static void test_pri_for_user_facility_and_error(void) {
  enableSyslog(1, SYSLOG_DEBUG);
  logMsgL(SYSLOG_ERR, "boom");
  // 1 * 8 + 3 = 11
  TEST_ASSERT_EQUAL_STRING("<11>ourbrewbot ourbrewbot: boom", udpTestLastPayload());
}

static void test_pri_at_both_extremes(void) {
  enableSyslog(0, SYSLOG_DEBUG);
  logMsgL(SYSLOG_EMERG, "x");
  TEST_ASSERT_EQUAL_STRING("<0>ourbrewbot ourbrewbot: x", udpTestLastPayload());

  udpTestReset();
  enableSyslog(23, SYSLOG_DEBUG);
  logMsgL(SYSLOG_DEBUG, "y");
  // 23 * 8 + 7 = 191, the largest valid PRI
  TEST_ASSERT_EQUAL_STRING("<191>ourbrewbot ourbrewbot: y", udpTestLastPayload());
}

static void test_packet_goes_to_the_resolved_host_and_configured_port(void) {
  WiFi.resolveTo = IPAddress(172, 16, 5, 9);
  enableSyslog();
  g_syslogConfig.port = 5514;
  logMsg("routed");
  TEST_ASSERT_EQUAL_INT(1, g_udpPacketCount);
  TEST_ASSERT_TRUE(g_udpPackets[0].dest == IPAddress(172, 16, 5, 9));
  TEST_ASSERT_EQUAL_UINT16(5514, g_udpPackets[0].port);
}

// ============================================================
// SEVERITY FILTER — lower number is more critical
// ============================================================

static void test_debug_minlevel_passes_everything(void) {
  enableSyslog(16, SYSLOG_DEBUG);
  logMsgL(SYSLOG_DEBUG, "chatter");
  TEST_ASSERT_EQUAL_INT(1, g_udpPacketCount);
}

static void test_warning_minlevel_passes_warning_itself(void) {
  enableSyslog(16, SYSLOG_WARNING);
  logMsgL(SYSLOG_WARNING, "at the boundary");
  TEST_ASSERT_EQUAL_INT(1, g_udpPacketCount);
}

static void test_warning_minlevel_drops_notice_and_below(void) {
  enableSyslog(16, SYSLOG_WARNING);
  logMsgL(SYSLOG_NOTICE, "less important");
  logMsgL(SYSLOG_INFO,   "less important");
  logMsgL(SYSLOG_DEBUG,  "less important");
  TEST_ASSERT_EQUAL_INT(0, g_udpPacketCount);
}

static void test_emerg_minlevel_passes_only_emerg(void) {
  enableSyslog(16, SYSLOG_EMERG);
  logMsgL(SYSLOG_ALERT, "dropped");
  TEST_ASSERT_EQUAL_INT(0, g_udpPacketCount);
  logMsgL(SYSLOG_EMERG, "sent");
  TEST_ASSERT_EQUAL_INT(1, g_udpPacketCount);
}

// ============================================================
// THE THREE GATES — each suppresses independently
// ============================================================

static void test_no_packet_when_syslog_is_disabled(void) {
  enableSyslog();
  g_syslogConfig.enabled = false;   // resolved, but switched off
  logMsg("nowhere");
  TEST_ASSERT_EQUAL_INT(0, g_udpPacketCount);
}

static void test_no_packet_when_the_host_never_resolved(void) {
  g_syslogConfig.enabled  = true;
  g_syslogConfig.facility = 16;
  g_syslogConfig.minLevel = SYSLOG_DEBUG;
  g_syslogConfig.port     = 514;
  TEST_ASSERT_FALSE(s_ipResolved);
  logMsg("nowhere");
  TEST_ASSERT_EQUAL_INT(0, g_udpPacketCount);
}

// The link can drop after a successful resolve, so the check is per-message,
// not just at init.
static void test_no_packet_when_wifi_dropped_after_resolving(void) {
  enableSyslog();
  WiFi.connected = false;
  logMsg("nowhere");
  TEST_ASSERT_EQUAL_INT(0, g_udpPacketCount);
}

// ============================================================
// TIMESTAMP
// ============================================================

static void test_timestamp_at_zero_uptime(void) {
  s_millis = 0;
  logMsg("start");
  TEST_ASSERT_EQUAL_STRING("[000:00:00] start\r\n", Serial.buf);
}

static void test_timestamp_composes_hours_minutes_seconds(void) {
  s_millis = (3600 + 60 + 1) * 1000UL;   // 1h 1m 1s
  logMsg("tick");
  TEST_ASSERT_EQUAL_STRING("[001:01:01] tick\r\n", Serial.buf);
}

// The %03lu hour field grows past three digits after ~41 days of uptime. That
// is fine, but only because millis() wraps first: the largest value it can
// ever hold is 2^32-1 ms = 1193h 02m 47s, which is 13 characters including the
// trailing space - inside the 16-byte ts[] buffer with two to spare. Widening
// the format, or moving to a 64-bit uptime, would overflow it.
static void test_widest_possible_timestamp_still_fits_the_buffer(void) {
  s_millis = 0xFFFFFFFFu;
  logMsg("wrap");
  TEST_ASSERT_EQUAL_STRING("[1193:02:47] wrap\r\n", Serial.buf);
  TEST_ASSERT_LESS_THAN_UINT(16, strlen("[1193:02:47] ") + 1);
}

static void test_format_arguments_are_expanded(void) {
  logMsg("[TAG] value %d and %s", 42, "text");
  TEST_ASSERT_EQUAL_STRING("[000:00:00] [TAG] value 42 and text\r\n", Serial.buf);
}

// ============================================================
// MQTT LOG MIRROR
// ============================================================

static void test_mirror_is_silent_when_mqtt_is_disabled(void) {
  g_mqttConfig.enabled    = false;
  g_mqttConfig.logEnabled = true;
  logMsg("quiet");
  TEST_ASSERT_EQUAL_INT(0, s_mqttCalls);
}

static void test_mirror_is_silent_when_log_topic_is_disabled(void) {
  g_mqttConfig.enabled    = true;
  g_mqttConfig.logEnabled = false;
  logMsg("quiet");
  TEST_ASSERT_EQUAL_INT(0, s_mqttCalls);
}

static void test_mirror_sends_timestamp_and_message_with_the_level(void) {
  g_mqttConfig.enabled    = true;
  g_mqttConfig.logEnabled = true;
  s_millis = 2000;
  logMsgL(SYSLOG_ERR, "problem");
  TEST_ASSERT_EQUAL_INT(1, s_mqttCalls);
  TEST_ASSERT_EQUAL_INT(SYSLOG_ERR, s_mqttLastLevel);
  TEST_ASSERT_EQUAL_STRING("[000:00:02] problem", s_mqttLastLine);
}

// The mirror is independent of the syslog gates - MQTT logging works with
// syslog switched off entirely.
static void test_mirror_works_with_syslog_disabled(void) {
  g_syslogConfig.enabled  = false;
  g_mqttConfig.enabled    = true;
  g_mqttConfig.logEnabled = true;
  logMsg("mqtt only");
  TEST_ASSERT_EQUAL_INT(0, g_udpPacketCount);
  TEST_ASSERT_EQUAL_INT(1, s_mqttCalls);
}

// buf[] caps a message at 191 characters, and line[208] has to hold the
// 12-character timestamp on top of that: 203 bytes, so the mirrored line is
// never itself truncated. Worth pinning because the two buffer sizes are
// declared 30 lines apart and only work together by arithmetic.
static void test_longest_possible_message_is_not_truncated_by_the_mirror(void) {
  g_mqttConfig.enabled    = true;
  g_mqttConfig.logEnabled = true;
  char big[300];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';

  logMsgL(SYSLOG_INFO, "%s", big);
  TEST_ASSERT_EQUAL_INT(1, s_mqttCalls);
  // 12 (timestamp) + 191 (vsnprintf's 192-byte buffer less its NUL) = 203
  TEST_ASSERT_EQUAL_UINT(203, strlen(s_mqttLastLine));
}

// ============================================================

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_disabled_syslog_never_resolves);
  RUN_TEST(test_empty_host_never_resolves);
  RUN_TEST(test_no_resolution_attempt_while_wifi_is_down);
  RUN_TEST(test_successful_lookup_caches_the_address);
  RUN_TEST(test_failed_lookup_leaves_the_address_unresolved);
  RUN_TEST(test_reinit_clears_a_previously_resolved_address);

  RUN_TEST(test_pri_for_local0_and_info);
  RUN_TEST(test_pri_for_user_facility_and_error);
  RUN_TEST(test_pri_at_both_extremes);
  RUN_TEST(test_packet_goes_to_the_resolved_host_and_configured_port);

  RUN_TEST(test_debug_minlevel_passes_everything);
  RUN_TEST(test_warning_minlevel_passes_warning_itself);
  RUN_TEST(test_warning_minlevel_drops_notice_and_below);
  RUN_TEST(test_emerg_minlevel_passes_only_emerg);

  RUN_TEST(test_no_packet_when_syslog_is_disabled);
  RUN_TEST(test_no_packet_when_the_host_never_resolved);
  RUN_TEST(test_no_packet_when_wifi_dropped_after_resolving);

  RUN_TEST(test_timestamp_at_zero_uptime);
  RUN_TEST(test_timestamp_composes_hours_minutes_seconds);
  RUN_TEST(test_widest_possible_timestamp_still_fits_the_buffer);
  RUN_TEST(test_format_arguments_are_expanded);

  RUN_TEST(test_mirror_is_silent_when_mqtt_is_disabled);
  RUN_TEST(test_mirror_is_silent_when_log_topic_is_disabled);
  RUN_TEST(test_mirror_sends_timestamp_and_message_with_the_level);
  RUN_TEST(test_mirror_works_with_syslog_disabled);
  RUN_TEST(test_longest_possible_message_is_not_truncated_by_the_mirror);

  return UNITY_END();
}
