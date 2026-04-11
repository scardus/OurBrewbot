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

// Poll for Tilt BLE advertisements via HM-10 AT+DISI? scan
void checkTilt();

// Process a received Tilt reading (isPro: auto-detected as Tilt Pro)
void processTiltReading(uint8_t colour, float sg, float tempC, bool isPro);

// Tilt UUID prefixes by colour (standard Tilt iBeacon UUIDs)
const char* getTiltUUID(uint8_t colour);

// Get colour name string
const char* getTiltColourName(uint8_t colour);
