/*
 * Fermenter.cpp — Fermentation temperature controller
 */

#include "Fermenter.h"
#include "SmartPlugs.h"
#include "Profile.h"
#include "Log.h"

// Track compressor delay per fermenter (millis of last cooling stop)
static unsigned long s_lastCoolingStop[MAX_FERMENTERS] = {0};

// Per-fermenter control state for the hysteresis state machine
static uint8_t s_state[MAX_FERMENTERS] = {STATUS_IDLE};  // STATUS_IDLE / STATUS_HEATING / STATUS_COOLING

// ============================================================
// MAIN PROCESSOR
// ============================================================

void processFermenters() {
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (!g_fermenters[i].power) {
      // Fermenter off — ensure all its plugs are off
      setFermenterPlugs(i, false, false);
      g_fermenters[i].status = STATUS_IDLE;
      s_state[i] = STATUS_IDLE;
      continue;
    }
    if (!g_fermenters[i].tempControl) {
      // Temp control paused — plugs off but fermenter still watches for alarms
      setFermenterPlugs(i, false, false);
      g_fermenters[i].status = STATUS_IDLE;
      s_state[i] = STATUS_IDLE;
      checkFermenterAlarm(i);
      continue;
    }
    processSingleFermenter(i);
    checkFermenterAlarm(i);
  }

  // Run active fermentation profiles
  processProfiles();
}

// ============================================================
// SINGLE FERMENTER CONTROL LOGIC
//
// Algorithm (state machine):
//   IDLE    → HEATING  if temp < FloorTemp
//   IDLE    → COOLING  if temp > CeilingTemp (and compressor delay passed)
//   HEATING → IDLE     if temp >= FloorTemp + Hysteresis
//   COOLING → IDLE     if temp <= CeilingTemp - Hysteresis
//
// Hysteresis is applied at the STOP point (inside the safe zone),
// not the trigger point, so the system pushes the temperature well
// into the safe zone before stopping.
// ============================================================

void processSingleFermenter(uint8_t i) {
  float temp = getControlTemp(i);

  // Can't control without a temperature reading
  if (temp < -100.0f) {
    logMsg("[FERM] F%d (%s): no temperature reading", i, g_fermenters[i].fermenterName);
    setFermenterPlugs(i, false, false);
    g_fermenters[i].status = STATUS_IDLE;
    return;
  }

  float ceiling    = g_fermenters[i].ceilingTemp;
  float floor_     = g_fermenters[i].floorTemp;
  float hysteresis = g_fermenters[i].hysteresis;

  switch (s_state[i]) {

    case STATUS_IDLE:
      if (temp < floor_) {
        s_state[i] = STATUS_HEATING;
      } else if (temp > ceiling) {
        // Enforce compressor delay — protects refrigeration compressor
        unsigned long delaySec = (unsigned long)g_fermenters[i].compressorDelay * 60000UL;
        unsigned long now = millis();
        if ((now - s_lastCoolingStop[i]) >= delaySec) {
          s_state[i] = STATUS_COOLING;
        } else {
          logMsg("[FERM] F%d (%s): in compressor delay (%.0fs remaining)",
            i, g_fermenters[i].fermenterName, (delaySec - (now - s_lastCoolingStop[i])) / 1000.0f);
        }
      }
      break;

    case STATUS_HEATING:
      if (temp >= floor_ + hysteresis) {
        s_state[i] = STATUS_IDLE;
      }
      break;

    case STATUS_COOLING:
      if (temp <= ceiling - hysteresis) {
        s_lastCoolingStop[i] = millis();
        s_state[i] = STATUS_IDLE;
      }
      break;
  }

  bool shouldHeat = (s_state[i] == STATUS_HEATING);
  bool shouldCool = (s_state[i] == STATUS_COOLING);

  // Update status
  g_fermenters[i].status = s_state[i];

  // Operate the smart plugs
  setFermenterPlugs(i, shouldHeat, shouldCool);

  // SG-based control (if enabled and gravity source available)
  if (g_fermenters[i].sgControl) {
    float sg = getCurrentSG(i);
    if (sg > 0.5f) {
      // SG reached target — could stop heating (e.g. diacetyl rest trigger)
      if (sg <= (g_fermenters[i].tg + 0.002f)) {
        // Near or past target gravity — notify via serial
        logMsg("[FERM] F%d (%s): SG %.4f near target %.4f",
          i, g_fermenters[i].fermenterName, sg, g_fermenters[i].tg);
      }
    }
  }
}

// ============================================================
// SMART PLUG CONTROL
// ============================================================

void setFermenterPlugs(uint8_t fermenterIndex, bool heat, bool cool) {
  // Map fermenter index to plug function codes
  // F1 Hot/Cold = fermenter 0, F2 = fermenter 1, etc.
  uint8_t hotFn  = PLUG_FN_F1_HOT  + (fermenterIndex * 2);
  uint8_t coldFn = PLUG_FN_F1_COLD + (fermenterIndex * 2);

  for (int p = 0; p < MAX_SMART_PLUGS; p++) {
    if (g_smartPlugs[p].fermenter != fermenterIndex) continue;

    if (g_smartPlugs[p].function == hotFn) {
      smartPlugSwitch(p, heat);
    } else if (g_smartPlugs[p].function == coldFn) {
      smartPlugSwitch(p, cool);
    }
  }
}

// ============================================================
// SWITCH OFF ALL
// ============================================================

void switchOffAll() {
  logMsg("[FERM] SwitchOffAll - emergency off");
  for (int p = 0; p < MAX_SMART_PLUGS; p++) {
    smartPlugSwitch(p, false);
  }
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    g_fermenters[i].status = STATUS_IDLE;
    s_state[i] = STATUS_IDLE;
  }
}

// ============================================================
// ALARM CHECKING
// ============================================================

void checkFermenterAlarm(uint8_t i) {
  if (!g_fermenters[i].power) return;

  float temp = getControlTemp(i);
  if (temp < -100.0f) return;

  float ceiling   = g_fermenters[i].ceilingTemp;
  float floor_    = g_fermenters[i].floorTemp;
  float tolerance = g_fermenters[i].alarmTolerance;

  bool alarmActive = false;

  if (temp > ceiling + tolerance) {
    logMsg("[ALARM] F%d (%s): over temperature threshold! %.1f > %.1f",
      i, g_fermenters[i].fermenterName, temp, ceiling + tolerance);
    alarmActive = true;
  } else if (temp < floor_ - tolerance) {
    logMsg("[ALARM] F%d (%s): under temperature threshold! %.1f < %.1f",
      i, g_fermenters[i].fermenterName, temp, floor_ - tolerance);
    alarmActive = true;
  }

  g_fermenters[i].alarm = alarmActive;

  // To re-add notifications: send HTTP POST to webhook here
  if (alarmActive && g_globalConfig.notifyOn) {
    logMsg("[ALARM] F%d (%s): alarm active", i, g_fermenters[i].fermenterName);
  }
}

// ============================================================
// GRAVITY ESTIMATION
// Uses a simple attenuation model based on time & OG/TG
//
// No change for the first 24 hours (lag phase), then a smooth drop over the next 3 days,
// then a final slow drop over the next 3 days until it reaches TG.
// ============================================================

float estimateGravity(uint8_t i) {
  float og = g_fermenters[i].og;
  float tg = g_fermenters[i].tg;
  float h  = g_fermenters[i].currentHour;
  float range = og - tg;

  if (h <= 24.0f || og <= tg) {
    return og;
  } else if (h <= 96.0f) {
    float p = (h - 24.0f) / 72.0f;
    return og - (p * 0.9f * range);
  } else {
    float p = min(1.0f, (h - 96.0f) / 72.0f);
    return og - ((0.9f + p * 0.1f) * range);
  }
}

float getCurrentSG(uint8_t i) {
  // Priority: Tilt > iSpindel > Plaato > Estimate
  for (int t = 0; t < MAX_TILTS; t++) {
    if (g_tilts[t].active && g_tilts[t].fermenter == i) {
      return g_tilts[t].sg;
    }
  }
  for (int s = 0; s < MAX_ISPINDELS; s++) {
    if (g_iSpindels[s].collectData && g_iSpindels[s].fermenter == i) {
      return g_iSpindels[s].sg;
    }
  }
  return estimateGravity(i);
}

const char* getGravitySource(uint8_t i) {
  // Mirror getCurrentSG() priority chain exactly
  for (int t = 0; t < MAX_TILTS; t++) {
    if (g_tilts[t].active && g_tilts[t].fermenter == i) {
      return "Tilt";
    }
  }
  for (int s = 0; s < MAX_ISPINDELS; s++) {
    if (g_iSpindels[s].collectData && g_iSpindels[s].fermenter == i) {
      return "iSpindel";
    }
  }
  return "Estimated";
}

float getAttenuation(uint8_t i) {
  float og = g_fermenters[i].og;
  float sg = getCurrentSG(i);
  if (og <= 1.0f || sg >= og) return 0.0f;
  return ((og - sg) / (og - 1.0f)) * 100.0f;
}

// ============================================================
// POWER CONTROL
// ============================================================

void setFermenterPower(uint8_t index, bool on) {
  if (index >= MAX_FERMENTERS) return;
  g_fermenters[index].power = on;
  if (!on) {
    setFermenterPlugs(index, false, false);
    g_fermenters[index].status = STATUS_IDLE;
    logMsg("[FERM] F%d (%s): powered off", index, g_fermenters[index].fermenterName);
  }
}

// ============================================================
// STATUS STRING
// ============================================================

const char* getFermenterStatusStr(uint8_t status) {
  switch (status) {
    case STATUS_HEATING: return "Heating";
    case STATUS_COOLING: return "Cooling";
    case STATUS_ALARM:   return "Alarm";
    default:             return "Idle";
  }
}

// ============================================================
// FIELD VALIDATION — shared by WebAPI and MQTT command handler
// ============================================================

bool validateFermenterField(uint8_t idx, const char* key, float value, const char** errMsg) {
  if (errMsg) *errMsg = nullptr;

  if (strcmp(key, "ceiling_temperature") == 0 || strcmp(key, "floor_temperature") == 0) {
    if (value < -20.0f || value > 50.0f) {
      if (errMsg) *errMsg = "temperature out of range (-20 to 50)";
      return false;
    }
    float ceiling = (strcmp(key, "ceiling_temperature") == 0) ? value : g_fermenters[idx].ceilingTemp;
    float flr     = (strcmp(key, "floor_temperature")   == 0) ? value : g_fermenters[idx].floorTemp;
    float hyst    = g_fermenters[idx].hysteresis;
    if (flr >= ceiling) {
      if (errMsg) *errMsg = "floor must be below ceiling";
      return false;
    }
    if ((ceiling - flr) <= 2.0f * hyst) {
      if (errMsg) *errMsg = "safe zone must be wider than 2x hysteresis";
      return false;
    }
  } else if (strcmp(key, "hysteresis") == 0) {
    if (value < 0.0f || value > 10.0f) {
      if (errMsg) *errMsg = "hysteresis out of range (0 to 10)";
      return false;
    }
    if ((g_fermenters[idx].ceilingTemp - g_fermenters[idx].floorTemp) <= 2.0f * value) {
      if (errMsg) *errMsg = "safe zone must be wider than 2x hysteresis";
      return false;
    }
  } else if (strcmp(key, "compressor_delay") == 0) {
    if (value < 0.0f || value > 1440.0f) {
      if (errMsg) *errMsg = "compressor delay out of range (0 to 1440 min)";
      return false;
    }
  } else if (strcmp(key, "og") == 0 || strcmp(key, "tg") == 0) {
    if (value < 0.990f || value > 1.200f) {
      if (errMsg) *errMsg = "gravity out of range (0.990 to 1.200)";
      return false;
    }
  }
  return true;
}

