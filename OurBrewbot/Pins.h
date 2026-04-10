#pragma once
/*
 * Pins.h — Hardware pin assignments
 *
 * Adjust these to match your specific OurBrewbot hardware version.
 *
 * Hardware labels:
 *   "Green Jack" = primary probe bus (Bus 1)
 *   "Black Jack" = secondary probe bus (Bus 2)
 *   "TxTr"       = RF transmitter for smart plugs
 *   "Onboard"    = internal temperature probe
 *   "BLE"        = Bluetooth (not available on ESP8266 without bridge)
 *   "PSI"        = pressure sensor
 */

// Pin definitions use raw GPIO numbers for portability across board targets
// (NodeMCU D-pin labels are not available on the generic ESP8266 board)

// OneWire temperature probe buses
#define PIN_ONE_WIRE_BUS1   0     // GPIO0  = D3 — Green Jack, primary bus
#define PIN_ONE_WIRE_BUS2   2     // GPIO2  = D4 — Black Jack, secondary bus

// RF transmitter for 433MHz smart plugs (fs1000a antenna data line)
#define PIN_RF_TRANSMIT     4     // GPIO4  = D2

// Built-in LED (active LOW on most ESP8266 boards)
#define PIN_LED             16     // LED_BUILTIN on NodeMCU

// RF receiver for code learning (MX-RM-5V data line)
#define PIN_RF_RECEIVE      14    // GPIO14 = D5

// BLE module (KeyeStudio Bluetooth 4.0 v2 / HM-10 compatible)
// Module TX → ESP RX on D7 (GPIO13), Module RX → ESP TX on D6 (GPIO12)
#define PIN_BLE_RX          13    // GPIO13 = D7 — BLE module TX connects here
#define PIN_BLE_TX          12    // GPIO12 = D6 — BLE module RX connects here

// Pressure sensor (PSI) — analog input
#define PIN_PRESSURE        A0    // A0 is defined on both generic and NodeMCU

// Optional: double-reset detect uses a virtual "pin" via LittleFS flag
