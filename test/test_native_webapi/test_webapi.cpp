// Native (host) tests for OurBrewbot/WebAPI.cpp - the REST surface the WebUI,
// Home Assistant and any script drive the controller through.
//
// The immediate reason for this suite is the partial-write bug in
// handleFermenter's POST path: the temperature trio was applied to
// g_fermenters[idx] before OG/TG/CompressorDelay/AlarmTolerance had been
// validated, so a body rejected with a 400 still moved the ceiling, floor and
// hysteresis that the control loop reads out of RAM. The first group below
// pins that shut. The rest covers the endpoints where a silent misbehaviour
// would either corrupt persisted config (the /fs/save whitelist and its
// JSON pre-validation) or break the WebUI contract (the build*Json payload
// shapes, which the endpoint-baseline diffs in the build process also rely on).
//
// WebAPI.cpp is #included directly, together with the four modules whose real
// behaviour the payloads depend on - Config.cpp (which owns the g_* globals and
// the persistence this suite asserts on), Temperatures.cpp, Fermenter.cpp and
// Profile.cpp. Stubbing those would have made the unit conversions and the
// profile step engine tautological. The MQTT / Reports / Tilt / iSpindel /
// SmartPlugs entry points WebAPI.cpp calls are test doubles below, since none
// of them affect the responses under test.
//
// test/stubs/ESP8266WebServer.h is the piece that makes this reachable: it
// scripts the request (method, URI, args, POST body) and records the response
// (code, content type, body, headers) - including anything serialised straight
// into the WiFiClient from server.client(), which is how sendJsonDoc() emits.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "../../OurBrewbot/Config.h"
#include "../../OurBrewbot/Pins.h"
// The library types whose globals this file has to define for WebAPI.cpp:
// g_bleSerial (Tilt.h), g_rcSwitch (SmartPlugs.h) and g_webServer (the .ino).
#include <SoftwareSerial.h>
#include <RCSwitch.h>
#include <ESP8266WebServer.h>

// ---- millis(), settable per test ----
static uint32_t s_millis = 1000000;   // large, so dwell-timer sentinels of 0 read as "never"
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op / recording doubles for the modules not compiled in ----
void logMsgImpl(uint8_t, PGM_P, ...) {}
void logInit() {}

// Mqtt.cpp
void mqttApplyControlSubscription() {}
void publishAllHaDiscovery() {}
bool forcePublishAllHaDiscovery() { return true; }
void cleanupAllHaDiscovery() {}
bool testMqtt() { return true; }

// Reports.cpp
int testBrewService(uint8_t) { return 200; }

// Tilt.cpp
static const char* const TILT_COLOUR_NAMES[] = {
  "Red", "Green", "Black", "Purple", "Orange", "Blue", "Yellow", "Pink"
};
const char* getTiltColourName(uint8_t colour) {
  return (colour < 8) ? TILT_COLOUR_NAMES[colour] : "Unknown";
}
bool g_bleSniffActive = false;
SoftwareSerial g_bleSerial(PIN_BLE_RX, PIN_BLE_TX);

// iSpindel.cpp
void handleiSpindelPost(const String&) {}

// WebAdmin.cpp serves the admin page and is out of scope for native testing
// (1800 lines of PROGMEM HTML/CSS/JS with nothing assertable). WebAPI.cpp's
// route table takes its address, so the symbol still has to resolve.
void handleAdmin(ESP8266WebServer&) {}

// SmartPlugs.cpp - the RF layer. rfTransmit is recorded so the plug-test
// endpoint can be checked without an RCSwitch.
static int s_rfTransmits = 0;
void rfTransmit(uint32_t, uint8_t, uint16_t, uint8_t) { s_rfTransmits++; }
void smartPlugSwitch(uint8_t, bool) {}
bool getPlugState(uint8_t) { return false; }
RCSwitch g_rcSwitch;

// The main sketch owns the debug overrides, not Config.cpp.
bool g_fermenterDebugMode = false;
FermenterDebugOverride g_fermenterDebugOverrides[MAX_FERMENTERS];

// WebAPI.cpp declares this extern (the real one lives in the .ino). Tests
// drive handlers directly through their own server, so this only has to exist.
ESP8266WebServer g_webServer;

// The code under test, plus the modules its payloads genuinely depend on.
#include "../../OurBrewbot/Config.cpp"
#include "../../OurBrewbot/Temperatures.cpp"
#include "../../OurBrewbot/Fermenter.cpp"
#include "../../OurBrewbot/Profile.cpp"
#include "../../OurBrewbot/WebAPI.cpp"

// ============================================================
// FIXTURE
// ============================================================

static ESP8266WebServer srv;

static const uint8_t F0 = 0;   // fermenter used by most tests

// A known-good fermenter: 2 degC of safe zone against 0.5 hysteresis, so the
// holistic trio rule (span >= 2 * hysteresis) passes with room to spare.
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
  strlcpy(g_fermenters[idx].fermenterName, "Fermenter 1",
          sizeof(g_fermenters[idx].fermenterName));
}

void setUp(void) {
  fsTestReset();
  httpRespReset();
  espTestSetResetReason(REASON_DEFAULT_RST);
  clientTestSetConnected(true);
  s_millis      = 1000000;
  s_rfTransmits = 0;

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
  memset(&g_syslogConfig, 0, sizeof(g_syslogConfig));
  g_fermenterDebugMode = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    g_fermenterDebugOverrides[i] = FermenterDebugOverride{};
  }

  srv.clearArgs();
  srv.setMethod(HTTP_GET);
  srv.setUri("/");
}

void tearDown(void) {}

// ---- request helpers ----

static void postBody(const char* json) {
  srv.setMethod(HTTP_POST);
  srv.setBody(json);
}

static bool bodyContains(const char* needle) {
  return strstr(g_httpResp.body, needle) != nullptr;
}

// ============================================================
// handleFermenter POST — THE PARTIAL-WRITE FIX
//
// Each of these posts a VALID temperature trio alongside one invalid field
// that is checked further down the handler. The response must be a 400 and the
// live config must be exactly as it was: before the fix the trio had already
// been written, so the fermenter was driven to the rejected setpoints until the
// next config load.
// ============================================================

static void test_rejected_og_leaves_the_temperature_trio_untouched(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":25,\"FloorTemp\":21,\"Hysteresis\":1.0,\"OG\":1.5}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("gravity out of range"));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, g_fermenters[F0].floorTemp);
  TEST_ASSERT_EQUAL_FLOAT(0.5f,  g_fermenters[F0].hysteresis);
}

static void test_rejected_tg_leaves_the_temperature_trio_untouched(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":25,\"FloorTemp\":21,\"TG\":0.5}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, g_fermenters[F0].floorTemp);
}

static void test_rejected_compressor_delay_leaves_the_temperature_trio_untouched(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":25,\"FloorTemp\":21,\"CompressorDelay\":5000}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("compressor delay out of range"));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_EQUAL_UINT16(10, g_fermenters[F0].compressorDelay);
}

// AlarmTolerance is validated last of all, below even the VALIDATE_AND_SET
// block, so it is the widest version of the bug.
static void test_rejected_alarm_tolerance_leaves_everything_untouched(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":25,\"FloorTemp\":21,\"Hysteresis\":1.0,"
           "\"OG\":1.060,\"CompressorDelay\":20,\"AlarmTolerance\":99}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("alarm tolerance out of range"));
  TEST_ASSERT_EQUAL_FLOAT(20.0f,  g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(18.0f,  g_fermenters[F0].floorTemp);
  TEST_ASSERT_EQUAL_FLOAT(0.5f,   g_fermenters[F0].hysteresis);
  TEST_ASSERT_EQUAL_FLOAT(1.050f, g_fermenters[F0].og);
  TEST_ASSERT_EQUAL_UINT16(10,    g_fermenters[F0].compressorDelay);
  TEST_ASSERT_EQUAL_FLOAT(3.0f,   g_fermenters[F0].alarmTolerance);
}

// The name fields are applied after every numeric check, so they are the other
// side of the same guarantee: a rejected body must not rename the fermenter.
static void test_rejected_body_does_not_apply_the_name_fields(void) {
  postBody("{\"Fermenter\":0,\"BeerName\":\"Should Not Stick\","
           "\"FermenterName\":\"Renamed\",\"OG\":9.9}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_EQUAL_STRING("", g_fermenters[F0].beerName);
  TEST_ASSERT_EQUAL_STRING("Fermenter 1", g_fermenters[F0].fermenterName);
}

static void test_rejected_body_does_not_change_power_or_temp_control(void) {
  g_fermenters[F0].power       = false;
  g_fermenters[F0].tempControl = false;
  postBody("{\"Fermenter\":0,\"Power\":true,\"TempControl\":true,\"TG\":5.0}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_FALSE(g_fermenters[F0].power);
  TEST_ASSERT_FALSE(g_fermenters[F0].tempControl);
}

// A rejected body must not reach the filesystem either.
static void test_rejected_body_is_not_persisted(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":25,\"OG\":1.5}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_FALSE(LittleFS.exists("/jsonFermenter.txt"));
}

// ---- the accepted path still works, and commits everything ----

static void test_accepted_body_applies_every_field_and_persists(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":24,\"FloorTemp\":20,\"Hysteresis\":1.0,"
           "\"OG\":1.060,\"TG\":1.012,\"CompressorDelay\":15,\"AlarmTolerance\":2.5,"
           "\"Power\":false,\"TempControl\":false,\"BeerName\":\"Saison\","
           "\"FermenterName\":\"Left\",\"YeastName\":\"3711\",\"ProfileNo\":2,"
           "\"BrewServices\":6,\"LiveTest\":true}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Configuration saved"));
  TEST_ASSERT_EQUAL_FLOAT(24.0f,  g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(20.0f,  g_fermenters[F0].floorTemp);
  TEST_ASSERT_EQUAL_FLOAT(1.0f,   g_fermenters[F0].hysteresis);
  TEST_ASSERT_EQUAL_FLOAT(1.060f, g_fermenters[F0].og);
  TEST_ASSERT_EQUAL_FLOAT(1.012f, g_fermenters[F0].tg);
  TEST_ASSERT_EQUAL_UINT16(15,    g_fermenters[F0].compressorDelay);
  TEST_ASSERT_EQUAL_FLOAT(2.5f,   g_fermenters[F0].alarmTolerance);
  TEST_ASSERT_FALSE(g_fermenters[F0].power);
  TEST_ASSERT_FALSE(g_fermenters[F0].tempControl);
  TEST_ASSERT_EQUAL_STRING("Saison", g_fermenters[F0].beerName);
  TEST_ASSERT_EQUAL_STRING("Left",   g_fermenters[F0].fermenterName);
  TEST_ASSERT_EQUAL_STRING("3711",   g_fermenters[F0].yeastName);
  TEST_ASSERT_EQUAL_UINT8(2, g_fermenters[F0].profileNo);
  TEST_ASSERT_EQUAL_UINT8(6, g_fermenters[F0].brewServices);
  TEST_ASSERT_TRUE(g_fermenters[F0].liveTest);
  TEST_ASSERT_TRUE(LittleFS.exists("/jsonFermenter.txt"));
}

// A body naming only one field must leave its siblings alone - the WebUI posts
// partial bodies from individual form controls.
static void test_partial_body_only_touches_the_named_field(void) {
  postBody("{\"Fermenter\":0,\"OG\":1.075}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_FLOAT(1.075f, g_fermenters[F0].og);
  TEST_ASSERT_EQUAL_FLOAT(20.0f,  g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(1.010f, g_fermenters[F0].tg);
}

static void test_only_the_addressed_fermenter_is_modified(void) {
  postBody("{\"Fermenter\":2,\"CeilingTemp\":24}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_FLOAT(24.0f, g_fermenters[2].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g_fermenters[0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g_fermenters[1].ceilingTemp);
}

// ============================================================
// handleFermenter POST — THE HOLISTIC TRIO RULES
//
// Ceiling, floor and hysteresis are validated as a set against the
// would-be-combined state, deliberately: a partial POST is checked against the
// STORED values for the fields it omits, which is what lets a save repair an
// already-invalid in-memory config.
// ============================================================

static void test_ceiling_above_the_range_is_rejected(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":51}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("ceiling temperature out of range"));
}

static void test_floor_below_the_range_is_rejected(void) {
  postBody("{\"Fermenter\":0,\"FloorTemp\":-21}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("floor temperature out of range"));
}

static void test_hysteresis_above_the_range_is_rejected(void) {
  postBody("{\"Fermenter\":0,\"Hysteresis\":11}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("hysteresis out of range"));
}

static void test_floor_at_or_above_ceiling_is_rejected(void) {
  postBody("{\"Fermenter\":0,\"FloorTemp\":20}");   // equal to the stored ceiling
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("floor must be below ceiling"));
}

// The safe zone must be at least twice the hysteresis, or heating and cooling
// overlap and the fermenter oscillates between both.
static void test_safe_zone_narrower_than_twice_hysteresis_is_rejected(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":20,\"FloorTemp\":19,\"Hysteresis\":0.6}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("safe zone must be at least 2x hysteresis"));
}

// A lone Hysteresis change is checked against the STORED ceiling and floor -
// the documented behaviour, pinned here so a refactor cannot quietly drop it.
static void test_lone_hysteresis_is_validated_against_the_stored_span(void) {
  postBody("{\"Fermenter\":0,\"Hysteresis\":1.5}");   // stored span is 2.0
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("safe zone must be at least 2x hysteresis"));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, g_fermenters[F0].hysteresis);
}

static void test_exactly_twice_hysteresis_is_accepted(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":20,\"FloorTemp\":18,\"Hysteresis\":1.0}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, g_fermenters[F0].hysteresis);
}

// ============================================================
// handleFermenter — INDEX AND BODY VALIDATION
// ============================================================

static void test_missing_fermenter_index_is_rejected(void) {
  postBody("{\"CeilingTemp\":22}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Configuration invalid"));
}

static void test_out_of_range_fermenter_index_is_rejected(void) {
  postBody("{\"Fermenter\":4,\"CeilingTemp\":22}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
}

static void test_negative_fermenter_index_is_rejected(void) {
  postBody("{\"Fermenter\":-1,\"CeilingTemp\":22}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
}

static void test_malformed_json_body_is_rejected(void) {
  postBody("{\"Fermenter\":0,");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Configuration invalid"));
}

// ---- GET ----

static void test_get_returns_the_requested_fermenter(void) {
  srv.setArg("id", "1");
  strlcpy(g_fermenters[1].fermenterName, "Second", sizeof(g_fermenters[1].fermenterName));
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_STRING("application/json", g_httpResp.contentType);
  TEST_ASSERT_TRUE(bodyContains("\"Second\""));
}

// An out-of-range id silently falls back to fermenter 0 rather than erroring.
// Deliberate (the WebUI never sends one), but worth pinning so the fallback is
// a decision rather than an accident.
static void test_get_clamps_an_out_of_range_id_to_the_first_fermenter(void) {
  srv.setArg("id", "99");
  strlcpy(g_fermenters[0].fermenterName, "First", sizeof(g_fermenters[0].fermenterName));
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("\"First\""));
}

static void test_get_with_no_id_returns_the_first_fermenter(void) {
  strlcpy(g_fermenters[0].fermenterName, "First", sizeof(g_fermenters[0].fermenterName));
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("\"First\""));
}

// ============================================================
// RESPONSE HELPERS
// ============================================================

static void test_send_ok_envelope(void) {
  sendOk(srv, F("All good"));
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_STRING("{\"status\":\"ok\",\"msg\":\"All good\"}", g_httpResp.body);
}

static void test_send_err_envelope_carries_the_code(void) {
  sendErr(srv, 400, F("Bad thing"));
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_EQUAL_STRING("{\"status\":\"error\",\"msg\":\"Bad thing\"}", g_httpResp.body);
}

static void test_cors_headers_are_set(void) {
  sendCORSHeaders(srv);
  TEST_ASSERT_EQUAL_STRING("*", httpRespHeader("Access-Control-Allow-Origin"));
  TEST_ASSERT_EQUAL_STRING("GET, POST, OPTIONS", httpRespHeader("Access-Control-Allow-Methods"));
  TEST_ASSERT_EQUAL_STRING("Content-Type", httpRespHeader("Access-Control-Allow-Headers"));
}

static void test_parse_json_body_accepts_valid_json(void) {
  JsonDocument doc;
  srv.setBody("{\"a\":1}");
  TEST_ASSERT_TRUE(parseJsonBody(srv, doc));
  TEST_ASSERT_EQUAL_INT(0, g_httpResp.code);   // nothing sent on success
}

static void test_parse_json_body_rejects_garbage(void) {
  JsonDocument doc;
  srv.setBody("not json at all");
  TEST_ASSERT_FALSE(parseJsonBody(srv, doc));
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Invalid JSON"));
}

static void test_get_valid_index_accepts_an_in_range_value(void) {
  JsonDocument doc;
  deserializeJson(doc, "{\"index\":2}");
  TEST_ASSERT_EQUAL_INT(2, getValidIndex(srv, doc, "index", 4, F("bad")));
}

static void test_get_valid_index_rejects_out_of_range_and_absent(void) {
  JsonDocument doc;
  deserializeJson(doc, "{\"index\":9}");
  TEST_ASSERT_EQUAL_INT(-1, getValidIndex(srv, doc, "index", 4, F("bad")));
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);

  httpRespReset();
  JsonDocument empty;
  deserializeJson(empty, "{}");
  TEST_ASSERT_EQUAL_INT(-1, getValidIndex(srv, empty, "index", 4, F("bad")));
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
}

// `doc[key] | -1` is a type-CHECKED read, not a coercion: a numeric string is
// not an integer, so it yields the -1 default rather than 1. Same construct
// that caused the v0.4.4 BrewServiceSend migration bug, pinned here so the
// behaviour is explicit.
static void test_get_valid_index_treats_a_numeric_string_as_absent(void) {
  JsonDocument doc;
  deserializeJson(doc, "{\"index\":\"1\"}");
  TEST_ASSERT_EQUAL_INT(-1, getValidIndex(srv, doc, "index", 4, F("bad")));
}

// ============================================================
// JSON PAYLOAD SHAPES
//
// The keys the WebUI reads and the endpoint-baseline diffs compare. A renamed
// or dropped key breaks the UI silently.
// ============================================================

static void test_fermenter_payload_carries_its_documented_keys(void) {
  JsonDocument doc;
  buildFermenterJson(doc, F0);
  const char* keys[] = {
    "Fermenter", "FermenterName", "BeerName", "CeilingTemp", "FloorTemp",
    "Hysteresis", "OG", "TG", "Power", "TempControl", "Status", "Alarm",
    "AlarmTolerance", "CompressorDelay", "ProfileNo", "ProfileRunning",
    "CurrentStep", "CurrentHour", "LiveTest", "ProfileName", "TotalSteps",
    "SGCalibration", "BrewServices", "YeastName", "BeerTemp", "AmbientTemp",
    "SG", "Attenuation", "EstABV", "TempUnit", "BeerTempSource", "GravitySource"
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    TEST_ASSERT_TRUE_MESSAGE(!doc[keys[i]].isNull(), keys[i]);
  }
}

static void test_fermenter_payload_reports_celsius_by_default(void) {
  JsonDocument doc;
  buildFermenterJson(doc, F0);
  TEST_ASSERT_EQUAL_STRING("C", doc["TempUnit"].as<const char*>());
  TEST_ASSERT_EQUAL_FLOAT(20.0f, doc["CeilingTemp"].as<float>());
}

// Documents an ASYMMETRY in this payload, rather than asserting an ideal.
//
// In Fahrenheit mode the live READINGS are converted (toDisplayTemp on
// BeerTemp/AmbientTemp) but the SETPOINTS are not - CeilingTemp, FloorTemp,
// Hysteresis and AlarmTolerance are emitted as stored, in Celsius. So this
// payload mixes units, and the WebUI renders both verbatim.
//
// The round trip is at least self-consistent: the UI posts the same Celsius
// number back, and handleFermenter validates it against Celsius ranges, so
// nothing is corrupted. It differs from the MQTT command path, which does
// convert display units to Celsius (applyFermenterFieldFromDisplay, v0.4.3).
//
// Pinned as-is so a deliberate change to either half is visible; changing the
// behaviour is a separate decision from the partial-write fix this suite
// accompanies, and would need the WebUI updated in the same commit.
static void test_fermenter_payload_mixes_celsius_setpoints_with_display_readings(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  g_fermenterDebugMode = true;
  g_fermenterDebugOverrides[F0].enabled  = true;
  g_fermenterDebugOverrides[F0].beerTemp = 20.0f;   // stored Celsius

  JsonDocument doc;
  buildFermenterJson(doc, F0);

  TEST_ASSERT_EQUAL_STRING("F", doc["TempUnit"].as<const char*>());
  // reading: converted
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 68.0f, doc["BeerTemp"].as<float>());
  // setpoints: not converted
  TEST_ASSERT_EQUAL_FLOAT(20.0f, doc["CeilingTemp"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(18.0f, doc["FloorTemp"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(0.5f,  doc["Hysteresis"].as<float>());
}

// profileNo 0 means the plain ceiling/floor mode, reported as "Standard" with
// no steps rather than as an empty profile.
static void test_fermenter_payload_names_the_standard_profile(void) {
  g_fermenters[F0].profileNo = 0;
  JsonDocument doc;
  buildFermenterJson(doc, F0);
  TEST_ASSERT_EQUAL_STRING("Standard", doc["ProfileName"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(0, doc["TotalSteps"].as<int>());
}

static void test_fermenter_payload_names_an_assigned_profile_and_counts_steps(void) {
  strlcpy(g_profiles[0].profileName, "Lager", sizeof(g_profiles[0].profileName));
  g_profileSteps[0].stepType  = 1;
  g_profileSteps[0].days      = 3;
  g_profileSteps[0].startTemp = 12.0f;
  g_fermenters[F0].profileNo  = 1;
  JsonDocument doc;
  buildFermenterJson(doc, F0);
  TEST_ASSERT_EQUAL_STRING("Lager", doc["ProfileName"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(1, doc["TotalSteps"].as<int>());
}

static void test_controller_payload_carries_its_documented_keys(void) {
  JsonDocument doc;
  buildControllerJson(doc);
  const char* keys[] = { "ChipId", "FreeHeap", "WiFiSSID", "IP", "RSSI" };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    TEST_ASSERT_TRUE_MESSAGE(!doc[keys[i]].isNull(), keys[i]);
  }
}

static void test_board_info_payload_carries_its_documented_keys(void) {
  JsonDocument doc;
  buildBoardInfoJson(doc);
  const char* keys[] = {
    "chip_id", "flash_size", "free_heap", "sdk_version", "reset_reason"
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    TEST_ASSERT_TRUE_MESSAGE(!doc[keys[i]].isNull(), keys[i]);
  }
}

static void test_profile_payload_lists_every_step_slot(void) {
  strlcpy(g_profiles[1].profileName, "Ale", sizeof(g_profiles[1].profileName));
  JsonDocument doc;
  buildProfileJson(doc, 1);
  TEST_ASSERT_EQUAL_INT(1, doc["index"].as<int>());
  TEST_ASSERT_EQUAL_STRING("Ale", doc["name"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(MAX_STEPS_PER_PROFILE, doc["steps"].as<JsonArray>().size());
}

// Slot addressing: profile p reads from g_profileSteps[p * MAX_STEPS_PER_PROFILE].
// Every payload test using slot 0 would pass even with the offset dropped.
static void test_profile_payload_reads_from_the_right_step_slot(void) {
  uint8_t base = 2 * MAX_STEPS_PER_PROFILE;
  g_profileSteps[base].startTemp = 17.5f;
  JsonDocument doc;
  buildProfileJson(doc, 2);
  TEST_ASSERT_EQUAL_FLOAT(17.5f, doc["steps"][0]["startTemp"].as<float>());
}

// sendJsonDoc streams into the client, so a disconnect between header and body
// must leave the payload empty rather than writing into a dead socket.
static void test_send_json_doc_writes_nothing_when_the_client_is_gone(void) {
  clientTestSetConnected(false);
  JsonDocument doc;
  doc["x"] = 1;
  sendJsonDoc(srv, doc);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_UINT(0, g_httpResp.bodyLen);
}

static void test_send_json_doc_sets_a_content_length(void) {
  JsonDocument doc;
  doc["x"] = 1;
  sendJsonDoc(srv, doc);
  TEST_ASSERT_TRUE(g_httpResp.contentLengthSet);
  TEST_ASSERT_EQUAL_UINT(strlen("{\"x\":1}"), g_httpResp.declaredContentLength);
  TEST_ASSERT_EQUAL_STRING("{\"x\":1}", g_httpResp.body);
}

// ============================================================
// handleFermenterProfile — ACTION DISPATCH
// ============================================================

// A profile with one usable step, which start requires.
static void giveProfileOneStep(uint8_t profileSlot) {
  uint8_t base = profileSlot * MAX_STEPS_PER_PROFILE;
  g_profileSteps[base].stepType  = 1;
  g_profileSteps[base].days      = 5;
  g_profileSteps[base].startTemp = 18.0f;
}

static void test_profile_start_runs_the_requested_profile(void) {
  giveProfileOneStep(0);
  postBody("{\"Fermenter\":0,\"action\":\"start\",\"ProfileIndex\":1}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Profile started"));
  TEST_ASSERT_TRUE(g_fermenters[F0].profileRunning);
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[F0].profileNo);
}

static void test_profile_start_rejects_an_index_outside_one_to_four(void) {
  postBody("{\"Fermenter\":0,\"action\":\"start\",\"ProfileIndex\":0}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Invalid profile"));

  httpRespReset();
  postBody("{\"Fermenter\":0,\"action\":\"start\",\"ProfileIndex\":5}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
}

// Starting an empty profile would leave the fermenter running a profile with
// nothing to do, which never advances and never completes.
static void test_profile_start_rejects_a_profile_with_no_steps(void) {
  postBody("{\"Fermenter\":0,\"action\":\"start\",\"ProfileIndex\":1}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Profile has no steps"));
  TEST_ASSERT_FALSE(g_fermenters[F0].profileRunning);
}

static void test_profile_stop_clears_the_run_state(void) {
  giveProfileOneStep(0);
  startProfile(F0, 1);
  postBody("{\"Fermenter\":0,\"action\":\"stop\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FALSE(g_fermenters[F0].profileRunning);
}

static void test_profile_pause_holds_the_step(void) {
  giveProfileOneStep(0);
  startProfile(F0, 1);
  postBody("{\"Fermenter\":0,\"action\":\"pause\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(g_fermenters[F0].profilePaused);
}

static void test_profile_resume_restarts_a_paused_profile(void) {
  giveProfileOneStep(0);
  startProfile(F0, 1);
  pauseProfile(F0);
  postBody("{\"Fermenter\":0,\"action\":\"resume\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Profile resumed"));
  TEST_ASSERT_TRUE(g_fermenters[F0].profileRunning);
}

static void test_profile_resume_rejected_when_nothing_is_paused(void) {
  g_fermenters[F0].profileNo = 0;
  postBody("{\"Fermenter\":0,\"action\":\"resume\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("No paused profile to resume"));
}

static void test_profile_resume_rejected_when_already_running(void) {
  giveProfileOneStep(0);
  startProfile(F0, 1);
  postBody("{\"Fermenter\":0,\"action\":\"resume\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
}

static void test_profile_next_advances_a_running_profile(void) {
  uint8_t base = 0;
  for (int s = 0; s < 3; s++) {
    g_profileSteps[base + s].stepType  = 1;
    g_profileSteps[base + s].days      = 2;
    g_profileSteps[base + s].startTemp = 18.0f;
  }
  startProfile(F0, 1);
  postBody("{\"Fermenter\":0,\"action\":\"next\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Advanced to next step"));
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[F0].currentStep);
}

static void test_profile_next_on_a_stopped_profile_is_rejected(void) {
  g_fermenters[F0].profileRunning = false;
  g_fermenters[F0].profileNo      = 0;
  postBody("{\"Fermenter\":0,\"action\":\"next\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Profile not running"));
}

static void test_profile_prev_on_the_first_step_reports_success(void) {
  giveProfileOneStep(0);
  startProfile(F0, 1);
  postBody("{\"Fermenter\":0,\"action\":\"prev\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Already on first step"));
}

static void test_profile_unknown_action_is_rejected(void) {
  postBody("{\"Fermenter\":0,\"action\":\"fly\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Unknown action"));
}

static void test_profile_action_needs_a_valid_fermenter(void) {
  postBody("{\"Fermenter\":9,\"action\":\"stop\"}");
  handleFermenterProfile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Invalid fermenter"));
}

// ============================================================
// FILESYSTEM BROWSER
// ============================================================

static void test_fs_files_lists_what_is_on_the_filesystem(void) {
  fsTestWrite("/jsonGlobal.txt", "{\"a\":1}");
  fsTestWrite("/jsonMqtt.txt", "{}");
  handleFsFiles(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("/jsonGlobal.txt"));
  TEST_ASSERT_TRUE(bodyContains("/jsonMqtt.txt"));
  TEST_ASSERT_TRUE(bodyContains("\"size\":7"));
}

// The traversal guard. Without it any file on the device is readable.
static void test_fs_file_rejects_a_dot_dot_path(void) {
  srv.setArg("name", "/../secrets.txt");
  handleFsFile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_FALSE(g_httpResp.fileStreamed);
}

static void test_fs_file_rejects_an_empty_name(void) {
  srv.setArg("name", "");
  handleFsFile(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
}

static void test_fs_file_reports_a_missing_file(void) {
  srv.setArg("name", "/nope.txt");
  handleFsFile(srv);
  TEST_ASSERT_EQUAL_INT(404, g_httpResp.code);
}

static void test_fs_file_streams_an_existing_file(void) {
  fsTestWrite("/jsonMqtt.txt", "{\"host\":\"broker\"}");
  srv.setArg("name", "/jsonMqtt.txt");
  handleFsFile(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(g_httpResp.fileStreamed);
  TEST_ASSERT_EQUAL_STRING("{\"host\":\"broker\"}", g_httpResp.body);
}

// A name without a leading slash is normalised rather than rejected, so the
// WebUI can pass either form.
static void test_fs_file_accepts_a_name_without_a_leading_slash(void) {
  fsTestWrite("/jsonMqtt.txt", "{}");
  srv.setArg("name", "jsonMqtt.txt");
  handleFsFile(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(g_httpResp.fileStreamed);
}

// ---- save ----

static void test_fs_save_rejects_a_path_outside_the_whitelist(void) {
  srv.setArg("name", "/etc/passwd");
  srv.setBody("{}");
  handleFsFileSave(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Not allowed"));
  TEST_ASSERT_FALSE(LittleFS.exists("/etc/passwd"));
}

// The whitelist is exact-match, so a lookalike must not slip through.
static void test_fs_save_rejects_a_lookalike_path(void) {
  srv.setArg("name", "/jsonGlobal.txt.bak");
  srv.setBody("{}");
  handleFsFileSave(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
}

static void test_fs_save_rejects_an_empty_body(void) {
  srv.setArg("name", "/jsonGlobal.txt");
  srv.setBody("");
  handleFsFileSave(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Empty body"));
}

// Invalid JSON must be caught BEFORE the file is opened: this content is read
// back by the config loader at next boot, and a syntax error there costs the
// stored settings.
static void test_fs_save_rejects_invalid_json_without_touching_the_file(void) {
  fsTestWrite("/jsonGlobal.txt", "{\"original\":true}");
  srv.setArg("name", "/jsonGlobal.txt");
  srv.setBody("{\"broken\":");
  handleFsFileSave(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Invalid JSON"));
  TEST_ASSERT_EQUAL_STRING("{\"original\":true}", fsTestRead("/jsonGlobal.txt"));
}

static void test_fs_save_writes_a_whitelisted_file(void) {
  srv.setArg("name", "/jsonSyslog.txt");
  srv.setBody("{\"enabled\":true,\"port\":514}");
  handleFsFileSave(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_STRING("{\"enabled\":true,\"port\":514}", fsTestRead("/jsonSyslog.txt"));
}

static void test_fs_save_reports_a_write_failure(void) {
  srv.setArg("name", "/jsonSyslog.txt");
  srv.setBody("{}");
  fsTestSetFull(true);
  handleFsFileSave(srv);
  TEST_ASSERT_EQUAL_INT(500, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Write failed"));
}

// ============================================================
// handleDebug — runtime-only sensor overrides
// ============================================================

static void test_debug_post_sets_the_global_mode(void) {
  postBody("{\"DebugMode\":true}");
  handleDebug(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(g_fermenterDebugMode);
}

static void test_debug_post_stores_per_fermenter_overrides(void) {
  postBody("{\"Fermenter\":1,\"Enabled\":true,\"BeerTemp\":19.5,"
           "\"AmbientTemp\":21.0,\"SG\":1.030}");
  handleDebug(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(g_fermenterDebugOverrides[1].enabled);
  TEST_ASSERT_EQUAL_FLOAT(19.5f,  g_fermenterDebugOverrides[1].beerTemp);
  TEST_ASSERT_EQUAL_FLOAT(21.0f,  g_fermenterDebugOverrides[1].ambientTemp);
  TEST_ASSERT_EQUAL_FLOAT(1.030f, g_fermenterDebugOverrides[1].sg);
}

// Overrides arrive in the display unit and are stored in Celsius, the same
// contract as the MQTT command path.
static void test_debug_post_converts_temperatures_from_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"Fermenter\":0,\"BeerTemp\":68.0}");
  handleDebug(srv);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 20.0f, g_fermenterDebugOverrides[0].beerTemp);
}

static void test_debug_post_ignores_an_out_of_range_fermenter(void) {
  postBody("{\"Fermenter\":9,\"Enabled\":true}");
  handleDebug(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);   // accepted, but applied nowhere
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    TEST_ASSERT_FALSE(g_fermenterDebugOverrides[i].enabled);
  }
}

static void test_debug_get_reports_mode_unit_and_every_slot(void) {
  g_fermenterDebugMode = true;
  handleDebug(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("\"DebugMode\":true"));
  TEST_ASSERT_TRUE(bodyContains("\"TempUnit\":\"C\""));
  TEST_ASSERT_TRUE(bodyContains("\"Fermenter\":3"));
}

static void test_debug_post_rejects_a_malformed_body(void) {
  postBody("{oops");
  handleDebug(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
}

// ============================================================
// PROBE AND SMART PLUG CONFIG
// ============================================================

static void test_probe_post_requires_a_valid_index(void) {
  postBody("{\"index\":9,\"function\":1}");
  handleProbePost(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Invalid probe index"));
}

// An empty address means the slot holds no discovered probe, so there is
// nothing to configure.
static void test_probe_post_rejects_an_unpopulated_slot(void) {
  postBody("{\"index\":0,\"function\":1}");
  handleProbePost(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Invalid probe index"));
}

static void test_probe_post_updates_a_populated_slot(void) {
  strlcpy(g_probes[0].address, "28FF001122334455", sizeof(g_probes[0].address));
  postBody("{\"index\":0,\"function\":1,\"fermenter\":2,\"tempAdjust\":-0.5}");
  handleProbePost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_UINT8(1, g_probes[0].function);
  TEST_ASSERT_EQUAL_UINT8(2, g_probes[0].fermenter);
  TEST_ASSERT_EQUAL_FLOAT(-0.5f, g_probes[0].tempAdjust);
}

// A fermenter number is either a real slot or the unassigned sentinel; a value
// in between would index past the array in the control loop.
static void test_probe_post_ignores_an_impossible_fermenter_number(void) {
  strlcpy(g_probes[0].address, "28FF001122334455", sizeof(g_probes[0].address));
  g_probes[0].fermenter = PROBE_UNASSIGNED;
  postBody("{\"index\":0,\"fermenter\":50}");
  handleProbePost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[0].fermenter);
}

static void test_smartplug_post_updates_a_slot(void) {
  postBody("{\"index\":0,\"manufacturer\":\"Dial\",\"model\":\"D1\","
           "\"onCode\":1193046,\"offCode\":11259375,\"protocol\":2,"
           "\"bits\":24,\"delay\":427,\"codeset\":1,\"function\":0,\"fermenter\":0}");
  handleSmartPlugPost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_STRING("Dial", g_smartPlugs[0].manufacturer);
  TEST_ASSERT_EQUAL_UINT32(1193046,  g_smartPlugs[0].onCode);
  TEST_ASSERT_EQUAL_UINT32(11259375, g_smartPlugs[0].offCode);
  TEST_ASSERT_EQUAL_UINT8(24, g_smartPlugs[0].bits);
}

// Zero bits would make RCSwitch transmit nothing at all.
static void test_smartplug_post_ignores_an_impossible_bit_count(void) {
  g_smartPlugs[0].bits = 24;
  postBody("{\"index\":0,\"bits\":0}");
  handleSmartPlugPost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_UINT8(24, g_smartPlugs[0].bits);

  httpRespReset();
  postBody("{\"index\":0,\"bits\":33}");
  handleSmartPlugPost(srv);
  TEST_ASSERT_EQUAL_UINT8(24, g_smartPlugs[0].bits);
}

static void test_smartplug_post_requires_a_valid_index(void) {
  postBody("{\"index\":10,\"bits\":24}");
  handleSmartPlugPost(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("Invalid plug index"));
}

static void test_smartplug_test_transmits_the_selected_code(void) {
  g_smartPlugs[0].onCode  = 0x123456;
  g_smartPlugs[0].offCode = 0xABCDEF;
  postBody("{\"index\":0,\"action\":\"on\"}");
  handleSmartPlugTest(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_INT(1, s_rfTransmits);
}

static void test_smartplug_test_refuses_when_no_code_is_configured(void) {
  postBody("{\"index\":0,\"action\":\"on\"}");
  handleSmartPlugTest(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("No code configured"));
  TEST_ASSERT_EQUAL_INT(0, s_rfTransmits);
}

// ============================================================

int main(int, char**) {
  UNITY_BEGIN();

  // the partial-write fix
  RUN_TEST(test_rejected_og_leaves_the_temperature_trio_untouched);
  RUN_TEST(test_rejected_tg_leaves_the_temperature_trio_untouched);
  RUN_TEST(test_rejected_compressor_delay_leaves_the_temperature_trio_untouched);
  RUN_TEST(test_rejected_alarm_tolerance_leaves_everything_untouched);
  RUN_TEST(test_rejected_body_does_not_apply_the_name_fields);
  RUN_TEST(test_rejected_body_does_not_change_power_or_temp_control);
  RUN_TEST(test_rejected_body_is_not_persisted);
  RUN_TEST(test_accepted_body_applies_every_field_and_persists);
  RUN_TEST(test_partial_body_only_touches_the_named_field);
  RUN_TEST(test_only_the_addressed_fermenter_is_modified);

  // holistic trio
  RUN_TEST(test_ceiling_above_the_range_is_rejected);
  RUN_TEST(test_floor_below_the_range_is_rejected);
  RUN_TEST(test_hysteresis_above_the_range_is_rejected);
  RUN_TEST(test_floor_at_or_above_ceiling_is_rejected);
  RUN_TEST(test_safe_zone_narrower_than_twice_hysteresis_is_rejected);
  RUN_TEST(test_lone_hysteresis_is_validated_against_the_stored_span);
  RUN_TEST(test_exactly_twice_hysteresis_is_accepted);

  // index / body validation and GET
  RUN_TEST(test_missing_fermenter_index_is_rejected);
  RUN_TEST(test_out_of_range_fermenter_index_is_rejected);
  RUN_TEST(test_negative_fermenter_index_is_rejected);
  RUN_TEST(test_malformed_json_body_is_rejected);
  RUN_TEST(test_get_returns_the_requested_fermenter);
  RUN_TEST(test_get_clamps_an_out_of_range_id_to_the_first_fermenter);
  RUN_TEST(test_get_with_no_id_returns_the_first_fermenter);

  // helpers
  RUN_TEST(test_send_ok_envelope);
  RUN_TEST(test_send_err_envelope_carries_the_code);
  RUN_TEST(test_cors_headers_are_set);
  RUN_TEST(test_parse_json_body_accepts_valid_json);
  RUN_TEST(test_parse_json_body_rejects_garbage);
  RUN_TEST(test_get_valid_index_accepts_an_in_range_value);
  RUN_TEST(test_get_valid_index_rejects_out_of_range_and_absent);
  RUN_TEST(test_get_valid_index_treats_a_numeric_string_as_absent);

  // payload shapes
  RUN_TEST(test_fermenter_payload_carries_its_documented_keys);
  RUN_TEST(test_fermenter_payload_reports_celsius_by_default);
  RUN_TEST(test_fermenter_payload_mixes_celsius_setpoints_with_display_readings);
  RUN_TEST(test_fermenter_payload_names_the_standard_profile);
  RUN_TEST(test_fermenter_payload_names_an_assigned_profile_and_counts_steps);
  RUN_TEST(test_controller_payload_carries_its_documented_keys);
  RUN_TEST(test_board_info_payload_carries_its_documented_keys);
  RUN_TEST(test_profile_payload_lists_every_step_slot);
  RUN_TEST(test_profile_payload_reads_from_the_right_step_slot);
  RUN_TEST(test_send_json_doc_writes_nothing_when_the_client_is_gone);
  RUN_TEST(test_send_json_doc_sets_a_content_length);

  // profile dispatch
  RUN_TEST(test_profile_start_runs_the_requested_profile);
  RUN_TEST(test_profile_start_rejects_an_index_outside_one_to_four);
  RUN_TEST(test_profile_start_rejects_a_profile_with_no_steps);
  RUN_TEST(test_profile_stop_clears_the_run_state);
  RUN_TEST(test_profile_pause_holds_the_step);
  RUN_TEST(test_profile_resume_restarts_a_paused_profile);
  RUN_TEST(test_profile_resume_rejected_when_nothing_is_paused);
  RUN_TEST(test_profile_resume_rejected_when_already_running);
  RUN_TEST(test_profile_next_advances_a_running_profile);
  RUN_TEST(test_profile_next_on_a_stopped_profile_is_rejected);
  RUN_TEST(test_profile_prev_on_the_first_step_reports_success);
  RUN_TEST(test_profile_unknown_action_is_rejected);
  RUN_TEST(test_profile_action_needs_a_valid_fermenter);

  // filesystem browser
  RUN_TEST(test_fs_files_lists_what_is_on_the_filesystem);
  RUN_TEST(test_fs_file_rejects_a_dot_dot_path);
  RUN_TEST(test_fs_file_rejects_an_empty_name);
  RUN_TEST(test_fs_file_reports_a_missing_file);
  RUN_TEST(test_fs_file_streams_an_existing_file);
  RUN_TEST(test_fs_file_accepts_a_name_without_a_leading_slash);
  RUN_TEST(test_fs_save_rejects_a_path_outside_the_whitelist);
  RUN_TEST(test_fs_save_rejects_a_lookalike_path);
  RUN_TEST(test_fs_save_rejects_an_empty_body);
  RUN_TEST(test_fs_save_rejects_invalid_json_without_touching_the_file);
  RUN_TEST(test_fs_save_writes_a_whitelisted_file);
  RUN_TEST(test_fs_save_reports_a_write_failure);

  // debug overrides
  RUN_TEST(test_debug_post_sets_the_global_mode);
  RUN_TEST(test_debug_post_stores_per_fermenter_overrides);
  RUN_TEST(test_debug_post_converts_temperatures_from_fahrenheit);
  RUN_TEST(test_debug_post_ignores_an_out_of_range_fermenter);
  RUN_TEST(test_debug_get_reports_mode_unit_and_every_slot);
  RUN_TEST(test_debug_post_rejects_a_malformed_body);

  // probe and plug config
  RUN_TEST(test_probe_post_requires_a_valid_index);
  RUN_TEST(test_probe_post_rejects_an_unpopulated_slot);
  RUN_TEST(test_probe_post_updates_a_populated_slot);
  RUN_TEST(test_probe_post_ignores_an_impossible_fermenter_number);
  RUN_TEST(test_smartplug_post_updates_a_slot);
  RUN_TEST(test_smartplug_post_ignores_an_impossible_bit_count);
  RUN_TEST(test_smartplug_post_requires_a_valid_index);
  RUN_TEST(test_smartplug_test_transmits_the_selected_code);
  RUN_TEST(test_smartplug_test_refuses_when_no_code_is_configured);

  return UNITY_END();
}
