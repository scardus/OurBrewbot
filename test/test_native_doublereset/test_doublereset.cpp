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

// Native (host) tests for OurBrewbot/DoubleReset.cpp - two presses of the
// reset button within 10 s clear the saved WiFi.
//
// DoubleReset.cpp is #included directly so its static marker flag is in scope:
// restart() below clears it, as a real restart clears RAM, while the stub's
// RTC memory (test/stubs/Esp.h) keeps its contents, as the real one does.

#include <unity.h>
#include <cstdint>
#include <cstring>

// ---- millis(), settable per test ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

#include "../../OurBrewbot/DoubleReset.cpp"

// A restart for the given reason, then the check setup() makes first thing
static bool restart(uint32_t reason) {
  s_markerSet = false;
  s_millis    = 0;
  espTestSetResetReason(reason);
  return doubleResetDetected();
}

static uint32_t rtcWord(uint32_t offset) {
  uint32_t value = 0;
  ESP.rtcUserMemoryRead(offset, &value, sizeof(value));
  return value;
}

void setUp(void) {
  memset(g_espRtcMem, 0, sizeof(g_espRtcMem));
  s_markerSet = false;
  s_millis    = 0;
}

void tearDown(void) {}

void test_power_on_is_not_a_double_reset(void) {
  TEST_ASSERT_FALSE(restart(REASON_DEFAULT_RST));
  TEST_ASSERT_EQUAL_UINT32(0, rtcWord(DOUBLE_RESET_OFFSET));
}

void test_one_press_leaves_the_marker(void) {
  TEST_ASSERT_FALSE(restart(REASON_EXT_SYS_RST));
  TEST_ASSERT_EQUAL_UINT32(DOUBLE_RESET_MAGIC, rtcWord(DOUBLE_RESET_OFFSET));
}

void test_two_presses_within_the_window_are_a_double_reset(void) {
  restart(REASON_EXT_SYS_RST);
  test_setMillis(9999);
  doubleResetLoop();
  TEST_ASSERT_TRUE(restart(REASON_EXT_SYS_RST));
  TEST_ASSERT_EQUAL_UINT32(0, rtcWord(DOUBLE_RESET_OFFSET));
}

void test_the_marker_is_forgotten_after_10_seconds(void) {
  restart(REASON_EXT_SYS_RST);
  test_setMillis(DOUBLE_RESET_WINDOW_MS);
  doubleResetLoop();
  TEST_ASSERT_EQUAL_UINT32(0, rtcWord(DOUBLE_RESET_OFFSET));
  // So a later press is a first press again
  TEST_ASSERT_FALSE(restart(REASON_EXT_SYS_RST));
  TEST_ASSERT_EQUAL_UINT32(DOUBLE_RESET_MAGIC, rtcWord(DOUBLE_RESET_OFFSET));
}

void test_a_third_press_starts_counting_again(void) {
  restart(REASON_EXT_SYS_RST);
  TEST_ASSERT_TRUE(restart(REASON_EXT_SYS_RST));
  TEST_ASSERT_FALSE(restart(REASON_EXT_SYS_RST));
}

void test_other_restarts_never_count_and_remove_the_marker(void) {
  const uint32_t others[] = { REASON_WDT_RST, REASON_EXCEPTION_RST, REASON_SOFT_WDT_RST,
                              REASON_SOFT_RESTART, REASON_DEEP_SLEEP_AWAKE, REASON_DEFAULT_RST };
  for (uint32_t reason : others) {
    restart(REASON_EXT_SYS_RST);
    TEST_ASSERT_FALSE(restart(reason));
    TEST_ASSERT_EQUAL_UINT32(0, rtcWord(DOUBLE_RESET_OFFSET));
    // A press after the crash or reboot is a first press
    TEST_ASSERT_FALSE(restart(REASON_EXT_SYS_RST));
    restart(REASON_SOFT_RESTART);
  }
}

void test_rtc_memory_left_over_from_power_on_is_ignored(void) {
  // RTC memory holds whatever it powered up with - even, by chance, the magic
  uint32_t magic = DOUBLE_RESET_MAGIC;
  ESP.rtcUserMemoryWrite(DOUBLE_RESET_OFFSET, &magic, sizeof(magic));
  TEST_ASSERT_FALSE(restart(REASON_DEFAULT_RST));
  TEST_ASSERT_FALSE(restart(REASON_EXT_SYS_RST));
}

void test_the_marker_leaves_crash_records_alone(void) {
  // Crash.cpp's crash record and checkpoint use words 0-54
  for (uint32_t w = 0; w <= 54; w++) {
    uint32_t value = 0xA5A50000u | w;
    ESP.rtcUserMemoryWrite(w, &value, sizeof(value));
  }
  restart(REASON_EXT_SYS_RST);
  restart(REASON_EXT_SYS_RST);
  for (uint32_t w = 0; w <= 54; w++) {
    TEST_ASSERT_EQUAL_UINT32(0xA5A50000u | w, rtcWord(w));
  }
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_power_on_is_not_a_double_reset);
  RUN_TEST(test_one_press_leaves_the_marker);
  RUN_TEST(test_two_presses_within_the_window_are_a_double_reset);
  RUN_TEST(test_the_marker_is_forgotten_after_10_seconds);
  RUN_TEST(test_a_third_press_starts_counting_again);
  RUN_TEST(test_other_restarts_never_count_and_remove_the_marker);
  RUN_TEST(test_rtc_memory_left_over_from_power_on_is_ignored);
  RUN_TEST(test_the_marker_leaves_crash_records_alone);
  return UNITY_END();
}
