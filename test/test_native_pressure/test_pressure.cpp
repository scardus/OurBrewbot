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

#include <unity.h>
#include <math.h>
#include "../../OurBrewbot/PressureCalc.cpp"

static PressureCalibration calibration;

void setUp(void) {
  calibration = { 3, {{0, 160}, {5, 190}, {20, 280}} };
}

void tearDown(void) {}

// ---- Calibration checks ----

static void test_calibration_requires_increasing_finite_points(void) {
  TEST_ASSERT_TRUE(pressureCalibrationValid(calibration));
  calibration.count = 1;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
  calibration.count = 6;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
  calibration.count = 3;
  calibration.points[1].raw = 160;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
  calibration.points[1].raw = 190;
  calibration.points[2].psi = 5;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
  calibration.points[2].psi = NAN;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
  calibration.points[2].psi = 20;
  calibration.points[2].raw = 1023;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
}

static void test_calibration_requires_atmospheric_zero_and_respects_sender_rating(void) {
  calibration.points[0].psi = 1;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
  calibration.points[0].psi = 0;
  calibration.points[1].psi = -1;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
  calibration.points[1].psi = 5;
  calibration.points[2].psi = 200;
  TEST_ASSERT_FALSE(pressureCalibrationValid(calibration));
  calibration.points[2].psi = 100;
  TEST_ASSERT_TRUE(pressureCalibrationValid(calibration));
}

// ---- Conversion ----

static void test_five_points_use_the_middle_segment_and_include_the_highest_point(void) {
  calibration = {5, {{0,160}, {5,190}, {10,230}, {15,270}, {20,320}}};
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, pressureConvert(calibration,250).psi);
  TEST_ASSERT_EQUAL(PRESSURE_IN_RANGE, pressureConvert(calibration,320).state);
}

static void test_a_small_dip_below_zero_is_noise_but_a_larger_dip_is_unavailable(void) {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pressureConvert(calibration,159).psi);
  TEST_ASSERT_EQUAL(PRESSURE_IN_RANGE, pressureConvert(calibration,158).state);
  TEST_ASSERT_EQUAL(PRESSURE_BELOW_RANGE, pressureConvert(calibration,157).state);
  TEST_ASSERT_TRUE(isnan(pressureConvert(calibration,157).psi));
}

static void test_interpolates_between_neighbouring_points(void) {
  calibration.points[2].raw = 310;
  PressureReading reading = pressureConvert(calibration, 250);
  TEST_ASSERT_EQUAL(PRESSURE_IN_RANGE, reading.state);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, reading.psi);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, pressureConvert(calibration, 190).psi);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, pressureConvert(calibration, 310).psi);
}

static void test_two_points_and_extrapolation(void) {
  calibration.count = 2;
  PressureReading reading = pressureConvert(calibration, 220);
  TEST_ASSERT_EQUAL(PRESSURE_ABOVE_RANGE, reading.state);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, reading.psi);
}

static void test_unavailable_below_range_and_adc_limit(void) {
  TEST_ASSERT_EQUAL(PRESSURE_BELOW_RANGE, pressureConvert(calibration, 150).state);
  TEST_ASSERT_EQUAL(PRESSURE_IN_RANGE, pressureConvert(calibration, 160).state);
  TEST_ASSERT_EQUAL(PRESSURE_ADC_LIMIT, pressureConvert(calibration, 1023).state);
  TEST_ASSERT_TRUE(isnan(pressureConvert(calibration, 1023).psi));
  calibration.count = 0;
  TEST_ASSERT_TRUE(isnan(pressureConvert(calibration, 190).psi));
  TEST_ASSERT_EQUAL(PRESSURE_UNAVAILABLE, pressureConvert(calibration, 190).state);
}

// ---- Freshness ----

static void test_reading_expires_at_two_minutes_even_across_wrap(void) {
  uint32_t start = 0xfffff000u;
  TEST_ASSERT_TRUE(pressureReadingFresh(start + 119999u, start));
  TEST_ASSERT_FALSE(pressureReadingFresh(start + 120000u, start));
}

// ---- Capture ----

static void test_capture_averages_ten_samples_five_seconds_apart(void) {
  PressureCapture capture = {};
  uint16_t average = 999;
  TEST_ASSERT_TRUE(pressureStartCapture(capture, 100, 100));
  TEST_ASSERT_FALSE(pressureStartCapture(capture, 100, 200));
  TEST_ASSERT_EQUAL(PRESSURE_CAPTURE_WAITING, pressureCaptureSample(capture, 5099, 200, average));
  for (int i = 1; i < 9; i++)
    TEST_ASSERT_EQUAL(PRESSURE_CAPTURE_WAITING, pressureCaptureSample(capture, 100 + i * 5000, 100 + i, average));
  TEST_ASSERT_EQUAL(PRESSURE_CAPTURE_DONE, pressureCaptureSample(capture, 45100, 109, average));
  TEST_ASSERT_EQUAL_UINT16(105, average);
  TEST_ASSERT_FALSE(capture.active);
  TEST_ASSERT_TRUE(pressureStartCapture(capture, 50000, 200));
  for (int i = 1; i < 9; i++)
    pressureCaptureSample(capture, 50000 + i * 5000, 200, average);
  TEST_ASSERT_EQUAL(PRESSURE_CAPTURE_DONE, pressureCaptureSample(capture, 95000, 200, average));
  TEST_ASSERT_EQUAL_UINT16(200, average);
}

static void test_capture_wrap_cancel_and_adc_limit(void) {
  PressureCapture capture = {};
  uint16_t average = 999;
  uint32_t start = 0xfffff000u;
  TEST_ASSERT_FALSE(pressureStartCapture(capture, start, 1023));
  TEST_ASSERT_TRUE(pressureStartCapture(capture, start, 160));
  TEST_ASSERT_EQUAL(PRESSURE_CAPTURE_WAITING, pressureCaptureSample(capture, start + 5000u, 161, average));
  TEST_ASSERT_EQUAL_UINT8(2, capture.samples);
  TEST_ASSERT_EQUAL(PRESSURE_CAPTURE_CANCELLED, pressureCaptureSample(capture, start + 10000u, 1023, average));
  TEST_ASSERT_FALSE(capture.active);
  TEST_ASSERT_EQUAL_UINT16(999, average);
  TEST_ASSERT_TRUE(pressureStartCapture(capture, 0, 160));
  pressureCancelCapture(capture);
  TEST_ASSERT_EQUAL(PRESSURE_CAPTURE_CANCELLED, pressureCaptureSample(capture, 5000, 160, average));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  // Calibration checks
  RUN_TEST(test_calibration_requires_increasing_finite_points);
  RUN_TEST(test_calibration_requires_atmospheric_zero_and_respects_sender_rating);
  // Conversion
  RUN_TEST(test_five_points_use_the_middle_segment_and_include_the_highest_point);
  RUN_TEST(test_a_small_dip_below_zero_is_noise_but_a_larger_dip_is_unavailable);
  RUN_TEST(test_interpolates_between_neighbouring_points);
  RUN_TEST(test_two_points_and_extrapolation);
  RUN_TEST(test_unavailable_below_range_and_adc_limit);
  // Freshness
  RUN_TEST(test_reading_expires_at_two_minutes_even_across_wrap);
  // Capture
  RUN_TEST(test_capture_averages_ten_samples_five_seconds_apart);
  RUN_TEST(test_capture_wrap_cancel_and_adc_limit);
  return UNITY_END();
}
