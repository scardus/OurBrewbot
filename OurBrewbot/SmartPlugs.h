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

// RF transmission via RCSwitch
void rfTransmit(uint32_t code, uint8_t bits, uint16_t delayUs, uint8_t protocol);
