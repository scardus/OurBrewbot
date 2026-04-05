/*
 * SmartPlugs.cpp — 433MHz RF smart plug control
 * Uses RCSwitch library for reliable protocol timing
 */

#include "SmartPlugs.h"
#include "Pins.h"
#include "Log.h"

// RCSwitch instance — handles all RF protocol timing
RCSwitch g_rcSwitch;

// Track plug states
static bool s_plugState[MAX_SMART_PLUGS] = {false};

// ============================================================
// RF TRANSMISSION via RCSwitch
// ============================================================

void rfTransmit(uint32_t code, uint8_t bits, uint16_t delayUs, uint8_t protocol) {
  // Disable receiver during transmit to avoid self-reception
  g_rcSwitch.disableReceive();
  g_rcSwitch.enableTransmit(PIN_RF_TRANSMIT);
  g_rcSwitch.setProtocol(protocol);
  g_rcSwitch.setPulseLength(delayUs);
  g_rcSwitch.setRepeatTransmit(10);
  g_rcSwitch.send(code, bits);
  g_rcSwitch.disableTransmit();
  // Re-enable receiver if sniffing was active
  g_rcSwitch.enableReceive(digitalPinToInterrupt(PIN_RF_RECEIVE));
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

  logMsg("[PLUG] Plug %d %s (code 0x%06X, %d-bit, %dus)",
    plugIndex, on ? "ON" : "OFF", code, plug.bits, plug.delayLength);

  rfTransmit(code, plug.bits, plug.delayLength, plug.protocol);

  s_plugState[plugIndex] = on;
}

// ============================================================
// PLUG STATE
// ============================================================

bool getPlugState(uint8_t plugIndex) {
  if (plugIndex >= MAX_SMART_PLUGS) return false;
  return s_plugState[plugIndex];
}

void refreshPlugStates() {
  // Re-transmit current state of all active plugs
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    if (g_smartPlugs[i].function == PLUG_FN_UNASSIGNED) continue;
    if (g_smartPlugs[i].onCode == 0) continue;
    smartPlugSwitch(i, s_plugState[i]);
    delay(50);  // Small gap between transmissions
  }
}

// ============================================================
// PROGRAMMING MODE
// ============================================================

void enterPlugProgramMode(uint8_t plugIndex) {
  if (plugIndex >= MAX_SMART_PLUGS) return;

  logMsg("[PLUG] Put Smartplug in Program Mode then Press On Button");
  logMsg("[PLUG] Waiting for plug to enter program mode...");
  logMsg("[PLUG] Manual programming: use REST API to set plug codes");
  logMsg("[PLUG] POST to /controller with SmartPlug configuration for slot %d", plugIndex);
}
