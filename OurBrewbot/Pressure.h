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
 * Pressure.h - The local pressure sensor on A0: which fermenter it belongs
 * to, its live reading and the unfinished calibration kept in RAM. All A0
 * reads happen in Pressure.cpp. The arithmetic is in PressureCalc.h, which
 * needs no hardware, so it can be tested on its own.
 */

#include "PressureCalc.h"

// Why pressureBeginPoint() did or didn't start a capture, so the API can tell
// the user what to fix.
enum PressureBeginResult : uint8_t {
  PRESSURE_BEGIN_OK,
  PRESSURE_BEGIN_DISABLED,       // the sensor is switched off
  PRESSURE_BEGIN_BUSY,           // a capture is already running
  PRESSURE_BEGIN_DRAFT_FULL,     // the draft already has PRESSURE_MAX_POINTS points
  PRESSURE_BEGIN_BAD_PSI,        // not 0 PSI first, not higher than the last point, or over the limit
  PRESSURE_BEGIN_ADC_LIMIT,      // the input is at full scale, so its true value is unknown
};

extern PressureCalibration g_pressureDraft;
extern PressureCapture g_pressureCapture;
// Sent by the API as a number, like PressureReadingState.
enum PressureCaptureFailure : uint8_t {
  PRESSURE_CAPTURE_NO_FAILURE,
  PRESSURE_CAPTURE_ADC_LIMIT,
  PRESSURE_CAPTURE_NOT_INCREASING,
};

extern PressureCaptureFailure g_pressureCaptureFailure;
extern float g_pressureCapturePsi;

// Return the assigned fermenter, or -1 when no fermenter has the sensor.
int pressureFermenter();
// Service the ADC and calibration across loop passes without waiting.
void pressureLoop();
// Return the current reading, with NAN for unavailable or expired pressure.
PressureReading pressureCurrentReading();
// Start capturing the next draft point at the reference gauge's pressure.
PressureBeginResult pressureBeginPoint(float psi);
// Discard unfinished calibration and sampling after a configuration change.
void pressureResetRuntime();
