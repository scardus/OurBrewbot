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

// Native (host) tests for the seconds-since-boot clock in
// OurBrewbot/Uptime.cpp - above all, that it keeps counting when millis()
// wraps back to 0 after 49.7 days, which is the whole point of it.
//
// Uptime.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test. That also puts its file-static
// running total in scope, so setUp() can put the clock back to boot.

#include <unity.h>
#include <cstdint>

// ---- millis(), settable per test ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// The code under test
#include "../../OurBrewbot/Uptime.cpp"

static const uint32_t ONE_DAY_MS = 86400000UL;

void setUp(void) {
  s_lastMs  = 0;
  s_totalMs = 0;
  test_setMillis(0);
}

void tearDown(void) {}

// ============================================================

static void test_the_clock_starts_at_zero(void) {
  uptimeTick();
  TEST_ASSERT_EQUAL_UINT32(0, uptimeSeconds());
}

static void test_the_clock_counts_seconds(void) {
  test_setMillis(5999);
  uptimeTick();
  TEST_ASSERT_EQUAL_UINT32(5, uptimeSeconds());
  test_setMillis(61000);
  uptimeTick();
  TEST_ASSERT_EQUAL_UINT32(61, uptimeSeconds());
}

// A read between ticks includes the time since the last tick, so a stamp taken
// in the middle of a long loop pass is still right
static void test_a_read_between_ticks_is_current(void) {
  test_setMillis(10000);
  uptimeTick();
  test_setMillis(17500);   // no tick yet
  TEST_ASSERT_EQUAL_UINT32(17, uptimeSeconds());
}

// The reason the clock exists: millis() goes from 0xFFFFFFFF back to 0 after
// 49.7 days, and the uptime must carry on counting up regardless
static void test_the_clock_carries_on_when_millis_wraps(void) {
  // Step up to just before the wrap one day at a time, as loop() would
  for (uint32_t ms = ONE_DAY_MS; ms < 0xFFFFFFFFUL - ONE_DAY_MS; ms += ONE_DAY_MS) {
    test_setMillis(ms);
    uptimeTick();
  }
  test_setMillis(0xFFFFFFFFUL - 999);   // 1 s before the wrap
  uptimeTick();
  uint32_t before = uptimeSeconds();
  TEST_ASSERT_EQUAL_UINT32(4294966, before);

  test_setMillis(9000);                 // 10 s later, past the wrap
  uptimeTick();
  TEST_ASSERT_EQUAL_UINT32(before + 10, uptimeSeconds());
}

static void test_the_clock_keeps_counting_past_fifty_days(void) {
  for (int day = 1; day <= 60; day++) {
    test_setMillis((uint32_t)((uint64_t)day * ONE_DAY_MS));   // wraps once, near day 50
    uptimeTick();
  }
  TEST_ASSERT_EQUAL_UINT32(60UL * 86400UL, uptimeSeconds());
}

// 0 means "never" for the stamps taken with uptimeStamp(), so a stamp taken
// in the first second after boot must not be 0
static void test_a_stamp_is_never_zero(void) {
  test_setMillis(0);
  uptimeTick();
  TEST_ASSERT_EQUAL_UINT32(1, uptimeStamp());
  test_setMillis(999);
  TEST_ASSERT_EQUAL_UINT32(1, uptimeStamp());
}

static void test_a_stamp_is_the_uptime_after_the_first_second(void) {
  test_setMillis(42000);
  uptimeTick();
  TEST_ASSERT_EQUAL_UINT32(42, uptimeStamp());
}

// ============================================================

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_the_clock_starts_at_zero);
  RUN_TEST(test_the_clock_counts_seconds);
  RUN_TEST(test_a_read_between_ticks_is_current);
  RUN_TEST(test_the_clock_carries_on_when_millis_wraps);
  RUN_TEST(test_the_clock_keeps_counting_past_fifty_days);
  RUN_TEST(test_a_stamp_is_never_zero);
  RUN_TEST(test_a_stamp_is_the_uptime_after_the_first_second);

  return UNITY_END();
}
