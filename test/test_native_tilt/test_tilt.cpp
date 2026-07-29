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
// reportsPending() gates startTiltScan() (a queued cloud POST can block the
// loop for seconds, starving a scan), so it's settable per test rather than a
// fixed false.
static bool s_reportsPending = false;
bool reportsPending() { return s_reportsPending; }

// The functions under test (hexToU16/identifyTiltUuid/decodeTiltReading/
// parseDiscLine are all file-static), plus everything else in Tilt.cpp.
#include "../../OurBrewbot/Tilt.cpp"

// ---- test fixture ----

void setUp(void) {
  for (int i = 0; i < MAX_TILTS; i++) g_tilts[i] = TiltConfig{};
  // File-static miss counter (reachable because Tilt.cpp is #included) - a
  // count left over from a previous test would change when a Tilt gets
  // deregistered in the next one.
  for (int i = 0; i < MAX_TILTS; i++) s_missedReads[i] = 0;
  s_millis = 0;
  // Scan-state file-statics, so each scan test starts from "idle, module ready".
  s_scanActive        = false;
  s_scanStart         = 0;
  s_bleBufLen         = 0;
  s_bleBuf[0]         = '\0';
  s_bleReady          = true;
  s_lastBleInitRetry  = 0;
  g_bleSniffActive    = false;
  s_reportsPending    = false;
  g_bleSerial.reset();
  g_globalConfig = GlobalConfig{};
}

void tearDown(void) {}

// A full colon-delimited DISC record for a given colour digit and
// major/minor/power field, in the on-air format the HM-10 emits.
static const char* discRecord(int colourDigit, const char* majorMinorPower) {
  static char buf[128];
  snprintf(buf, sizeof(buf),
           "OK+DISC:4C000215:A495BB%d0C5B14B44B5121370F02D74DE:%s:F42DC96DA4F2:-053",
           colourDigit, majorMinorPower);
  return buf;
}

// Advance the simulated clock past the 4 s scan window so serviceTilt() takes
// its scan-over path instead of waiting for more bytes.
static void expireScanWindow(void) {
  test_setMillis(s_scanStart + BLE_SCAN_TIMEOUT_MS + 1);
}

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

// ---- processTiltReading ----
// Where a decoded frame becomes the stored reading: calibration offsets are
// applied here, so a sign error would silently skew every reading. The
// parseDiscLine tests above only reach this with zero offsets.

void test_processTiltReading_applies_calibration_offsets(void) {
  const uint8_t C = 2;  // Black
  g_tilts[C].tempAdjust = -0.7f;
  g_tilts[C].sgAdjust   =  0.002f;

  processTiltReading(C, 1.050f, 20.0f, false);

  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.052f, g_tilts[C].sg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f,   19.3f,  g_tilts[C].temperature);
}

void test_processTiltReading_claims_an_unassigned_colour_slot(void) {
  const uint8_t C = 3;
  g_tilts[C].colour = PROBE_UNASSIGNED;  // as initDefaultTiltConfig() leaves it
  processTiltReading(C, 1.040f, 18.0f, false);
  // Claiming the slot is what makes saveTiltConfig() persist it.
  TEST_ASSERT_EQUAL_UINT8(C, g_tilts[C].colour);
}

void test_processTiltReading_preserves_existing_slot_config(void) {
  const uint8_t C = 3;
  g_tilts[C].colour    = C;
  g_tilts[C].fermenter = 1;
  g_tilts[C].function  = PROBE_FN_BEER;

  processTiltReading(C, 1.040f, 18.0f, false);

  TEST_ASSERT_EQUAL_UINT8(C, g_tilts[C].colour);
  TEST_ASSERT_EQUAL_UINT8(1, g_tilts[C].fermenter);
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER, g_tilts[C].function);
}

void test_processTiltReading_marks_active_and_stamps_lastSeen(void) {
  const uint8_t C = 0;
  test_setMillis(5000);
  processTiltReading(C, 1.0500f, 20.0f, /*isPro=*/true);
  TEST_ASSERT_TRUE(g_tilts[C].active);
  TEST_ASSERT_TRUE(g_tilts[C].isPro);
  TEST_ASSERT_EQUAL_UINT32(5000, g_tilts[C].lastSeen);
}

void test_processTiltReading_resets_missed_read_count(void) {
  const uint8_t C = 1;
  s_missedReads[C] = 250;  // most of the way to deregistration
  processTiltReading(C, 1.040f, 18.0f, false);
  TEST_ASSERT_EQUAL_INT(0, s_missedReads[C]);
}

void test_processTiltReading_ignores_out_of_range_colour(void) {
  processTiltReading(MAX_TILTS, 1.040f, 18.0f, false);
  for (int i = 0; i < MAX_TILTS; i++) TEST_ASSERT_FALSE(g_tilts[i].active);
}

// ---- startTiltScan: when a scan is allowed to start ----

// Sitting mid-scan, as startTiltScan() would have left things.
static void beginScan(void) {
  test_setMillis(1000000);
  s_scanActive = true;
  s_scanStart  = 1000000;
  s_bleBufLen  = 0;
}

void test_startTiltScan_sends_discovery_command(void) {
  test_setMillis(1000000);
  startTiltScan();
  TEST_ASSERT_TRUE(s_scanActive);
  TEST_ASSERT_EQUAL_STRING("AT+DISI?", g_bleSerial.lastPrint());
  TEST_ASSERT_EQUAL_UINT32(1000000, s_scanStart);
  TEST_ASSERT_EQUAL_INT(0, s_bleBufLen);
}

void test_startTiltScan_skipped_while_sniff_page_owns_the_port(void) {
  g_bleSniffActive = true;
  startTiltScan();
  TEST_ASSERT_FALSE(s_scanActive);
  TEST_ASSERT_EQUAL_STRING("", g_bleSerial.lastPrint());
}

void test_startTiltScan_skipped_while_a_cloud_report_is_queued(void) {
  // A queued POST can block the loop for up to 5 s, which would starve the
  // drain; Tilts advertise continuously so the next tick picks them up.
  s_reportsPending = true;
  startTiltScan();
  TEST_ASSERT_FALSE(s_scanActive);
  TEST_ASSERT_EQUAL_STRING("", g_bleSerial.lastPrint());
}

void test_startTiltScan_skipped_while_previous_scan_still_draining(void) {
  beginScan();
  startTiltScan();
  TEST_ASSERT_EQUAL_STRING("", g_bleSerial.lastPrint());  // no second AT+DISI?
}

void test_startTiltScan_counts_a_miss_when_module_not_ready(void) {
  s_bleReady = false;
  test_setMillis(1000000);
  s_lastBleInitRetry = 1000000;  // retry not due for another 5 min
  g_tilts[0].active = true;

  startTiltScan();

  TEST_ASSERT_FALSE(s_scanActive);
  TEST_ASSERT_EQUAL_INT(1, s_missedReads[0]);  // still ages a stale Tilt
}

// ---- initBLE: HM-10 handshake ----

void test_initBLE_marks_ready_on_ok_response(void) {
  s_bleReady = false;
  g_bleSerial.feed("OK");
  initBLE();
  TEST_ASSERT_TRUE(s_bleReady);
}

void test_initBLE_marks_not_ready_when_module_silent(void) {
  s_bleReady = true;
  // Nothing fed: the AT probe reads back an empty response.
  initBLE();
  TEST_ASSERT_FALSE(s_bleReady);
}

// ---- serviceTilt: draining an AT+DISI? response ----

void test_serviceTilt_parses_two_records_in_one_response(void) {
  // The key case for the boundary-byte save/restore in serviceTilt(): the
  // first record is parsed as soon as the second "OK+DISC:" appears, and the
  // 'O' that got null-terminated must be put back before the memmove or the
  // second record loses its first character and is silently dropped.
  beginScan();
  g_bleSerial.feed(discRecord(1, "0044041AF6"));  // Red:   20.0 C, 1.050
  g_bleSerial.feed(discRecord(2, "0050042EF6"));  // Green: 26.7 C, 1.070
  g_bleSerial.feed("OK+DISCE");

  serviceTilt();

  TEST_ASSERT_TRUE(g_tilts[0].active);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, g_tilts[0].sg);
  TEST_ASSERT_TRUE(g_tilts[1].active);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.070f, g_tilts[1].sg);
  TEST_ASSERT_FALSE(s_scanActive);  // end marker closed the scan
}

void test_serviceTilt_reassembles_a_record_split_across_passes(void) {
  // Real loop passes see partial responses; the buffer has to carry the
  // fragment over rather than parsing or discarding it.
  beginScan();
  const char* rec = discRecord(1, "0044041AF6");
  char head[40];
  strncpy(head, rec, 30);
  head[30] = '\0';

  g_bleSerial.feed(head);
  serviceTilt();
  TEST_ASSERT_FALSE(g_tilts[0].active);  // nothing usable yet
  TEST_ASSERT_TRUE(s_scanActive);        // still inside the scan window

  g_bleSerial.feed(rec + 30);
  g_bleSerial.feed("OK+DISCE");
  serviceTilt();
  TEST_ASSERT_TRUE(g_tilts[0].active);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, g_tilts[0].sg);
}

void test_serviceTilt_parses_buffered_record_on_timeout(void) {
  // No end marker ever arrives (module reset, garbled tail). Whatever is in
  // the buffer when the 4 s window expires must still be parsed.
  beginScan();
  g_bleSerial.feed(discRecord(1, "0044041AF6"));
  serviceTilt();                    // buffers it, keeps waiting
  TEST_ASSERT_TRUE(s_scanActive);

  expireScanWindow();
  serviceTilt();
  TEST_ASSERT_TRUE(g_tilts[0].active);
  TEST_ASSERT_FALSE(s_scanActive);
}

void test_serviceTilt_ignores_the_scan_start_marker(void) {
  // The HM-10 prefixes the response with OK+DISCS. It shares 7 characters with
  // a real record header, so only an exact "OK+DISC:" may be parsed.
  beginScan();
  g_bleSerial.feed("OK+DISCS");
  g_bleSerial.feed(discRecord(1, "0044041AF6"));
  g_bleSerial.feed("OK+DISCE");

  serviceTilt();

  TEST_ASSERT_TRUE(g_tilts[0].active);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, g_tilts[0].sg);
}

void test_serviceTilt_overflow_shift_keeps_the_newest_bytes(void) {
  // Safety net for a malformed response with no record boundaries: the buffer
  // discards its OLDEST half, so a good record arriving afterwards survives.
  beginScan();
  char junk[301];
  memset(junk, 'x', sizeof(junk) - 1);
  junk[sizeof(junk) - 1] = '\0';
  g_bleSerial.feed(junk);                          // ~300 B of noise
  g_bleSerial.feed(discRecord(1, "0044041AF6"));   // pushes past BLE_BUF_SIZE
  g_bleSerial.feed("OK+DISCE");

  serviceTilt();

  TEST_ASSERT_TRUE(g_tilts[0].active);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, g_tilts[0].sg);
}

void test_serviceTilt_abandons_scan_when_sniff_page_opens(void) {
  beginScan();
  g_bleSerial.feed(discRecord(1, "0044041AF6"));
  g_bleSniffActive = true;   // sniff page took the port mid-scan

  serviceTilt();

  TEST_ASSERT_FALSE(s_scanActive);
  TEST_ASSERT_EQUAL_INT(0, s_bleBufLen);
  TEST_ASSERT_FALSE(g_tilts[0].active);  // frame dropped, not parsed
}

// ---- serviceTilt: missed-read ageing and deregistration ----

void test_serviceTilt_deregisters_a_tilt_after_300_misses(void) {
  g_tilts[0].active = true;
  s_missedReads[0]  = 299;

  beginScan();
  expireScanWindow();
  serviceTilt();   // empty scan - the Tilt wasn't seen

  TEST_ASSERT_FALSE(g_tilts[0].active);
  TEST_ASSERT_EQUAL_INT(0, s_missedReads[0]);  // counter reset with the slot
}

void test_serviceTilt_keeps_a_tilt_that_was_seen_this_scan(void) {
  g_tilts[0].active = true;
  s_missedReads[0]  = 299;   // one scan away from deregistration

  beginScan();
  g_bleSerial.feed(discRecord(1, "0044041AF6"));
  g_bleSerial.feed("OK+DISCE");
  serviceTilt();

  TEST_ASSERT_TRUE(g_tilts[0].active);
  // processTiltReading() zeroed the counter, then the end-of-scan ageing pass
  // bumped every active Tilt - including this one - so a Tilt seen on every
  // scan sits at 1 rather than 0. Far from the 300 threshold either way.
  TEST_ASSERT_EQUAL_INT(1, s_missedReads[0]);
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

  RUN_TEST(test_processTiltReading_applies_calibration_offsets);
  RUN_TEST(test_processTiltReading_claims_an_unassigned_colour_slot);
  RUN_TEST(test_processTiltReading_preserves_existing_slot_config);
  RUN_TEST(test_processTiltReading_marks_active_and_stamps_lastSeen);
  RUN_TEST(test_processTiltReading_resets_missed_read_count);
  RUN_TEST(test_processTiltReading_ignores_out_of_range_colour);

  RUN_TEST(test_startTiltScan_sends_discovery_command);
  RUN_TEST(test_startTiltScan_skipped_while_sniff_page_owns_the_port);
  RUN_TEST(test_startTiltScan_skipped_while_a_cloud_report_is_queued);
  RUN_TEST(test_startTiltScan_skipped_while_previous_scan_still_draining);
  RUN_TEST(test_startTiltScan_counts_a_miss_when_module_not_ready);

  RUN_TEST(test_initBLE_marks_ready_on_ok_response);
  RUN_TEST(test_initBLE_marks_not_ready_when_module_silent);

  RUN_TEST(test_serviceTilt_parses_two_records_in_one_response);
  RUN_TEST(test_serviceTilt_reassembles_a_record_split_across_passes);
  RUN_TEST(test_serviceTilt_parses_buffered_record_on_timeout);
  RUN_TEST(test_serviceTilt_ignores_the_scan_start_marker);
  RUN_TEST(test_serviceTilt_overflow_shift_keeps_the_newest_bytes);
  RUN_TEST(test_serviceTilt_abandons_scan_when_sniff_page_opens);
  RUN_TEST(test_serviceTilt_deregisters_a_tilt_after_300_misses);
  RUN_TEST(test_serviceTilt_keeps_a_tilt_that_was_seen_this_scan);

  return UNITY_END();
}
