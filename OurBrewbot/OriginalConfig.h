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
 * OriginalConfig.h — One-time conversion of an original MyBrewbot config
 *
 * The original firmware stores its settings in the same LittleFS files, under
 * the same keys, so they are still there after OurBrewbot is flashed. Several
 * values mean something different in OurBrewbot, though (Power is inverted,
 * plug roles come from the slot number, Tilt colours start at 1, ...). The
 * conversion rewrites those files once, in OurBrewbot's meaning, before any
 * of them is loaded.
 */

#include <Arduino.h>

#define ORIG_DIR          "/orig"
// Report of every change the conversion made, readable from the WebUI's
// file viewer (boot log lines only reach the serial port)
#define ORIG_REPORT_FILE  "/orig/conversion.txt"

// Call first thing in loadAllConfig(). Converts the config files if they were
// written by the original firmware; returns true if it did.
bool convertOriginalConfig();

// True if this boot converted an original config (for the DEFERRED log line
// sent once syslog is running).
bool originalConfigConverted();

// True if the conversion failed to save a file on its last try, so this boot
// runs on default settings (see OriginalConfig.cpp).
bool originalConversionGaveUp();

// The DEFERRED log lines for a conversion that needed a restart or gave up
void logOriginalConversionProblems();

// The 24-bit raw RF code the original sent for a "Dial" plug (group and
// button 1-4); 0 if either is out of range.
uint32_t dialPlugCode(uint8_t group, uint8_t button, bool on);
