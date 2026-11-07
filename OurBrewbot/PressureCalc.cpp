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

#include "PressureCalc.h"
#include <math.h>

bool pressureCalibrationValid(const PressureCalibration& calibration) {
  if (calibration.count < 2 || calibration.count > PRESSURE_MAX_POINTS) return false;
  if (calibration.points[0].psi != 0.0f) return false;

  for (uint8_t i = 0; i < calibration.count; i++) {
    const PressurePoint& point = calibration.points[i];
    if (!isfinite(point.psi) || point.psi < 0.0f || point.psi > PRESSURE_MAX_PSI ||
        point.raw >= PRESSURE_ADC_MAX) return false;
    if (i > 0 && (point.psi <= calibration.points[i - 1].psi ||
                  point.raw <= calibration.points[i - 1].raw)) return false;
  }
  return true;
}

PressureReading pressureConvert(const PressureCalibration& calibration, uint16_t raw) {
  PressureReading reading = { PRESSURE_UNAVAILABLE, raw, NAN };
  if (!pressureCalibrationValid(calibration)) return reading;
  if (raw >= PRESSURE_ADC_MAX) {
    reading.state = PRESSURE_ADC_LIMIT;
    return reading;
  }
  if (raw + PRESSURE_ZERO_MARGIN < calibration.points[0].raw) {
    reading.state = PRESSURE_BELOW_RANGE;
    return reading;
  }

  // Small excursions below the averaged zero point are ADC noise. Larger
  // ones stay below range rather than being mistaken for an empty vessel.
  if (raw <= calibration.points[0].raw) {
    reading.state = PRESSURE_IN_RANGE;
    reading.psi = 0.0f;
    return reading;
  }

  // Find the first point above the reading. Stop at the last point so that
  // extrapolation uses the final segment, not a line from atmospheric zero.
  uint8_t last = calibration.count - 1;
  uint8_t upper = 1;
  while (upper < last && raw > calibration.points[upper].raw) upper++;
  const PressurePoint& low  = calibration.points[upper - 1];
  const PressurePoint& high = calibration.points[upper];
  reading.psi = low.psi + (high.psi - low.psi) * (raw - low.raw) / (high.raw - low.raw);
  reading.state = raw > calibration.points[last].raw ? PRESSURE_ABOVE_RANGE : PRESSURE_IN_RANGE;
  return reading;
}

bool pressureReadingFresh(uint32_t now, uint32_t sampledAt) {
  return (uint32_t)(now - sampledAt) < PRESSURE_STALE_MS;
}

bool pressureStartCapture(PressureCapture& capture, uint32_t now, uint16_t raw) {
  if (capture.active || raw >= PRESSURE_ADC_MAX) return false;
  capture.active       = true;
  capture.samples      = 1;
  capture.sum          = raw;
  capture.lastSampleMs = now;
  return true;
}

PressureCaptureResult pressureCaptureSample(PressureCapture& capture, uint32_t now,
                                            uint16_t raw, uint16_t& average) {
  if (!capture.active) return PRESSURE_CAPTURE_CANCELLED;
  if ((uint32_t)(now - capture.lastSampleMs) < PRESSURE_CAPTURE_INTERVAL_MS)
    return PRESSURE_CAPTURE_WAITING;
  if (raw >= PRESSURE_ADC_MAX) {
    // Averaging a clipped sample would understate its unknown true value.
    // Discard the whole capture so it cannot become a calibration point.
    pressureCancelCapture(capture);
    return PRESSURE_CAPTURE_CANCELLED;
  }
  capture.lastSampleMs = now;
  capture.sum += raw;
  capture.samples++;
  if (capture.samples < PRESSURE_CAPTURE_SAMPLES) return PRESSURE_CAPTURE_WAITING;
  average = (uint16_t)((capture.sum + PRESSURE_CAPTURE_SAMPLES / 2) / PRESSURE_CAPTURE_SAMPLES);
  capture.active = false;
  return PRESSURE_CAPTURE_DONE;
}

void pressureCancelCapture(PressureCapture& capture) {
  capture.active = false;
  capture.samples = 0;
  capture.sum = 0;
}
