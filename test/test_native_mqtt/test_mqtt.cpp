// Native (host) tests for the pieces of mqttMessageCallback() in
// OurBrewbot/Mqtt.cpp that were extracted so they could be tested:
// parseMqttCommandTopic()/fermenterIndexFromScope() (topic parsing, in
// MqttParse.cpp) and applyFermenterFieldFromDisplay() (the display-unit ->
// Celsius conversion, validation and store, in Fermenter.cpp).
//
// This suite does NOT #include Mqtt.cpp itself - it pulls in
// ESP8266WiFi.h/PubSubClient.h and instantiates a WiFiClient/PubSubClient at
// file scope, none of which are stubbed (or worth stubbing just for this).
// That's exactly why both pieces were extracted into files that carry no such
// dependency.
//
// Temperatures.cpp is compiled in for real rather than stubbed, because the
// conversion it provides (toCelsius/toCelsiusTempDelta) is the thing under
// test here: a Fahrenheit setpoint arriving from Home Assistant must not be
// stored as Celsius.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../OurBrewbot/MqttParse.cpp"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
FermenterConfig g_fermenters[MAX_FERMENTERS];
SmartPlugConfig g_smartPlugs[MAX_SMART_PLUGS];
ProbeConfig     g_probes[MAX_PROBES];
TiltConfig      g_tilts[MAX_TILTS];
iSpindelConfig  g_iSpindels[MAX_ISPINDELS];
GlobalConfig    g_globalConfig;
bool g_fermenterDebugMode = false;
FermenterDebugOverride g_fermenterDebugOverrides[MAX_FERMENTERS];

// ---- millis(), unused here but referenced by the included sources ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}
bool saveFermenterConfig() { return true; }
bool saveProbeConfig()     { return true; }
void smartPlugSwitch(uint8_t, bool) {}
void processProfiles() {}

// The real conversions, then the function under test.
#include "../../OurBrewbot/Temperatures.cpp"
#include "../../OurBrewbot/Fermenter.cpp"

static const uint8_t F = 0;

void setUp(void) {
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenters[i] = FermenterConfig{};
  g_globalConfig = GlobalConfig{};
  g_globalConfig.unit = UNIT_CELSIUS;
  // A valid starting band, so a single-field write isn't rejected by the
  // cross-field checks in validateFermenterField().
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  g_fermenters[F].hysteresis  = 0.5f;
}

void tearDown(void) {}

// ---- parseMqttCommandTopic ----

void test_parse_valid_fermenter_topic(void) {
  char scope[16], key[32];
  TEST_ASSERT_TRUE(parseMqttCommandTopic("brewbot/Fermenter0/power/set", "brewbot",
                                          scope, sizeof(scope), key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING("Fermenter0", scope);
  TEST_ASSERT_EQUAL_STRING("power", key);
}

void test_parse_valid_device_topic(void) {
  char scope[16], key[32];
  TEST_ASSERT_TRUE(parseMqttCommandTopic("brewbot/Device/reboot/set", "brewbot",
                                          scope, sizeof(scope), key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING("Device", scope);
  TEST_ASSERT_EQUAL_STRING("reboot", key);
}

void test_parse_rejects_wrong_base_prefix(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("other/Fermenter0/power/set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_missing_slash_after_base(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbotX/Fermenter0/power/set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_missing_scope_segment(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/Fermenter0", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_empty_scope(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot//power/set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_scope_too_long(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/ThisScopeIsWayTooLong/power/set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_missing_set_suffix(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/Fermenter0/power", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_wrong_suffix(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/Fermenter0/power/get", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_empty_key(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/Fermenter0//set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_key_too_long(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic(
      "brewbot/Fermenter0/this_key_name_is_far_too_long_to_fit/set", "brewbot",
      scope, sizeof(scope), key, sizeof(key)));
}

// ---- fermenterIndexFromScope ----

void test_index_first_fermenter(void) {
  TEST_ASSERT_EQUAL_INT(0, fermenterIndexFromScope("Fermenter0"));
}

void test_index_last_valid_fermenter(void) {
  // MAX_FERMENTERS is 4, so valid indices are 0-3.
  TEST_ASSERT_EQUAL_INT(3, fermenterIndexFromScope("Fermenter3"));
}

void test_index_rejects_out_of_range_at_boundary(void) {
  TEST_ASSERT_EQUAL_INT(-1, fermenterIndexFromScope("Fermenter4"));
}

void test_index_rejects_far_out_of_range(void) {
  TEST_ASSERT_EQUAL_INT(-1, fermenterIndexFromScope("Fermenter99"));
}

void test_index_rejects_non_fermenter_scope(void) {
  TEST_ASSERT_EQUAL_INT(-1, fermenterIndexFromScope("Device"));
}

void test_index_rejects_wrong_case(void) {
  TEST_ASSERT_EQUAL_INT(-1, fermenterIndexFromScope("fermenter0"));
}

void test_index_nonnumeric_suffix_is_preexisting_atoi_quirk(void) {
  // atoi() on a non-numeric string returns 0, not an error - this was true of
  // the inline code before extraction too. Documenting current behaviour, not
  // asserting it's correct: "FermenterX" is treated the same as "Fermenter0".
  TEST_ASSERT_EQUAL_INT(0, fermenterIndexFromScope("FermenterX"));
}

void test_index_no_digits_is_preexisting_atoi_quirk(void) {
  // Same quirk: atoi("") also returns 0.
  TEST_ASSERT_EQUAL_INT(0, fermenterIndexFromScope("Fermenter"));
}

// ---- applyFermenterFieldFromDisplay ----
// Setpoints arrive from Home Assistant in the configured display unit, so this
// is where a Fahrenheit "72" becomes 22.2 C. Getting it wrong sets the
// fermenter ~50 degrees out, which is why every case below checks the STORED
// Celsius value, not just the return code.

void test_apply_celsius_is_a_pass_through(void) {
  const char* err = nullptr;
  TEST_ASSERT_TRUE(applyFermenterFieldFromDisplay(F, "ceiling_temperature", 24.0f, &err));
  TEST_ASSERT_NULL(err);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 24.0f, g_fermenters[F].ceilingTemp);
}

void test_apply_fahrenheit_converts_absolute_temperatures(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_TRUE(applyFermenterFieldFromDisplay(F, "ceiling_temperature", 75.2f, nullptr));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 24.0f, g_fermenters[F].ceilingTemp);   // NOT 75.2
  TEST_ASSERT_TRUE(applyFermenterFieldFromDisplay(F, "floor_temperature", 64.4f, nullptr));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f, g_fermenters[F].floorTemp);     // NOT 64.4
}

void test_apply_fahrenheit_converts_hysteresis_as_a_span(void) {
  // Hysteresis is a difference, so it scales without the +32 offset. Treating
  // it as an absolute temperature would turn 1.8 into -16.8 C and get it
  // rejected as out of range rather than stored.
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_TRUE(applyFermenterFieldFromDisplay(F, "hysteresis", 1.8f, nullptr));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, g_fermenters[F].hysteresis);
}

void test_apply_validates_after_converting(void) {
  // -20 is a legal Celsius setpoint but -28.9 C once converted from
  // Fahrenheit - proof the range check sees the converted value.
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  const char* err = nullptr;
  TEST_ASSERT_FALSE(applyFermenterFieldFromDisplay(F, "floor_temperature", -20.0f, &err));
  TEST_ASSERT_EQUAL_STRING("temperature out of range (-20 to 50)", err);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f, g_fermenters[F].floorTemp);   // untouched
}

void test_apply_rejects_out_of_range_and_changes_nothing(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  const char* err = nullptr;
  TEST_ASSERT_FALSE(applyFermenterFieldFromDisplay(F, "ceiling_temperature", 200.0f, &err));
  TEST_ASSERT_EQUAL_STRING("temperature out of range (-20 to 50)", err);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 22.0f, g_fermenters[F].ceilingTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f, g_fermenters[F].floorTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f,  g_fermenters[F].hysteresis);
}

void test_apply_leaves_unit_independent_fields_alone(void) {
  // Minutes and gravities have no unit, so they must pass through even in
  // Fahrenheit mode - converting 1.050 would give -17.2 and be rejected.
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_TRUE(applyFermenterFieldFromDisplay(F, "compressor_delay", 30.0f, nullptr));
  TEST_ASSERT_EQUAL_UINT16(30, g_fermenters[F].compressorDelay);
  TEST_ASSERT_TRUE(applyFermenterFieldFromDisplay(F, "og", 1.050f, nullptr));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, g_fermenters[F].og);
  TEST_ASSERT_TRUE(applyFermenterFieldFromDisplay(F, "tg", 1.010f, nullptr));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.010f, g_fermenters[F].tg);
}

void test_apply_rejects_a_non_numeric_field(void) {
  // Text fields are handled by the caller, not here - and unlike a validation
  // failure there's no error string to report.
  const char* err = nullptr;
  TEST_ASSERT_FALSE(applyFermenterFieldFromDisplay(F, "name", 0.0f, &err));
  TEST_ASSERT_NULL(err);
}

void test_apply_rejects_out_of_range_fermenter_index(void) {
  TEST_ASSERT_FALSE(applyFermenterFieldFromDisplay(MAX_FERMENTERS, "ceiling_temperature",
                                                   24.0f, nullptr));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_parse_valid_fermenter_topic);
  RUN_TEST(test_parse_valid_device_topic);
  RUN_TEST(test_parse_rejects_wrong_base_prefix);
  RUN_TEST(test_parse_rejects_missing_slash_after_base);
  RUN_TEST(test_parse_rejects_missing_scope_segment);
  RUN_TEST(test_parse_rejects_empty_scope);
  RUN_TEST(test_parse_rejects_scope_too_long);
  RUN_TEST(test_parse_rejects_missing_set_suffix);
  RUN_TEST(test_parse_rejects_wrong_suffix);
  RUN_TEST(test_parse_rejects_empty_key);
  RUN_TEST(test_parse_rejects_key_too_long);

  RUN_TEST(test_index_first_fermenter);
  RUN_TEST(test_index_last_valid_fermenter);
  RUN_TEST(test_index_rejects_out_of_range_at_boundary);
  RUN_TEST(test_index_rejects_far_out_of_range);
  RUN_TEST(test_index_rejects_non_fermenter_scope);
  RUN_TEST(test_index_rejects_wrong_case);
  RUN_TEST(test_index_nonnumeric_suffix_is_preexisting_atoi_quirk);
  RUN_TEST(test_index_no_digits_is_preexisting_atoi_quirk);

  RUN_TEST(test_apply_celsius_is_a_pass_through);
  RUN_TEST(test_apply_fahrenheit_converts_absolute_temperatures);
  RUN_TEST(test_apply_fahrenheit_converts_hysteresis_as_a_span);
  RUN_TEST(test_apply_validates_after_converting);
  RUN_TEST(test_apply_rejects_out_of_range_and_changes_nothing);
  RUN_TEST(test_apply_leaves_unit_independent_fields_alone);
  RUN_TEST(test_apply_rejects_a_non_numeric_field);
  RUN_TEST(test_apply_rejects_out_of_range_fermenter_index);

  return UNITY_END();
}
