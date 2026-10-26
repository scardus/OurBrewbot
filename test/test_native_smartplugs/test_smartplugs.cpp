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

// Native (host) tests for the RF smart-plug switch path in
// OurBrewbot/SmartPlugs.cpp.
//
// smartPlugSwitch() has five early-returns that are silent by design - a
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

// ---- millis(), for the once-a-minute resend ----
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
    s_plugSent[i]   = false;
    s_plugSentAt[i] = 0;
  }
  rcTestReset();
  test_setMillis(0);
  g_rfSniffActive = false;
  s_plugsHeldOff  = false;
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

// A plug with only its ON code learnt must never be switched on: there would
// be no OFF code to switch it off again, so a heater or fridge would stay on
// for good. Before the fix the ON was sent and the OFF silently dropped.
static void test_a_plug_without_an_off_code_is_never_switched_on(void) {
  configurePlug(P);
  g_smartPlugs[P].offCode = 0;

  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);
  TEST_ASSERT_FALSE(getPlugState(P));

  smartPlugSwitch(P, false);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);
}

// The guard is one-sided on purpose: sending OFF is always safe, so a plug
// with only its OFF code learnt is still switched off.
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

// The WiFi task gets a moment before each burst, so a syslog line queued
// just before it is sent rather than lost (0.6.4)
static void test_transmit_yields_before_the_burst(void) {
  configurePlug(P);
  delayTestTotal() = 0;
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_UINT32(10, delayTestTotal());
}

// Normal running only transmits. Before 0.6.3 every transmit switched the
// receiver on, so an idle receiver's noise interrupted the CPU from then on.
static void test_receiver_stays_off_after_the_burst(void) {
  configurePlug(P);
  smartPlugSwitch(P, true);
  TEST_ASSERT_FALSE(g_rfTest.receiveEnabled);
  TEST_ASSERT_FALSE(g_rfTest.transmitEnabled);
}

// While the RF sniffer page is open the receiver is muted for the burst, so
// the sniffer doesn't hear our own transmission, then switched back on.
static void test_receiver_is_re_enabled_after_the_burst_while_sniffing(void) {
  configurePlug(P);
  g_rfSniffActive = true;
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
// RESEND — a change is sent at once, an unchanged state once a minute (0.6.4)
// ============================================================

// The control loop asks every 10 s; before 0.6.4 each ask was an RF burst
static void test_an_unchanged_state_is_not_resent_within_a_minute(void) {
  configurePlug(P);
  test_setMillis(1000);
  smartPlugSwitch(P, false);                 // first ask after boot is sent
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
  for (uint32_t t = 11000; t < 1000 + PLUG_RESEND_MS; t += 10000) {
    test_setMillis(t);
    smartPlugSwitch(P, false);
  }
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
}

static void test_an_unchanged_state_is_resent_after_a_minute(void) {
  configurePlug(P);
  test_setMillis(1000);
  smartPlugSwitch(P, true);
  test_setMillis(1000 + PLUG_RESEND_MS);
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(2, g_rfTest.sendCount);
  TEST_ASSERT_EQUAL_HEX32(0x123456, g_rfTest.lastCode);
}

// On the device the send time is taken after the ~0.5 s burst, but the 10 s
// control loop must still re-send on its sixth pass - every 60 s, not 70 s
static void test_a_plug_asked_every_10_s_is_resent_every_60_s(void) {
  configurePlug(P);
  test_setMillis(1900);                      // sent 0.9 s into the pass
  smartPlugSwitch(P, false);
  for (uint32_t t = 11000; t <= 51000; t += 10000) {
    test_setMillis(t);
    smartPlugSwitch(P, false);
  }
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
  test_setMillis(61000);                     // the sixth pass
  smartPlugSwitch(P, false);
  TEST_ASSERT_EQUAL_INT(2, g_rfTest.sendCount);
}

// Heating/cooling must still start and stop without waiting for the resend
static void test_a_change_of_state_is_sent_at_once(void) {
  configurePlug(P);
  test_setMillis(1000);
  smartPlugSwitch(P, false);
  test_setMillis(11000);
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(2, g_rfTest.sendCount);
  TEST_ASSERT_EQUAL_HEX32(0x123456, g_rfTest.lastCode);
}

// millis() is 0 at boot, so "last sent at 0" must not count as already sent
static void test_the_first_ask_at_boot_is_sent(void) {
  configurePlug(P);
  test_setMillis(0);
  smartPlugSwitch(P, false);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
}

static void test_the_resend_survives_millis_wrapping(void) {
  configurePlug(P);
  test_setMillis(0xFFFFFFFFUL - 5000);
  smartPlugSwitch(P, true);
  test_setMillis(10000);                     // 15 s later, past the wrap
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
  test_setMillis(PLUG_RESEND_MS - 5000);     // a minute later
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(2, g_rfTest.sendCount);
}

// After a force-off (a plug's codes or role changing) the control loop's next
// ask is sent at once, with whatever codes the plug has by then
static void test_the_next_ask_after_a_force_off_is_sent_at_once(void) {
  configurePlug(P);
  test_setMillis(1000);
  smartPlugSwitch(P, false);
  smartPlugForceOff(P);
  g_smartPlugs[P].offCode = 0x654321;        // new codes saved
  test_setMillis(11000);
  smartPlugSwitch(P, false);
  TEST_ASSERT_EQUAL_INT(3, g_rfTest.sendCount);
  TEST_ASSERT_EQUAL_HEX32(0x654321, g_rfTest.lastCode);
}

// ============================================================
// HELD OFF — for a firmware upload
// ============================================================

// The control loop keeps asking for heat or cooling; while held, nothing is sent
static void test_a_held_plug_is_not_switched_on(void) {
  configurePlug(P);
  holdPlugsOff(true);
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);
  TEST_ASSERT_FALSE(getPlugState(P));
}

static void test_a_released_plug_is_switched_again(void) {
  configurePlug(P);
  holdPlugsOff(true);
  holdPlugsOff(false);
  smartPlugSwitch(P, true);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
  TEST_ASSERT_TRUE(getPlugState(P));
}

// A forced OFF still goes out while the plugs are held
static void test_force_off_still_works_while_held(void) {
  configurePlug(P);
  holdPlugsOff(true);
  smartPlugForceOff(P);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
}

static void test_all_plugs_force_off_reaches_every_plug_with_an_off_code(void) {
  configurePlug(0);
  configurePlug(3);
  configurePlug(9);
  g_smartPlugs[3].function = PLUG_FN_UNASSIGNED;   // a plug with no role too
  allPlugsForceOff();
  TEST_ASSERT_EQUAL_INT(3, g_rfTest.sendCount);
}

// ============================================================
// FORCE OFF — reaches a plug whatever its role
// ============================================================

// The case smartPlugSwitch() deliberately ignores: a plug that was ON and then
// lost its role must still be sent its OFF code, or it stays on for good.
static void test_force_off_reaches_a_plug_with_no_role(void) {
  configurePlug(P);
  smartPlugSwitch(P, true);                       // on while it had a role
  g_smartPlugs[P].function = PLUG_FN_UNASSIGNED;  // then unassigned
  rcTestReset();

  smartPlugForceOff(P);
  TEST_ASSERT_EQUAL_INT(1, g_rfTest.sendCount);
  TEST_ASSERT_EQUAL_HEX32(0xABCDEF, g_rfTest.lastCode);
  TEST_ASSERT_FALSE(getPlugState(P));
}

static void test_force_off_uses_the_plugs_rf_parameters(void) {
  configurePlug(P);
  smartPlugForceOff(P);
  TEST_ASSERT_EQUAL_UINT32(24, g_rfTest.lastBits);
  TEST_ASSERT_EQUAL_INT(427,   g_rfTest.lastPulseLength);
  TEST_ASSERT_EQUAL_INT(2,     g_rfTest.lastProtocol);
}

static void test_force_off_sends_nothing_without_an_off_code(void) {
  configurePlug(P);
  g_smartPlugs[P].offCode = 0;
  smartPlugForceOff(P);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);
}

static void test_force_off_ignores_an_out_of_range_index(void) {
  configurePlug(P);
  smartPlugForceOff(MAX_SMART_PLUGS);
  TEST_ASSERT_EQUAL_INT(0, g_rfTest.sendCount);
}

// ============================================================

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_index_beyond_the_last_slot_never_transmits);
  RUN_TEST(test_unconfigured_plug_with_both_codes_zero_never_transmits);
  RUN_TEST(test_unassigned_function_never_transmits);
  RUN_TEST(test_a_plug_without_an_off_code_is_never_switched_on);
  RUN_TEST(test_missing_on_code_blocks_only_the_on_direction);

  RUN_TEST(test_on_and_off_select_their_own_codes);
  RUN_TEST(test_rf_parameters_reach_the_transmitter_unmodified);
  RUN_TEST(test_transmit_repeats_ten_times_on_the_configured_pin);
  RUN_TEST(test_receiver_stays_off_after_the_burst);
  RUN_TEST(test_transmit_yields_before_the_burst);
  RUN_TEST(test_an_unchanged_state_is_not_resent_within_a_minute);
  RUN_TEST(test_an_unchanged_state_is_resent_after_a_minute);
  RUN_TEST(test_a_plug_asked_every_10_s_is_resent_every_60_s);
  RUN_TEST(test_a_change_of_state_is_sent_at_once);
  RUN_TEST(test_the_first_ask_at_boot_is_sent);
  RUN_TEST(test_the_resend_survives_millis_wrapping);
  RUN_TEST(test_the_next_ask_after_a_force_off_is_sent_at_once);
  RUN_TEST(test_receiver_is_re_enabled_after_the_burst_while_sniffing);

  RUN_TEST(test_state_defaults_to_off);
  RUN_TEST(test_state_follows_a_successful_switch);
  RUN_TEST(test_state_is_unchanged_when_the_switch_was_dropped);
  RUN_TEST(test_state_query_for_an_out_of_range_index_is_off);
  RUN_TEST(test_slots_keep_independent_state);

  RUN_TEST(test_force_off_reaches_a_plug_with_no_role);
  RUN_TEST(test_force_off_uses_the_plugs_rf_parameters);
  RUN_TEST(test_force_off_sends_nothing_without_an_off_code);
  RUN_TEST(test_force_off_ignores_an_out_of_range_index);
  RUN_TEST(test_a_held_plug_is_not_switched_on);
  RUN_TEST(test_a_released_plug_is_switched_again);
  RUN_TEST(test_force_off_still_works_while_held);
  RUN_TEST(test_all_plugs_force_off_reaches_every_plug_with_an_off_code);

  return UNITY_END();
}
