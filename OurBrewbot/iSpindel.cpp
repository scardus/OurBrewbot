/*
 * iSpindel.cpp — iSpindel WiFi hydrometer integration
 *
 * Handles incoming POST data from iSpindel devices. Matches by device ID
 * (primary key) or name (fallback), converts Plato→SG if needed, updates
 * runtime state, and auto-registers new devices into free slots.
 */

#include "iSpindel.h"
#include "Log.h"
#include <ArduinoJson.h>

// ============================================================
// ISPINDEL RECEIVE
// POST /iSpindel — iSpindel sends: name, ID, temperature, gravity, battery, RSSI
// ============================================================

void handleiSpindelPost(const String& body) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err) {
    logMsg("[ISPINDEL] Parse error: %s", err.c_str());
    return;
  }

  const char* name    = doc["name"]        | "";
  uint32_t    id      = doc["ID"]          | 0;
  float       temp    = doc["temperature"] | 0.0f;
  float       sg      = doc["gravity"]     | 0.0f;
  float       battery = doc["battery"]     | 0.0f;
  int         rssi    = doc["RSSI"]        | 0;

  // Match by device ID first (primary key), then by name as fallback
  int matched = -1;
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (id > 0 && g_iSpindels[i].id == id) {
      matched = i;
      break;
    }
  }
  if (matched < 0) {
    for (int i = 0; i < MAX_ISPINDELS; i++) {
      if (strlen(name) > 0 && strcmp(g_iSpindels[i].name, name) == 0
          && strcmp(g_iSpindels[i].name, "None") != 0) {
        matched = i;
        break;
      }
    }
  }

  if (matched >= 0) {
    // Convert Plato to SG if device is configured for Plato output
    if (g_iSpindels[matched].unit == 1) {
      sg = 1.0f + (sg / (258.6f - (sg / 258.2f * 227.1f)));
    }
    // Update runtime data
    g_iSpindels[matched].sg          = sg;
    g_iSpindels[matched].temperature = temp;
    g_iSpindels[matched].battery     = battery;
    g_iSpindels[matched].rssi        = rssi;

    // Sync name/ID if changed
    bool configChanged = false;
    if (id > 0 && g_iSpindels[matched].id != id) {
      g_iSpindels[matched].id = id;
      configChanged = true;
    }
    if (strlen(name) > 0 && strcmp(g_iSpindels[matched].name, name) != 0) {
      strlcpy(g_iSpindels[matched].name, name, sizeof(g_iSpindels[matched].name));
      configChanged = true;
    }
    if (configChanged) saveiSpindelConfig();

    logMsg("[ISPINDEL] Slot %d (%s): SG=%.4f T=%.1f Batt=%.2fV RSSI=%d",
      matched, g_iSpindels[matched].name, sg, temp, battery, rssi);
    return;
  }

  // New iSpindel — try to register in first free slot
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (strcmp(g_iSpindels[i].name, "None") == 0 || strlen(g_iSpindels[i].name) == 0) {
      // At registration unit is unknown — store raw value, user sets unit via admin tab
      strlcpy(g_iSpindels[i].name, name, sizeof(g_iSpindels[i].name));
      g_iSpindels[i].id          = id;
      g_iSpindels[i].sg          = sg;
      g_iSpindels[i].temperature = temp;
      g_iSpindels[i].battery     = battery;
      g_iSpindels[i].rssi        = rssi;
      g_iSpindels[i].collectData = true;
      logMsg("[ISPINDEL] Registered %s (ID:%u) in slot %d", name, id, i);
      saveiSpindelConfig();
      return;
    }
  }

  logMsg("[ISPINDEL] Ignored %s (ID:%u) - no free slots", name, id);
}
