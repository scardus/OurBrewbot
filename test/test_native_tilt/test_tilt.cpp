// Native (host) tests for the Tilt iBeacon parsing functions in
// OurBrewbot/Tilt.cpp: hexToU16, identifyTiltUuid, decodeTiltReading, and
// parseDiscLine.
//
// Tilt.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test - this is what makes the file's
// `static` parsing functions reachable from here, same trick used for
// Profile.cpp's isStepComplete(). See test/stubs/ for the minimal
// Arduino-core stand-ins, and below for the storage/test-doubles the rest
// of the firmware would normally provide.
//
// Two of these cases guard real past incidents (see project memory):
//   - the Apple company-ID byte-order bug (a simulator emitted the
//     byte-swapped form 004C0215; fixed in v0.4.2 to require the real
//     on-air order 4C000215)
//   - the HM-10 UART character-drop corruption, caught here by
//     decodeTiltReading's raw major/minor range checks

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
TiltConfig   g_tilts[MAX_TILTS];
GlobalConfig g_globalConfig;

// ---- millis(), settable per test ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}
// reportsPending() is only referenced inside startTiltScan() (never called
// by these tests), but the whole file compiles as one TU so its symbol
// still needs to resolve at link time.
bool reportsPending() { return false; }

// The functions under test (hexToU16/identifyTiltUuid/decodeTiltReading/
// parseDiscLine are all file-static), plus everything else in Tilt.cpp.
#include "../../OurBrewbot/Tilt.cpp"

// ---- test fixture ----

void setUp(void) {
  for (int i = 0; i < MAX_TILTS; i++) g_tilts[i] = TiltConfig{};
  s_millis = 0;
}

void tearDown(void) {}

// ---- hexToU16 ----

void test_hexToU16_accepts_uppercase(void) {
  uint16_t v;
  TEST_ASSERT_TRUE(hexToU16("1A2B", &v));
  TEST_ASSERT_EQUAL_HEX16(0x1A2B, v);
}

void test_hexToU16_accepts_lowercase(void) {
  uint16_t v;
  TEST_ASSERT_TRUE(hexToU16("1a2b", &v));
  TEST_ASSERT_EQUAL_HEX16(0x1A2B, v);
}

void test_hexToU16_rejects_non_hex_char(void) {
  uint16_t v;
  TEST_ASSERT_FALSE(hexToU16("12G4", &v));
}

// ---- identifyTiltUuid ----

void test_identifyTiltUuid_recognises_each_colour(void) {
  // A495BB{digit}0 + fixed 24-char tail, digit 1-8 = Red..Pink (index 0-7).
  const char* tail = "C5B14B44B5121370F02D74DE";
  char uuid[40];
  for (int i = 0; i < MAX_TILTS; i++) {
    snprintf(uuid, sizeof(uuid), "A495BB%d0%s", i + 1, tail);
    TEST_ASSERT_EQUAL_INT(i, identifyTiltUuid(uuid));
  }
}

void test_identifyTiltUuid_rejects_wrong_prefix(void) {
  TEST_ASSERT_EQUAL_INT(-1, identifyTiltUuid("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
}

void test_identifyTiltUuid_rejects_corrupted_tail(void) {
  // Right prefix and colour digit, tail doesn't match - a prefix-only
  // check would wrongly accept this as a Tilt.
  TEST_ASSERT_EQUAL_INT(-1, identifyTiltUuid("A495BB10FFFFFFFFFFFFFFFFFFFFFFFF"));
}

void test_identifyTiltUuid_rejects_short_input(void) {
  TEST_ASSERT_EQUAL_INT(-1, identifyTiltUuid("A495BB10C5B14B44"));
}

// ---- decodeTiltReading ----

void test_decodeTiltReading_standard_range(void) {
  float sg, tempC;
  bool  isPro;
  // major=0x0044 (68 -> 20.0C), minor=0x041A (1050 -> 1.050 SG)
  TEST_ASSERT_TRUE(decodeTiltReading("0044041AF6", 0, &sg, &tempC, &isPro));
  TEST_ASSERT_FALSE(isPro);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, tempC);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, sg);
}

void test_decodeTiltReading_pro_range(void) {
  float sg, tempC;
  bool  isPro;
  // major=0x02BC (700 -> 70.0F/10 -> 21.11C), minor=0x2904 (10500 -> 1.0500 SG)
  TEST_ASSERT_TRUE(decodeTiltReading("02BC2904", 0, &sg, &tempC, &isPro));
  TEST_ASSERT_TRUE(isPro);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.11f, tempC);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0500f, sg);
}

void test_decodeTiltReading_rejects_non_hex_major(void) {
  float sg, tempC;
  bool  isPro;
  TEST_ASSERT_FALSE(decodeTiltReading("00G4041A", 0, &sg, &tempC, &isPro));
}

void test_decodeTiltReading_rejects_non_hex_minor(void) {
  float sg, tempC;
  bool  isPro;
  TEST_ASSERT_FALSE(decodeTiltReading("0044G41A", 0, &sg, &tempC, &isPro));
}

void test_decodeTiltReading_rejects_standard_major_out_of_range(void) {
  float sg, tempC;
  bool  isPro;
  // major=0x00FA (250 > TILT_MAJOR_MAX 212)
  TEST_ASSERT_FALSE(decodeTiltReading("00FA041A", 0, &sg, &tempC, &isPro));
}

void test_decodeTiltReading_rejects_standard_minor_out_of_range(void) {
  float sg, tempC;
  bool  isPro;
  // minor=0x0320 (800 < TILT_MINOR_MIN 900)
  TEST_ASSERT_FALSE(decodeTiltReading("00440320", 0, &sg, &tempC, &isPro));
}

void test_decodeTiltReading_rejects_pro_major_out_of_range(void) {
  float sg, tempC;
  bool  isPro;
  // minor=0x2904 (10500, selects Pro), major=0x2AF8 (11000 > TILT_PRO_MAJOR_MAX 2120)
  TEST_ASSERT_FALSE(decodeTiltReading("2AF82904", 0, &sg, &tempC, &isPro));
}

void test_decodeTiltReading_rejects_pro_minor_out_of_range(void) {
  float sg, tempC;
  bool  isPro;
  // minor=0x1580 (5504 > threshold 5000, selects Pro) but < TILT_PRO_MINOR_MIN 9000
  TEST_ASSERT_FALSE(decodeTiltReading("02BC1580", 0, &sg, &tempC, &isPro));
}

// ---- parseDiscLine ----

void test_parseDiscLine_valid_colon_delimited_frame(void) {
  parseDiscLine("OK+DISC:4C000215:A495BB10C5B14B44B5121370F02D74DE:0044041AF6:F42DC96DA4F2:-053");
  TEST_ASSERT_TRUE(g_tilts[0].active);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, g_tilts[0].sg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, g_tilts[0].temperature);
}

void test_parseDiscLine_rejects_byte_swapped_company_id(void) {
  // 004C0215 is the byte-swapped form a simulator emitted - v0.4.2 fix.
  parseDiscLine("OK+DISC:004C0215:A495BB10C5B14B44B5121370F02D74DE:0044041AF6:F42DC96DA4F2:-053");
  TEST_ASSERT_FALSE(g_tilts[0].active);
}

void test_parseDiscLine_ignores_non_tilt_apple_ibeacon(void) {
  parseDiscLine("OK+DISC:4C000215:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA:0044041AF6:F42DC96DA4F2:-053");
  for (int i = 0; i < MAX_TILTS; i++) TEST_ASSERT_FALSE(g_tilts[i].active);
}

void test_parseDiscLine_valid_legacy_concatenated_frame(void) {
  parseDiscLine("OK+DISC:00000000:00000000:4C000215A495BB10C5B14B44B5121370F02D74DE0044041AC5");
  TEST_ASSERT_TRUE(g_tilts[0].active);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, g_tilts[0].sg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, g_tilts[0].temperature);
}

void test_parseDiscLine_legacy_defers_to_colon_delimited_fragment(void) {
  // No "OK+DISC:" prefix at all, so the colon-delimited branch never
  // matches; "4C000215" is found by the legacy strstr(), but it's
  // immediately followed by ':' - the dataStart[8]==':' guard must reject
  // this rather than misread it with legacy's fixed offsets.
  parseDiscLine("junkjunkjunk4C000215:A495BB10C5B14B44B5121370F02D74DE:0044041AF6");
  TEST_ASSERT_FALSE(g_tilts[0].active);
}

void test_parseDiscLine_ignores_short_or_null_line(void) {
  parseDiscLine("short");
  parseDiscLine("");
  parseDiscLine(nullptr);
  for (int i = 0; i < MAX_TILTS; i++) TEST_ASSERT_FALSE(g_tilts[i].active);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_hexToU16_accepts_uppercase);
  RUN_TEST(test_hexToU16_accepts_lowercase);
  RUN_TEST(test_hexToU16_rejects_non_hex_char);

  RUN_TEST(test_identifyTiltUuid_recognises_each_colour);
  RUN_TEST(test_identifyTiltUuid_rejects_wrong_prefix);
  RUN_TEST(test_identifyTiltUuid_rejects_corrupted_tail);
  RUN_TEST(test_identifyTiltUuid_rejects_short_input);

  RUN_TEST(test_decodeTiltReading_standard_range);
  RUN_TEST(test_decodeTiltReading_pro_range);
  RUN_TEST(test_decodeTiltReading_rejects_non_hex_major);
  RUN_TEST(test_decodeTiltReading_rejects_non_hex_minor);
  RUN_TEST(test_decodeTiltReading_rejects_standard_major_out_of_range);
  RUN_TEST(test_decodeTiltReading_rejects_standard_minor_out_of_range);
  RUN_TEST(test_decodeTiltReading_rejects_pro_major_out_of_range);
  RUN_TEST(test_decodeTiltReading_rejects_pro_minor_out_of_range);

  RUN_TEST(test_parseDiscLine_valid_colon_delimited_frame);
  RUN_TEST(test_parseDiscLine_rejects_byte_swapped_company_id);
  RUN_TEST(test_parseDiscLine_ignores_non_tilt_apple_ibeacon);
  RUN_TEST(test_parseDiscLine_valid_legacy_concatenated_frame);
  RUN_TEST(test_parseDiscLine_legacy_defers_to_colon_delimited_fragment);
  RUN_TEST(test_parseDiscLine_ignores_short_or_null_line);

  return UNITY_END();
}
