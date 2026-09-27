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

// On-device runner smoke test - runs on the real ESP8266 over serial, NOT on
// this dev machine.
//
// Uploading this REPLACES the running firmware on the device for the
// duration of the test run (`pio test -e nodemcuv2_test --upload-port COM6`).
// Do not run against the production controller without explicit, separate
// go-ahead.
//
// This was the placeholder that established the on-device pattern; the real
// suites are now test_device_rtc and test_device_fs. It earns its keep as
// the cheapest way to tell "board, port or upload is broken" apart from "a
// test failed" - run it first when a device run misbehaves, since it asserts
// almost nothing and so can only fail for environmental reasons.

#include <Arduino.h>
#include <unity.h>

void test_board_boots_and_reports_uptime(void) {
  // Deliberately trivial: the assertion is that the runner reached this
  // point on real hardware at all.
  TEST_ASSERT_TRUE(millis() >= 0);
}

void setup() {
  delay(2000);  // let the board settle after upload before tests start
  UNITY_BEGIN();
  RUN_TEST(test_board_boots_and_reports_uptime);
  UNITY_END();
}

void loop() {}
