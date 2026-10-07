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

// UpdateCheck.cpp - /controller and /update/check report its status.
#include "../../OurBrewbot/UpdateCheck.h"
UpdateStatus g_updateStatus;
static int s_updateChecks = 0;
bool runUpdateCheck() { s_updateChecks++; return true; }

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

// OurBrewbot.cpp - restartDevice() sends the mDNS goodbye, then restarts.
// Recorded so the reboot endpoints can be checked without restarting anything.
static int  s_restarts       = 0;
static bool s_lastForgetWiFi = false;
void restartDevice(bool forgetWiFi) {
  s_restarts++;
  s_lastForgetWiFi = forgetWiFi;
}
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
#include "../../OurBrewbot/TextSafe.cpp"
// ---- stack depth check: reads the ESP8266 loop stack, nothing to do here ----
void stackCheck(uint8_t, const char*) {}

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
  s_restarts       = 0;
  s_lastForgetWiFi = false;

  memset(&g_updateStatus, 0, sizeof(g_updateStatus));
  s_updateChecks = 0;
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

// A name longer than its 31-byte field used to be cut by strlcpy() part way
// through an accented character, leaving an invalid UTF-8 byte that broke the
// JSON for strict readers (Python, Home Assistant). The whole character is now
// dropped, and any invalid byte that arrives is stored as '?'.
static void test_names_are_stored_as_valid_utf8(void) {
  postBody("{\"Fermenter\":0,"
           "\"FermenterName\":\"Fermenter number one, the Bier\xC3\xA9\","
           "\"BeerName\":\"Ale \xFF\"}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  // 30 bytes, then the 2-byte e acute would need bytes 31-32 - only 31 fit.
  TEST_ASSERT_EQUAL_STRING("Fermenter number one, the Bier", g_fermenters[F0].fermenterName);
  TEST_ASSERT_EQUAL_STRING("Ale ?", g_fermenters[F0].beerName);
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

// The whole payload is in one unit (0.4.6). Up to 0.4.5 the live READINGS were
// converted but the SETPOINTS were emitted as stored, so a Fahrenheit response
// carried BeerTemp 68 next to CeilingTemp 20 - both unlabelled, both meaning
// 20 degC. This test previously pinned that asymmetry; it now pins the fix.
//
// Note which helper each field uses: ceiling and floor are absolute (scale and
// +32 offset), hysteresis is a SPAN (scale only). That distinction is the
// likeliest defect in this area, so it is asserted with explicit numbers.
static void test_fermenter_payload_converts_setpoints_and_readings_alike(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  g_fermenterDebugMode = true;
  g_fermenterDebugOverrides[F0].enabled  = true;
  g_fermenterDebugOverrides[F0].beerTemp = 20.0f;   // stored Celsius

  JsonDocument doc;
  buildFermenterJson(doc, F0);

  TEST_ASSERT_EQUAL_STRING("F", doc["TempUnit"].as<const char*>());
  // reading
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 68.0f, doc["BeerTemp"].as<float>());
  // absolute setpoints: 20 -> 68, 18 -> 64.4
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 68.0f, doc["CeilingTemp"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 64.4f, doc["FloorTemp"].as<float>());
  // spans: 0.5 -> 0.9 and 3.0 -> 5.4, NOT 32.9 and 37.4
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.9f, doc["Hysteresis"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.4f, doc["AlarmTolerance"].as<float>());
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

// ---- worst-case payloads: every buffered handler must send valid JSON ----
//
// Several GET handlers serialize each item into a fixed-size buffer and
// serializeJson() cuts the output off silently when it does not fit - the
// browser then gets invalid JSON and the whole tab fails to load. These tests
// fill every text field to its full size with '"', which JSON escapes to two
// bytes, so each item is as long as it can ever be. If a field or buffer size
// changes and the worst case no longer fits, the matching test fails here
// instead of on someone's device.

// Fill a char array field with '"' to its full length (keeping the NUL).
#define FILL_WORST(field) do {                    \
    memset((field), '"', sizeof(field) - 1);        \
    (field)[sizeof(field) - 1] = '\0';              \
  } while (0)

// The longest text ArduinoJson writes for a float: 13 characters, with sign,
// seven digits and a two-digit negative exponent.
static const float LONG_FLOAT = -3.023374e-11f;

// Parse the response body, failing with the body itself if it is not JSON.
static void parseResponse(JsonDocument& doc) {
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_TRUE_MESSAGE(g_httpResp.bodyLen < HTTP_RESP_MAX_BODY - 1, "body hit the recorder limit");
  DeserializationError err = deserializeJson(doc, g_httpResp.body, g_httpResp.bodyLen);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Ok", err.c_str(), g_httpResp.body);
}

// Every fermenter as long as its JSON can get. Celsius (the setUp default)
// keeps the temperatures as set - converting to Fahrenheit would shorten them.
// The live readings come from the debug overrides. Attenuation and ABV are
// only printed for 1 < SG < OG, so OG and SG get realistic long values rather
// than LONG_FLOAT: four 8-character numbers beat two 13s and two zeros.
static void fillWorstFermenters(void) {
  g_fermenterDebugMode = true;
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    FILL_WORST(g_fermenters[i].fermenterName);
    FILL_WORST(g_fermenters[i].beerName);
    FILL_WORST(g_fermenters[i].yeastName);
    FILL_WORST(g_fermenters[i].bjcp);
    g_fermenters[i].profileNo       = i + 1;   // ProfileName comes from the profile
    g_fermenters[i].status          = STATUS_COOLING;
    g_fermenters[i].compressorDelay = 0xFFFF;
    g_fermenters[i].currentStep     = 0xFF;
    g_fermenters[i].currentHour     = 0xFFFF;
    g_fermenters[i].brewServices    = 0xFF;
    g_fermenters[i].ceilingTemp     = LONG_FLOAT;
    g_fermenters[i].floorTemp       = LONG_FLOAT;
    g_fermenters[i].hysteresis      = LONG_FLOAT;
    g_fermenters[i].alarmTolerance  = LONG_FLOAT;
    g_fermenters[i].sgCalibration   = LONG_FLOAT;
    g_fermenters[i].tg              = LONG_FLOAT;
    g_fermenters[i].og              = 1.0987654f;
    g_fermenterDebugOverrides[i].enabled     = true;
    g_fermenterDebugOverrides[i].beerTemp    = LONG_FLOAT;
    g_fermenterDebugOverrides[i].ambientTemp = LONG_FLOAT;
    g_fermenterDebugOverrides[i].sg          = 1.0123457f;
  }
  for (int p = 0; p < MAX_PROFILES; p++) FILL_WORST(g_profiles[p].profileName);
}

static void test_fermenters_get_sends_worst_case_names_whole(void) {
  fillWorstFermenters();
  // The data above reaches 940 bytes per fermenter, close to the 979-byte
  // ceiling the handler's buffer is sized for. If it drops well below that
  // (a field was shortened or the fill stopped working), this test no longer
  // proves the buffer is big enough - fix the fill, not this number.
  JsonDocument one;
  buildFermenterJson(one, 0);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT(900, measureJson(one));
  handleFermenters(srv);

  JsonDocument doc;
  parseResponse(doc);
  TEST_ASSERT_EQUAL_INT(MAX_FERMENTERS, doc.as<JsonArray>().size());
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    TEST_ASSERT_EQUAL_STRING(g_fermenters[i].fermenterName, doc[i]["FermenterName"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(g_fermenters[i].yeastName,     doc[i]["YeastName"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(g_profiles[i].profileName,     doc[i]["ProfileName"].as<const char*>());
  }
}

static void test_status_get_sends_worst_case_names_whole(void) {
  fillWorstFermenters();
  handleStatus(srv);

  JsonDocument doc;
  parseResponse(doc);
  TEST_ASSERT_EQUAL_INT(MAX_FERMENTERS, doc["fermenters"].as<JsonArray>().size());
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    TEST_ASSERT_EQUAL_STRING(g_fermenters[i].fermenterName, doc["fermenters"][i]["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(g_fermenters[i].beerName,      doc["fermenters"][i]["beer"].as<const char*>());
  }
}

static void test_ispindels_get_sends_worst_case_names_whole(void) {
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    FILL_WORST(g_iSpindels[i].name);
    FILL_WORST(g_iSpindels[i].id);
    FILL_WORST(g_iSpindels[i].gravityUnit);
    g_iSpindels[i].sg          = LONG_FLOAT;
    g_iSpindels[i].temperature = LONG_FLOAT;
    g_iSpindels[i].corrGravity = LONG_FLOAT;
    g_iSpindels[i].velocity    = LONG_FLOAT;
    g_iSpindels[i].sgAdjust    = LONG_FLOAT;
    g_iSpindels[i].tempAdjust  = LONG_FLOAT;
  }
  handleiSpindels(srv);

  JsonDocument doc;
  parseResponse(doc);
  TEST_ASSERT_EQUAL_INT(MAX_ISPINDELS, doc["ispindels"].as<JsonArray>().size());
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    TEST_ASSERT_EQUAL_STRING(g_iSpindels[i].name, doc["ispindels"][i]["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(g_iSpindels[i].id,   doc["ispindels"][i]["id"].as<const char*>());
  }
}

static void test_smartplugs_get_sends_worst_case_names_whole(void) {
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    FILL_WORST(g_smartPlugs[i].manufacturer);
    FILL_WORST(g_smartPlugs[i].model);
    g_smartPlugs[i].onCode  = 0xFFFFFFFF;
    g_smartPlugs[i].offCode = 0xFFFFFFFF;
  }
  handleSmartPlugs(srv);

  JsonDocument doc;
  parseResponse(doc);
  TEST_ASSERT_EQUAL_INT(MAX_SMART_PLUGS, doc["plugs"].as<JsonArray>().size());
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    TEST_ASSERT_EQUAL_STRING(g_smartPlugs[i].manufacturer, doc["plugs"][i]["manufacturer"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(g_smartPlugs[i].model,        doc["plugs"][i]["model"].as<const char*>());
  }
}

static void test_debug_get_sends_worst_case_overrides_whole(void) {
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    g_fermenterDebugOverrides[i].enabled     = true;
    g_fermenterDebugOverrides[i].beerTemp    = LONG_FLOAT;
    g_fermenterDebugOverrides[i].ambientTemp = LONG_FLOAT;
    g_fermenterDebugOverrides[i].sg          = LONG_FLOAT;
  }
  handleDebug(srv);

  JsonDocument doc;
  parseResponse(doc);
  TEST_ASSERT_EQUAL_INT(MAX_FERMENTERS, doc["Overrides"].as<JsonArray>().size());
}

// ---- GET /profiles, streamed as chunks ----

// Fills every step of a profile with values that print long, and gives it the
// longest name the config holds.
static void fillProfileWithLongValues(uint8_t p) {
  strlcpy(g_profiles[p].profileName, "Belgian \"Tripel\" Long Lager 001",
          sizeof(g_profiles[p].profileName));
  uint8_t base = p * MAX_STEPS_PER_PROFILE;
  for (uint8_t s = 0; s < MAX_STEPS_PER_PROFILE; s++) {
    g_profileSteps[base + s].stepType  = 1 + (s % 9);
    g_profileSteps[base + s].startTemp = 12.37f + s;
    g_profileSteps[base + s].endTemp   = 18.73f + s;
    g_profileSteps[base + s].sgTrigger = 1.0125f;
    g_profileSteps[base + s].days      = 10.25f;
    g_profileSteps[base + s].stepNo    = s;
  }
}

// A full profile serializes to well over 1 KB. Before the step-by-step
// streaming, each profile went through a 1 KB buffer and serializeJson cut it
// off silently, so the WebUI got broken JSON and the profile pages failed.
static void test_profiles_get_sends_a_full_profile_whole(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  fillProfileWithLongValues(0);
  handleProfiles(srv);

  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, g_httpResp.body, g_httpResp.bodyLen);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Ok", err.c_str(), g_httpResp.body);

  JsonObject first = doc["profiles"][0];
  TEST_ASSERT_EQUAL_STRING("Belgian \"Tripel\" Long Lager 001", first["name"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(MAX_STEPS_PER_PROFILE, first["steps"].as<JsonArray>().size());
  JsonObject last = first["steps"][MAX_STEPS_PER_PROFILE - 1];
  TEST_ASSERT_EQUAL_INT(1 + ((MAX_STEPS_PER_PROFILE - 1) % 9), last["stepType"].as<int>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, (18.73f + 14) * 1.8f + 32.0f, last["endTemp"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0125f, last["sgTrigger"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.25f, last["days"].as<float>());
}

// Streaming step by step must not change the format: each profile in the body
// is byte-for-byte what serializing the whole profile document gives.
static void test_profiles_get_streams_the_same_bytes_as_one_document(void) {
  fillProfileWithLongValues(2);
  handleProfiles(srv);

  for (int p = 0; p < MAX_PROFILES; p++) {
    JsonDocument doc;
    buildProfileJson(doc, p);
    char expected[2048];
    size_t n = serializeJson(doc, expected, sizeof(expected));
    TEST_ASSERT_TRUE(n < sizeof(expected) - 1);   // not cut off here either
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_httpResp.body, expected), expected);
  }
}

// Every profile and every step type is listed, each profile with all its slots
// and the empty ones still reading as the all-zero sentinel.
static void test_profiles_get_lists_every_profile_and_step_type(void) {
  fillProfileWithLongValues(1);
  handleProfiles(srv);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, g_httpResp.body, g_httpResp.bodyLen);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Ok", err.c_str(), g_httpResp.body);
  TEST_ASSERT_EQUAL_INT(MAX_PROFILES, doc["profiles"].as<JsonArray>().size());
  for (int p = 0; p < MAX_PROFILES; p++) {
    TEST_ASSERT_EQUAL_INT(p, doc["profiles"][p]["index"].as<int>());
    TEST_ASSERT_EQUAL_INT(MAX_STEPS_PER_PROFILE, doc["profiles"][p]["steps"].as<JsonArray>().size());
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.37f, doc["profiles"][1]["steps"][0]["startTemp"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, doc["profiles"][2]["steps"][0]["startTemp"].as<float>());
  TEST_ASSERT_EQUAL_INT(10, doc["stepTypes"].as<JsonArray>().size());
  TEST_ASSERT_EQUAL_STRING("Time and Attn% Step", doc["stepTypes"][9]["name"].as<const char*>());
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

// ============================================================
// handleMqttConfigPost - base topic checks
// ============================================================

static void seedMqttConfig(void) {
  g_mqttConfig.enabled = false;
  g_mqttConfig.port    = 1883;
  strlcpy(g_mqttConfig.host,      "old.host",   sizeof(g_mqttConfig.host));
  strlcpy(g_mqttConfig.baseTopic, "ourbrewbot", sizeof(g_mqttConfig.baseTopic));
}

// The fields before the bad one in the body must not have been applied.
static void assertMqttConfigUnchanged(void) {
  TEST_ASSERT_FALSE(g_mqttConfig.enabled);
  TEST_ASSERT_EQUAL_UINT16(1883, g_mqttConfig.port);
  TEST_ASSERT_EQUAL_STRING("old.host",   g_mqttConfig.host);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot", g_mqttConfig.baseTopic);
}

static void test_mqtt_post_rejects_an_unusable_base_topic_and_changes_nothing(void) {
  // A wildcard makes the broker drop the connection on every publish, '$' is
  // the broker's own, and a topic too long for its field used to be cut short,
  // quietly moving every topic. None of them is saved, and neither is anything
  // else in the same body.
  const char* bad[] = {
    "", "brew/+", "brew/#", "$SYS", "brew\tbot", "brew\xFF",
    "abcdefghijklmnopqrstuvwxyz123456",   // 32 bytes - one too many
  };
  for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
    seedMqttConfig();
    JsonDocument body;
    body["enabled"]   = true;
    body["host"]      = "new.host";
    body["port"]      = 1884;
    body["baseTopic"] = bad[k];
    static char json[256];
    serializeJson(body, json, sizeof(json));
    httpRespReset();
    postBody(json);
    handleMqttConfigPost(srv);

    TEST_ASSERT_EQUAL_INT_MESSAGE(400, g_httpResp.code, bad[k]);
    TEST_ASSERT_TRUE_MESSAGE(bodyContains("Base topic"), bad[k]);
    assertMqttConfigUnchanged();
  }
}

static void test_mqtt_post_rejected_port_changes_nothing(void) {
  // The port was always checked, but only after enabled and host had already
  // been applied to the live config.
  seedMqttConfig();
  postBody("{\"enabled\":true,\"host\":\"new.host\",\"port\":70000}");
  handleMqttConfigPost(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  assertMqttConfigUnchanged();
}

static void test_mqtt_post_accepts_a_base_topic_with_levels_and_spaces(void) {
  seedMqttConfig();
  postBody("{\"baseTopic\":\"home/brew bot/12345678901234567\"}");   // 31 bytes
  handleMqttConfigPost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_STRING("home/brew bot/12345678901234567", g_mqttConfig.baseTopic);
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
// RESTARTS GO THROUGH restartDevice() (0.4.14)
//
// A bare ESP.restart() skips the mDNS goodbye, leaving phones and PCs holding
// the old .local record for up to two minutes.
// ============================================================

static void test_reboot_restarts_through_restart_device(void) {
  handleReboot(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_INT(1, s_restarts);
  TEST_ASSERT_FALSE(s_lastForgetWiFi);
}

// The goodbye needs a working connection, so forgetting WiFi has to be left to
// restartDevice() to do after it - not done by the handler beforehand.
static void test_wifi_reset_leaves_forgetting_wifi_until_after_the_goodbye(void) {
  handleWiFiReset(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_INT(1, s_restarts);
  TEST_ASSERT_TRUE(s_lastForgetWiFi);
}

// ============================================================
// DISPLAY UNITS ACROSS THE REST BOUNDARY (0.4.6)
//
// One rule: every temperature on the wire is in the user's display unit, and
// stored config stays Celsius. Two distinctions carry all the risk.
//
//   ABSOLUTE (ceiling, floor, step temps) scales AND offsets: 20 C = 68 F.
//   SPAN     (hysteresis, alarm tolerance, tempAdjust) scales ONLY: 0.5 C
//            of hysteresis is 0.9 F, not 32.9 F.
//
// The property that actually matters is the ROUND TRIP - read a payload, post
// the same numbers back, and the stored Celsius must not have moved. Convert
// one direction and not the other and every save drifts the setpoints.
// ============================================================

// Deserialize whatever the handler just streamed to the client.
static void respJson(JsonDocument& doc) {
  TEST_ASSERT_TRUE(deserializeJson(doc, g_httpResp.body) == DeserializationError::Ok);
}

// Post a document back verbatim - the other half of a round trip.
static void postJson(JsonDocument& doc) {
  static char buf[2048];
  serializeJson(doc, buf, sizeof(buf));
  postBody(buf);
}

// ---- fermenter setpoints ----

// THE round-trip property. Read the fermenter in Fahrenheit, post those exact
// numbers back, and the stored Celsius must be unchanged.
static void test_fermenter_setpoints_round_trip_through_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;

  JsonDocument out;
  buildFermenterJson(out, F0);

  JsonDocument in;
  in["Fermenter"]      = F0;
  in["CeilingTemp"]    = out["CeilingTemp"];
  in["FloorTemp"]      = out["FloorTemp"];
  in["Hysteresis"]     = out["Hysteresis"];
  in["AlarmTolerance"] = out["AlarmTolerance"];
  postJson(in);
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f, g_fermenters[F0].floorTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f,  g_fermenters[F0].hysteresis);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f,  g_fermenters[F0].alarmTolerance);
}

static void test_fermenter_post_converts_the_trio_from_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"Fermenter\":0,\"CeilingTemp\":77,\"FloorTemp\":69.8,\"Hysteresis\":1.8}");
  handleFermenter(srv);

  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.0f, g_fermenters[F0].floorTemp);
  // 1.8 F is a SPAN: 1.0 C. Run through toCelsius() instead it would be
  // -16.8 C, which the 0-10 range check would have rejected outright.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, g_fermenters[F0].hysteresis);
}

static void test_fermenter_post_converts_alarm_tolerance_as_a_span(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"Fermenter\":0,\"AlarmTolerance\":5.4}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, g_fermenters[F0].alarmTolerance);
}

// Conversion must happen BEFORE validation: the range checks are Celsius, so a
// Fahrenheit number checked against them would reject valid setpoints.
// 122 F is exactly the 50 C ceiling limit.
static void test_fahrenheit_ceiling_at_the_celsius_limit_is_accepted(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"Fermenter\":0,\"CeilingTemp\":122,\"FloorTemp\":118.4}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, g_fermenters[F0].ceilingTemp);
}

// 124 F is about 51.1 C - past the same limit, and still rejected.
static void test_fahrenheit_ceiling_above_the_celsius_limit_is_rejected(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"Fermenter\":0,\"CeilingTemp\":124,\"FloorTemp\":118.4}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("ceiling temperature out of range"));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g_fermenters[F0].ceilingTemp);   // untouched
}

// The safe-zone rule (span >= 2 * hysteresis) also has to be applied in
// Celsius: 68/64.4 F is a 2 C span, which 1 C of hysteresis exactly fills.
static void test_fahrenheit_safe_zone_rule_is_applied_in_celsius(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"Fermenter\":0,\"CeilingTemp\":68,\"FloorTemp\":64.4,\"Hysteresis\":1.8}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);

  httpRespReset();
  // 2.7 F = 1.5 C of hysteresis needs 3 C of span, and there is only 2.
  postBody("{\"Fermenter\":0,\"CeilingTemp\":68,\"FloorTemp\":64.4,\"Hysteresis\":2.7}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(400, g_httpResp.code);
  TEST_ASSERT_TRUE(bodyContains("safe zone"));
}

// In Celsius every conversion is the identity, so nothing about the existing
// behaviour moves. This is what the on-device endpoint diff relies on.
static void test_celsius_mode_stores_setpoints_verbatim(void) {
  postBody("{\"Fermenter\":0,\"CeilingTemp\":22.5,\"FloorTemp\":19,"
           "\"Hysteresis\":0.4,\"AlarmTolerance\":2.5}");
  handleFermenter(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_FLOAT(22.5f, g_fermenters[F0].ceilingTemp);
  TEST_ASSERT_EQUAL_FLOAT(19.0f, g_fermenters[F0].floorTemp);
  TEST_ASSERT_EQUAL_FLOAT(0.4f,  g_fermenters[F0].hysteresis);
  TEST_ASSERT_EQUAL_FLOAT(2.5f,  g_fermenters[F0].alarmTolerance);
}

// ---- profile steps, and the empty-slot sentinel ----

static void test_profile_payload_converts_live_step_temperatures(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  g_profileSteps[0].stepType  = 1;
  g_profileSteps[0].days      = 3;
  g_profileSteps[0].startTemp = 12.0f;
  g_profileSteps[0].endTemp   = 20.0f;

  JsonDocument doc;
  buildProfileJson(doc, 0);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 53.6f, doc["steps"][0]["startTemp"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 68.0f, doc["steps"][0]["endTemp"].as<float>());
}

// An unused slot is all-zero, and that zero is a SENTINEL, not a temperature.
// Converted it would read 32 in Fahrenheit and the slot would stop looking
// empty - countProfileSteps() and the WebUI both test it against zero.
static void test_profile_payload_keeps_the_empty_step_sentinel_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  g_profileSteps[0].stepType  = 1;
  g_profileSteps[0].days      = 3;
  g_profileSteps[0].endTemp   = 20.0f;

  JsonDocument doc;
  buildProfileJson(doc, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, doc["steps"][1]["startTemp"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, doc["steps"][1]["endTemp"].as<float>());
}

static void test_profile_post_converts_live_step_temperatures_to_celsius(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"index\":0,\"name\":\"Ale\",\"steps\":["
           "{\"stepType\":1,\"startTemp\":53.6,\"endTemp\":68,\"sgTrigger\":0,\"days\":3}]}");
  handleProfilePost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.0f, g_profileSteps[0].startTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, g_profileSteps[0].endTemp);
}

// The WebUI pads unused slots with a literal all-zero step. Converting that
// padding would store -17.8 C, the slot would no longer count as empty, and a
// running profile would never reach its end.
static void test_profile_post_keeps_a_padded_blank_step_empty(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"index\":0,\"steps\":["
           "{\"stepType\":1,\"startTemp\":53.6,\"endTemp\":68,\"sgTrigger\":0,\"days\":3},"
           "{\"stepType\":0,\"startTemp\":0,\"endTemp\":0,\"sgTrigger\":0,\"days\":0}]}");
  handleProfilePost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_profileSteps[1].startTemp);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_profileSteps[1].endTemp);
  TEST_ASSERT_EQUAL_UINT8(1, countProfileSteps(0));
}

// The same guard, exercised through a full read-modify-write of every slot -
// the shape the WebUI's Save button actually produces.
static void test_profile_round_trips_through_fahrenheit_without_growing(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  for (uint8_t s = 0; s < 3; s++) {
    g_profileSteps[s].stepType  = 1;
    g_profileSteps[s].days      = 4;
    g_profileSteps[s].startTemp = 12.0f + s;
    g_profileSteps[s].endTemp   = 18.0f + s;
  }
  TEST_ASSERT_EQUAL_UINT8(3, countProfileSteps(0));

  JsonDocument out;
  buildProfileJson(out, 0);
  postJson(out);
  handleProfilePost(srv);

  TEST_ASSERT_EQUAL_INT_MESSAGE(200, g_httpResp.code, g_httpResp.body);
  TEST_ASSERT_EQUAL_UINT8(3, countProfileSteps(0));      // not 15
  for (uint8_t s = 0; s < 3; s++) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.0f + s, g_profileSteps[s].startTemp);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f + s, g_profileSteps[s].endTemp);
  }
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_profileSteps[3].startTemp);
}

static void test_profile_post_in_celsius_stores_step_temperatures_verbatim(void) {
  postBody("{\"index\":0,\"steps\":["
           "{\"stepType\":1,\"startTemp\":12,\"endTemp\":20,\"sgTrigger\":0,\"days\":3}]}");
  handleProfilePost(srv);
  TEST_ASSERT_EQUAL_FLOAT(12.0f, g_profileSteps[0].startTemp);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g_profileSteps[0].endTemp);
}

// ---- calibration offsets (probe / tilt / iSpindel tempAdjust) ----

// A calibration offset is a span: 0.5 C of correction is 0.9 F of correction.
static void test_probe_payload_converts_temp_adjust_as_a_span(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  strlcpy(g_probes[0].address, "28FF001122334455", sizeof(g_probes[0].address));
  g_probes[0].tempAdjust = 0.5f;

  handleProbes(srv);
  JsonDocument doc;
  respJson(doc);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.9f, doc["probes"][0]["tempAdjust"].as<float>());
}

static void test_probe_post_converts_temp_adjust_from_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  strlcpy(g_probes[0].address, "28FF001122334455", sizeof(g_probes[0].address));
  postBody("{\"index\":0,\"tempAdjust\":-0.9}");
  handleProbePost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.5f, g_probes[0].tempAdjust);
}

static void test_probe_temp_adjust_round_trips_through_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  strlcpy(g_probes[0].address, "28FF001122334455", sizeof(g_probes[0].address));
  g_probes[0].tempAdjust = 1.25f;

  handleProbes(srv);
  JsonDocument out;
  respJson(out);

  httpRespReset();
  JsonDocument in;
  in["index"]      = 0;
  in["tempAdjust"] = out["probes"][0]["tempAdjust"];
  postJson(in);
  handleProbePost(srv);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.25f, g_probes[0].tempAdjust);
}

static void test_tilt_payload_converts_temp_adjust_as_a_span(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  g_tilts[0].colour     = 0;
  g_tilts[0].tempAdjust = 0.5f;

  handleTilts(srv);
  JsonDocument doc;
  respJson(doc);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.9f, doc["tilts"][0]["tempAdjust"].as<float>());
}

static void test_tilt_post_converts_temp_adjust_from_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"colour\":0,\"tempAdjust\":1.8}");
  handleTiltPost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, g_tilts[0].tempAdjust);
}

// The clear path posts a literal zero offset, and zero converts to zero under
// the span helpers - so clearing a slot works identically in either unit.
static void test_clearing_a_tilt_slot_zeroes_the_offset_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  g_tilts[0].tempAdjust = 2.0f;
  postBody("{\"colour\":0,\"_clear\":true}");
  handleTiltPost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_tilts[0].tempAdjust);
}

static void test_ispindel_payload_converts_temp_adjust_as_a_span(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  g_iSpindels[0].tempAdjust = 0.5f;

  handleiSpindels(srv);
  JsonDocument doc;
  respJson(doc);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.9f, doc["ispindels"][0]["tempAdjust"].as<float>());
}

static void test_ispindel_config_post_converts_temp_adjust_from_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  postBody("{\"index\":0,\"tempAdjust\":-1.8}");
  handleiSpindelConfigPost(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, g_iSpindels[0].tempAdjust);
}

static void test_celsius_mode_leaves_temp_adjust_untouched(void) {
  strlcpy(g_probes[0].address, "28FF001122334455", sizeof(g_probes[0].address));
  postBody("{\"index\":0,\"tempAdjust\":0.75}");
  handleProbePost(srv);
  TEST_ASSERT_EQUAL_FLOAT(0.75f, g_probes[0].tempAdjust);

  g_probes[0].tempAdjust = -0.25f;
  httpRespReset();
  handleProbes(srv);
  JsonDocument doc;
  respJson(doc);
  TEST_ASSERT_EQUAL_FLOAT(-0.25f, doc["probes"][0]["tempAdjust"].as<float>());
}

// ============================================================

// ============================================================
// Firmware update check - /controller fields, the UpdateCheck setting and
// POST /update/check
// ============================================================

static void test_controller_reports_the_update_status(void) {
  g_globalConfig.updateCheck = true;
  g_updateStatus.checked = true;
  g_updateStatus.updateAvailable = true;
  strlcpy(g_updateStatus.latestVersion, "0.4.17", sizeof(g_updateStatus.latestVersion));
  strlcpy(g_updateStatus.notesUrl, "https://example.com/notes", sizeof(g_updateStatus.notesUrl));
  g_updateStatus.attempted = true;
  g_updateStatus.lastCheckMs = s_millis;
  s_millis += 5 * 60000UL;   // checked 5 minutes ago

  handleController(srv);
  JsonDocument doc;
  respJson(doc);
  TEST_ASSERT_TRUE(doc["UpdateCheck"].as<bool>());
  TEST_ASSERT_TRUE(doc["UpdateChecked"].as<bool>());
  TEST_ASSERT_TRUE(doc["UpdateAvailable"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("0.4.17", doc["LatestVersion"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("https://example.com/notes", doc["UpdateNotesUrl"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("", doc["UpdateCheckError"].as<const char*>());
  TEST_ASSERT_EQUAL_INT(5, doc["LastUpdateCheck"].as<int>());
}

static void test_controller_last_check_is_minus_one_before_any_attempt(void) {
  handleController(srv);
  JsonDocument doc;
  respJson(doc);
  TEST_ASSERT_EQUAL_INT(-1, doc["LastUpdateCheck"].as<int>());
  TEST_ASSERT_FALSE(doc["UpdateChecked"].as<bool>());
}

static void test_controller_post_saves_the_update_check_switch(void) {
  g_globalConfig.updateCheck = true;
  postBody("{\"UpdateCheck\":false}");
  handleController(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FALSE(g_globalConfig.updateCheck);

  // Leaving the field out keeps the current value
  postBody("{\"Unit\":1}");
  handleController(srv);
  TEST_ASSERT_FALSE(g_globalConfig.updateCheck);
}

static void test_controller_saves_and_reports_the_crash_reports_switch(void) {
  g_globalConfig.crashReports = true;
  postBody("{\"CrashReports\":false}");
  handleController(srv);
  TEST_ASSERT_EQUAL_INT(200, g_httpResp.code);
  TEST_ASSERT_FALSE(g_globalConfig.crashReports);

  httpRespReset();   // the recorder appends - start the GET with an empty body
  srv.setMethod(HTTP_GET);
  handleController(srv);
  JsonDocument doc;
  respJson(doc);
  TEST_ASSERT_TRUE(doc["CrashReports"].is<bool>());
  TEST_ASSERT_FALSE(doc["CrashReports"].as<bool>());
}

static void test_update_check_endpoint_runs_a_check_and_returns_the_result(void) {
  g_updateStatus.checked = true;
  strlcpy(g_updateStatus.latestVersion, "0.4.16", sizeof(g_updateStatus.latestVersion));
  postBody("");
  srv.setUri("/update/check");
  handleUpdateCheck(srv);
  TEST_ASSERT_EQUAL_INT(1, s_updateChecks);
  JsonDocument doc;
  respJson(doc);
  TEST_ASSERT_EQUAL_STRING("0.4.16", doc["LatestVersion"].as<const char*>());
  TEST_ASSERT_FALSE(doc["UpdateAvailable"].as<bool>());
}

int main(int, char**) {
  UNITY_BEGIN();

  // the partial-write fix
  RUN_TEST(test_rejected_og_leaves_the_temperature_trio_untouched);
  RUN_TEST(test_rejected_tg_leaves_the_temperature_trio_untouched);
  RUN_TEST(test_rejected_compressor_delay_leaves_the_temperature_trio_untouched);
  RUN_TEST(test_rejected_alarm_tolerance_leaves_everything_untouched);
  RUN_TEST(test_rejected_body_does_not_apply_the_name_fields);
  RUN_TEST(test_names_are_stored_as_valid_utf8);
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
  RUN_TEST(test_fermenter_payload_converts_setpoints_and_readings_alike);
  RUN_TEST(test_fermenter_payload_names_the_standard_profile);
  RUN_TEST(test_fermenter_payload_names_an_assigned_profile_and_counts_steps);
  RUN_TEST(test_controller_payload_carries_its_documented_keys);
  RUN_TEST(test_board_info_payload_carries_its_documented_keys);
  RUN_TEST(test_profile_payload_lists_every_step_slot);
  RUN_TEST(test_profile_payload_reads_from_the_right_step_slot);
  RUN_TEST(test_fermenters_get_sends_worst_case_names_whole);
  RUN_TEST(test_status_get_sends_worst_case_names_whole);
  RUN_TEST(test_ispindels_get_sends_worst_case_names_whole);
  RUN_TEST(test_smartplugs_get_sends_worst_case_names_whole);
  RUN_TEST(test_debug_get_sends_worst_case_overrides_whole);
  RUN_TEST(test_profiles_get_sends_a_full_profile_whole);
  RUN_TEST(test_profiles_get_streams_the_same_bytes_as_one_document);
  RUN_TEST(test_profiles_get_lists_every_profile_and_step_type);
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
  RUN_TEST(test_mqtt_post_rejects_an_unusable_base_topic_and_changes_nothing);
  RUN_TEST(test_mqtt_post_rejected_port_changes_nothing);
  RUN_TEST(test_mqtt_post_accepts_a_base_topic_with_levels_and_spaces);
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

  // Restarts go through restartDevice() (0.4.14)
  RUN_TEST(test_reboot_restarts_through_restart_device);
  RUN_TEST(test_wifi_reset_leaves_forgetting_wifi_until_after_the_goodbye);

  // display units across the REST boundary
  RUN_TEST(test_fermenter_setpoints_round_trip_through_fahrenheit);
  RUN_TEST(test_fermenter_post_converts_the_trio_from_fahrenheit);
  RUN_TEST(test_fermenter_post_converts_alarm_tolerance_as_a_span);
  RUN_TEST(test_fahrenheit_ceiling_at_the_celsius_limit_is_accepted);
  RUN_TEST(test_fahrenheit_ceiling_above_the_celsius_limit_is_rejected);
  RUN_TEST(test_fahrenheit_safe_zone_rule_is_applied_in_celsius);
  RUN_TEST(test_celsius_mode_stores_setpoints_verbatim);
  RUN_TEST(test_profile_payload_converts_live_step_temperatures);
  RUN_TEST(test_profile_payload_keeps_the_empty_step_sentinel_in_fahrenheit);
  RUN_TEST(test_profile_post_converts_live_step_temperatures_to_celsius);
  RUN_TEST(test_profile_post_keeps_a_padded_blank_step_empty);
  RUN_TEST(test_profile_round_trips_through_fahrenheit_without_growing);
  RUN_TEST(test_profile_post_in_celsius_stores_step_temperatures_verbatim);
  RUN_TEST(test_probe_payload_converts_temp_adjust_as_a_span);
  RUN_TEST(test_probe_post_converts_temp_adjust_from_fahrenheit);
  RUN_TEST(test_probe_temp_adjust_round_trips_through_fahrenheit);
  RUN_TEST(test_tilt_payload_converts_temp_adjust_as_a_span);
  RUN_TEST(test_tilt_post_converts_temp_adjust_from_fahrenheit);
  RUN_TEST(test_clearing_a_tilt_slot_zeroes_the_offset_in_fahrenheit);
  RUN_TEST(test_ispindel_payload_converts_temp_adjust_as_a_span);
  RUN_TEST(test_ispindel_config_post_converts_temp_adjust_from_fahrenheit);
  RUN_TEST(test_celsius_mode_leaves_temp_adjust_untouched);

  RUN_TEST(test_controller_reports_the_update_status);
  RUN_TEST(test_controller_last_check_is_minus_one_before_any_attempt);
  RUN_TEST(test_controller_post_saves_the_update_check_switch);
  RUN_TEST(test_update_check_endpoint_runs_a_check_and_returns_the_result);
  RUN_TEST(test_controller_saves_and_reports_the_crash_reports_switch);

  return UNITY_END();
}
