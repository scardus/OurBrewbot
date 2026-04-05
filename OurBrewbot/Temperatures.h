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
#ifdef ENABLE_BUS2
extern OneWire        g_oneWireBus2;
extern DallasTemperature g_sensors2;
#endif

// Poll all connected DS18B20 probes and update g_probes[].temperature
void pollTemperatures();

// Scan OneWire buses and register any new probes found
void scanBuses();

// Periodic scan: discover new probes and report inactive ones
void periodicProbeScan();

// Clean up duplicate probes caused by old truncated addresses
void cleanupDuplicateProbes();

// Map probe temperature readings to fermenter beer/ambient slots
void allocateProbeTemperatures();

// Get temperature for a probe slot by index
float getTempByIndex(uint8_t index);

// Fast single-probe read by OneWire address string
float getTempQuick(const char* addressStr);

// Convert OneWire address bytes to the hex string format used in JSON
// e.g. {0x28,0x80,0x55,0x55,0xa0,0x08,0x00} → "28805555a0080"
String addressToString(const DeviceAddress& addr);

// Convert hex string back to DeviceAddress bytes
bool stringToAddress(const char* str, DeviceAddress& addr);

// Get beer temperature for a fermenter (returns -127 if no probe assigned)
float getBeerTemp(uint8_t fermenterIndex);

// Get ambient/fridge temperature for a fermenter
float getAmbientTemp(uint8_t fermenterIndex);

// Get control temperature (beer preferred, ambient fallback)
float getControlTemp(uint8_t fermenterIndex);

// Convert temperature between units
float toDisplayTemp(float celsius);
float toCelsius(float displayTemp);
