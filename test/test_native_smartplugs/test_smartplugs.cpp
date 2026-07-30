// Native (host) tests for the RF smart-plug switch path in
// OurBrewbot/SmartPlugs.cpp.
//
// smartPlugSwitch() has four early-returns that are silent by design - a
// misconfigured plug simply doesn't transmit, with no error and no log line.
// On real hardware that presents as a fridge or heat belt that never comes on,
// which is exactly the failure this suite is meant to catch. The only
// observable effect of a switch is an RF burst, so test/stubs/RCSwitch.h
// records what was sent instead of transmitting it.
//
// SmartPlugs.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test - see test/stubs/ for the Arduino-core
// stand-ins and below for the storage/test-doubles the rest of the firmware
// would normally provide.

#include <unity.h>
#include <cstdint>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
SmartPlugConfig g_smartPlugs[MAX_SMART_PLUGS];

// ---- millis(), unused here but required by the Arduino stub ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}

// The code under test. Also brings the file-static s_plugState[] into scope
// so the fixture can clear it between tests.
#include "../../OurBrewbot/SmartPlugs.cpp"

// ---- test fixture ----

static const uint8_t P = 0;   // plug slot used by most tests

// A fully configured plug: distinct on/off codes so tests can tell which
// branch transmitted, and distinct RF parameters so pass-through is provable.
static void configurePlug(uint8_t idx) {
  g_smartPlugs[idx] = SmartPlugConfig{};
  g_smartPlugs[idx].onCode      = 0x123456;
  g_smartPlugs[idx].offCode     = 0xABCDEF;
  g_smartPlugs[idx].bits        = 24;
  g_smartPlugs[idx].delayLength = 427;
  g_smartPlugs[idx].protocol    = 2;
  g_smartPlugs[idx].function    = PLUG_FN_F1_HOT;
}

void setUp(void) {
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    g_smartPlugs[i] = SmartPlugConfig{};
    s_plugState[i]  = false;
  }
  rcTestReset();
}

void tearDown(void) {}

// ============================================================
// THE FOUR SILENT GUARDS
// ============================================================

static void test_index_beyond_the_last_slot_never_transmits(void) {
  configurePlug(P);
  smartPlugSwitch(MAX_SMART_PLUGS, true);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);
  // and it must not have disturbed a real slot
  TEST_ASSERT_FALSE(getPlugState(P));
}

static void test_unconfigured_plug_with_both_codes_zero_never_transmits(void) {
  g_smartPlugs[P].function = PLUG_FN_F1_HOT;   // assigned, but no codes learnt
  g_smartPlugs[P].onCode   = 0;
  g_smartPlugs[P].offCode  = 0;
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);
}

static void test_unassigned_function_never_transmits(void) {
  configurePlug(P);
  g_smartPlugs[P].function = PLUG_FN_UNASSIGNED;
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);
}

// The one asymmetric guard: a plug can have a usable ON code and a missing OFF
// code (or vice versa), so the check is on the *selected* code, not the pair.
static void test_missing_off_code_blocks_only_the_off_direction(void) {
  configurePlug(P);
  g_smartPlugs[P].offCode = 0;

  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
  TEST_ASSERT_EQUAL_HEX32(0x123456, g_rfTest.lastCode);

  smartPlugSwitch(P, false);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);   // still 1 - OFF was dropped
}

static void test_missing_on_code_blocks_only_the_on_direction(void) {
  configurePlug(P);
  g_smartPlugs[P].onCode = 0;

  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);

  smartPlugSwitch(P, false);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
  TEST_ASSERT_EQUAL_HEX32(0xABCDEF, g_rfTest.lastCode);
}

// ============================================================
// CODE SELECTION AND RF PARAMETER PASS-THROUGH
// ============================================================

static void test_on_and_off_select_their_own_codes(void) {
  configurePlug(P);

  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_HEX32(0x123456, g_rfTest.lastCode);

  smartPlugSwitch(P, false);
  TEST_ASSERT_EQUAL_HEX32(0xABCDEF, g_rfTest.lastCode);

  TEST_ASSERT_EQUAL_INT(2, g_rfTest.sendCount);
}

static void test_rf_parameters_reach_the_transmitter_unmodified(void) {
  configurePlug(P);
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_UINT32(24,  g_rfTest.lastBits);
  TEST_ASSERT_EQUAL_INT(427,    g_rfTest.lastPulseLength);
  TEST_ASSERT_EQUAL_INT(2,      g_rfTest.lastProtocol);
}

// Ten repeats is what makes these cheap receivers latch reliably; a change
// here would present as plugs that only sometimes respond.
static void test_transmit_repeats_ten_times_on_the_configured_pin(void) {
  configurePlug(P);
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(10, g_rfTest.lastRepeat);
  TEST_ASSERT_EQUAL_INT(PIN_RF_TRANSMIT, g_rfTest.lastTransmitPin);
}

// The receiver is muted for the duration of the burst so the sniffer doesn't
// hear our own transmission, then switched back on.
static void test_receiver_is_re_enabled_after_the_burst(void) {
  configurePlug(P);
  smartPlugSwitch(P, true);
  TEST_ASSERT_TRUE(g_rfTest.receiveEnabled);
  TEST_ASSERT_FALSE(g_rfTest.transmitEnabled);
}

// ============================================================
// PLUG STATE — only ever reflects a transmit that really happened
// ============================================================

static void test_state_defaults_to_off(void) {
  TEST_ASSERT_FALSE(getPlugState(P));
}

static void test_state_follows_a_successful_switch(void) {
  configurePlug(P);
  smartPlugSwitch(P, true);
  TEST_ASSERT_TRUE(getPlugState(P));
  smartPlugSwitch(P, false);
  TEST_ASSERT_FALSE(getPlugState(P));
}

// s_plugState is assigned after every guard, so a dropped switch must leave
// the previous state showing. If this ever regressed, the UI and the control
// loop would believe a fridge was off while it was still running.
static void test_state_is_unchanged_when_the_switch_was_dropped(void) {
  configurePlug(P);
  smartPlugSwitch(P, true);
  TEST_ASSERT_TRUE(getPlugState(P));

  g_smartPlugs[P].offCode = 0;      // OFF now hits the code==0 guard
  smartPlugSwitch(P, false);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
  TEST_ASSERT_TRUE(getPlugState(P));   // still reporting ON, correctly
}

static void test_state_query_for_an_out_of_range_index_is_off(void) {
  TEST_ASSERT_FALSE(getPlugState(MAX_SMART_PLUGS));
  TEST_ASSERT_FALSE(getPlugState(200));
}

static void test_slots_keep_independent_state(void) {
  configurePlug(0);
  configurePlug(1);
  smartPlugSwitch(0, true);
  TEST_ASSERT_TRUE(getPlugState(0));
  TEST_ASSERT_FALSE(getPlugState(1));
}

// ============================================================

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_index_beyond_the_last_slot_never_transmits);
  RUN_TEST(test_unconfigured_plug_with_both_codes_zero_never_transmits);
  RUN_TEST(test_unassigned_function_never_transmits);
  RUN_TEST(test_missing_off_code_blocks_only_the_off_direction);
  RUN_TEST(test_missing_on_code_blocks_only_the_on_direction);

  RUN_TEST(test_on_and_off_select_their_own_codes);
  RUN_TEST(test_rf_parameters_reach_the_transmitter_unmodified);
  RUN_TEST(test_transmit_repeats_ten_times_on_the_configured_pin);
  RUN_TEST(test_receiver_is_re_enabled_after_the_burst);

  RUN_TEST(test_state_defaults_to_off);
  RUN_TEST(test_state_follows_a_successful_switch);
  RUN_TEST(test_state_is_unchanged_when_the_switch_was_dropped);
  RUN_TEST(test_state_query_for_an_out_of_range_index_is_off);
  RUN_TEST(test_slots_keep_independent_state);

  return UNITY_END();
}
