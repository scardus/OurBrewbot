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
 *
 * Incoming gravities follow the same contract: g_iSpindels[].sg and
 * .corrGravity are ALWAYS Specific Gravity, converted from Plato on receipt.
 * Which unit arrived is decided per POST:
 *   1. the payload's "gravity-unit" field, if the device sends one
 *      (GravityMon sends "G" or "P"; it is authoritative and re-configures
 *      the slot when it disagrees with the stored setting),
 *   2. otherwise a "[SG]" suffix on the device name, the Brewfather
 *      convention for iSpindels, which cannot declare a unit,
 *   3. otherwise Plato, which is what Brewfather assumes for an
 *      undeclared iSpindel.
 * The user can override the stored unit from the admin tab afterwards.
 */

#include "Config.h"

// Process an incoming iSpindel POST body (called from WebAPI)
void handleiSpindelPost(const String& body);

// Clamp sg/temp to physically plausible ranges, zeroing (and logging) anything
// outside them. Both MUST already be in the firmware's canonical units — temp
// in Celsius and sg in Specific Gravity — because the ranges are expressed in
// those units. name/id are only used for the log message. Extracted from
// handleiSpindelPost so it can be unit tested directly.
void validateiSpindelValues(float& sg, float& temp, const char* name, const char* id);

// Convert a Plato reading to Specific Gravity.
float platoToSG(float plato);

// Which gravity unit did the device declare in its "gravity-unit" field?
// Returns ISPINDEL_UNIT_SG, ISPINDEL_UNIT_PLATO, or -1 when the device said
// nothing (an empty, null or unrecognised field). GravityMon sends "G" or "P";
// an iSpindel sends no field at all.
int8_t iSpindelDeclaredGravityUnit(const char* gravityUnit);

// Brewfather's legacy convention for iSpindels, which cannot declare a unit:
// a device name ending in "[SG]" means the calibration polynomial was fitted
// to Specific Gravity. Case-insensitive. Anything else is assumed to be Plato.
bool iSpindelNameDeclaresSG(const char* name);

// Convert an incoming iSpindel temperature to Celsius using the payload's
// temp_units field ("C", "F" or "K"). An empty or unrecognised unit is treated
// as Celsius, the firmware's canonical internal unit.
float iSpindelTempToCelsius(float temp, const char* units);
