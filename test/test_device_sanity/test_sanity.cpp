// On-device (hardware) test placeholder - runs on the real ESP8266 over
// serial, NOT on this dev machine. See Phase B of the unit-testing plan.
//
// Uploading this REPLACES the running firmware on the device for the
// duration of the test run (`pio test -e nodemcuv2_test --upload-port COM6`).
// Do not run against the production controller without explicit, separate
// go-ahead - this file exists to establish the pattern for future
// hardware-backed tests, not to be run yet.

#include <Arduino.h>
#include <unity.h>

void test_board_boots_and_reports_uptime(void) {
  // Sanity check that the test runner itself is alive on real hardware
  // before any device-specific test logic gets added here.
  TEST_ASSERT_TRUE(millis() >= 0);
}

void setup() {
  delay(2000);  // let the board settle after upload before tests start
  UNITY_BEGIN();
  RUN_TEST(test_board_boots_and_reports_uptime);
  UNITY_END();
}

void loop() {}
