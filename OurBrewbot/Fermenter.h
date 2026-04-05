#pragma once
/*
 * Fermenter.h — Fermentation controller
 */

#include "Config.h"
#include "Temperatures.h"
#include "SmartPlugs.h"

// Fermenter status codes (stored in g_fermenters[i].status)
#define STATUS_IDLE      0
#define STATUS_HEATING   1
#define STATUS_COOLING   2
#define STATUS_ALARM     3

// Process all 4 fermenters — called from main loop
void processFermenters();

// Process a single fermenter's control logic
void processSingleFermenter(uint8_t index);

// Check and trigger temperature/SG alarms
void checkFermenterAlarm(uint8_t index);

// Estimate current gravity from OG + attenuation model
float estimateGravity(uint8_t index);

// Get current SG (from Tilt/iSpindel/Plaato, or estimate)
float getCurrentSG(uint8_t index);

// Emergency off — all smart plugs off
void switchOffAll();

// Get attenuation percentage
float getAttenuation(uint8_t index);

// Fermenter power on/off
void setFermenterPower(uint8_t index, bool on);

// Get human-readable status string for REST API / dashboard
const char* getFermenterStatusStr(uint8_t status);

// Set heating/cooling plugs for a fermenter
// Implemented here, called internally and by switchOffAll()
void setFermenterPlugs(uint8_t fermenterIndex, bool heat, bool cool);
