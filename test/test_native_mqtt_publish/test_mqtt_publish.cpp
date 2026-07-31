// Native (host) tests for the half of OurBrewbot/Mqtt.cpp that talks to the
// broker - everything the earlier test_native_mqtt suite could not reach.
//
// That suite covers the pure logic Round 5 extracted into MqttParse.cpp, and it
// deliberately does NOT include Mqtt.cpp, because Mqtt.cpp instantiates a
// WiFiClient and a PubSubClient at file scope. test/stubs/PubSubClient.h now
// removes that blocker by recording every publish, subscribe and connect
// instead of opening a socket, so this suite includes Mqtt.cpp for real and
// asserts on the traffic it produces:
//
//   - the log mirror's JSON escaping and its 210 B truncation boundary
//   - Home Assistant discovery topics, payload fields and unit conversion
//   - discovery REMOVAL, including that nothing published is left behind
//   - the periodic state report, its display units and its sentinels
//   - the connect handshake (LWT, subscriptions) and the inbound command path
//
// Temperatures.cpp, Fermenter.cpp, Profile.cpp and Config.cpp are compiled in
// for real rather than stubbed: the unit conversions and the profile step count
// are the things under test, and stubbing them would make the assertions
// tautological (the Round 6 lesson).
//
// mqttMessageCallback() is static inside Mqtt.cpp and so cannot be called
// directly. It is reached the way the broker reaches it - through the callback
// PubSubClient captured at connect time - via mqttTestInject().

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "../../OurBrewbot/Config.h"
#include "../../OurBrewbot/Pins.h"

// ---- millis(), settable per test ----
// Starts large: mqttConnect() treats g_mqttLastAttempt == 0 as "never
// attempted", so a clock starting at 0 collides with that sentinel and the
// backoff window reads as already elapsed. Same trap as s_lastCoolingStop.
static uint32_t s_millis = 1000000;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- doubles for the modules not compiled in ----
void logMsgImpl(uint8_t, PGM_P, ...) {}
void logInit() {}

// Tilt.cpp - only the colour-name lookup is reachable from Mqtt.cpp.
static const char* const TILT_COLOUR_NAMES[] = {
  "Red", "Green", "Black", "Purple", "Orange", "Blue", "Yellow", "Pink"
};
const char* getTiltColourName(uint8_t colour) {
  return (colour < 8) ? TILT_COLOUR_NAMES[colour] : "Unknown";
}

// SmartPlugs.cpp - Fermenter.cpp drives the plugs; nothing here asserts on them.
void smartPlugSwitch(uint8_t, bool) {}
bool getPlugState(uint8_t) { return false; }

// The main sketch owns the debug overrides and the reboot reason string.
bool g_fermenterDebugMode = false;
FermenterDebugOverride g_fermenterDebugOverrides[MAX_FERMENTERS];
String g_rebootReason = "Power on";

// The code under test, plus the modules its payloads genuinely depend on.
#include "../../OurBrewbot/Config.cpp"
#include "../../OurBrewbot/Temperatures.cpp"
#include "../../OurBrewbot/Fermenter.cpp"
#include "../../OurBrewbot/Profile.cpp"
#include "../../OurBrewbot/MqttParse.cpp"
#include "../../OurBrewbot/Mqtt.cpp"

// ============================================================
// FIXTURE
// ============================================================

static const uint8_t F0 = 0;

// ESP.getChipId() is fixed at 0x2924FA in the stub. Discovery ids use %06X and
// the device report uses %06x - the case difference is real, not a typo here.
#define CHIP_UPPER "2924FA"
#define CHIP_LOWER "2924fa"
#define BASE       "brewbot"

static void configureFermenter(uint8_t idx) {
  g_fermenters[idx] = FermenterConfig{};
  g_fermenters[idx].ceilingTemp     = 20.0f;
  g_fermenters[idx].floorTemp       = 18.0f;
  g_fermenters[idx].hysteresis      = 0.5f;
  g_fermenters[idx].og              = 1.050f;
  g_fermenters[idx].tg              = 1.010f;
  g_fermenters[idx].compressorDelay = 10;
  g_fermenters[idx].alarmTolerance  = 3.0f;
  g_fermenters[idx].power           = true;
  g_fermenters[idx].tempControl     = true;
  g_fermenters[idx].brewServices    = (1 << MQTT_SERVICE_BIT);
  strlcpy(g_fermenters[idx].fermenterName, "Fermenter 1",
          sizeof(g_fermenters[idx].fermenterName));
}

// Drive a real connect so Mqtt.cpp's static s_availTopic is populated and its
// backoff is reset, then discard the resulting traffic. testMqtt() is used
// because it is the only non-static entry point that connects without also
// needing WiFi and the report path.
static void connectFixture() {
  mqttTestSetConnectOk(true);
  mqttTestSetConnected(false);
  const bool ha = g_mqttConfig.haDiscovery;
  g_mqttConfig.haDiscovery = false;   // keep 240 discovery publishes out of the fixture
  testMqtt();
  g_mqttConfig.haDiscovery = ha;
  mqttTestResetRecords();
}

void setUp(void) {
  fsTestReset();
  mqttTestReset();
  espTestSetResetReason(REASON_DEFAULT_RST);
  s_millis = 1000000;

  memset(&g_globalConfig, 0, sizeof(g_globalConfig));
  g_globalConfig.unit = UNIT_CELSIUS;
  for (int i = 0; i < MAX_FERMENTERS; i++)    configureFermenter(i);
  for (int i = 0; i < MAX_PROBES; i++)        g_probes[i]       = ProbeConfig{};
  for (int i = 0; i < MAX_SMART_PLUGS; i++)   g_smartPlugs[i]   = SmartPlugConfig{};
  for (int i = 0; i < MAX_PROFILES; i++)      g_profiles[i]     = ProfileConfig{};
  memset(g_profileSteps, 0, sizeof(g_profileSteps));
  initDefaultTiltConfig();   // colour sentinel is 99, not 0 - see the Config suite
  for (int i = 0; i < MAX_ISPINDELS; i++)     g_iSpindels[i]    = iSpindelConfig{};
  for (int i = 0; i < MAX_BREW_SERVICES; i++) g_brewServices[i] = BrewServiceConfig{};

  memset(&g_mqttConfig, 0, sizeof(g_mqttConfig));
  g_mqttConfig.enabled = true;
  strlcpy(g_mqttConfig.host,      "192.168.0.10", sizeof(g_mqttConfig.host));
  g_mqttConfig.port = 1883;
  strlcpy(g_mqttConfig.baseTopic, BASE,           sizeof(g_mqttConfig.baseTopic));
  memset(&g_syslogConfig, 0, sizeof(g_syslogConfig));

  g_fermenterDebugMode = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    g_fermenterDebugOverrides[i] = FermenterDebugOverride{};
  }

  connectFixture();
}

void tearDown(void) {}

// ---- assertion helpers ----

static JsonDocument s_doc;

// Parse the payload published to `topic`, failing the test if it was never
// published or is not valid JSON.
static JsonDocument& payloadJson(const char* topic) {
  const char* p = mqttTestPayloadFor(topic);
  TEST_ASSERT_NOT_NULL_MESSAGE(p, topic);
  s_doc.clear();
  TEST_ASSERT_FALSE_MESSAGE(deserializeJson(s_doc, p), topic);
  return s_doc;
}

static void assertPayload(const char* topic, const char* expected) {
  const char* p = mqttTestPayloadFor(topic);
  TEST_ASSERT_NOT_NULL_MESSAGE(p, topic);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, p, topic);
}

static void assertNotPublished(const char* topic) {
  TEST_ASSERT_FALSE_MESSAGE(mqttTestPublished(topic), topic);
}

// ============================================================
// dtostrf - the stub every published number goes through
//
// publishFloat() formats EVERY numeric MQTT topic with dtostrf(), so if the
// stub's width/precision handling were wrong, every value assertion in this
// file would be checking the wrong string. Pin it before trusting any of them.
// ============================================================

void test_dtostrf_respects_precision(void) {
  char buf[16];
  // 20.06 rounds up to one decimal; truncating would give 20.0. Deliberately
  // not 20.05, which is 20.04999... as a float and rounds DOWN - a "wrong"
  // looking result that is actually correct, and an easy way to write a test
  // that fails for the wrong reason.
  dtostrf(20.06f, 1, 1, buf);
  TEST_ASSERT_EQUAL_STRING("20.1", buf);
  dtostrf(1.0501f, 1, 4, buf);
  TEST_ASSERT_EQUAL_STRING("1.0501", buf);
}

void test_dtostrf_width_is_a_minimum_not_a_truncation(void) {
  char buf[16];
  dtostrf(-17.8f, 1, 1, buf);   // width 1, value needs 5 - must not be cut
  TEST_ASSERT_EQUAL_STRING("-17.8", buf);
  dtostrf(5.0f, 8, 2, buf);     // width 8 pads on the left
  TEST_ASSERT_EQUAL_STRING("    5.00", buf);
}

void test_dtostrf_zero_precision_drops_the_point(void) {
  char buf[16];
  dtostrf(21.6f, 1, 0, buf);
  TEST_ASSERT_EQUAL_STRING("22", buf);
}

// ============================================================
// A. mqttPublishLog() - escaping and truncation
// ============================================================

static const char* LOG_TOPIC = BASE "/Device/log";

static void enableLogMirror() {
  g_mqttConfig.logEnabled = true;
}

void test_log_publishes_to_device_log_topic_unretained(void) {
  enableLogMirror();
  mqttPublishLog(SYSLOG_INFO, "hello");
  TEST_ASSERT_TRUE(mqttTestPublished(LOG_TOPIC));
  // Retaining a log line would make the last message reappear on every
  // subscriber reconnect, forever.
  TEST_ASSERT_FALSE(mqttTestRetainedFor(LOG_TOPIC));
}

void test_log_payload_is_valid_json_with_level_and_message(void) {
  enableLogMirror();
  mqttPublishLog(SYSLOG_INFO, "plain message");
  JsonDocument& d = payloadJson(LOG_TOPIC);
  TEST_ASSERT_EQUAL_INT(SYSLOG_INFO, d["level"].as<int>());
  TEST_ASSERT_EQUAL_STRING("INFO", d["severity"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("plain message", d["msg"].as<const char*>());
}

void test_log_escapes_double_quote(void) {
  enableLogMirror();
  mqttPublishLog(SYSLOG_INFO, "probe \"Beer\" failed");
  // Parsing it back is the real assertion: an unescaped quote would end the
  // JSON string early and this deserialize would fail.
  JsonDocument& d = payloadJson(LOG_TOPIC);
  TEST_ASSERT_EQUAL_STRING("probe \"Beer\" failed", d["msg"].as<const char*>());
}

void test_log_escapes_backslash(void) {
  enableLogMirror();
  mqttPublishLog(SYSLOG_INFO, "path C:\\temp");
  JsonDocument& d = payloadJson(LOG_TOPIC);
  TEST_ASSERT_EQUAL_STRING("path C:\\temp", d["msg"].as<const char*>());
}

void test_log_escapes_both_in_one_line(void) {
  enableLogMirror();
  mqttPublishLog(SYSLOG_INFO, "\\\"mixed\"\\");
  JsonDocument& d = payloadJson(LOG_TOPIC);
  TEST_ASSERT_EQUAL_STRING("\\\"mixed\"\\", d["msg"].as<const char*>());
}

void test_log_severity_names_match_syslog_levels(void) {
  enableLogMirror();
  static const char* const expected[] = {
    "EMERG", "ALERT", "CRIT", "ERR", "WARNING", "NOTICE", "INFO", "DEBUG"
  };
  for (uint8_t level = 0; level < 8; level++) {
    mqttTestResetRecords();
    mqttPublishLog(level, "x");
    JsonDocument& d = payloadJson(LOG_TOPIC);
    TEST_ASSERT_EQUAL_STRING(expected[level], d["severity"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(level, d["level"].as<int>());
  }
}

void test_log_level_above_range_falls_back_to_info(void) {
  enableLogMirror();
  mqttPublishLog(8, "out of range");
  JsonDocument& d = payloadJson(LOG_TOPIC);
  TEST_ASSERT_EQUAL_STRING("INFO", d["severity"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(8, d["level"].as<int>());   // level itself is not clamped
}

void test_log_long_line_truncates_but_stays_valid_json(void) {
  enableLogMirror();
  char line[400];
  memset(line, 'A', sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  mqttPublishLog(SYSLOG_INFO, line);
  JsonDocument& d = payloadJson(LOG_TOPIC);
  const char* msg = d["msg"].as<const char*>();
  TEST_ASSERT_NOT_NULL(msg);
  // The escape buffer is 210 B and the loop stops at sizeof(safe) - 2.
  TEST_ASSERT_LESS_OR_EQUAL_UINT(208, strlen(msg));
  TEST_ASSERT_GREATER_THAN_UINT(200, strlen(msg));
}

void test_log_truncation_never_leaves_a_dangling_escape(void) {
  // The pathological case for the escaping loop: a line made entirely of
  // backslashes, so every input character emits two output characters and the
  // truncation boundary lands mid-pair. Ending on a lone '\' would put invalid
  // JSON on the topic - the loop's `j < sizeof(safe) - 2` guard is what
  // prevents it, and nothing else pins that guard.
  enableLogMirror();
  char line[400];
  memset(line, '\\', sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  mqttPublishLog(SYSLOG_INFO, line);
  const char* raw = mqttTestPayloadFor(LOG_TOPIC);
  TEST_ASSERT_NOT_NULL(raw);
  // Deserializing is the assertion; a trailing lone backslash escapes the
  // closing quote and the document no longer parses.
  s_doc.clear();
  TEST_ASSERT_FALSE_MESSAGE(deserializeJson(s_doc, raw), raw);
  // Every backslash in the message must have survived as a real pair.
  const char* msg = s_doc["msg"].as<const char*>();
  TEST_ASSERT_NOT_NULL(msg);
  for (size_t i = 0; msg[i]; i++) TEST_ASSERT_EQUAL_CHAR('\\', msg[i]);
}

void test_log_quote_at_the_truncation_boundary_stays_valid(void) {
  enableLogMirror();
  char line[400];
  memset(line, '"', sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  mqttPublishLog(SYSLOG_INFO, line);
  const char* raw = mqttTestPayloadFor(LOG_TOPIC);
  TEST_ASSERT_NOT_NULL(raw);
  s_doc.clear();
  TEST_ASSERT_FALSE_MESSAGE(deserializeJson(s_doc, raw), raw);
}

void test_log_suppressed_when_mirror_or_link_is_off(void) {
  // Three independent gates, each of which must stop the publish on its own.
  g_mqttConfig.logEnabled = false;
  mqttPublishLog(SYSLOG_INFO, "x");
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());

  enableLogMirror();
  g_mqttConfig.enabled = false;
  mqttPublishLog(SYSLOG_INFO, "x");
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());

  g_mqttConfig.enabled = true;
  mqttTestSetConnected(false);
  mqttPublishLog(SYSLOG_INFO, "x");
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());
}

void test_log_empty_line_publishes_nothing(void) {
  enableLogMirror();
  mqttPublishLog(SYSLOG_INFO, "");
  mqttPublishLog(SYSLOG_INFO, nullptr);
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());
}

// The re-entry guard: publishing a log line from inside a publish must not
// recurse. Reached by having the stub call back into mqttPublishLog() the
// moment a publish lands, which is what a logMsg() from inside PubSubClient
// would do on hardware.
static void reentrantPublishHook(const char*) {
  mqttPublishLog(SYSLOG_INFO, "from inside a publish");
}

void test_log_reentry_guard_stops_recursion(void) {
  enableLogMirror();
  g_mqttTest.onPublish = reentrantPublishHook;
  mqttPublishLog(SYSLOG_INFO, "outer");
  g_mqttTest.onPublish = nullptr;
  // Exactly one publish: the inner call returns immediately on the guard.
  TEST_ASSERT_EQUAL_INT(1, mqttTestPublishCount());
  JsonDocument& d = payloadJson(LOG_TOPIC);
  TEST_ASSERT_EQUAL_STRING("outer", d["msg"].as<const char*>());
}

// ============================================================
// B. Home Assistant discovery - topics, fields, units
// ============================================================

#define DEV_ID    "ourbrewbot_" CHIP_UPPER
#define F0_ID     DEV_ID "_f0"
#define DISC(component, devId, obj) "homeassistant/" component "/" devId "/" obj "/config"

void test_discovery_device_entity_topic_and_common_fields(void) {
  g_mqttConfig.haDiscovery = true;
  publishAllHaDiscovery();

  const char* topic = DISC("sensor", DEV_ID, "firmware_version");
  TEST_ASSERT_TRUE(mqttTestPublished(topic));
  // A discovery config MUST be retained or HA loses every entity on restart.
  TEST_ASSERT_TRUE(mqttTestRetainedFor(topic));

  JsonDocument& d = payloadJson(topic);
  TEST_ASSERT_EQUAL_STRING(DEV_ID "_firmware_version", d["uniq_id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("Firmware Version",         d["name"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(BASE "/Device/firmware_version", d["stat_t"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(BASE "/availability",       d["avty_t"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(DEV_ID,                     d["dev"]["ids"][0].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("OurBrewbot",               d["dev"]["name"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(FW_VERSION,                 d["dev"]["sw"].as<const char*>());
}

void test_discovery_component_matches_entity_kind(void) {
  g_mqttConfig.haDiscovery = true;
  publishAllHaDiscovery();

  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor",        F0_ID, "beer_temperature")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("binary_sensor", F0_ID, "alarm")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("number",        F0_ID, "ceiling_temperature")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("switch",        F0_ID, "power")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("select",        F0_ID, "profile_no")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("text",          F0_ID, "name")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("button",        DEV_ID, "reboot")));
}

void test_discovery_switch_carries_command_topic_and_payloads(void) {
  g_mqttConfig.haDiscovery = true;
  publishAllHaDiscovery();

  JsonDocument& d = payloadJson(DISC("switch", F0_ID, "power"));
  TEST_ASSERT_EQUAL_STRING(BASE "/Fermenter0/power",     d["stat_t"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING(BASE "/Fermenter0/power/set", d["cmd_t"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("ON",  d["pl_on"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("OFF", d["pl_off"].as<const char*>());
}

void test_discovery_select_lists_every_profile_slot(void) {
  g_mqttConfig.haDiscovery = true;
  publishAllHaDiscovery();

  JsonDocument& d = payloadJson(DISC("select", F0_ID, "profile_no"));
  // 0 means "no profile", then one option per profile slot.
  TEST_ASSERT_EQUAL_INT(MAX_PROFILES + 1, d["ops"].size());
  TEST_ASSERT_EQUAL_STRING("0", d["ops"][0].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("4", d["ops"][MAX_PROFILES].as<const char*>());
}

void test_discovery_button_has_command_but_no_state_topic(void) {
  g_mqttConfig.haDiscovery = true;
  publishAllHaDiscovery();

  JsonDocument& d = payloadJson(DISC("button", DEV_ID, "all_off"));
  TEST_ASSERT_EQUAL_STRING(BASE "/Device/all_off/set", d["cmd_t"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("1", d["pl_prs"].as<const char*>());
  TEST_ASSERT_TRUE(d["stat_t"].isNull());
}

void test_discovery_celsius_units_and_number_ranges(void) {
  g_mqttConfig.haDiscovery = true;
  g_globalConfig.unit = UNIT_CELSIUS;
  publishAllHaDiscovery();

  JsonDocument& d = payloadJson(DISC("number", F0_ID, "ceiling_temperature"));
  TEST_ASSERT_EQUAL_STRING("\xC2\xB0" "C", d["unit_of_meas"].as<const char*>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -20.0f, d["min"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f,  50.0f, d["max"].as<float>());
}

void test_discovery_fahrenheit_converts_absolute_min_max(void) {
  g_mqttConfig.haDiscovery = true;
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  publishAllHaDiscovery();

  JsonDocument& d = payloadJson(DISC("number", F0_ID, "ceiling_temperature"));
  TEST_ASSERT_EQUAL_STRING("\xC2\xB0" "F", d["unit_of_meas"].as<const char*>());
  // HAF_MINMAX_TEMP: absolute temperatures, so the +32 offset applies.
  TEST_ASSERT_FLOAT_WITHIN(0.01f,  -4.0f, d["min"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 122.0f, d["max"].as<float>());
}

void test_discovery_fahrenheit_converts_hysteresis_as_a_span(void) {
  // The distinction that keeps being got wrong: hysteresis is a DIFFERENCE, so
  // 10 degC of range is 18 degF, not 50. HAF_MAX_DELTA rather than
  // HAF_MINMAX_TEMP is what makes that happen, and the two flags look
  // interchangeable at the call site.
  g_mqttConfig.haDiscovery = true;
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  publishAllHaDiscovery();

  JsonDocument& d = payloadJson(DISC("number", F0_ID, "hysteresis"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f,  0.0f, d["min"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f, d["max"].as<float>());
}

void test_discovery_non_temperature_ranges_are_never_converted(void) {
  g_mqttConfig.haDiscovery = true;
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  publishAllHaDiscovery();

  JsonDocument& og = payloadJson(DISC("number", F0_ID, "og"));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.990f, og["min"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.200f, og["max"].as<float>());

  JsonDocument& cd = payloadJson(DISC("number", F0_ID, "compressor_delay"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f,    0.0f, cd["min"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1440.0f, cd["max"].as<float>());
  TEST_ASSERT_EQUAL_STRING("min", cd["unit_of_meas"].as<const char*>());
}

void test_discovery_optional_fields_omitted_when_absent(void) {
  g_mqttConfig.haDiscovery = true;
  publishAllHaDiscovery();

  // "status" has no device class, no unit and no state class - those keys must
  // be absent rather than present and empty, which HA rejects.
  JsonDocument& d = payloadJson(DISC("sensor", F0_ID, "status"));
  TEST_ASSERT_TRUE(d["dev_cla"].isNull());
  TEST_ASSERT_TRUE(d["unit_of_meas"].isNull());
  TEST_ASSERT_TRUE(d["stat_cla"].isNull());
  TEST_ASSERT_EQUAL_STRING("mdi:thermometer", d["ic"].as<const char*>());
}

void test_discovery_diagnostic_entities_are_categorised(void) {
  g_mqttConfig.haDiscovery = true;
  publishAllHaDiscovery();

  JsonDocument& d = payloadJson(DISC("sensor", DEV_ID, "rssi"));
  TEST_ASSERT_EQUAL_STRING("diagnostic",       d["ent_cat"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("signal_strength",  d["dev_cla"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("dBm",              d["unit_of_meas"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("measurement",      d["stat_cla"].as<const char*>());
}

void test_discovery_skips_fermenters_without_the_mqtt_service_bit(void) {
  g_mqttConfig.haDiscovery = true;
  g_fermenters[1].brewServices = 0;
  publishAllHaDiscovery();

  TEST_ASSERT_TRUE (mqttTestPublished(DISC("sensor", DEV_ID "_f0", "beer_temperature")));
  TEST_ASSERT_FALSE(mqttTestPublished(DISC("sensor", DEV_ID "_f1", "beer_temperature")));
}

void test_force_discovery_ignores_the_service_bit_and_the_ha_flag(void) {
  // The admin UI's Discover button exists to repair a broken HA state, so it
  // deliberately overrides both gates that publishAllHaDiscovery() honours.
  g_mqttConfig.haDiscovery = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenters[i].brewServices = 0;

  TEST_ASSERT_TRUE(forcePublishAllHaDiscovery());
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor", DEV_ID "_f0", "beer_temperature")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor", DEV_ID "_f3", "beer_temperature")));
}

void test_discovery_suppressed_when_flag_off_or_disconnected(void) {
  g_mqttConfig.haDiscovery = false;
  publishAllHaDiscovery();
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());

  g_mqttConfig.haDiscovery = true;
  mqttTestSetConnected(false);
  publishAllHaDiscovery();
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());
}

void test_force_discovery_refuses_when_mqtt_is_unconfigured(void) {
  g_mqttConfig.host[0] = '\0';
  TEST_ASSERT_FALSE(forcePublishAllHaDiscovery());
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());
}

void test_discovery_probe_uses_address_in_device_id_and_topic(void) {
  g_mqttConfig.haDiscovery = true;
  strlcpy(g_probes[0].address,   "28FF1234", sizeof(g_probes[0].address));
  strlcpy(g_probes[0].probeName, "Beer",     sizeof(g_probes[0].probeName));
  publishAllHaDiscovery();

  const char* topic = DISC("sensor", DEV_ID "_probe_28FF1234", "temperature");
  JsonDocument& d = payloadJson(topic);
  TEST_ASSERT_EQUAL_STRING(BASE "/Probe/28FF1234/temperature", d["stat_t"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("OurBrewbot Probe Beer", d["dev"]["name"].as<const char*>());
}

void test_discovery_skips_unconfigured_probe_tilt_and_ispindel_slots(void) {
  g_mqttConfig.haDiscovery = true;
  publishAllHaDiscovery();   // fixture leaves all three unconfigured

  TEST_ASSERT_EQUAL_INT(0, mqttTestCountContaining("_probe_"));
  TEST_ASSERT_EQUAL_INT(0, mqttTestCountContaining("_tilt_"));
  TEST_ASSERT_EQUAL_INT(0, mqttTestCountContaining("_ispindel_"));
}

void test_discovery_tilt_slot_index_names_the_colour(void) {
  g_mqttConfig.haDiscovery = true;
  g_tilts[2].colour = 2;     // slot index IS the colour - Black
  publishAllHaDiscovery();

  const char* topic = DISC("sensor", DEV_ID "_tilt_Black", "temperature");
  JsonDocument& d = payloadJson(topic);
  TEST_ASSERT_EQUAL_STRING(BASE "/Tilt/Black/temperature", d["stat_t"].as<const char*>());
}

void test_discovery_ispindel_named_none_is_skipped(void) {
  g_mqttConfig.haDiscovery = true;
  strlcpy(g_iSpindels[0].id,   "A1B2C3", sizeof(g_iSpindels[0].id));
  strlcpy(g_iSpindels[0].name, "None",   sizeof(g_iSpindels[0].name));
  publishAllHaDiscovery();
  TEST_ASSERT_EQUAL_INT(0, mqttTestCountContaining("_ispindel_"));

  mqttTestResetRecords();
  strlcpy(g_iSpindels[0].name, "Spindel 1", sizeof(g_iSpindels[0].name));
  publishAllHaDiscovery();
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor", DEV_ID "_ispindel_A1B2C3", "gravity")));
}

void test_no_discovery_payload_exceeds_the_client_buffer(void) {
  // publishAndReset() SKIPS any entity whose serialized config exceeds
  // s_discPayload (1024 B, sized to PubSubClient's buffer) - silently, from
  // HA's point of view. Adding a field to a descriptor table is exactly how
  // that limit gets crossed, so guard every row in every table at once.
  g_mqttConfig.haDiscovery = true;
  g_globalConfig.unit = UNIT_FAHRENHEIT;   // longer unit strings than Celsius
  strlcpy(g_probes[0].address, "28FF1234", sizeof(g_probes[0].address));
  g_tilts[0].colour = 0;
  strlcpy(g_iSpindels[0].id,   "A1B2C3",    sizeof(g_iSpindels[0].id));
  strlcpy(g_iSpindels[0].name, "Spindel 1", sizeof(g_iSpindels[0].name));

  publishAllHaDiscovery();

  TEST_ASSERT_GREATER_THAN_INT(0, mqttTestPublishCount());
  TEST_ASSERT_EQUAL_INT(0, g_mqttTest.overflowCount);
  for (int i = 0; i < mqttTestPublishCount(); i++) {
    TEST_ASSERT_LESS_THAN_UINT_MESSAGE(1024, g_mqttTest.records[i].payloadLen,
                                       g_mqttTest.records[i].topic);
  }
}

void test_discovery_publishes_every_row_of_every_table(void) {
  g_mqttConfig.haDiscovery = true;
  strlcpy(g_probes[0].address, "28FF1234", sizeof(g_probes[0].address));
  g_tilts[0].colour = 0;
  strlcpy(g_iSpindels[0].id,   "A1B2C3",    sizeof(g_iSpindels[0].id));
  strlcpy(g_iSpindels[0].name, "Spindel 1", sizeof(g_iSpindels[0].name));

  publishAllHaDiscovery();

  // 12 device + 24 per fermenter x 4 + 5 probe + 6 tilt + 11 iSpindel
  TEST_ASSERT_EQUAL_INT(12 + (24 * 4) + 5 + 6 + 11, mqttTestPublishCount());
}

void test_discovery_publish_failure_does_not_abort_the_burst(void) {
  // A broker dropping one publish mid-discovery must not cost the rest of the
  // entities - the failure is logged and the loop carries on.
  g_mqttConfig.haDiscovery = true;
  mqttTestSetPublishFailTopic("firmware_version");
  publishAllHaDiscovery();

  TEST_ASSERT_FALSE(mqttTestPublished(DISC("sensor", DEV_ID, "firmware_version")));
  TEST_ASSERT_TRUE (mqttTestPublished(DISC("sensor", DEV_ID, "ip_address")));
  TEST_ASSERT_TRUE (mqttTestPublished(DISC("sensor", F0_ID, "beer_temperature")));
}

// ============================================================
// C. Discovery removal
// ============================================================

void test_removal_publishes_empty_retained_payloads(void) {
  mqttTestSetConnected(true);
  cleanupAllHaDiscovery();

  const char* topic = DISC("sensor", DEV_ID, "firmware_version");
  TEST_ASSERT_TRUE(mqttTestPublished(topic));
  // Both halves matter: a retained empty payload is what actually deletes the
  // entity. Non-retained would leave the original config in place.
  TEST_ASSERT_TRUE(mqttTestRetainedFor(topic));
  TEST_ASSERT_EQUAL_UINT(0, mqttTestPayloadLenFor(topic));
}

// Snapshot of the topics one call produced.
#define SNAPSHOT_MAX 512
static char s_snapshot[SNAPSHOT_MAX][MQTT_TEST_MAX_TOPIC];
static int  s_snapshotCount = 0;

static void snapshotTopics() {
  s_snapshotCount = mqttTestPublishCount();
  TEST_ASSERT_LESS_OR_EQUAL_INT(SNAPSHOT_MAX, s_snapshotCount);
  for (int i = 0; i < s_snapshotCount; i++) {
    strncpy(s_snapshot[i], mqttTestTopicAt(i), MQTT_TEST_MAX_TOPIC - 1);
    s_snapshot[i][MQTT_TEST_MAX_TOPIC - 1] = '\0';
  }
}

void test_cleanup_removes_everything_discovery_published(void) {
  // Mqtt.cpp's descriptor tables exist so that "an entity added to a table can
  // never leak a retained HA config by being forgotten on the remove side".
  // Nothing enforced that until now, and the failure mode is invisible from the
  // device: the stale config lives on the broker.
  g_mqttConfig.haDiscovery = true;
  strlcpy(g_probes[0].address, "28FF1234", sizeof(g_probes[0].address));
  g_tilts[0].colour = 0;
  strlcpy(g_iSpindels[0].id,   "A1B2C3",    sizeof(g_iSpindels[0].id));
  strlcpy(g_iSpindels[0].name, "Spindel 1", sizeof(g_iSpindels[0].name));

  publishAllHaDiscovery();
  snapshotTopics();
  TEST_ASSERT_GREATER_THAN_INT(0, s_snapshotCount);

  mqttTestResetRecords();
  cleanupAllHaDiscovery();

  for (int i = 0; i < s_snapshotCount; i++) {
    TEST_ASSERT_TRUE_MESSAGE(mqttTestPublished(s_snapshot[i]), s_snapshot[i]);
  }
}

void test_cleanup_also_clears_legacy_fermenter_entities(void) {
  mqttTestSetConnected(true);
  cleanupAllHaDiscovery();

  // Object ids from v0.1.20-v0.1.23 that publish never uses; upgrades would
  // otherwise leave them stranded in HA forever.
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor",        F0_ID, "beer_temp")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("binary_sensor", F0_ID, "power")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor",        F0_ID, "free_heap")));
}

void test_cleanup_covers_all_fermenters_regardless_of_service_bit(void) {
  // Removal is deliberately broader than publish: a fermenter that had the
  // MQTT bit turned off still has retained configs from when it was on.
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenters[i].brewServices = 0;
  cleanupAllHaDiscovery();

  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor", DEV_ID "_f0", "beer_temperature")));
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor", DEV_ID "_f3", "beer_temperature")));
}

void test_cleanup_skips_unconfigured_sensor_slots(void) {
  cleanupAllHaDiscovery();
  TEST_ASSERT_EQUAL_INT(0, mqttTestCountContaining("_probe_"));
  TEST_ASSERT_EQUAL_INT(0, mqttTestCountContaining("_tilt_"));
  TEST_ASSERT_EQUAL_INT(0, mqttTestCountContaining("_ispindel_"));
}

void test_cleanup_does_nothing_when_disconnected(void) {
  mqttTestSetConnected(false);
  cleanupAllHaDiscovery();
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());
}

// ============================================================
// D. reportMqtt() - the periodic state publish
// ============================================================

#define F0_BASE BASE "/Fermenter0"

void test_report_publishes_fermenter_identity_and_state(void) {
  g_fermenters[F0].status = STATUS_COOLING;
  reportMqtt();

  assertPayload(F0_BASE "/name",         "Fermenter 1");
  assertPayload(F0_BASE "/power",        "ON");
  assertPayload(F0_BASE "/temp_control", "ON");
  assertPayload(F0_BASE "/alarm",        "OFF");
  assertPayload(F0_BASE "/status",       "cooling");
}

void test_report_publishes_celsius_values_unchanged(void) {
  g_globalConfig.unit = UNIT_CELSIUS;
  reportMqtt();

  assertPayload(F0_BASE "/ceiling_temperature", "20.0");
  assertPayload(F0_BASE "/floor_temperature",   "18.0");
  assertPayload(F0_BASE "/hysteresis",          "0.5");
  assertPayload(F0_BASE "/temperature_unit",    "C");
}

void test_report_converts_setpoints_to_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  reportMqtt();

  assertPayload(F0_BASE "/ceiling_temperature", "68.0");   // 20 degC
  assertPayload(F0_BASE "/floor_temperature",   "64.4");   // 18 degC
  // A span, not a temperature: 0.5 degC of hysteresis is 0.9 degF, not 32.9.
  assertPayload(F0_BASE "/hysteresis",          "0.9");
  assertPayload(F0_BASE "/temperature_unit",    "F");
}

void test_report_omits_beer_temperature_when_no_sensor(void) {
  // getBeerTemp() returns the -127 "no reading" sentinel with nothing assigned.
  // Publishing it would show -127 degC in HA as though it were real.
  reportMqtt();
  assertNotPublished(F0_BASE "/beer_temperature");
  assertNotPublished(F0_BASE "/ambient_temperature");
  TEST_ASSERT_TRUE(mqttTestPublished(F0_BASE "/beer_temperature_source"));
}

void test_report_publishes_beer_temperature_when_a_probe_is_assigned(void) {
  strlcpy(g_probes[0].address, "28FF1234", sizeof(g_probes[0].address));
  g_probes[0].function    = PROBE_FN_BEER;
  g_probes[0].fermenter   = F0;
  g_probes[0].temperature = 19.5f;

  g_globalConfig.unit = UNIT_FAHRENHEIT;
  reportMqtt();
  assertPayload(F0_BASE "/beer_temperature", "67.1");   // 19.5 degC
}

void test_report_keeps_the_probe_no_reading_sentinel_unconverted(void) {
  // -127 is a marker, not a temperature. Converting it would publish -196.6 in
  // Fahrenheit, which looks like a plausible (if absurd) reading rather than an
  // obviously absent one - the fourth place in this codebase where a sentinel
  // shares the domain of the values around it.
  strlcpy(g_probes[0].address, "28FF1234", sizeof(g_probes[0].address));
  g_probes[0].temperature = DEVICE_DISCONNECTED_C;
  g_globalConfig.unit = UNIT_FAHRENHEIT;

  reportMqtt();
  assertPayload(BASE "/Probe/28FF1234/temperature", "-127.0");
}

void test_report_publishes_probe_metadata(void) {
  strlcpy(g_probes[0].address,   "28FF1234", sizeof(g_probes[0].address));
  strlcpy(g_probes[0].probeName, "Beer",     sizeof(g_probes[0].probeName));
  g_probes[0].function  = PROBE_FN_BEER;
  g_probes[0].fermenter = 2;
  g_probes[0].failCount = 0;

  reportMqtt();
  assertPayload(BASE "/Probe/28FF1234/name",      "Beer");
  assertPayload(BASE "/Probe/28FF1234/function",  "beer");
  assertPayload(BASE "/Probe/28FF1234/fermenter", "2");
  assertPayload(BASE "/Probe/28FF1234/active",    "ON");
}

void test_report_marks_a_failed_probe_inactive(void) {
  strlcpy(g_probes[0].address, "28FF1234", sizeof(g_probes[0].address));
  g_probes[0].failCount = PROBE_FAIL_THRESHOLD;
  reportMqtt();
  assertPayload(BASE "/Probe/28FF1234/active", "OFF");
}

void test_report_publishes_the_gravity_estimate_when_no_sensor_reports(void) {
  // With no Tilt or iSpindel assigned, getCurrentSG() falls back to the
  // three-phase estimate, which at hour 0 is simply OG. The published value is
  // labelled so HA can tell a model output from a measurement.
  reportMqtt();
  assertPayload(F0_BASE "/gravity",        "1.0500");
  assertPayload(F0_BASE "/gravity_source", "Estimated");
  // OG/TG are configuration, not readings, so they publish regardless.
  assertPayload(F0_BASE "/og", "1.0500");
  assertPayload(F0_BASE "/tg", "1.0100");
}

void test_report_omits_gravity_when_there_is_nothing_to_estimate_from(void) {
  // No sensor and no OG: the estimate is 0, and the sg > 0 guard is what stops
  // a fermenter with no beer in it publishing a gravity of 0.0000.
  g_fermenters[F0].og = 0.0f;
  g_fermenters[F0].tg = 0.0f;
  reportMqtt();
  assertNotPublished(F0_BASE "/gravity");
  // The source label still publishes - HA keeps the entity, it just has no value.
  TEST_ASSERT_TRUE(mqttTestPublished(F0_BASE "/gravity_source"));
}

void test_report_zeroes_profile_progress_when_not_running(void) {
  g_fermenters[F0].profileRunning = false;
  reportMqtt();
  assertPayload(F0_BASE "/profile_running", "OFF");
  assertPayload(F0_BASE "/profile_step",    "0");
  assertPayload(F0_BASE "/profile_steps",   "0");
}

void test_report_publishes_profile_progress_when_running(void) {
  g_fermenters[F0].profileNo       = 1;
  g_fermenters[F0].profileRunning  = true;
  g_fermenters[F0].currentStep     = 1;
  // Two live steps in profile slot 1; the rest stay all-zero (the empty
  // sentinel), which is how countProfileSteps() finds the end.
  g_profileSteps[0].startTemp = 20.0f;
  g_profileSteps[0].endTemp   = 20.0f;
  g_profileSteps[0].days      = 2;
  g_profileSteps[1].startTemp = 22.0f;
  g_profileSteps[1].endTemp   = 22.0f;
  g_profileSteps[1].days      = 3;

  reportMqtt();
  assertPayload(F0_BASE "/profile_running", "ON");
  assertPayload(F0_BASE "/profile_step",    "2");   // currentStep is 0-based
  assertPayload(F0_BASE "/profile_steps",   "2");
}

void test_report_skips_fermenters_without_the_mqtt_service_bit(void) {
  g_fermenters[1].brewServices = 0;
  reportMqtt();
  TEST_ASSERT_TRUE (mqttTestPublished(F0_BASE "/name"));
  TEST_ASSERT_FALSE(mqttTestPublished(BASE "/Fermenter1/name"));
}

void test_report_publishes_device_diagnostics(void) {
  reportMqtt();
  assertPayload(BASE "/Device/firmware_version", FW_VERSION);
  assertPayload(BASE "/Device/ip_address",       "192.168.0.207");
  assertPayload(BASE "/Device/wifi_ssid",        "TestNetwork");
  assertPayload(BASE "/Device/rssi",             "-60");
  assertPayload(BASE "/Device/reboot_reason",    "Power on");
  // Uptime is minutes, and the fixture clock is 1,000,000 ms.
  assertPayload(BASE "/Device/uptime",           "16");
}

void test_report_device_ids_differ_in_case_between_report_and_discovery(void) {
  // Not a typo in either place: the reported chip_id is lowercase (%06x) while
  // the discovery device id is uppercase (%06X). Anything correlating the two
  // has to fold case.
  g_mqttConfig.haDiscovery = true;
  reportMqtt();
  assertPayload(BASE "/Device/chip_id", CHIP_LOWER);
  assertPayload(BASE "/Device/mdns_name", "ourbrewbot-" CHIP_LOWER ".local");

  mqttTestResetRecords();
  publishAllHaDiscovery();
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor", "ourbrewbot_" CHIP_UPPER, "chip_id")));
}

void test_report_does_nothing_when_disabled_or_offline(void) {
  g_mqttConfig.enabled = false;
  reportMqtt();
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());

  g_mqttConfig.enabled = true;
  WiFi.connected = false;
  reportMqtt();
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());
  WiFi.connected = true;
}

void test_report_values_are_retained(void) {
  // State topics must be retained: HA shows the last known value immediately on
  // restart rather than "unknown" until the next 60 s report.
  reportMqtt();
  TEST_ASSERT_TRUE(mqttTestRetainedFor(F0_BASE "/name"));
  TEST_ASSERT_TRUE(mqttTestRetainedFor(F0_BASE "/ceiling_temperature"));
}

// ============================================================
// E. Connect handshake and the inbound command path
// ============================================================

void test_connect_sets_the_last_will_to_offline(void) {
  mqttTestSetConnected(false);
  mqttTestSetConnectOk(true);
  testMqtt();

  const MqttConnectRecord& c = g_mqttTest.lastConnect;
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-" CHIP_UPPER, c.clientId);
  TEST_ASSERT_EQUAL_STRING(BASE "/availability", c.willTopic);
  TEST_ASSERT_EQUAL_STRING("offline", c.willMessage);
  // Retained: a subscriber connecting after the drop still learns the device
  // is gone.
  TEST_ASSERT_TRUE(c.willRetain);
}

void test_connect_without_credentials_passes_null_user(void) {
  g_mqttConfig.username[0] = '\0';
  mqttTestSetConnected(false);
  testMqtt();
  TEST_ASSERT_FALSE(g_mqttTest.lastConnect.hadCredentials);
}

void test_connect_with_credentials_passes_them_through(void) {
  strlcpy(g_mqttConfig.username, "brewer", sizeof(g_mqttConfig.username));
  strlcpy(g_mqttConfig.password, "secret", sizeof(g_mqttConfig.password));
  mqttTestSetConnected(false);
  testMqtt();

  TEST_ASSERT_TRUE(g_mqttTest.lastConnect.hadCredentials);
  TEST_ASSERT_EQUAL_STRING("brewer", g_mqttTest.lastConnect.username);
  TEST_ASSERT_EQUAL_STRING("secret", g_mqttTest.lastConnect.password);
}

void test_connect_marks_device_online_and_subscribes(void) {
  mqttTestSetConnected(false);
  mqttTestResetRecords();
  testMqtt();

  assertPayload(BASE "/availability", "online");
  TEST_ASSERT_TRUE(mqttTestRetainedFor(BASE "/availability"));
  TEST_ASSERT_TRUE(mqttTestWasSubscribed("homeassistant/status"));
}

void test_control_subscription_follows_the_allow_control_setting(void) {
  g_mqttConfig.allowControl = true;
  mqttApplyControlSubscription();
  TEST_ASSERT_TRUE(mqttTestWasSubscribed(BASE "/+/+/set"));

  g_mqttConfig.allowControl = false;
  mqttApplyControlSubscription();
  TEST_ASSERT_TRUE(mqttTestWasUnsubscribed(BASE "/+/+/set"));
}

void test_ha_birth_message_republishes_discovery(void) {
  // HA forgets every entity when it restarts; the birth message is the only
  // signal the device gets that it needs to advertise itself again.
  g_mqttConfig.haDiscovery = true;
  mqttTestInject("homeassistant/status", "online");
  TEST_ASSERT_TRUE(mqttTestPublished(DISC("sensor", F0_ID, "beer_temperature")));
}

void test_ha_offline_message_does_not_republish(void) {
  g_mqttConfig.haDiscovery = true;
  mqttTestInject("homeassistant/status", "offline");
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());
}

void test_inbound_setpoint_arrives_in_display_units(void) {
  // The end-to-end path this suite exists to reach: a Fahrenheit number typed
  // into HA has to land in the Celsius store, and the echo has to come back in
  // Fahrenheit. Only the conversion halves were testable before.
  g_mqttConfig.allowControl = true;
  g_globalConfig.unit = UNIT_FAHRENHEIT;

  mqttTestInject(BASE "/Fermenter0/ceiling_temperature/set", "70.0");

  TEST_ASSERT_FLOAT_WITHIN(0.05f, 21.11f, g_fermenters[F0].ceilingTemp);
  assertPayload(F0_BASE "/ceiling_temperature", "70.0");
}

void test_inbound_command_ignored_when_control_is_disabled(void) {
  g_mqttConfig.allowControl = false;
  mqttTestInject(BASE "/Fermenter0/power/set", "OFF");
  TEST_ASSERT_TRUE(g_fermenters[F0].power);
  TEST_ASSERT_EQUAL_INT(0, mqttTestPublishCount());
}

void test_inbound_switch_command_applies_and_echoes(void) {
  g_mqttConfig.allowControl = true;
  mqttTestInject(BASE "/Fermenter0/temp_control/set", "OFF");
  TEST_ASSERT_FALSE(g_fermenters[F0].tempControl);
  assertPayload(F0_BASE "/temp_control", "OFF");
}

void test_inbound_rejected_value_is_not_stored(void) {
  g_mqttConfig.allowControl = true;
  mqttTestInject(BASE "/Fermenter0/profile_no/set", "99");
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[F0].profileNo);
  assertNotPublished(F0_BASE "/profile_no");
}

void test_mqtt_loop_only_runs_while_connected(void) {
  g_mqttTest.loopCount = 0;
  mqttLoop();
  TEST_ASSERT_EQUAL_INT(1, g_mqttTest.loopCount);

  mqttTestSetConnected(false);
  mqttLoop();
  TEST_ASSERT_EQUAL_INT(1, g_mqttTest.loopCount);
}

void test_failed_connect_backs_off_before_retrying(void) {
  // Backoff starts at 5 s and doubles. Without it a broker that is down turns
  // every report tick into a blocking connect attempt.
  mqttTestSetConnected(false);
  mqttTestSetConnectOk(false);

  reportMqtt();                                   // attempt 1
  const int after1 = g_mqttTest.lastConnect.attempts;
  TEST_ASSERT_GREATER_THAN_INT(0, after1);

  s_millis += 1000;                               // still inside the 5 s window
  reportMqtt();
  TEST_ASSERT_EQUAL_INT(after1, g_mqttTest.lastConnect.attempts);

  s_millis += 10000;                              // past it
  reportMqtt();
  TEST_ASSERT_EQUAL_INT(after1 + 1, g_mqttTest.lastConnect.attempts);
}

void test_successful_connect_resets_the_backoff(void) {
  mqttTestSetConnected(false);
  mqttTestSetConnectOk(false);
  reportMqtt();
  s_millis += 10000;
  reportMqtt();          // backoff now 20 s

  mqttTestSetConnectOk(true);
  s_millis += 30000;
  reportMqtt();          // succeeds, backoff back to 5 s
  TEST_ASSERT_TRUE(mqttTestPublished(F0_BASE "/name"));

  mqttTestSetConnected(false);
  mqttTestSetConnectOk(false);
  mqttTestResetRecords();
  const int before = g_mqttTest.lastConnect.attempts;
  s_millis += 6000;      // longer than 5 s, shorter than the 20 s it had grown to
  reportMqtt();
  TEST_ASSERT_EQUAL_INT(before + 1, g_mqttTest.lastConnect.attempts);
}

// ============================================================

int main(int, char**) {
  UNITY_BEGIN();

  // dtostrf - the formatter every published number goes through
  RUN_TEST(test_dtostrf_respects_precision);
  RUN_TEST(test_dtostrf_width_is_a_minimum_not_a_truncation);
  RUN_TEST(test_dtostrf_zero_precision_drops_the_point);

  // A. log mirror
  RUN_TEST(test_log_publishes_to_device_log_topic_unretained);
  RUN_TEST(test_log_payload_is_valid_json_with_level_and_message);
  RUN_TEST(test_log_escapes_double_quote);
  RUN_TEST(test_log_escapes_backslash);
  RUN_TEST(test_log_escapes_both_in_one_line);
  RUN_TEST(test_log_severity_names_match_syslog_levels);
  RUN_TEST(test_log_level_above_range_falls_back_to_info);
  RUN_TEST(test_log_long_line_truncates_but_stays_valid_json);
  RUN_TEST(test_log_truncation_never_leaves_a_dangling_escape);
  RUN_TEST(test_log_quote_at_the_truncation_boundary_stays_valid);
  RUN_TEST(test_log_suppressed_when_mirror_or_link_is_off);
  RUN_TEST(test_log_empty_line_publishes_nothing);
  RUN_TEST(test_log_reentry_guard_stops_recursion);

  // B. discovery
  RUN_TEST(test_discovery_device_entity_topic_and_common_fields);
  RUN_TEST(test_discovery_component_matches_entity_kind);
  RUN_TEST(test_discovery_switch_carries_command_topic_and_payloads);
  RUN_TEST(test_discovery_select_lists_every_profile_slot);
  RUN_TEST(test_discovery_button_has_command_but_no_state_topic);
  RUN_TEST(test_discovery_celsius_units_and_number_ranges);
  RUN_TEST(test_discovery_fahrenheit_converts_absolute_min_max);
  RUN_TEST(test_discovery_fahrenheit_converts_hysteresis_as_a_span);
  RUN_TEST(test_discovery_non_temperature_ranges_are_never_converted);
  RUN_TEST(test_discovery_optional_fields_omitted_when_absent);
  RUN_TEST(test_discovery_diagnostic_entities_are_categorised);
  RUN_TEST(test_discovery_skips_fermenters_without_the_mqtt_service_bit);
  RUN_TEST(test_force_discovery_ignores_the_service_bit_and_the_ha_flag);
  RUN_TEST(test_discovery_suppressed_when_flag_off_or_disconnected);
  RUN_TEST(test_force_discovery_refuses_when_mqtt_is_unconfigured);
  RUN_TEST(test_discovery_probe_uses_address_in_device_id_and_topic);
  RUN_TEST(test_discovery_skips_unconfigured_probe_tilt_and_ispindel_slots);
  RUN_TEST(test_discovery_tilt_slot_index_names_the_colour);
  RUN_TEST(test_discovery_ispindel_named_none_is_skipped);
  RUN_TEST(test_no_discovery_payload_exceeds_the_client_buffer);
  RUN_TEST(test_discovery_publishes_every_row_of_every_table);
  RUN_TEST(test_discovery_publish_failure_does_not_abort_the_burst);

  // C. removal
  RUN_TEST(test_removal_publishes_empty_retained_payloads);
  RUN_TEST(test_cleanup_removes_everything_discovery_published);
  RUN_TEST(test_cleanup_also_clears_legacy_fermenter_entities);
  RUN_TEST(test_cleanup_covers_all_fermenters_regardless_of_service_bit);
  RUN_TEST(test_cleanup_skips_unconfigured_sensor_slots);
  RUN_TEST(test_cleanup_does_nothing_when_disconnected);

  // D. report
  RUN_TEST(test_report_publishes_fermenter_identity_and_state);
  RUN_TEST(test_report_publishes_celsius_values_unchanged);
  RUN_TEST(test_report_converts_setpoints_to_fahrenheit);
  RUN_TEST(test_report_omits_beer_temperature_when_no_sensor);
  RUN_TEST(test_report_publishes_beer_temperature_when_a_probe_is_assigned);
  RUN_TEST(test_report_keeps_the_probe_no_reading_sentinel_unconverted);
  RUN_TEST(test_report_publishes_probe_metadata);
  RUN_TEST(test_report_marks_a_failed_probe_inactive);
  RUN_TEST(test_report_publishes_the_gravity_estimate_when_no_sensor_reports);
  RUN_TEST(test_report_omits_gravity_when_there_is_nothing_to_estimate_from);
  RUN_TEST(test_report_zeroes_profile_progress_when_not_running);
  RUN_TEST(test_report_publishes_profile_progress_when_running);
  RUN_TEST(test_report_skips_fermenters_without_the_mqtt_service_bit);
  RUN_TEST(test_report_publishes_device_diagnostics);
  RUN_TEST(test_report_device_ids_differ_in_case_between_report_and_discovery);
  RUN_TEST(test_report_does_nothing_when_disabled_or_offline);
  RUN_TEST(test_report_values_are_retained);

  // E. connect / inbound
  RUN_TEST(test_connect_sets_the_last_will_to_offline);
  RUN_TEST(test_connect_without_credentials_passes_null_user);
  RUN_TEST(test_connect_with_credentials_passes_them_through);
  RUN_TEST(test_connect_marks_device_online_and_subscribes);
  RUN_TEST(test_control_subscription_follows_the_allow_control_setting);
  RUN_TEST(test_ha_birth_message_republishes_discovery);
  RUN_TEST(test_ha_offline_message_does_not_republish);
  RUN_TEST(test_inbound_setpoint_arrives_in_display_units);
  RUN_TEST(test_inbound_command_ignored_when_control_is_disabled);
  RUN_TEST(test_inbound_switch_command_applies_and_echoes);
  RUN_TEST(test_inbound_rejected_value_is_not_stored);
  RUN_TEST(test_mqtt_loop_only_runs_while_connected);
  RUN_TEST(test_failed_connect_backs_off_before_retrying);
  RUN_TEST(test_successful_connect_resets_the_backoff);

  return UNITY_END();
}
