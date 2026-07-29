#pragma once
/*
 * iSpindel.h — iSpindel Bluetooth/WiFi hydrometer integration
 *
 * The iSpindel POSTs JSON to /iSpindel via WiFi. Fields received:
 *   name, ID, temperature, gravity, battery, RSSI
 *
 * Devices are matched by ID (primary) or name (fallback) and mapped
 * to a slot in g_iSpindels[]. New devices are auto-registered into
 * the first free slot. Config persistence is handled by Config.cpp.
 */

#include "Config.h"

// Process an incoming iSpindel POST body (called from WebAPI)
void handleiSpindelPost(const String& body);

// Clamp sg/temp to physically plausible ranges, zeroing (and logging) anything
// outside them. name/id are only used for the log message. Extracted from
// handleiSpindelPost so it can be unit tested directly.
void validateiSpindelValues(float& sg, float& temp, const char* name, const char* id);

// Convert a Plato reading to Specific Gravity.
float platoToSG(float plato);
