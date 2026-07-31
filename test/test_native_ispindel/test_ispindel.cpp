// Native (host) tests for OurBrewbot/iSpindel.cpp: the pure helpers
// validateiSpindelValues() (range clamp), platoToSG() (gravity unit
// conversion) and iSpindelTempToCelsius() (temperature unit conversion),
// plus end-to-end coverage of handleiSpindelPost() itself.
//
// iSpindel.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test - same pattern used for
// Profile/Fermenter/Tilt/Temperatures. See test/stubs/ for the minimal
// Arduino-core stand-ins, and below for the storage/test-doubles the rest
// of the firmware would normally provide.
//
// The handleiSpindelPost() tests drive the real JSON path, which is why the
// ArduinoJson Arduino String reader/converter has to be enabled via
// -D ARDUINOJSON_ENABLE_ARDUINO_STRING=1 in platformio.ini. The stub String
// holds 2 KB - ample for the ~250-byte payloads below, but note it truncates
// silently rather than growing, so keep test bodies short.

#include <unity.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
iSpindelConfig g_iSpindels[MAX_ISPINDELS];

// ---- millis(), referenced (but never exercised) inside handleiSpindelPost ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}

// Counted so the tests can assert that a re-configured gravity unit is
// actually persisted (and that an unchanged one is not written needlessly).
static int s_saveCalls = 0;
bool saveiSpindelConfig() { s_saveCalls++; return true; }

// The functions under test, plus everything else in iSpindel.cpp.
#include "../../OurBrewbot/iSpindel.cpp"

// ---- test fixture ----

// Every slot empty ("None"), unassigned, no calibration offsets. The
// handleiSpindelPost() tests below register/match against this clean state.
void setUp(void) {
  memset(g_iSpindels, 0, sizeof(g_iSpindels));
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    strcpy(g_iSpindels[i].name, "None");
    g_iSpindels[i].fermenter = PROBE_UNASSIGNED;
    g_iSpindels[i].function  = PROBE_UNASSIGNED;
    g_iSpindels[i].unit      = ISPINDEL_UNIT_SG;
  }
  s_saveCalls = 0;
}

void tearDown(void) {}

// Build an iSpindel POST body. tempUnits == nullptr omits the temp_units field
// entirely, which is how a device that never sends one behaves.
static String makeBody(const char* name, const char* id, float temp,
                       const char* tempUnits, float gravity = 1.050f,
                       const char* gravityUnit = nullptr, float corrGravity = 0.0f) {
  char buf[512];
  char unitField[48] = "";
  if (tempUnits != nullptr) {
    snprintf(unitField, sizeof(unitField), "\"temp_units\":\"%s\",", tempUnits);
  }
  // Omitted entirely when null - an iSpindel sends no gravity-unit field at all,
  // which is exactly the case the "assume Plato" rule exists for.
  char gravUnitField[48] = "";
  if (gravityUnit != nullptr) {
    snprintf(gravUnitField, sizeof(gravUnitField), "\"gravity-unit\":\"%s\",", gravityUnit);
  }
  char corrField[48] = "";
  if (corrGravity != 0.0f) {
    snprintf(corrField, sizeof(corrField), "\"corr-gravity\":%.4f,", corrGravity);
  }
  snprintf(buf, sizeof(buf),
           "{\"name\":\"%s\",\"ID\":\"%s\",\"temperature\":%.4f,%s%s%s"
           "\"gravity\":%.4f,\"battery\":3.9,\"RSSI\":-50,\"angle\":45.0}",
           name, id, temp, unitField, gravUnitField, corrField, gravity);
  return String(buf);
}

// Put a known, already-registered device in slot 0 so posts match rather
// than register.
static void seedSlot0(const char* name, const char* id) {
  strcpy(g_iSpindels[0].name, name);
  strcpy(g_iSpindels[0].id, id);
  g_iSpindels[0].collectData = true;
}

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

// ---- iSpindelTempToCelsius ----

void test_temp_fahrenheit_converts_to_celsius(void) {
  // The reading that exposed the original bug: a live iSpindel reporting
  // 62.3 F was being stored as 62.3 C.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 16.83f, iSpindelTempToCelsius(62.3f, "F"));
}

void test_temp_celsius_passes_through(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, iSpindelTempToCelsius(20.0f, "C"));
}

void test_temp_kelvin_converts_to_celsius(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, iSpindelTempToCelsius(293.15f, "K"));
}

void test_temp_lowercase_unit_is_recognised(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 16.83f, iSpindelTempToCelsius(62.3f, "f"));
}

void test_temp_full_word_unit_is_recognised(void) {
  // Only the first character is inspected, so "Fahrenheit" works too.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 16.83f, iSpindelTempToCelsius(62.3f, "Fahrenheit"));
}

void test_temp_empty_unit_assumed_celsius(void) {
  // A device that sends no temp_units must behave exactly as it did before
  // the conversion was added - unchanged, i.e. assumed Celsius.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, iSpindelTempToCelsius(20.0f, ""));
}

void test_temp_unknown_unit_assumed_celsius(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, iSpindelTempToCelsius(20.0f, "Rankine"));
}

void test_temp_null_unit_assumed_celsius(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, iSpindelTempToCelsius(20.0f, nullptr));
}

void test_temp_negative_fahrenheit_converts(void) {
  // -40 is the point where the two scales meet - a good sign-handling check.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -40.0f, iSpindelTempToCelsius(-40.0f, "F"));
}

// ---- handleiSpindelPost: temperature unit handling end to end ----

void test_post_fahrenheit_stored_as_celsius(void) {
  seedSlot0("ispindel-1", "C2A080");
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 62.3f, "F"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 16.83f, g_iSpindels[0].temperature);
}

void test_post_celsius_stored_unchanged(void) {
  seedSlot0("ispindel-1", "C2A080");
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.5f, "C"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.5f, g_iSpindels[0].temperature);
}

void test_post_missing_temp_units_stored_unchanged(void) {
  seedSlot0("ispindel-1", "C2A080");
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.5f, nullptr));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.5f, g_iSpindels[0].temperature);
}

void test_post_kelvin_stored_as_celsius(void) {
  seedSlot0("ispindel-1", "C2A080");
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 293.15f, "K"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, g_iSpindels[0].temperature);
}

void test_post_warm_fahrenheit_survives_range_check(void) {
  // Regression guard: the -40..80 range in validateiSpindelValues is a CELSIUS
  // range. Before the fix it screened the raw Fahrenheit number, so 85 F - an
  // ordinary ale fermentation temperature - was > 80 and got silently zeroed.
  seedSlot0("ispindel-1", "C2A080");
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 85.0f, "F"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 29.44f, g_iSpindels[0].temperature);
}

void test_post_absurd_fahrenheit_still_rejected(void) {
  // The range check must still do its job after conversion: 250 F is 121 C,
  // outside the plausible range, so it is zeroed.
  seedSlot0("ispindel-1", "C2A080");
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 250.0f, "F"));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_iSpindels[0].temperature);
}

void test_post_temp_adjust_applied_as_celsius_delta(void) {
  // Regression guard: tempAdjust is stored as a Celsius delta (WebAPI.cpp
  // converts it with toCelsiusTempDelta). Before the fix it was added to a
  // Fahrenheit value, so a +1.0 C offset moved the reading by 1 F (0.56 C).
  seedSlot0("ispindel-1", "C2A080");
  g_iSpindels[0].tempAdjust = 1.0f;
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 62.3f, "F"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 17.83f, g_iSpindels[0].temperature);
}

void test_post_registration_path_stores_celsius(void) {
  // A brand new device lands in the first free slot - that path must store
  // the converted value too, not the raw one.
  handleiSpindelPost(makeBody("newspindel", "AABBCC", 62.3f, "F"));
  TEST_ASSERT_EQUAL_STRING("newspindel", g_iSpindels[0].name);
  TEST_ASSERT_EQUAL_STRING("AABBCC", g_iSpindels[0].id);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 16.83f, g_iSpindels[0].temperature);
}

void test_post_conversion_does_not_disturb_gravity(void) {
  // Guards against a copy/paste slip putting the temperature maths on the
  // wrong field.
  seedSlot0("ispindel-1", "C2A080");
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 62.3f, "F", 1.0500f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0500f, g_iSpindels[0].sg);
}

// ---- iSpindelDeclaredGravityUnit: what the device said it was sending ----

void test_declared_unit_g_is_sg(void) {
  // GravityMon's wire value for Specific Gravity.
  TEST_ASSERT_EQUAL_INT(ISPINDEL_UNIT_SG, iSpindelDeclaredGravityUnit("G"));
}

void test_declared_unit_p_is_plato(void) {
  TEST_ASSERT_EQUAL_INT(ISPINDEL_UNIT_PLATO, iSpindelDeclaredGravityUnit("P"));
}

void test_declared_unit_lowercase_recognised(void) {
  TEST_ASSERT_EQUAL_INT(ISPINDEL_UNIT_SG,    iSpindelDeclaredGravityUnit("g"));
  TEST_ASSERT_EQUAL_INT(ISPINDEL_UNIT_PLATO, iSpindelDeclaredGravityUnit("p"));
}

void test_declared_unit_full_words_recognised(void) {
  // Only the first character is examined, so spelled-out units work too.
  TEST_ASSERT_EQUAL_INT(ISPINDEL_UNIT_SG,    iSpindelDeclaredGravityUnit("SG"));
  TEST_ASSERT_EQUAL_INT(ISPINDEL_UNIT_PLATO, iSpindelDeclaredGravityUnit("Plato"));
}

void test_declared_unit_empty_is_undeclared(void) {
  // An iSpindel sends no field, which arrives here as "".
  TEST_ASSERT_EQUAL_INT(-1, iSpindelDeclaredGravityUnit(""));
}

void test_declared_unit_null_is_undeclared(void) {
  TEST_ASSERT_EQUAL_INT(-1, iSpindelDeclaredGravityUnit(nullptr));
}

void test_declared_unit_unknown_is_undeclared(void) {
  // Never guess from a string we do not recognise - fall back to the rule.
  TEST_ASSERT_EQUAL_INT(-1, iSpindelDeclaredGravityUnit("X"));
}

// ---- iSpindelNameDeclaresSG: the Brewfather "[SG]" name convention ----

void test_name_suffix_sg_is_recognised(void) {
  TEST_ASSERT_TRUE(iSpindelNameDeclaresSG("ispindel-1[SG]"));
}

void test_name_suffix_sg_is_case_insensitive(void) {
  TEST_ASSERT_TRUE(iSpindelNameDeclaresSG("ispindel-1[sg]"));
}

void test_name_without_suffix_is_not_sg(void) {
  TEST_ASSERT_FALSE(iSpindelNameDeclaresSG("ispindel-1"));
}

void test_name_sg_prefix_is_not_a_suffix(void) {
  // The marker only counts at the END of the name.
  TEST_ASSERT_FALSE(iSpindelNameDeclaresSG("[SG] ispindel-1"));
}

void test_name_shorter_than_suffix_is_not_sg(void) {
  TEST_ASSERT_FALSE(iSpindelNameDeclaresSG("SG"));
  TEST_ASSERT_FALSE(iSpindelNameDeclaresSG(""));
  TEST_ASSERT_FALSE(iSpindelNameDeclaresSG(nullptr));
}

// ---- handleiSpindelPost: gravity unit handling end to end ----

void test_post_plato_slot_converts_gravity(void) {
  // THE regression this change exists for. validateiSpindelValues screens
  // gravity against an SG range (0.900-1.200), so before the fix a 12.5 P
  // reading failed the check, was zeroed, and platoToSG(0) stored exactly
  // 1.0000 - a Plato device reported a flat 1.0000 forever.
  seedSlot0("ispindel-1", "C2A080");
  g_iSpindels[0].unit = ISPINDEL_UNIT_PLATO;
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.0f, "C", 12.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0505f, g_iSpindels[0].sg);
}

void test_post_plato_absent_gravity_stays_zero(void) {
  // Zero means "the device did not send a gravity", and platoToSG(0) is
  // exactly 1.0000 - a plausible-looking reading. It must stay zero.
  seedSlot0("ispindel-1", "C2A080");
  g_iSpindels[0].unit = ISPINDEL_UNIT_PLATO;
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.0f, "C", 0.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_iSpindels[0].sg);
}

void test_post_plato_corr_gravity_converted(void) {
  // corr-gravity arrives in the same unit as gravity, so it converts too.
  seedSlot0("ispindel-1", "C2A080");
  g_iSpindels[0].unit = ISPINDEL_UNIT_PLATO;
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.0f, "C", 12.5f, nullptr, 12.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0505f, g_iSpindels[0].corrGravity);
}

void test_post_plato_sg_adjust_applied_after_conversion(void) {
  // sgAdjust is an SG offset, so it must be added to the converted value and
  // not to the Plato one.
  seedSlot0("ispindel-1", "C2A080");
  g_iSpindels[0].unit     = ISPINDEL_UNIT_PLATO;
  g_iSpindels[0].sgAdjust = 0.0010f;
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.0f, "C", 12.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0515f, g_iSpindels[0].sg);
}

void test_post_plato_out_of_range_still_rejected(void) {
  // Validation still does its job after conversion: 60 P is SG 1.29, outside
  // the plausible range, so it is zeroed.
  seedSlot0("ispindel-1", "C2A080");
  g_iSpindels[0].unit = ISPINDEL_UNIT_PLATO;
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.0f, "C", 60.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g_iSpindels[0].sg);
}

void test_post_sg_slot_stores_gravity_unchanged(void) {
  // The SG path is the identity - the case every live device is on today.
  seedSlot0("ispindel-1", "C2A080");
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.0f, "C", 1.0500f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0500f, g_iSpindels[0].sg);
}

// ---- handleiSpindelPost: a declared unit re-configures the slot ----

void test_post_declared_sg_overrides_stored_plato(void) {
  // The device is stating what it just sent, so it wins over the stored
  // setting - and the slot is corrected and saved.
  seedSlot0("gravitymon", "9B5C5E");
  g_iSpindels[0].unit = ISPINDEL_UNIT_PLATO;
  handleiSpindelPost(makeBody("gravitymon", "9B5C5E", 18.0f, "C", 1.0500f, "G"));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0500f, g_iSpindels[0].sg);
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_SG, g_iSpindels[0].unit);
  TEST_ASSERT_EQUAL_INT(1, s_saveCalls);
}

void test_post_declared_plato_overrides_stored_sg(void) {
  seedSlot0("gravitymon", "9B5C5E");
  handleiSpindelPost(makeBody("gravitymon", "9B5C5E", 18.0f, "C", 12.5f, "P"));
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0505f, g_iSpindels[0].sg);
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_PLATO, g_iSpindels[0].unit);
  TEST_ASSERT_EQUAL_INT(1, s_saveCalls);
}

void test_post_matching_declaration_does_not_rewrite_config(void) {
  // A device that agrees with the stored setting must not cause a flash write
  // on every report.
  seedSlot0("gravitymon", "9B5C5E");
  handleiSpindelPost(makeBody("gravitymon", "9B5C5E", 18.0f, "C", 1.0500f, "G"));
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_SG, g_iSpindels[0].unit);
  TEST_ASSERT_EQUAL_INT(0, s_saveCalls);
}

void test_post_undeclared_leaves_stored_unit_alone(void) {
  // An iSpindel declares nothing, so the user's setting keeps applying.
  seedSlot0("ispindel-1", "C2A080");
  g_iSpindels[0].unit = ISPINDEL_UNIT_PLATO;
  handleiSpindelPost(makeBody("ispindel-1", "C2A080", 18.0f, "C", 12.5f));
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_PLATO, g_iSpindels[0].unit);
  TEST_ASSERT_EQUAL_INT(0, s_saveCalls);
}

// ---- handleiSpindelPost: unit resolution when a new device registers ----

void test_register_undeclared_assumes_plato(void) {
  // No gravity-unit field and no "[SG]" name: Brewfather's assumption for an
  // iSpindel, so the reading is treated as Plato and converted.
  handleiSpindelPost(makeBody("newspindel", "AABBCC", 18.0f, "C", 12.5f));
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_PLATO, g_iSpindels[0].unit);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0505f, g_iSpindels[0].sg);
}

void test_register_name_suffix_declares_sg(void) {
  handleiSpindelPost(makeBody("newspindel[SG]", "AABBCC", 18.0f, "C", 1.0500f));
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_SG, g_iSpindels[0].unit);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0500f, g_iSpindels[0].sg);
}

void test_register_declared_unit_beats_name_suffix(void) {
  // An explicit field is stronger evidence than a naming convention.
  handleiSpindelPost(makeBody("newspindel[SG]", "AABBCC", 18.0f, "C", 12.5f, "P"));
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_PLATO, g_iSpindels[0].unit);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 1.0505f, g_iSpindels[0].sg);
}

void test_register_declared_sg_stores_sg(void) {
  // What a GravityMon does on its first report.
  handleiSpindelPost(makeBody("gravitymon", "9B5C5E", 18.0f, "C", 1.0500f, "G"));
  TEST_ASSERT_EQUAL_UINT8(ISPINDEL_UNIT_SG, g_iSpindels[0].unit);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0500f, g_iSpindels[0].sg);
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

  RUN_TEST(test_temp_fahrenheit_converts_to_celsius);
  RUN_TEST(test_temp_celsius_passes_through);
  RUN_TEST(test_temp_kelvin_converts_to_celsius);
  RUN_TEST(test_temp_lowercase_unit_is_recognised);
  RUN_TEST(test_temp_full_word_unit_is_recognised);
  RUN_TEST(test_temp_empty_unit_assumed_celsius);
  RUN_TEST(test_temp_unknown_unit_assumed_celsius);
  RUN_TEST(test_temp_null_unit_assumed_celsius);
  RUN_TEST(test_temp_negative_fahrenheit_converts);

  RUN_TEST(test_post_fahrenheit_stored_as_celsius);
  RUN_TEST(test_post_celsius_stored_unchanged);
  RUN_TEST(test_post_missing_temp_units_stored_unchanged);
  RUN_TEST(test_post_kelvin_stored_as_celsius);
  RUN_TEST(test_post_warm_fahrenheit_survives_range_check);
  RUN_TEST(test_post_absurd_fahrenheit_still_rejected);
  RUN_TEST(test_post_temp_adjust_applied_as_celsius_delta);
  RUN_TEST(test_post_registration_path_stores_celsius);
  RUN_TEST(test_post_conversion_does_not_disturb_gravity);

  RUN_TEST(test_declared_unit_g_is_sg);
  RUN_TEST(test_declared_unit_p_is_plato);
  RUN_TEST(test_declared_unit_lowercase_recognised);
  RUN_TEST(test_declared_unit_full_words_recognised);
  RUN_TEST(test_declared_unit_empty_is_undeclared);
  RUN_TEST(test_declared_unit_null_is_undeclared);
  RUN_TEST(test_declared_unit_unknown_is_undeclared);

  RUN_TEST(test_name_suffix_sg_is_recognised);
  RUN_TEST(test_name_suffix_sg_is_case_insensitive);
  RUN_TEST(test_name_without_suffix_is_not_sg);
  RUN_TEST(test_name_sg_prefix_is_not_a_suffix);
  RUN_TEST(test_name_shorter_than_suffix_is_not_sg);

  RUN_TEST(test_post_plato_slot_converts_gravity);
  RUN_TEST(test_post_plato_absent_gravity_stays_zero);
  RUN_TEST(test_post_plato_corr_gravity_converted);
  RUN_TEST(test_post_plato_sg_adjust_applied_after_conversion);
  RUN_TEST(test_post_plato_out_of_range_still_rejected);
  RUN_TEST(test_post_sg_slot_stores_gravity_unchanged);

  RUN_TEST(test_post_declared_sg_overrides_stored_plato);
  RUN_TEST(test_post_declared_plato_overrides_stored_sg);
  RUN_TEST(test_post_matching_declaration_does_not_rewrite_config);
  RUN_TEST(test_post_undeclared_leaves_stored_unit_alone);

  RUN_TEST(test_register_undeclared_assumes_plato);
  RUN_TEST(test_register_name_suffix_declares_sg);
  RUN_TEST(test_register_declared_unit_beats_name_suffix);
  RUN_TEST(test_register_declared_sg_stores_sg);

  return UNITY_END();
}
