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

#include "Pressure.h"
#include "Config.h"
#include "Pins.h"
#include <math.h>

PressureCalibration g_pressureDraft = {};
PressureCapture g_pressureCapture = {};
bool g_pressureCaptureFailed = false;

static PressureReading s_reading = {PRESSURE_UNAVAILABLE, 0, NAN};
static uint32_t s_readingAt = 0;
static bool s_haveReading = false;
static float s_pointPsi = 0;
static const uint32_t PRESSURE_READ_INTERVAL_MS = 60000UL;

// ============================================================
// LIVE READING - a short average of the A0 input
// ============================================================

static const uint8_t  PRESSURE_LIVE_SAMPLES = 10;
static const uint32_t PRESSURE_LIVE_INTERVAL_MS = 10;

static uint8_t  s_samples = 0;
static uint32_t s_sum = 0;
static uint32_t s_lastSampleMs = 0;

// Discard an unfinished reading when the sensor is disabled or reconfigured.
static void pressureCancelReading() {
  s_samples = 0;
  s_sum = 0;
}

// Call once per loop while a reading is needed. Returns true after ten
// samples spaced ten milliseconds apart and writes their rounded average.
// A saturated sample completes immediately with the ADC maximum; averaging
// must not hide the fact that the input voltage exceeded the readable range.
static bool pressureReadRaw(uint16_t& raw) {
  uint32_t now = millis();
  if (s_samples && (uint32_t)(now - s_lastSampleMs) < PRESSURE_LIVE_INTERVAL_MS)
    return false;

  // Spread the samples across loop passes so networking and temperature
  // control can continue while ADC noise is averaged out.
  uint16_t sample = analogRead(PIN_PRESSURE);
  s_lastSampleMs = now;
  if (sample >= PRESSURE_ADC_MAX) {
    raw = PRESSURE_ADC_MAX;
    pressureCancelReading();
    return true;
  }
  s_sum += sample;
  s_samples++;
  if (s_samples < PRESSURE_LIVE_SAMPLES) return false;

  raw = (uint16_t)((s_sum + PRESSURE_LIVE_SAMPLES / 2) / PRESSURE_LIVE_SAMPLES);
  pressureCancelReading();
  return true;
}

// ============================================================
// ASSIGNMENT, CALIBRATION AND THE LOOP
// ============================================================

int pressureFermenter() {
  for (uint8_t i = 0; i < MAX_FERMENTERS; i++)
    if (g_fermenters[i].psiCollect) return i;
  return -1;
}

PressureReading pressureCurrentReading() {
  if (!g_pressureEnabled || !s_haveReading || !pressureReadingFresh(millis(), s_readingAt))
    return {PRESSURE_UNAVAILABLE, 0, NAN};
  return s_reading;
}

void pressureResetRuntime() {
  pressureCancelReading();
  pressureCancelCapture(g_pressureCapture);
  g_pressureDraft = {};
  g_pressureCaptureFailed = false;
  s_haveReading = false;
}

PressureBeginResult pressureBeginPoint(float psi) {
  if (!g_pressureEnabled) return PRESSURE_BEGIN_DISABLED;
  if (g_pressureCapture.active) return PRESSURE_BEGIN_BUSY;
  if (g_pressureDraft.count >= PRESSURE_MAX_POINTS) return PRESSURE_BEGIN_DRAFT_FULL;

  // The first point is atmospheric zero, and each later one is higher than the last.
  bool inOrder = (g_pressureDraft.count == 0)
                   ? psi == 0
                   : psi > g_pressureDraft.points[g_pressureDraft.count - 1].psi;
  if (!isfinite(psi) || psi > PRESSURE_MAX_PSI || !inOrder) return PRESSURE_BEGIN_BAD_PSI;

  // Capture reads A0 itself every five seconds, independently of live averages.
  // With the checks above passed, the only refusal left is a saturated input.
  if (!pressureStartCapture(g_pressureCapture, millis(), analogRead(PIN_PRESSURE)))
    return PRESSURE_BEGIN_ADC_LIMIT;
  s_pointPsi = psi;
  g_pressureCaptureFailed = false;
  return PRESSURE_BEGIN_OK;
}

void pressureLoop() {
  if (!g_pressureEnabled) return;
  uint32_t now = millis();
  if (g_pressureCapture.active &&
      (uint32_t)(now - g_pressureCapture.lastSampleMs) >= PRESSURE_CAPTURE_INTERVAL_MS) {
    uint16_t average;
    PressureCaptureResult result = pressureCaptureSample(g_pressureCapture, now,
                                                         analogRead(PIN_PRESSURE), average);
    if (result == PRESSURE_CAPTURE_CANCELLED) g_pressureCaptureFailed = true;
    if (result == PRESSURE_CAPTURE_DONE) {
      // Refuse a point that would reverse the calibration slope. The previous
      // completed draft points remain available so this point can be retried.
      if (g_pressureDraft.count && average <= g_pressureDraft.points[g_pressureDraft.count - 1].raw)
        g_pressureCaptureFailed = true;
      else g_pressureDraft.points[g_pressureDraft.count++] = {s_pointPsi, average};
    }
  }
  if (!s_haveReading || (uint32_t)(now - s_readingAt) >= PRESSURE_READ_INTERVAL_MS) {
    uint16_t raw;
    if (pressureReadRaw(raw)) {
      s_reading = pressureConvert(g_pressureCalibration, raw);
      s_readingAt = now;
      s_haveReading = true;
    }
  }
}
