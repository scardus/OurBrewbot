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
#include "Log.h"
#include "Notify.h"
#include "Pins.h"
#include <math.h>
#include <stdio.h>

PressureCalibration g_pressureDraft = {};
PressureCapture g_pressureCapture = {};
PressureCaptureFailure g_pressureCaptureFailure = PRESSURE_CAPTURE_NO_FAILURE;
float g_pressureCapturePsi = 0;

static PressureReading s_reading = {PRESSURE_UNAVAILABLE, 0, NAN};
static uint32_t s_readingAt = 0;
static bool s_haveReading = false;
static const uint32_t PRESSURE_READ_INTERVAL_MS = 60000UL;
// The state the last fault warning or recovery line described, so each is
// logged once when the reading changes instead of every minute.
static PressureReadingState s_loggedState = PRESSURE_UNAVAILABLE;

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

// ============================================================
// LOG LINES - [PSI], in the same style as the [TEMP] and [TILT] lines
// ============================================================

void pressureOwnerName(char* out, size_t size) {
  int fermenter = pressureFermenter();
  if (fermenter < 0) snprintf_P(out, size, PSTR("Not assigned"));
  else snprintf_P(out, size, PSTR("F%d (%s)"), fermenter, g_fermenters[fermenter].fermenterName);
}

static bool pressureIsFault(PressureReadingState state) {
  return state == PRESSURE_BELOW_ZERO || state == PRESSURE_ADC_LIMIT;
}

// Log each new reading. A fault is also logged once as a warning when it
// starts, and once more when the reading is back to normal.
static void pressureLogReading() {
  char owner[PRESSURE_OWNER_NAME_SIZE];
  pressureOwnerName(owner, sizeof(owner));
  unsigned raw = s_reading.raw;
  switch (s_reading.state) {
    case PRESSURE_CALIBRATED:
      logMsg("[PSI] %s: %.1f PSI (raw %u)", owner, s_reading.psi, raw);
      break;
    case PRESSURE_ESTIMATE:
      logMsg("[PSI] %s: %.1f PSI, estimated above the top calibration point (raw %u)",
             owner, s_reading.psi, raw);
      break;
    case PRESSURE_BELOW_ZERO:
      logMsg("[PSI] %s: below zero (raw %u)", owner, raw);
      break;
    case PRESSURE_ADC_LIMIT:
      logMsg("[PSI] %s: at full scale, pressure unknown (raw %u)", owner, raw);
      break;
    default:
      logMsg("[PSI] %s: no calibration yet (raw %u)", owner, raw);
      break;
  }

  if (s_reading.state == s_loggedState) return;
  if (s_reading.state == PRESSURE_BELOW_ZERO)
    logMsgL(SYSLOG_WARNING, "[PSI] %s: pressure sensor needs checking - reading below zero (raw %u)",
            owner, raw);
  else if (s_reading.state == PRESSURE_ADC_LIMIT)
    logMsgL(SYSLOG_WARNING, "[PSI] %s: pressure sensor needs checking - input at full scale (raw %u)",
            owner, raw);
  else if (pressureIsFault(s_loggedState))
    logMsgL(SYSLOG_NOTICE, "[PSI] %s: pressure sensor reading normally again (%.1f PSI)",
            owner, s_reading.psi);
  s_loggedState = s_reading.state;
}

// Tell the notifications (see Notify.h) whether the latest reading shows a
// fault. Called after every new reading, and only while a fermenter uses the
// sensor, as for the other sensors. With no calibration there is no reading
// to judge, so nothing is reported.
static void pressureReportToNotify() {
  if (pressureFermenter() < 0 || s_reading.state == PRESSURE_UNAVAILABLE) return;
  bool fault = (s_reading.state == PRESSURE_BELOW_ZERO || s_reading.state == PRESSURE_ADC_LIMIT);
  notifySensorState(NOTIFY_SENSOR_PRESSURE, 0, fault, 0.0f, s_reading.state);
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
  g_pressureCaptureFailure = PRESSURE_CAPTURE_NO_FAILURE;
  s_haveReading = false;
  s_loggedState = PRESSURE_UNAVAILABLE;
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
  g_pressureCapturePsi = psi;
  g_pressureCaptureFailure = PRESSURE_CAPTURE_NO_FAILURE;
  logMsgL(SYSLOG_NOTICE, "[PSI] Calibration: capturing point %u at %.1f PSI (%u s)",
          (unsigned)(g_pressureDraft.count + 1), psi,
          (unsigned)((PRESSURE_CAPTURE_SAMPLES - 1) * PRESSURE_CAPTURE_INTERVAL_MS / 1000));
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
    unsigned point = g_pressureDraft.count + 1;
    if (result == PRESSURE_CAPTURE_CANCELLED) {
      g_pressureCaptureFailure = PRESSURE_CAPTURE_ADC_LIMIT;
      logMsgL(SYSLOG_WARNING, "[PSI] Calibration: point %u at %.1f PSI failed - the input reached full scale",
              point, g_pressureCapturePsi);
    }
    if (result == PRESSURE_CAPTURE_DONE) {
      // Refuse a point that would reverse the calibration slope. The previous
      // completed draft points remain available so this point can be retried.
      if (g_pressureDraft.count && average <= g_pressureDraft.points[g_pressureDraft.count - 1].raw) {
        g_pressureCaptureFailure = PRESSURE_CAPTURE_NOT_INCREASING;
        logMsgL(SYSLOG_WARNING,
                "[PSI] Calibration: point %u at %.1f PSI failed - raw %u is not above the previous point (raw %u)",
                point, g_pressureCapturePsi, (unsigned)average,
                (unsigned)g_pressureDraft.points[g_pressureDraft.count - 1].raw);
      } else {
        g_pressureDraft.points[g_pressureDraft.count++] = {g_pressureCapturePsi, average};
        logMsgL(SYSLOG_NOTICE, "[PSI] Calibration: point %u captured - %.1f PSI = raw %u",
                point, g_pressureCapturePsi, (unsigned)average);
      }
    }
  }
  if (!s_haveReading || (uint32_t)(now - s_readingAt) >= PRESSURE_READ_INTERVAL_MS) {
    uint16_t raw;
    if (pressureReadRaw(raw)) {
      s_reading = pressureConvert(g_pressureCalibration, raw);
      s_readingAt = now;
      s_haveReading = true;
      pressureLogReading();
      pressureReportToNotify();
    }
  }
}
