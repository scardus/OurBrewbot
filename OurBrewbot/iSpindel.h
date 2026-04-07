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
