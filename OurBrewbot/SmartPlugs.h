#pragma once
/*
 * SmartPlugs.h — RF smart plug control (RCSwitch)
 */

#include "Config.h"
#include <RCSwitch.h>

// RCSwitch instance (defined in SmartPlugs.cpp)
extern RCSwitch g_rcSwitch;

// Transmit RF code to switch a plug on or off
void smartPlugSwitch(uint8_t plugIndex, bool on);

// Get current state of a plug (on/off)
bool getPlugState(uint8_t plugIndex);

// Re-transmit current state of all active plugs
void refreshPlugStates();

// Enter plug programming/learning mode
void enterPlugProgramMode(uint8_t plugIndex);

// RF transmission via RCSwitch
void rfTransmit(uint32_t code, uint8_t bits, uint16_t delayUs, uint8_t protocol);
