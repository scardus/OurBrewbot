// Native (host) tests for the beer/ambient/control temperature priority
// chains and unit conversions in OurBrewbot/Temperatures.cpp.
//
// Temperatures.cpp is #included directly (not linked) so the real,
// unmodified production source is what's under test - see test/stubs/ for
// the minimal Arduino-core stand-ins (extended for this file to cover the
// DallasTemperature/OneWire/String surface Temperatures.cpp references
// elsewhere in the file, even though those other functions aren't
// exercised here), and below for the storage the rest of the firmware
// would normally provide.

#include <unity.h>
#include <cstdint>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
ProbeConfig      g_probes[MAX_PROBES];
TiltConfig       g_tilts[MAX_TILTS];
iSpindelConfig   g_iSpindels[MAX_ISPINDELS];
GlobalConfig     g_globalConfig;
bool g_fermenterDebugMode = false;
FermenterDebugOverride g_fermenterDebugOverrides[MAX_FERMENTERS];

// ---- millis(), settable per test (not used by the functions under test,
// kept for consistency with the other native test files) ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}
bool saveProbeConfig() { return true; }

// The functions under test, plus everything else in Temperatures.cpp.
#include "../../OurBrewbot/Temperatures.cpp"

// ---- test fixture ----

static const uint8_t F = 0;  // fermenter index used by every test

void setUp(void) {
  for (int i = 0; i < MAX_PROBES; i++)    g_probes[i]    = ProbeConfig{};
  for (int i = 0; i < MAX_TILTS; i++)     g_tilts[i]     = TiltConfig{};
  for (int i = 0; i < MAX_ISPINDELS; i++) g_iSpindels[i] = iSpindelConfig{};
  g_fermenterDebugMode = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenterDebugOverrides[i] = FermenterDebugOverride{};
  g_globalConfig = GlobalConfig{};
}

void tearDown(void) {}

// ---- getBeerTemp / getBeerTempSource: Tilt > Probe > iSpindel > None ----

void test_getBeerTemp_none_when_nothing_assigned(void) {
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("None", getBeerTempSource(F));
}

void test_getBeerTemp_uses_ispindel_when_assigned(void) {
  g_iSpindels[0].collectData = true;
  g_iSpindels[0].fermenter   = F;
  g_iSpindels[0].function    = PROBE_FN_BEER;
  g_iSpindels[0].temperature = 19.5f;
  TEST_ASSERT_EQUAL_FLOAT(19.5f, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("iSpindel", getBeerTempSource(F));
}

void test_getBeerTemp_probe_beats_ispindel(void) {
  g_iSpindels[0].collectData = true;
  g_iSpindels[0].fermenter   = F;
  g_iSpindels[0].function    = PROBE_FN_BEER;
  g_iSpindels[0].temperature = 19.5f;

  strlcpy(g_probes[0].address, "0000000000000001", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_BEER;
  g_probes[0].temperature = 20.1f;

  TEST_ASSERT_EQUAL_FLOAT(20.1f, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("Probe", getBeerTempSource(F));
}

void test_getBeerTemp_tilt_beats_probe(void) {
  strlcpy(g_probes[0].address, "0000000000000001", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_BEER;
  g_probes[0].temperature = 20.1f;

  g_tilts[0].active      = true;
  g_tilts[0].fermenter    = F;
  g_tilts[0].function     = PROBE_FN_BEER;
  g_tilts[0].temperature  = 18.7f;

  TEST_ASSERT_EQUAL_FLOAT(18.7f, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("Tilt", getBeerTempSource(F));
}

void test_getBeerTemp_debug_override_beats_everything(void) {
  g_tilts[0].active      = true;
  g_tilts[0].fermenter    = F;
  g_tilts[0].function     = PROBE_FN_BEER;
  g_tilts[0].temperature  = 18.7f;

  g_fermenterDebugMode = true;
  g_fermenterDebugOverrides[F].enabled  = true;
  g_fermenterDebugOverrides[F].beerTemp = 25.0f;

  TEST_ASSERT_EQUAL_FLOAT(25.0f, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("Debug", getBeerTempSource(F));
}

// ---- getAmbientTemp ----

void test_getAmbientTemp_none_when_nothing_assigned(void) {
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getAmbientTemp(F));
}

void test_getAmbientTemp_uses_assigned_probe(void) {
  strlcpy(g_probes[0].address, "0000000000000002", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_AMBIENT;
  g_probes[0].temperature = 4.2f;
  TEST_ASSERT_EQUAL_FLOAT(4.2f, getAmbientTemp(F));
}

void test_getAmbientTemp_debug_override_beats_probe(void) {
  strlcpy(g_probes[0].address, "0000000000000002", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_AMBIENT;
  g_probes[0].temperature = 4.2f;

  g_fermenterDebugMode = true;
  g_fermenterDebugOverrides[F].enabled     = true;
  g_fermenterDebugOverrides[F].ambientTemp = 6.6f;

  TEST_ASSERT_EQUAL_FLOAT(6.6f, getAmbientTemp(F));
}

// ---- getControlTemp: beer preferred, ambient fallback ----

void test_getControlTemp_prefers_beer_when_usable(void) {
  strlcpy(g_probes[0].address, "0000000000000001", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_BEER;
  g_probes[0].temperature = 20.1f;

  strlcpy(g_probes[1].address, "0000000000000002", sizeof(g_probes[1].address));
  g_probes[1].fermenter   = F;
  g_probes[1].function    = PROBE_FN_AMBIENT;
  g_probes[1].temperature = 4.2f;

  TEST_ASSERT_EQUAL_FLOAT(20.1f, getControlTemp(F));
}

void test_getControlTemp_falls_back_to_ambient(void) {
  strlcpy(g_probes[0].address, "0000000000000002", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_AMBIENT;
  g_probes[0].temperature = 4.2f;
  // No beer source assigned - beer resolves to TEMP_NONE.
  TEST_ASSERT_EQUAL_FLOAT(4.2f, getControlTemp(F));
}

// ---- unit conversions ----

void test_toDisplayTemp_identity_in_celsius(void) {
  g_globalConfig.unit = UNIT_CELSIUS;
  TEST_ASSERT_EQUAL_FLOAT(20.0f, toDisplayTemp(20.0f));
}

void test_toDisplayTemp_scales_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_EQUAL_FLOAT(68.0f, toDisplayTemp(20.0f));
}

void test_toCelsius_identity_in_celsius(void) {
  g_globalConfig.unit = UNIT_CELSIUS;
  TEST_ASSERT_EQUAL_FLOAT(20.0f, toCelsius(20.0f));
}

void test_toCelsius_scales_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_EQUAL_FLOAT(20.0f, toCelsius(68.0f));
}

void test_temp_roundtrip_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, toCelsius(toDisplayTemp(20.0f)));
}

void test_tempDelta_scales_without_offset_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_EQUAL_FLOAT(1.8f, toDisplayTempDelta(1.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, toCelsiusTempDelta(1.8f));
}

void test_tempDelta_identity_in_celsius(void) {
  g_globalConfig.unit = UNIT_CELSIUS;
  TEST_ASSERT_EQUAL_FLOAT(1.0f, toDisplayTempDelta(1.0f));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, toCelsiusTempDelta(1.0f));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_getBeerTemp_none_when_nothing_assigned);
  RUN_TEST(test_getBeerTemp_uses_ispindel_when_assigned);
  RUN_TEST(test_getBeerTemp_probe_beats_ispindel);
  RUN_TEST(test_getBeerTemp_tilt_beats_probe);
  RUN_TEST(test_getBeerTemp_debug_override_beats_everything);

  RUN_TEST(test_getAmbientTemp_none_when_nothing_assigned);
  RUN_TEST(test_getAmbientTemp_uses_assigned_probe);
  RUN_TEST(test_getAmbientTemp_debug_override_beats_probe);

  RUN_TEST(test_getControlTemp_prefers_beer_when_usable);
  RUN_TEST(test_getControlTemp_falls_back_to_ambient);

  RUN_TEST(test_toDisplayTemp_identity_in_celsius);
  RUN_TEST(test_toDisplayTemp_scales_in_fahrenheit);
  RUN_TEST(test_toCelsius_identity_in_celsius);
  RUN_TEST(test_toCelsius_scales_in_fahrenheit);
  RUN_TEST(test_temp_roundtrip_in_fahrenheit);
  RUN_TEST(test_tempDelta_scales_without_offset_in_fahrenheit);
  RUN_TEST(test_tempDelta_identity_in_celsius);

  return UNITY_END();
}
