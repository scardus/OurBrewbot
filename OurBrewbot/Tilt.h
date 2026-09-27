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
 * Tilt.h — Tilt Bluetooth hydrometer integration
 *
 * Hardware: KeyeStudio Bluetooth 4.0 v2 (HM-10/CC2541 compatible)
 *   Module TX → ESP GPIO13 (D7), Module RX → ESP GPIO12 (D6)
 *   SoftwareSerial at 9600 baud, AT command interface
 *
 * Tilt hydrometer broadcasts as iBeacon:
 *   UUID encodes colour, Major = temp °F, Minor = SG × 1000 (standard) or × 10000 (Pro)
 */

#include "Config.h"
#include <SoftwareSerial.h>

// BLE serial port (defined in Tilt.cpp)
extern SoftwareSerial g_bleSerial;

// BLE sniff mode flag — pauses Tilt scanning when active
extern bool g_bleSniffActive;

// Initialise BLE module — call once in setup()
void initBLE();

// Non-blocking Tilt scanning: startTiltScan() kicks off an AT+DISI? scan on the
// 5 s tick; serviceTilt() drains the response across loop passes.
void startTiltScan();   // 5 s tick: begin a scan if idle and allowed
void serviceTilt();     // every loop pass: drain an in-flight scan, finish on OK+DISCE or 4 s

// Process a received Tilt reading (isPro: auto-detected as Tilt Pro)
void processTiltReading(uint8_t colour, float sg, float tempC, bool isPro);

// Tilt UUID prefixes by colour (standard Tilt iBeacon UUIDs)
const char* getTiltUUID(uint8_t colour);

// Get colour name string
const char* getTiltColourName(uint8_t colour);
