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

// True while the RF sniffer page is polling (set in WebAPI.cpp). The receiver
// is only switched on then.
extern bool g_rfSniffActive;

// How often a plug is re-sent a state it already has. The plugs can't confirm
// a command, so this puts right a plug that missed one, or was switched by its
// own button or power-cycled. A change of state is always sent straight away.
// Just under a minute: the control loop runs every 10 s and the send time is
// taken after the ~0.5 s burst, so a full 60000 would only be passed on the
// seventh pass, making it every 70 s. 55000 gives a resend every 60 s.
#define PLUG_RESEND_MS 55000

// Set a plug on or off. Called by the control loop every 10 s: the RF code is
// only transmitted when the state changes, or every PLUG_RESEND_MS otherwise.
void smartPlugSwitch(uint8_t plugIndex, bool on);

// Send a plug its OFF code whatever its role, even when it has none. Used
// before a plug's settings change, and by the emergency switch-off.
void smartPlugForceOff(uint8_t plugIndex);

// Send every plug its OFF code (smartPlugForceOff() for each). Used before a
// firmware update, a restart or a config reset, so no heater or cooler is
// left running while the controller isn't.
void allPlugsForceOff();

// Hold every plug off (true) or let the control loop switch them again
// (false). While held, smartPlugSwitch() sends nothing. Set for a firmware
// upload and cleared if the upload fails; a successful one ends in a restart.
void holdPlugsOff(bool hold);

// Get current state of a plug (on/off)
bool getPlugState(uint8_t plugIndex);

// RF transmission via RCSwitch
void rfTransmit(uint32_t code, uint8_t bits, uint16_t delayUs, uint8_t protocol);
