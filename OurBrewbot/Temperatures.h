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
 * Temperatures.h — DS18B20 temperature probe management
 */

#include "Config.h"
#include <DallasTemperature.h>
#include <OneWire.h>

// Hardware bus instances (defined in Temperatures.cpp)
extern OneWire        g_oneWireBus1;
extern DallasTemperature g_sensors1;

extern OneWire        g_oneWireBus2;
extern DallasTemperature g_sensors2;

// Request DS18B20 conversion on all buses (returns immediately, no delay)
void requestTempConversion();

// Read conversion results — call only after the conversion time has elapsed
// (94 << (resolution-9)) ms after requestTempConversion()
void readTempResults();

// Request, wait and read in one go (blocks for the conversion time). Used once
// in setup() so the first control pass has live readings.
void readTemperaturesNow();

// Scan OneWire buses, assign position-based names, register new probes.
// Returns true if any probe config changed (new probe or name updated).
bool scanBuses();

// Periodic scan: discover new probes and report inactive ones
void periodicProbeScan();

// Clean up duplicate probes caused by old truncated addresses
void cleanupDuplicateProbes();

// Convert OneWire address bytes to the hex string format used in JSON
// e.g. {0x28,0xff,0x64,0x1f,0x8b,0x2c,0x00,0x11} → "28ff641f8b2c0011"
// (all 8 bytes, 2 lowercase hex digits each: always 16 characters)
String addressToString(const DeviceAddress& addr);

// Convert hex string back to DeviceAddress bytes
bool stringToAddress(const char* str, DeviceAddress& addr);

// True when `stored` is `addr` as the original MyBrewbot firmware saved it:
// each byte without its leading zero (e.g. "28805555a0080")
bool isUnpaddedAddress(const char* stored, const DeviceAddress& addr);

// Get beer temperature for a fermenter (returns -127 if no probe assigned)
float getBeerTemp(uint8_t fermenterIndex);

// Get the source of the beer temperature reading ("Tilt", "Probe", "iSpindel", or "None")
const char* getBeerTempSource(uint8_t fermenterIndex);

// Get ambient/fridge temperature for a fermenter
float getAmbientTemp(uint8_t fermenterIndex);

// Get control temperature (beer preferred, ambient fallback)
float getControlTemp(uint8_t fermenterIndex);

// Convert temperature between units
float toDisplayTemp(float celsius);
float toCelsius(float displayTemp);

// Delta (span) conversions for values like hysteresis — scale only, no offset
float toDisplayTempDelta(float dCelsius);
float toCelsiusTempDelta(float dDisplay);
