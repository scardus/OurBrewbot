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
