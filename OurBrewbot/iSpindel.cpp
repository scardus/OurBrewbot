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

// Normalise an incoming temperature to Celsius, which is what the rest of the
// firmware stores and calculates in. The iSpindel/GravityMon payload says which
// unit it used in its "temp_units" field: "C", "F" or "K".
//
// Only the first character is checked, so "F" and "Fahrenheit" both work. An
// empty or unrecognised unit falls back to Celsius — that keeps the behaviour
// of devices that send no unit at all exactly as it was.
float iSpindelTempToCelsius(float temp, const char* units) {
  if (units == nullptr) return temp;

  switch (units[0]) {
    case 'F':
    case 'f':
      return (temp - 32.0f) * 5.0f / 9.0f;
    case 'K':
    case 'k':
      return temp - 273.15f;
    default:
      return temp;   // already Celsius (or unit not supplied)
  }
}

// Reject physically impossible values — guards against corrupted payloads or wrong unit config.
// temp is expected in Celsius: convert with iSpindelTempToCelsius() before calling.
void validateiSpindelValues(float& sg, float& temp, const char* name, const char* id) {
  if (sg != 0.0f && (sg < 0.900f || sg > 1.200f)) {
    logMsg("[ISPINDEL] %s (ID:%s): gravity %.4f out of range, ignoring", name, id, sg);
    sg = 0.0f;
  }
  if (temp != 0.0f && (temp < -40.0f || temp > 80.0f)) {
    logMsg("[ISPINDEL] %s (ID:%s): temperature %.1f out of range, ignoring", name, id, temp);
    temp = 0.0f;
  }
}

float platoToSG(float plato) {
  return 1.0f + (plato / (258.6f - (plato / 258.2f * 227.1f)));
}

// Which gravity unit did the device say it was sending? GravityMon puts "G"
// (Specific Gravity) or "P" (Plato) in the payload's "gravity-unit" field; an
// iSpindel sends no such field at all.
//
// Only the first character is checked, so "G", "SG", "P" and "Plato" all work.
// Returns -1 when the device declared nothing usable, leaving the choice to
// the caller (see iSpindel.h for the full rule).
int8_t iSpindelDeclaredGravityUnit(const char* gravityUnit) {
  if (gravityUnit == nullptr) return -1;

  switch (gravityUnit[0]) {
    case 'G':
    case 'g':
    case 'S':   // "SG"
    case 's':
      return ISPINDEL_UNIT_SG;
    case 'P':
    case 'p':
      return ISPINDEL_UNIT_PLATO;
    default:
      return -1;   // empty or unrecognised - the device told us nothing
  }
}

// An iSpindel cannot declare its unit, so Brewfather's convention is to read a
// "[SG]" suffix on the device name as "this one is calibrated to Specific
// Gravity". Compared case-insensitively, so "[sg]" works too. The suffix must
// be at the END of the name - the four characters are checked in place rather
// than with strcasecmp(), which the native test toolchain does not provide.
bool iSpindelNameDeclaresSG(const char* name) {
  if (name == nullptr) return false;

  size_t len = strlen(name);
  if (len < 4) return false;

  const char* tail = name + len - 4;   // the last four characters
  return tail[0] == '['
      && (tail[1] == 'S' || tail[1] == 's')
      && (tail[2] == 'G' || tail[2] == 'g')
      && tail[3] == ']';
}

// Log-friendly name for a gravity unit.
static const char* gravityUnitName(uint8_t unit) {
  return (unit == ISPINDEL_UNIT_PLATO) ? "Plato" : "SG";
}

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

  const char* name        = doc["name"]         | "";
  String      idStr       = doc["ID"].as<String>();
  const char* id          = idStr.c_str();
  float       temp        = doc["temperature"]  | 0.0f;
  const char* tempUnits   = doc["temp_units"]   | "";
  uint32_t    interval    = doc["interval"]     | 0;
  float       sg          = doc["gravity"]      | 0.0f;
  float       battery     = doc["battery"]      | 0.0f;
  int         rssi        = doc["RSSI"]         | 0;
  float       angle       = doc["angle"]        | 0.0f;
  float       velocity    = doc["velocity"]     | 0.0f;
  float       corrGravity = doc["corr-gravity"] | 0.0f;
  float       runTime     = doc["run-time"]     | 0.0f;
  const char* gravityUnit = doc["gravity-unit"] | "";

  // Normalise to Celsius before anything else touches the value. Doing it here
  // means the range check below, the tempAdjust offset (a Celsius delta) and
  // both storage paths further down all work in the same unit.
  float rawTemp = temp;                              // kept only for the log line
  temp = iSpindelTempToCelsius(temp, tempUnits);

  // Match by device ID first (primary key), then by name as fallback.
  // This happens before the gravity is validated because the slot carries the
  // unit the reading is in, and the reading has to be in SG to be validated.
  int matched = -1;
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (strlen(id) > 0 && strcmp(g_iSpindels[i].id, id) == 0) {
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

  // Work out which unit this gravity reading is in. A device that declares one
  // is believed over the stored setting - it is telling us what it just sent -
  // and the slot is re-configured to match further down.
  int8_t  declared    = iSpindelDeclaredGravityUnit(gravityUnit);
  uint8_t unit        = ISPINDEL_UNIT_PLATO;
  bool    unitChanged = false;

  if (matched >= 0) {
    unit        = (declared >= 0) ? (uint8_t)declared : g_iSpindels[matched].unit;
    unitChanged = (declared >= 0 && unit != g_iSpindels[matched].unit);
  } else {
    // A brand new device: believe its declaration, else the "[SG]" name
    // convention, else assume Plato - what an undeclared iSpindel is taken to
    // be sending. The user can change it in the admin tab afterwards.
    unit = (declared >= 0) ? (uint8_t)declared
                           : (iSpindelNameDeclaresSG(name) ? ISPINDEL_UNIT_SG
                                                           : ISPINDEL_UNIT_PLATO);
  }

  // Convert Plato to SG BEFORE validating: the range check is an SG range, so
  // a Plato reading would fail it and be thrown away. Zero means "field not
  // sent" and must stay zero - platoToSG(0) is exactly 1.0000, which would
  // read as a real gravity rather than a missing one.
  if (unit == ISPINDEL_UNIT_PLATO) {
    if (sg != 0.0f)         sg          = platoToSG(sg);
    if (corrGravity > 0.0f) corrGravity = platoToSG(corrGravity);
  }

  validateiSpindelValues(sg, temp, name, id);

  if (matched >= 0) {
    // Apply calibration offsets
    sg   += g_iSpindels[matched].sgAdjust;
    temp += g_iSpindels[matched].tempAdjust;
    if (corrGravity > 0.0f) corrGravity += g_iSpindels[matched].sgAdjust;

    // Update runtime data
    g_iSpindels[matched].sg          = sg;
    g_iSpindels[matched].temperature = temp;
    g_iSpindels[matched].battery     = battery;
    g_iSpindels[matched].rssi        = rssi;
    g_iSpindels[matched].angle       = angle;
    g_iSpindels[matched].velocity    = velocity;
    g_iSpindels[matched].corrGravity = corrGravity;
    g_iSpindels[matched].runTime     = runTime;
    strlcpy(g_iSpindels[matched].gravityUnit, gravityUnit, sizeof(g_iSpindels[matched].gravityUnit));
    g_iSpindels[matched].lastSeen    = millis();

    // Sync name/ID if changed
    bool configChanged = false;
    if (unitChanged) {
      logMsg("[ISPINDEL] Slot %d (%s) reports %s, was configured as %s - updating slot",
        matched, g_iSpindels[matched].name, gravityUnitName(unit),
        gravityUnitName(g_iSpindels[matched].unit));
      g_iSpindels[matched].unit = unit;
      configChanged = true;
    }
    if (strlen(id) > 0 && strcmp(g_iSpindels[matched].id, id) != 0) {
      strlcpy(g_iSpindels[matched].id, id, sizeof(g_iSpindels[matched].id));
      configChanged = true;
    }
    if (strlen(name) > 0 && strcmp(g_iSpindels[matched].name, name) != 0) {
      strlcpy(g_iSpindels[matched].name, name, sizeof(g_iSpindels[matched].name));
      configChanged = true;
    }
    if (configChanged) saveiSpindelConfig();

    logMsg("[ISPINDEL] Slot %d (%s) ID:%s SG=%.4f Corr=%.4f Unit=%s (dev:%s) T=%.1fC (raw %.1f%s) Angle=%.1f Vel=%.4f Batt=%.2fV RSSI=%d Interval=%us Runtime=%.1fs",
      matched, g_iSpindels[matched].name, id, sg, corrGravity, gravityUnitName(unit), gravityUnit, temp, rawTemp, tempUnits, angle, velocity, battery, rssi, interval, runTime);
    return;
  }

  // New iSpindel — try to register in first free slot
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (strcmp(g_iSpindels[i].name, "None") == 0 || strlen(g_iSpindels[i].name) == 0) {
      // The gravity unit resolved above is stored with the slot, so the user
      // sees what was assumed and can correct it from the admin tab. Both the
      // gravity and the temperature are already in the firmware's own units.
      strlcpy(g_iSpindels[i].name, name, sizeof(g_iSpindels[i].name));
      strlcpy(g_iSpindels[i].id, id, sizeof(g_iSpindels[i].id));
      g_iSpindels[i].unit        = unit;
      g_iSpindels[i].sg          = sg;
      g_iSpindels[i].temperature = temp;
      g_iSpindels[i].battery     = battery;
      g_iSpindels[i].rssi        = rssi;
      g_iSpindels[i].angle       = angle;
      g_iSpindels[i].velocity    = velocity;
      g_iSpindels[i].corrGravity = corrGravity;
      g_iSpindels[i].runTime     = runTime;
      strlcpy(g_iSpindels[i].gravityUnit, gravityUnit, sizeof(g_iSpindels[i].gravityUnit));
      g_iSpindels[i].collectData = true;
      logMsg("[ISPINDEL] Registered %s (ID:%s) in slot %d as %s", name, id, i, gravityUnitName(unit));
      saveiSpindelConfig();
      return;
    }
  }

  logMsg("[ISPINDEL] Ignored %s (ID:%s) - no free slots", name, id);
}
