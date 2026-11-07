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

#pragma once
/*
 * PressureCalc.h - Local analog pressure conversion and calibration capture.
 * Values without a usable pressure have psi = NAN; JSON must emit null.
 * Pure arithmetic with no hardware access, so any test suite can include it.
 * Pressure.h reads the sensor and uses these functions.
 */

#include <stdint.h>

#define PRESSURE_MAX_POINTS          5
#define PRESSURE_ADC_MAX             1023
#define PRESSURE_MAX_PSI             100.0f
#define PRESSURE_CAPTURE_SAMPLES     10
#define PRESSURE_CAPTURE_INTERVAL_MS 5000UL
#define PRESSURE_STALE_MS            120000UL
// Allow two counts of noise below atmospheric zero. Confirm on real hardware.
#define PRESSURE_ZERO_MARGIN         2

struct PressurePoint {
  float    psi;              // pressure on the reference gauge
  uint16_t raw;              // average ADC reading at that pressure
};

struct PressureCalibration {
  uint8_t       count;
  PressurePoint points[PRESSURE_MAX_POINTS];
};

// The API sends these values as numbers, and pressureLabel() in the WebUI's
// sensors.js picks its label by the same number. Only add new states at the end.
enum PressureReadingState : uint8_t {
  PRESSURE_UNAVAILABLE,
  PRESSURE_BELOW_RANGE,
  PRESSURE_IN_RANGE,
  PRESSURE_ABOVE_RANGE,
  PRESSURE_ADC_LIMIT,
};

struct PressureReading {
  PressureReadingState state;
  uint16_t             raw;
  float                psi;
};

// A draft capture never changes the calibration used for live readings.
struct PressureCapture {
  bool     active;
  uint8_t  samples;
  uint32_t sum;
  uint32_t lastSampleMs;
};

// Check the zero point, sender rating and increasing pressure/ADC values.
bool pressureCalibrationValid(const PressureCalibration& calibration);
// Interpolate between points, or estimate above the last point. A reading
// well below zero is reported separately because it may be a sensor fault.
PressureReading pressureConvert(const PressureCalibration& calibration, uint16_t raw);
// A previously obtained reading expires after two missed minute samples.
// The caller must separately track whether any reading has been obtained.
bool pressureReadingFresh(uint32_t now, uint32_t sampledAt);

enum PressureCaptureResult : uint8_t {
  PRESSURE_CAPTURE_WAITING,
  PRESSURE_CAPTURE_DONE,
  PRESSURE_CAPTURE_CANCELLED,
};

// Start with the first ADC sample immediately. Refuse a second start while
// capturing, and refuse a saturated input whose actual voltage is unknown.
bool pressureStartCapture(PressureCapture& capture, uint32_t now, uint16_t raw);

// Accept one sample every five seconds. The tenth completes at 45 seconds
// with regular calls. Only DONE writes the rounded average; CANCELLED means
// the capture is no longer running, including after a saturated sample.
PressureCaptureResult pressureCaptureSample(PressureCapture& capture, uint32_t now,
                                            uint16_t raw, uint16_t& average);

// Discard the partial capture. The caller's working calibration is unchanged.
void pressureCancelCapture(PressureCapture& capture);
