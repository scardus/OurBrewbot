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
// Recording stand-in for the rc-switch library. SmartPlugs.h declares
// "extern RCSwitch g_rcSwitch", so every suite that reaches SmartPlugs.h (via
// Fermenter.h) needs the type to exist; test_native_smartplugs additionally
// needs to see what was transmitted, since the only observable effect of
// smartPlugSwitch() on real hardware is an RF burst.
//
// Every setter records into g_rfTest rather than touching hardware, so a test
// can assert both that a transmit happened and that the code/bits/pulse/
// protocol carried through from the plug config unmodified.

#include <cstdint>

struct RfTestState {
  int      sendCount;      // how many send() calls since the last reset
  uint32_t lastCode;
  uint32_t lastBits;
  int      lastProtocol;
  int      lastPulseLength;
  int      lastRepeat;
  int      lastTransmitPin;
  bool     receiveEnabled;
  bool     transmitEnabled;
};

static RfTestState g_rfTest = {};

class RCSwitch {
public:
  void enableTransmit(int pin)      { g_rfTest.lastTransmitPin = pin;
                                      g_rfTest.transmitEnabled = true; }
  void disableTransmit()            { g_rfTest.transmitEnabled = false; }
  void enableReceive(int)           { g_rfTest.receiveEnabled  = true; }
  void disableReceive()             { g_rfTest.receiveEnabled  = false; }
  void setProtocol(int p)           { g_rfTest.lastProtocol    = p; }
  void setPulseLength(int p)        { g_rfTest.lastPulseLength = p; }
  void setRepeatTransmit(int r)     { g_rfTest.lastRepeat      = r; }

  void send(uint32_t code, uint32_t bits) {
    g_rfTest.sendCount++;
    g_rfTest.lastCode = code;
    g_rfTest.lastBits = bits;
  }

  // Receive-side surface, present so the RF sniffer code in WebAPI.cpp
  // compiles; no native test reads a code off the air.
  bool     available()      { return false; }
  void     resetAvailable() {}
  uint32_t getReceivedValue()       { return 0; }
  unsigned int getReceivedBitlength()  { return 0; }
  unsigned int getReceivedDelay()      { return 0; }
  unsigned int getReceivedProtocol()   { return 0; }
};

static void rcTestReset() { g_rfTest = RfTestState{}; }
