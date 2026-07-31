#pragma once
/*
 * iSpindel.h — iSpindel Bluetooth/WiFi hydrometer integration
 *
 * The iSpindel POSTs JSON to /iSpindel via WiFi. Fields received:
 *   name, ID, temperature, temp_units, gravity, battery, RSSI
 *
 * Devices are matched by ID (primary) or name (fallback) and mapped
 * to a slot in g_iSpindels[]. New devices are auto-registered into
 * the first free slot. Config persistence is handled by Config.cpp.
 *
 * Incoming temperatures are normalised to Celsius on receipt using the
 * payload's temp_units field, so g_iSpindels[].temperature is ALWAYS in
 * Celsius — the same contract every other temperature source in the
 * firmware follows (see Tilt.cpp, which converts its Fahrenheit broadcast
 * the same way). Display conversion happens later, in toDisplayTemp().
 */

#include "Config.h"

// Process an incoming iSpindel POST body (called from WebAPI)
void handleiSpindelPost(const String& body);

// Clamp sg/temp to physically plausible ranges, zeroing (and logging) anything
// outside them. temp MUST already be in Celsius — the range is a Celsius range.
// name/id are only used for the log message. Extracted from handleiSpindelPost
// so it can be unit tested directly.
void validateiSpindelValues(float& sg, float& temp, const char* name, const char* id);

// Convert a Plato reading to Specific Gravity.
float platoToSG(float plato);

// Convert an incoming iSpindel temperature to Celsius using the payload's
// temp_units field ("C", "F" or "K"). An empty or unrecognised unit is treated
// as Celsius, the firmware's canonical internal unit.
float iSpindelTempToCelsius(float temp, const char* units);
