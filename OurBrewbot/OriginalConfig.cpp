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
 * OriginalConfig.cpp — One-time conversion of an original MyBrewbot config
 *
 * How it works:
 *   1. Detect: the original's jsonFermenter.txt has "BrewServiceSend" and no
 *      "BrewServices". Every OurBrewbot save writes "BrewServices", so a
 *      converted config is never converted again. (The original's other files,
 *      e.g. jsonPlaato.txt, stay on flash after the switch, so they can't be
 *      used to detect it.)
 *   2. Copy the untouched files to /orig/, once. jsonConfig.txt is not copied
 *      and is deleted: it holds the WiFi password and Blynk token in plain
 *      text, and anyone on the network can read files through /fs/file.
 *   3. Convert each file, reading from the copy in /orig/ and writing the live
 *      file. Reading from the copy means a conversion cut short by a power cut
 *      can simply run again: it starts from the same files and gets the same
 *      result. The fermenter file is written LAST, because its "BrewServices"
 *      key is what marks the conversion as done.
 *
 * Every change is logged and added to /orig/conversion.txt.
 *
 * What the original's values mean comes from decompiling it - see
 * ORIGINAL_FIRMWARE_MAP.md (kept outside this repository).
 */

#include "OriginalConfig.h"
#include "Config.h"
#include "Log.h"
#include <stdarg.h>

// A JSON key kept in flash. On the ESP8266 a plain string literal takes RAM for
// as long as the firmware runs, and this code only runs once. (Config.cpp
// keeps its keys in flash the same way.)
#define KEY(name) FPSTR(PSTR(name))

// Present while a conversion is under way: the files in /orig/ are the
// originals, and some live files may already be converted
#define ORIG_IN_PROGRESS  "/orig/converting.txt"

// The files the original firmware writes (each also has a "bkup" twin).
// jsonConfig.txt is left out on purpose - see removeSecrets().
// Kept in flash, like the JSON keys below.
static const char kOriginalFiles[][20] PROGMEM = {
  "jsonGlobal", "jsonFermenter", "jsonProbe", "jsonSmartPlugs", "jsonProfile",
  "jsonProfileSteps", "jsonTilt", "jsoniSpindel", "jsonReBoot", "jsonPressure",
  "jsonPlaato", "jsonRepeater", "jsonchartNames", "jsonchartSeries"
};

static bool s_converted = false;

bool originalConfigConverted() { return s_converted; }

// Settings from one file that the conversion of another file needs
struct OriginalFacts {
  bool    fahrenheit;                  // original "unit" 0 - temperatures saved in F
  uint8_t tempControl[MAX_FERMENTERS]; // 1 beer probe, 2 Tilt, 3 ambient probe, 4 iSpindel
  int8_t  brewServiceSlot;             // g_brewServices slot the original's service maps to, -1 none
  bool    sendsToService[MAX_FERMENTERS];
};

// ============================================================
// HELPERS
// ============================================================

// Logs one line and adds it to the conversion report. The report matters
// because the conversion runs before WiFi starts, so its log lines only reach
// the serial port.
static void report(PGM_P fmt, ...) {
  char line[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf_P(line, sizeof(line), fmt, args);
  va_end(args);
  logMsg("[CFG] %s", line);
  File f = LittleFS.open(ORIG_REPORT_FILE, "a");
  if (f) {
    f.print(line);
    f.print("\n");
    f.close();
  }
}
#define REPORT(fmt, ...) report(PSTR(fmt), ##__VA_ARGS__)

static void livePath(char* buf, size_t size, const char* base, bool backup) {
  snprintf_P(buf, size, PSTR("/%s%s.txt"), base, backup ? "bkup" : "");
}

static void origPath(char* buf, size_t size, const char* base, bool backup) {
  snprintf_P(buf, size, PSTR(ORIG_DIR "/%s%s.txt"), base, backup ? "bkup" : "");
}

// Loads an original file from its copy in /orig/ (primary, then backup).
// `file` is a name like PSTR("jsonGlobal").
static bool loadOriginal(JsonDocument& doc, PGM_P file) {
  char base[20], primary[40], backup[40];
  strncpy_P(base, file, sizeof(base) - 1);
  base[sizeof(base) - 1] = '\0';
  origPath(primary, sizeof(primary), base, false);
  origPath(backup,  sizeof(backup),  base, true);
  if (!LittleFS.exists(primary) && !LittleFS.exists(backup)) return false;
  return loadJsonDocSafe(doc, primary, backup);
}

static bool copyFile(const char* from, const char* to) {
  File in = LittleFS.open(from, "r");
  if (!in) return false;
  File out = LittleFS.open(to, "w");
  if (!out) {
    in.close();
    return false;
  }
  uint8_t buf[128];
  bool ok = true;
  size_t n;
  while ((n = in.readBytes((char*)buf, sizeof(buf))) > 0) {
    if (out.write(buf, n) != n) {
      ok = false;
      break;
    }
  }
  in.close();
  out.close();
  return ok;
}

// ============================================================
// DETECT, COPY, SECRETS
// ============================================================

static bool fermenterFileIsOriginal() {
  if (!LittleFS.exists(FILE_FERMENTER) && !LittleFS.exists(FILE_FERMENTER_BKP)) return false;
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_FERMENTER, FILE_FERMENTER_BKP)) return false;
  return !doc[KEY("BrewServiceSend")].isNull() && doc[KEY("BrewServices")].isNull();
}

// Copies every original file (and backup) to /orig/. A copy left there by an
// earlier conversion is replaced, or removed if the file no longer exists, so
// /orig/ always holds the config that was converted last.
static bool copyOriginalFiles() {
  LittleFS.mkdir(ORIG_DIR);
  for (size_t f = 0; f < sizeof(kOriginalFiles) / sizeof(kOriginalFiles[0]); f++) {
    char base[sizeof(kOriginalFiles[0])];
    memcpy_P(base, kOriginalFiles[f], sizeof(base));
    for (int b = 0; b < 2; b++) {
      char live[40], orig[40];
      livePath(live, sizeof(live), base, b == 1);
      origPath(orig, sizeof(orig), base, b == 1);
      if (LittleFS.exists(live)) {
        if (!copyFile(live, orig)) {
          logMsgL(SYSLOG_ERR, "[CFG] Cannot copy %s to %s", live, orig);
          return false;
        }
      } else if (LittleFS.exists(orig)) {
        LittleFS.remove(orig);
      }
    }
  }
  return true;
}

static void removeSecrets() {
  if (LittleFS.exists(FILE_CONFIG) || LittleFS.exists(FILE_CONFIG_BKP)) {
    LittleFS.remove(FILE_CONFIG);
    LittleFS.remove(FILE_CONFIG_BKP);
    REPORT("Deleted jsonConfig.txt (WiFi password and Blynk token in plain text)");
  }
}

static void readFacts(OriginalFacts& facts) {
  facts.fahrenheit      = false;
  facts.brewServiceSlot = -1;
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    facts.tempControl[i]    = 1;
    facts.sendsToService[i] = false;
  }

  JsonDocument doc;
  if (loadOriginal(doc, PSTR("jsonGlobal"))) {
    facts.fahrenheit = ((doc[KEY("unit")] | 1) == 0);

    // One service for the whole controller: 2 Brewer's Friend, 3 Brewfather,
    // 4 monitor.beer. "Brewservice" is the original's default (unset) ID.
    int service    = doc[KEY("brewservice")] | 0;
    const char* id = doc[KEY("brewserviceid")] | "";
    bool hasId     = strlen(id) > 0 && strcmp_P(id, PSTR("Brewservice")) != 0;
    if (hasId && service == 2) facts.brewServiceSlot = BREW_SERVICE_BREWERS_FRIEND - 1;
    if (hasId && service == 3) facts.brewServiceSlot = BREW_SERVICE_BREWFATHER - 1;
  }
  doc.clear();
  if (loadOriginal(doc, PSTR("jsonFermenter"))) {
    for (int i = 0; i < MAX_FERMENTERS; i++) {
      facts.tempControl[i] = doc[KEY("TempControl")][i] | 1;
      // Inverted like Power: 0 = send, 1 = don't
      facts.sendsToService[i] = ((doc[KEY("BrewServiceSend")][i] | 1) == 0);
    }
  }
}

// ============================================================
// GLOBAL (jsonGlobal.txt)
// ============================================================

static void convertGlobal(const OriginalFacts& facts) {
  JsonDocument doc;
  if (!loadOriginal(doc, PSTR("jsonGlobal"))) return;

  // The Blynk token - nothing in OurBrewbot uses it
  if (strlen(doc[KEY("authcode")] | "") > 0) {
    doc[KEY("authcode")] = "";
    REPORT("Global: Blynk token cleared");
  }
  // Original: 0 = Fahrenheit, 1 = Celsius. OurBrewbot: UNIT_FAHRENHEIT is 2.
  doc[KEY("unit")] = facts.fahrenheit ? UNIT_FAHRENHEIT : UNIT_CELSIUS;
  if (facts.fahrenheit) REPORT("Global: Fahrenheit (unit 0 -> %d)", UNIT_FAHRENHEIT);

  saveJsonDocSafe(doc, FILE_GLOBAL, FILE_GLOBAL_BKP);
}

// ============================================================
// PROBES (jsonProbe.txt)
// ============================================================

// Original probe roles: 1 Discovered (found, not assigned), 2 Beer, 3 Ambient,
// 4 Air, 7 Common Ambient and 8 Common Air (both fed every fermenter). Only 2
// and 3 mean the same in OurBrewbot, where 4 is Tilt and 7 is control.
static uint8_t convertProbeRole(uint8_t role) {
  switch (role) {
    case 2:  return PROBE_FN_BEER;
    case 3:  return PROBE_FN_AMBIENT;
    case 4:  return PROBE_FN_AIR;
    case 7:  return PROBE_FN_AMBIENT;   // OurBrewbot has no "every fermenter" role
    case 8:  return PROBE_FN_AIR;
    default: return PROBE_UNASSIGNED;
  }
}

static void convertProbes() {
  JsonDocument doc;
  if (!loadOriginal(doc, PSTR("jsonProbe"))) return;

  for (int i = 0; i < MAX_PROBES; i++) {
    uint8_t role      = doc[KEY("Function")][i]  | PROBE_UNASSIGNED;
    uint8_t fermenter = doc[KEY("Fermenter")][i] | PROBE_UNASSIGNED;
    uint8_t newRole   = convertProbeRole(role);
    if (newRole == PROBE_UNASSIGNED) {
      fermenter = PROBE_UNASSIGNED;
    } else if (fermenter >= MAX_FERMENTERS) {
      fermenter = 0;   // a "Common" probe saved without a fermenter
    }
    if (newRole != role) {
      REPORT("Probe %d (%s): role %u -> %u", i + 1, doc[KEY("Address")][i] | "", role, newRole);
      if (role == 7) {
        REPORT("Probe %d: was the ambient probe for every fermenter - now for fermenter %u only",
               i + 1, fermenter + 1);
      }
    }
    doc[KEY("Function")][i]  = newRole;
    doc[KEY("Fermenter")][i] = fermenter;
  }

  saveJsonDocSafe(doc, FILE_PROBE, FILE_PROBE_BKP);
}

// ============================================================
// TILTS (jsonTilt.txt)
// ============================================================

// For the report, kept in flash. Same order as TiltColour in Config.h
// (Tilt.cpp has the display names, but would bring the BLE code into the
// config tests).
static const char kTiltColours[MAX_TILTS][7] PROGMEM = {
  "red", "green", "black", "purple", "orange", "blue", "yellow", "pink"
};

static void convertTilts(const OriginalFacts& facts) {
  JsonDocument doc;
  if (!loadOriginal(doc, PSTR("jsonTilt"))) return;

  for (int i = 0; i < MAX_TILT_SLOTS; i++) {
    // Original colours are 1-8 in the same order as OurBrewbot's 0-7
    // (1 = red, 2 = green, ...)
    uint8_t colour = doc[KEY("Address")][i] | PROBE_UNASSIGNED;
    if (colour < 1 || colour > MAX_TILTS) {
      doc[KEY("Address")][i]   = PROBE_UNASSIGNED;
      doc[KEY("Function")][i]  = PROBE_UNASSIGNED;
      doc[KEY("Fermenter")][i] = PROBE_UNASSIGNED;
      continue;
    }
    colour -= 1;
    doc[KEY("Address")][i] = colour;
    char colourName[sizeof(kTiltColours[0])];
    memcpy_P(colourName, kTiltColours[colour], sizeof(colourName));

    // Original role 2 = assigned to its fermenter. Its temperature was only
    // used for control when that fermenter's TempControl chose the Tilt (2);
    // in OurBrewbot that is the Beer role. Without it the Tilt still gives
    // the fermenter its SG.
    uint8_t role      = doc[KEY("Function")][i]  | PROBE_UNASSIGNED;
    uint8_t fermenter = doc[KEY("Fermenter")][i] | PROBE_UNASSIGNED;
    if (role != 2 || fermenter >= MAX_FERMENTERS) {
      fermenter = PROBE_UNASSIGNED;
      role      = PROBE_UNASSIGNED;
    } else {
      role = (facts.tempControl[fermenter] == 2) ? PROBE_FN_BEER : PROBE_UNASSIGNED;
    }
    doc[KEY("Function")][i]  = role;
    doc[KEY("Fermenter")][i] = fermenter;
    if (fermenter != PROBE_UNASSIGNED) {
      REPORT("Tilt %s: fermenter %u, %s", colourName, fermenter + 1,
             role == PROBE_FN_BEER ? "beer temperature" : "gravity only");
    }

    // The original stored adjust = reading - true value and SUBTRACTED it;
    // OurBrewbot adds its adjustment. The original's temperature adjustment
    // is in the display unit, and its SG adjustment in thousandths.
    float tempAdjust = doc[KEY("Temp_Adjust")][i] | 0.0f;
    float sgAdjust   = doc[KEY("SG_Adjust")][i]   | 0.0f;
    if (facts.fahrenheit) tempAdjust = tempAdjust * 5.0f / 9.0f;
    tempAdjust = -tempAdjust;
    sgAdjust   = -sgAdjust / 1000.0f;
    doc[KEY("Temp_Adjust")][i] = tempAdjust;
    doc[KEY("SG_Adjust")][i]   = sgAdjust;
    if (tempAdjust != 0.0f || sgAdjust != 0.0f) {
      REPORT("Tilt %s: calibration now %+.2f C, %+.4f SG", colourName, tempAdjust, sgAdjust);
    }
  }

  saveJsonDocSafe(doc, FILE_TILT, FILE_TILT_BKP);
}

// ============================================================
// iSPINDELS (jsoniSpindel.txt)
// ============================================================

static void convertiSpindels(const OriginalFacts& facts) {
  JsonDocument doc;
  if (!loadOriginal(doc, PSTR("jsoniSpindel"))) return;

  for (int i = 0; i < MAX_ISPINDELS; i++) {
    // The original saved the ID as a number, 0 for an empty slot. OurBrewbot
    // saves it as text, and would read a number as no ID at all.
    char id[16] = "";
    JsonVariant idValue = doc[KEY("ID")][i];
    if (idValue.is<const char*>()) {
      strlcpy(id, idValue.as<const char*>(), sizeof(id));
    } else if (idValue.is<long>() && idValue.as<long>() != 0) {
      snprintf(id, sizeof(id), "%ld", idValue.as<long>());
    }
    doc[KEY("ID")][i] = id;

    uint8_t fermenter = doc[KEY("iSpindelFermenter")][i] | PROBE_UNASSIGNED;
    if (id[0] == '\0' || fermenter >= MAX_FERMENTERS) fermenter = PROBE_UNASSIGNED;
    doc[KEY("iSpindelFermenter")][i]   = fermenter;
    doc[KEY("iSpindelCollectData")][i] = (fermenter != PROBE_UNASSIGNED);

    // Original unit 2 = Plato, anything else SG. OurBrewbot: 0 SG, 1 Plato.
    uint8_t unit = doc[KEY("Unit")][i] | 0;
    doc[KEY("Unit")][i] = (unit == 2) ? ISPINDEL_UNIT_PLATO : ISPINDEL_UNIT_SG;

    // The original only used an iSpindel's temperature for control when its
    // fermenter's TempControl chose the iSpindel (4). The file has no role
    // key, and OurBrewbot's default would make every iSpindel a beer
    // temperature source.
    bool beer = (fermenter != PROBE_UNASSIGNED && facts.tempControl[fermenter] == 4);
    doc[KEY("Function")][i] = beer ? PROBE_FN_BEER : PROBE_UNASSIGNED;

    if (fermenter != PROBE_UNASSIGNED) {
      REPORT("iSpindel %s (ID %s): fermenter %u, %s, %s", doc[KEY("iSpindelName")][i] | "", id, fermenter + 1,
             unit == 2 ? "Plato" : "SG", beer ? "beer temperature" : "gravity only");
    }
  }

  saveJsonDocSafe(doc, FILE_ISPINDEL, FILE_ISPINDEL_BKP);
}

// ============================================================
// SMART PLUGS (jsonSmartPlugs.txt)
// ============================================================

// rc-switch's switchOn()/switchOff(group, button), which the original used for
// "Dial" plugs: a 12-symbol tri-state word - 4 for the group, 4 for the
// button, "FFF", then F for on or 0 for off. rc-switch sends each '0' as bits
// 00 and each 'F' as 01, giving the 24-bit code OurBrewbot sends as a raw code.
uint32_t dialPlugCode(uint8_t group, uint8_t button, bool on) {
  if (group < 1 || group > 4 || button < 1 || button > 4) return 0;
  uint32_t code = 0;
  for (int i = 1; i <= 4; i++) code = (code << 2) | (i == group ? 0 : 1);
  for (int i = 1; i <= 4; i++) code = (code << 2) | (i == button ? 0 : 1);
  for (int i = 0; i < 3; i++)  code = (code << 2) | 1;
  code = (code << 2) | (on ? 1 : 0);
  return code;
}

static void convertSmartPlugs() {
  JsonDocument doc;
  if (!loadOriginal(doc, PSTR("jsonSmartPlugs"))) return;

  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    uint32_t onCode  = doc[KEY("OnCode")][i]  | 0UL;
    uint32_t offCode = doc[KEY("OffCode")][i] | 0UL;

    // Type 1 ("Dial") saved the dial group (Codeset) and the button numbers,
    // not the codes. Every other type saved the raw codes.
    if ((doc[KEY("Type")][i] | 0) == 1 && onCode != 0 && offCode != 0) {
      uint8_t group = doc[KEY("Codeset")][i] | 0;
      onCode  = (onCode  <= 4) ? dialPlugCode(group, onCode,  true)  : 0;
      offCode = (offCode <= 4) ? dialPlugCode(group, offCode, false) : 0;
      if (onCode == 0 || offCode == 0) {
        REPORT("Plug %d: Dial settings not recognised - set its codes again", i + 1);
      } else {
        doc[KEY("Bits")][i] = 24;
        REPORT("Plug %d: Dial group %u -> codes %lu / %lu", i + 1, group,
               (unsigned long)onCode, (unsigned long)offCode);
      }
      doc[KEY("OnCode")][i]  = onCode;
      doc[KEY("OffCode")][i] = offCode;
    }

    // The original fixed each plug's job by its slot: 0 F1 heat, 1 F1 cool,
    // ... 7 F4 cool, 8 and 9 Aux 1 and 2. OurBrewbot's PLUG_FN_ codes are the
    // same numbers, but it reads them from Function, which the original left
    // at 99. A slot without both codes did nothing in the original.
    bool used = (onCode != 0 && offCode != 0);
    doc[KEY("Function")][i]  = used ? i : PLUG_FN_UNASSIGNED;
    doc[KEY("Fermenter")][i] = (used && i < 2 * MAX_FERMENTERS) ? i / 2 : PROBE_UNASSIGNED;
    if (used) {
      if (i < 2 * MAX_FERMENTERS) {
        REPORT("Plug %d: fermenter %d %s", i + 1, i / 2 + 1, (i % 2 == 0) ? "heating" : "cooling");
      } else {
        REPORT("Plug %d: Aux %d", i + 1, i - 2 * MAX_FERMENTERS + 1);
      }
    }
  }

  saveJsonDocSafe(doc, FILE_SMARTPLUGS, FILE_SMARTPLUGS_BKP);
}

// ============================================================
// PROFILE STEPS (jsonProfileSteps.txt)
// ============================================================

#define STEP_EMPTY 0xFF

// Original step types are numbered from 1 in a different order; 0 is an
// empty step. 7 and 8 only existed in the app (the original never acted on
// them), but they are what the user chose.
static uint8_t convertStepType(uint8_t type) {
  switch (type) {
    case 1:  return STEP_TIME_OVER_TEMP;
    case 2:  return STEP_TEMP_OVER_TIME;
    case 3:  return STEP_FREE_RISE;
    case 4:  return STEP_SPECIFIC_GRAVITY;
    case 5:  return STEP_ATTENUATION;
    case 6:  return STEP_TEMP_REACHED;
    case 7:  return STEP_SG_REACHED;
    case 8:  return STEP_ATTN_REACHED;
    default: return STEP_EMPTY;
  }
}

static void convertProfileSteps(const OriginalFacts& facts) {
  JsonDocument doc;
  if (!loadOriginal(doc, PSTR("jsonProfileSteps"))) return;

  int converted[MAX_PROFILES] = {0};
  for (int k = 0; k < MAX_PROFILE_STEPS; k++) {
    uint8_t type    = doc[KEY("StepType")][k] | 0;
    uint8_t newType = convertStepType(type);
    if (newType == STEP_EMPTY) {
      // All zeros is OurBrewbot's empty step - and must not be converted
      // from Fahrenheit (0 F would become -17.8 C)
      doc[KEY("StepType")][k]  = 0;
      doc[KEY("StartTemp")][k] = 0;
      doc[KEY("EndTemp")][k]   = 0;
      doc[KEY("SGTrigger")][k] = 0;
      doc[KEY("Days")][k]      = 0;
      continue;
    }
    doc[KEY("StepType")][k] = newType;

    // SG targets were saved x1000 (1010). An attenuation target is a plain
    // percentage in the same field, and stays as it is.
    float sgTrigger = doc[KEY("SGTrigger")][k] | 0.0f;
    if ((type == 4 || type == 7) && sgTrigger > 2.0f) sgTrigger /= 1000.0f;
    doc[KEY("SGTrigger")][k] = sgTrigger;

    if (facts.fahrenheit) {
      doc[KEY("StartTemp")][k] = ((doc[KEY("StartTemp")][k] | 0.0f) - 32.0f) * 5.0f / 9.0f;
      doc[KEY("EndTemp")][k]   = ((doc[KEY("EndTemp")][k]   | 0.0f) - 32.0f) * 5.0f / 9.0f;
    }
    converted[k / MAX_STEPS_PER_PROFILE]++;
  }
  for (int p = 0; p < MAX_PROFILES; p++) {
    if (converted[p] > 0) REPORT("Profile %d: %d step(s) converted", p + 1, converted[p]);
  }

  saveJsonDocSafe(doc, FILE_STEPS, FILE_STEPS_BKP);
}

// ============================================================
// BREW SERVICES (jsonBrewServices.txt - OurBrewbot's own file)
// ============================================================

static void convertBrewServices(const OriginalFacts& facts) {
  JsonDocument doc;
  if (!loadOriginal(doc, PSTR("jsonGlobal"))) return;
  int service = doc[KEY("brewservice")] | 0;

  initDefaultBrewServiceConfig();
  if (facts.brewServiceSlot >= 0) {
    BrewServiceConfig& svc = g_brewServices[facts.brewServiceSlot];
    svc.enabled = true;
    strlcpy(svc.serviceId, doc[KEY("brewserviceid")] | "", sizeof(svc.serviceId));
    REPORT("Brew service: %s, ID kept - press its Test button to check it",
           service == 2 ? "Brewer's Friend" : "Brewfather");
  } else if (service == 4) {
    REPORT("Brew service: monitor.beer is not supported yet - not converted");
  }
  saveBrewServiceConfig();
}

// ============================================================
// FERMENTERS (jsonFermenter.txt) - written last
// ============================================================

static void convertFermenters(const OriginalFacts& facts) {
  JsonDocument doc;
  if (!loadOriginal(doc, PSTR("jsonFermenter"))) return;

  REPORT("Alarm tolerance set to 3.0 C on every fermenter (the original's was a percentage)");

  for (int i = 0; i < MAX_FERMENTERS; i++) {
    // Power is inverted: the original's 0 means running, 1 means off
    bool running = ((doc[KEY("Power")][i] | 1) == 0);
    doc[KEY("Power")][i] = running;

    // TempControl and SGControl chose a SOURCE (1-4) in the original; there
    // was no "off". The temperature source is expressed through the probe,
    // Tilt and iSpindel roles instead. SGControl in OurBrewbot is an unused
    // switch.
    if (facts.tempControl[i] == 3) {
      REPORT("Fermenter %d: controlled on its ambient probe before - OurBrewbot uses the beer "
             "probe when there is one", i + 1);
    }
    doc[KEY("TempControl")][i] = true;
    doc[KEY("SGControl")][i]   = false;

    // A percentage of the setpoint in the original; degrees in OurBrewbot
    doc[KEY("AlarmTolerance")][i] = 3.0f;

    // The original moved the switch points OUTWARDS: heat below floor - H,
    // cool above ceiling + H. OurBrewbot heats below its floor and cools above
    // its ceiling, so widening the band by H keeps the same switch-on points.
    // The original also allowed floor = ceiling, which OurBrewbot rejects.
    float ceiling = doc[KEY("CeilingTemp")][i] | 22.0f;
    float floorT  = doc[KEY("FloorTemp")][i]   | 18.0f;
    float hyst    = doc[KEY("Hysteresis")][i]  | 0.0f;
    if (facts.fahrenheit) {
      ceiling = (ceiling - 32.0f) * 5.0f / 9.0f;
      floorT  = (floorT  - 32.0f) * 5.0f / 9.0f;
      hyst    = hyst * 5.0f / 9.0f;
    }
    float low  = (floorT < ceiling) ? floorT : ceiling;
    float high = (floorT < ceiling) ? ceiling : floorT;
    float widen = (hyst > 0.0f) ? hyst : ((high - low < 0.01f) ? 0.5f : 0.0f);
    doc[KEY("FloorTemp")][i]   = low - widen;
    doc[KEY("CeilingTemp")][i] = high + widen;
    doc[KEY("Hysteresis")][i]  = hyst;
    REPORT("Fermenter %d: %s, floor %.2f C, ceiling %.2f C, hysteresis %.2f C", i + 1,
           running ? "on" : "off", low - widen, high + widen, hyst);

    // A running profile is stopped: the original's profile number, hour and
    // step don't carry across reliably. The user starts it again (and can
    // skip to the right step).
    if ((doc[KEY("ProfileRunning")][i] | 0) != 0) {
      REPORT("Fermenter %d: profile %u was running - stopped, start it again", i + 1,
             (unsigned)(doc[KEY("ProfileNo")][i] | 0));
    }
    doc[KEY("ProfileRunning")][i] = false;
    doc[KEY("ProfilePaused")][i]  = false;
    doc[KEY("ProfileNo")][i]      = 0;
    doc[KEY("CurrentStep")][i]    = 0;
    doc[KEY("CurrentHour")][i]    = 0;
  }

  // Each fermenter's services are a bitmask in OurBrewbot (bit N = slot N)
  doc.remove(KEY("BrewServiceSend"));
  JsonArray services = doc[KEY("BrewServices")].to<JsonArray>();
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    bool sends = (facts.brewServiceSlot >= 0 && facts.sendsToService[i]);
    services.add(sends ? (1 << facts.brewServiceSlot) : 0);
    if (sends) REPORT("Fermenter %d: sends to the brew service", i + 1);
  }

  saveJsonDocSafe(doc, FILE_FERMENTER, FILE_FERMENTER_BKP);
}

// ============================================================
// ENTRY POINT
// ============================================================

bool convertOriginalConfig() {
  if (!fermenterFileIsOriginal()) {
    // A conversion that saved the fermenter file but lost power before it
    // could tidy up
    if (LittleFS.exists(ORIG_IN_PROGRESS)) LittleFS.remove(ORIG_IN_PROGRESS);
    return false;
  }
  logMsgL(SYSLOG_NOTICE, "[CFG] Original MyBrewbot config found - converting it");

  // Copy the untouched files only on the first attempt: after a power cut
  // some live files may already be converted.
  if (!LittleFS.exists(ORIG_IN_PROGRESS)) {
    if (!copyOriginalFiles()) {
      logMsgL(SYSLOG_ERR, "[CFG] Original config NOT converted - could not keep a copy");
      return false;
    }
    File marker = LittleFS.open(ORIG_IN_PROGRESS, "w");
    if (marker) marker.close();
  }
  LittleFS.remove(ORIG_REPORT_FILE);   // each attempt writes the whole report
  REPORT("Original MyBrewbot config converted. The untouched files are in /orig/.");

  removeSecrets();

  OriginalFacts facts;
  readFacts(facts);
  convertGlobal(facts);
  convertProbes();
  convertTilts(facts);
  convertiSpindels(facts);
  convertSmartPlugs();
  convertProfileSteps(facts);
  convertBrewServices(facts);
  convertFermenters(facts);   // last - see the top of this file

  LittleFS.remove(ORIG_IN_PROGRESS);
  s_converted = true;
  return true;
}
