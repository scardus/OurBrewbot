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

/*
 * SmartPlugs.cpp — 433MHz RF smart plug control
 * Uses RCSwitch library for reliable protocol timing
 */

#include "SmartPlugs.h"
#include "Pins.h"
#include "Log.h"

// RCSwitch instance — handles all RF protocol timing
RCSwitch g_rcSwitch;

// True while the RF sniffer page is polling (managed in WebAPI.cpp)
bool g_rfSniffActive = false;

// Track plug states
static bool s_plugState[MAX_SMART_PLUGS] = {false};

// When each plug was last transmitted to. s_plugSent says whether it has been
// sent anything yet: a plain time can't, as millis() starts at 0 after a boot.
static bool          s_plugSent[MAX_SMART_PLUGS]   = {false};
static unsigned long s_plugSentAt[MAX_SMART_PLUGS] = {0};

// ============================================================
// RF TRANSMISSION via RCSwitch
// ============================================================

void rfTransmit(uint32_t code, uint8_t bits, uint16_t delayUs, uint8_t protocol) {
  // Let the WiFi task send anything already queued (e.g. the syslog line just
  // logged) first. The burst below busy-waits for ~0.5 s without yielding, and
  // a datagram queued just before it was lost - which is why no "Plug 0" line
  // ever reached syslog while plug 1 transmitted straight after it.
  delay(10);

  // Disable receiver during transmit to avoid self-reception
  g_rcSwitch.disableReceive();
  g_rcSwitch.enableTransmit(PIN_RF_TRANSMIT);
  g_rcSwitch.setProtocol(protocol);
  g_rcSwitch.setPulseLength(delayUs);
  g_rcSwitch.setRepeatTransmit(10);
  g_rcSwitch.send(code, bits);
  g_rcSwitch.disableTransmit();
  // Re-enable the receiver only if the RF sniffer is in use. Left on, an idle
  // 433 MHz receiver outputs constant noise and every edge interrupts the CPU.
  if (g_rfSniffActive) {
    g_rcSwitch.enableReceive(digitalPinToInterrupt(PIN_RF_RECEIVE));
  }
}

// ============================================================
// SMART PLUG SWITCH
// ============================================================

void smartPlugSwitch(uint8_t plugIndex, bool on) {
  if (plugIndex >= MAX_SMART_PLUGS) return;

  SmartPlugConfig& plug = g_smartPlugs[plugIndex];

  // Check plug is configured
  if (plug.onCode == 0 && plug.offCode == 0) return;
  if (plug.function == PLUG_FN_UNASSIGNED) return;

  uint32_t code = on ? plug.onCode : plug.offCode;

  if (code == 0) return;

  // Each transmit blocks the loop for ~0.5 s, so a plug already in the wanted
  // state is only re-sent once a minute. Before 0.6.4 every plug was re-sent
  // every 10 s, which kept the loop blocked ~10% of the time with two plugs.
  bool changed = !s_plugSent[plugIndex] || on != s_plugState[plugIndex];
  if (!changed && millis() - s_plugSentAt[plugIndex] < PLUG_RESEND_MS) return;

  rfTransmit(code, plug.bits, plug.delayLength, plug.protocol);

  logMsg("[PLUG] Plug %d %s (code 0x%06X, %d-bit, %dus)",
    plugIndex, on ? "ON" : "OFF", code, plug.bits, plug.delayLength);

  s_plugState[plugIndex]  = on;
  s_plugSent[plugIndex]   = true;
  s_plugSentAt[plugIndex] = millis();
}

// ============================================================
// FORCE OFF
// ============================================================

// smartPlugSwitch() ignores a plug with no role, which is right for the
// control loop. But a plug that was ON when it lost its role, moved to another
// fermenter or was given new codes would then never be sent an OFF, and would
// stay on for good. This sends the OFF code whatever the plug's role.
void smartPlugForceOff(uint8_t plugIndex) {
  if (plugIndex >= MAX_SMART_PLUGS) return;

  SmartPlugConfig& plug = g_smartPlugs[plugIndex];
  if (plug.offCode == 0) return;   // no OFF code learnt - nothing to send

  rfTransmit(plug.offCode, plug.bits, plug.delayLength, plug.protocol);

  logMsg("[PLUG] Plug %d forced OFF (code 0x%06X, %d-bit, %dus)",
    plugIndex, plug.offCode, plug.bits, plug.delayLength);

  s_plugState[plugIndex] = false;
  // The plug's codes or role may be about to change, so have the control loop
  // send its wanted state on its next pass rather than waiting for the resend
  s_plugSent[plugIndex]  = false;
}

// ============================================================
// PLUG STATE
// ============================================================

bool getPlugState(uint8_t plugIndex) {
  if (plugIndex >= MAX_SMART_PLUGS) return false;
  return s_plugState[plugIndex];
}

