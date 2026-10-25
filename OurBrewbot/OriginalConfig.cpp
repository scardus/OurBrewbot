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

// Present while a conversion is under way: the files in /orig/ are the
// originals, and some live files may already be converted
#define ORIG_IN_PROGRESS  "/orig/converting.txt"

// The files the original firmware writes (each also has a "bkup" twin).
// jsonConfig.txt is left out on purpose - see removeSecrets().
static const char* const kOriginalFiles[] = {
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
  snprintf(buf, size, "/%s%s.txt", base, backup ? "bkup" : "");
}

static void origPath(char* buf, size_t size, const char* base, bool backup) {
  snprintf(buf, size, ORIG_DIR "/%s%s.txt", base, backup ? "bkup" : "");
}

// Loads an original file from its copy in /orig/ (primary, then backup)
static bool loadOriginal(JsonDocument& doc, const char* base) {
  char primary[40], backup[40];
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
  return !doc["BrewServiceSend"].isNull() && doc["BrewServices"].isNull();
}

// Copies every original file (and backup) to /orig/. A copy left there by an
// earlier conversion is replaced, or removed if the file no longer exists, so
// /orig/ always holds the config that was converted last.
static bool copyOriginalFiles() {
  LittleFS.mkdir(ORIG_DIR);
  for (const char* base : kOriginalFiles) {
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
  facts.fahrenheit = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) facts.tempControl[i] = 1;

  JsonDocument doc;
  if (loadOriginal(doc, "jsonGlobal")) {
    facts.fahrenheit = ((doc["unit"] | 1) == 0);
  }
  doc.clear();
  if (loadOriginal(doc, "jsonFermenter")) {
    for (int i = 0; i < MAX_FERMENTERS; i++) {
      facts.tempControl[i] = doc["TempControl"][i] | 1;
    }
  }
}

// ============================================================
// GLOBAL (jsonGlobal.txt)
// ============================================================

static void convertGlobal(const OriginalFacts& facts) {
  JsonDocument doc;
  if (!loadOriginal(doc, "jsonGlobal")) return;

  // The Blynk token - nothing in OurBrewbot uses it
  if (strlen(doc["authcode"] | "") > 0) {
    doc["authcode"] = "";
    REPORT("Global: Blynk token cleared");
  }
  // Original: 0 = Fahrenheit, 1 = Celsius. OurBrewbot: UNIT_FAHRENHEIT is 2.
  doc["unit"] = facts.fahrenheit ? UNIT_FAHRENHEIT : UNIT_CELSIUS;
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
  if (!loadOriginal(doc, "jsonProbe")) return;

  for (int i = 0; i < MAX_PROBES; i++) {
    uint8_t role      = doc["Function"][i]  | PROBE_UNASSIGNED;
    uint8_t fermenter = doc["Fermenter"][i] | PROBE_UNASSIGNED;
    uint8_t newRole   = convertProbeRole(role);
    if (newRole == PROBE_UNASSIGNED) {
      fermenter = PROBE_UNASSIGNED;
    } else if (fermenter >= MAX_FERMENTERS) {
      fermenter = 0;   // a "Common" probe saved without a fermenter
    }
    if (newRole != role) {
      REPORT("Probe %d (%s): role %u -> %u", i + 1, doc["Address"][i] | "", role, newRole);
      if (role == 7) {
        REPORT("Probe %d: was the ambient probe for every fermenter - now for fermenter %u only",
               i + 1, fermenter + 1);
      }
    }
    doc["Function"][i]  = newRole;
    doc["Fermenter"][i] = fermenter;
  }

  saveJsonDocSafe(doc, FILE_PROBE, FILE_PROBE_BKP);
}

// ============================================================
// TILTS (jsonTilt.txt)
// ============================================================

// For the report. Same order as TiltColour in Config.h (Tilt.cpp has the
// display names, but would bring the BLE code into the config tests).
static const char* const kTiltColours[MAX_TILTS] = {
  "red", "green", "black", "purple", "orange", "blue", "yellow", "pink"
};

static void convertTilts(const OriginalFacts& facts) {
  JsonDocument doc;
  if (!loadOriginal(doc, "jsonTilt")) return;

  for (int i = 0; i < MAX_TILT_SLOTS; i++) {
    // Original colours are 1-8 in the same order as OurBrewbot's 0-7
    // (1 = red, 2 = green, ...)
    uint8_t colour = doc["Address"][i] | PROBE_UNASSIGNED;
    if (colour < 1 || colour > MAX_TILTS) {
      doc["Address"][i]   = PROBE_UNASSIGNED;
      doc["Function"][i]  = PROBE_UNASSIGNED;
      doc["Fermenter"][i] = PROBE_UNASSIGNED;
      continue;
    }
    colour -= 1;
    doc["Address"][i] = colour;

    // Original role 2 = assigned to its fermenter. Its temperature was only
    // used for control when that fermenter's TempControl chose the Tilt (2);
    // in OurBrewbot that is the Beer role. Without it the Tilt still gives
    // the fermenter its SG.
    uint8_t role      = doc["Function"][i]  | PROBE_UNASSIGNED;
    uint8_t fermenter = doc["Fermenter"][i] | PROBE_UNASSIGNED;
    if (role != 2 || fermenter >= MAX_FERMENTERS) {
      fermenter = PROBE_UNASSIGNED;
      role      = PROBE_UNASSIGNED;
    } else {
      role = (facts.tempControl[fermenter] == 2) ? PROBE_FN_BEER : PROBE_UNASSIGNED;
    }
    doc["Function"][i]  = role;
    doc["Fermenter"][i] = fermenter;
    if (fermenter != PROBE_UNASSIGNED) {
      REPORT("Tilt %s: fermenter %u, %s", kTiltColours[colour], fermenter + 1,
             role == PROBE_FN_BEER ? "beer temperature" : "gravity only");
    }

    // The original stored adjust = reading - true value and SUBTRACTED it;
    // OurBrewbot adds its adjustment. The original's temperature adjustment
    // is in the display unit, and its SG adjustment in thousandths.
    float tempAdjust = doc["Temp_Adjust"][i] | 0.0f;
    float sgAdjust   = doc["SG_Adjust"][i]   | 0.0f;
    if (facts.fahrenheit) tempAdjust = tempAdjust * 5.0f / 9.0f;
    tempAdjust = -tempAdjust;
    sgAdjust   = -sgAdjust / 1000.0f;
    doc["Temp_Adjust"][i] = tempAdjust;
    doc["SG_Adjust"][i]   = sgAdjust;
    if (tempAdjust != 0.0f || sgAdjust != 0.0f) {
      REPORT("Tilt %s: calibration now %+.2f C, %+.4f SG", kTiltColours[colour], tempAdjust, sgAdjust);
    }
  }

  saveJsonDocSafe(doc, FILE_TILT, FILE_TILT_BKP);
}

// ============================================================
// FERMENTERS (jsonFermenter.txt) - written last
// ============================================================

static void convertFermenters(const OriginalFacts& facts) {
  (void)facts;
  JsonDocument doc;
  if (!loadOriginal(doc, "jsonFermenter")) return;

  doc.remove("BrewServiceSend");
  JsonArray services = doc["BrewServices"].to<JsonArray>();
  for (int i = 0; i < MAX_FERMENTERS; i++) services.add(0);

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
  convertFermenters(facts);   // last - see the top of this file

  LittleFS.remove(ORIG_IN_PROGRESS);
  s_converted = true;
  return true;
}
