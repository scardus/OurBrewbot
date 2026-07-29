// Native (host) tests for the pure gravity/temperature helpers extracted
// from handleiSpindelPost() in OurBrewbot/iSpindel.cpp: validateiSpindelValues()
// (range clamp) and platoToSG() (unit conversion).
//
// iSpindel.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test - same pattern used for
// Profile/Fermenter/Tilt/Temperatures. See test/stubs/ for the minimal
// Arduino-core stand-ins, and below for the storage/test-doubles the rest
// of the firmware would normally provide.
//
// handleiSpindelPost() itself (JSON parsing, slot matching/registration) is
// NOT under test here - only the two helpers pulled out of it - but it still
// compiles as part of this translation unit, which is why the JSON-related
// stub support (ArduinoJson's Arduino String reader/converter, enabled via
// -D ARDUINOJSON_ENABLE_ARDUINO_STRING=1 in platformio.ini) has to be in place
// even though no test here calls it.

#include <unity.h>
#include <cstdint>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
iSpindelConfig g_iSpindels[MAX_ISPINDELS];

// ---- millis(), referenced (but never exercised) inside handleiSpindelPost ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}
bool saveiSpindelConfig() { return true; }

// The functions under test, plus everything else in iSpindel.cpp.
#include "../../OurBrewbot/iSpindel.cpp"

// ---- test fixture ----

void setUp(void) {}
void tearDown(void) {}

// ---- validateiSpindelValues: gravity range ----

void test_validate_keeps_sg_in_range(void) {
  float sg = 1.050f, temp = 20.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(1.050f, sg);
}

void test_validate_zeroes_sg_below_floor(void) {
  float sg = 0.850f, temp = 20.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(0.0f, sg);
}

void test_validate_zeroes_sg_above_ceiling(void) {
  float sg = 1.250f, temp = 20.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(0.0f, sg);
}

void test_validate_keeps_sg_at_exact_boundaries(void) {
  float sg = 0.900f, temp = 20.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(0.900f, sg);

  sg = 1.200f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(1.200f, sg);
}

void test_validate_leaves_zero_sg_alone(void) {
  // 0.0 means "field absent from the POST body", not "reading of zero" -
  // it must not be treated as an out-of-range value and logged/clamped.
  float sg = 0.0f, temp = 20.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(0.0f, sg);
}

// ---- validateiSpindelValues: temperature range ----

void test_validate_keeps_temp_in_range(void) {
  float sg = 1.050f, temp = 20.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(20.0f, temp);
}

void test_validate_zeroes_temp_below_floor(void) {
  float sg = 1.050f, temp = -50.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(0.0f, temp);
}

void test_validate_zeroes_temp_above_ceiling(void) {
  float sg = 1.050f, temp = 90.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(0.0f, temp);
}

void test_validate_keeps_temp_at_exact_boundaries(void) {
  float sg = 1.050f, temp = -40.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(-40.0f, temp);

  temp = 80.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(80.0f, temp);
}

void test_validate_leaves_zero_temp_alone(void) {
  float sg = 1.050f, temp = 0.0f;
  validateiSpindelValues(sg, temp, "Test", "abc123");
  TEST_ASSERT_EQUAL_FLOAT(0.0f, temp);
}

// ---- platoToSG ----

void test_plato_zero_is_sg_one(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0000f, platoToSG(0.0f));
}

void test_plato_ten_matches_reference_conversion(void) {
  // 10 degP ~= 1.0400 SG by the standard Plato<->SG approximation this
  // formula implements.
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0400f, platoToSG(10.0f));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_validate_keeps_sg_in_range);
  RUN_TEST(test_validate_zeroes_sg_below_floor);
  RUN_TEST(test_validate_zeroes_sg_above_ceiling);
  RUN_TEST(test_validate_keeps_sg_at_exact_boundaries);
  RUN_TEST(test_validate_leaves_zero_sg_alone);

  RUN_TEST(test_validate_keeps_temp_in_range);
  RUN_TEST(test_validate_zeroes_temp_below_floor);
  RUN_TEST(test_validate_zeroes_temp_above_ceiling);
  RUN_TEST(test_validate_keeps_temp_at_exact_boundaries);
  RUN_TEST(test_validate_leaves_zero_temp_alone);

  RUN_TEST(test_plato_zero_is_sg_one);
  RUN_TEST(test_plato_ten_matches_reference_conversion);

  return UNITY_END();
}
