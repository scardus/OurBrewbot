/*
 * Config.cpp — Configuration persistence
 * Load/save all config to/from LittleFS.
 */

#include "Config.h"
#include "Log.h"
#include <user_interface.h>     // rst_info struct, REASON_EXCEPTION_RST

// ============================================================
// GLOBAL INSTANCES
// ============================================================
GlobalConfig    g_globalConfig;
FermenterConfig g_fermenters[MAX_FERMENTERS];
ProbeConfig     g_probes[MAX_PROBES];
SmartPlugConfig g_smartPlugs[MAX_SMART_PLUGS];
ProfileConfig   g_profiles[MAX_PROFILES];
ProfileStep     g_profileSteps[MAX_PROFILE_STEPS];
TiltConfig      g_tilts[MAX_TILTS];
iSpindelConfig  g_iSpindels[MAX_ISPINDELS];
PlaatoConfig    g_plaato[MAX_ISPINDELS];
WiFiConfig      g_wifiConfig;
BrewServiceConfig g_brewServices[MAX_BREW_SERVICES];
MqttConfig        g_mqttConfig;
SyslogConfig      g_syslogConfig;

// ============================================================
// FILE UTILITIES
// ============================================================

String loadJsonFile(const char* path) {
  if (!LittleFS.exists(path)) {
    logMsg("[CFG] File not found: %s", path);
    return "";
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    logMsg("[CFG] Cannot open: %s", path);
    return "";
  }
  String content = f.readString();
  f.close();
  return content;
}

// Legacy String-based saver — only used by recordReboot, removed in next commit.
bool saveJsonFile(const char* path, const String& json) {
  File f = LittleFS.open(path, "w");
  if (!f) {
    logMsg("[CFG] Cannot write: %s", path);
    return false;
  }
  f.print(json);
  f.close();
  return true;
}

bool saveJsonFileSafe(const char* primary, const char* backup, const String& json) {
  if (!saveJsonFile(backup, json)) return false;
  return saveJsonFile(primary, json);
}

// Stream-serialize a JsonDocument directly to a LittleFS file.
static bool saveJsonDocToFile(JsonDocument& doc, const char* path) {
  File f = LittleFS.open(path, "w");
  if (!f) {
    logMsg("[CFG] Cannot write: %s", path);
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.close();
  return written > 0;
}

bool saveJsonDocSafe(JsonDocument& doc, const char* primary, const char* backup) {
  if (!saveJsonDocToFile(doc, backup)) return false;
  return saveJsonDocToFile(doc, primary);
}

// Stream-parse a JsonDocument directly from File. Tries primary first, falls
// back to backup on missing file or parse error. Returns true on success.
static bool loadJsonDocFromFile(JsonDocument& doc, const char* path) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

bool loadJsonDocSafe(JsonDocument& doc, const char* primary, const char* backup) {
  if (loadJsonDocFromFile(doc, primary)) return true;
  doc.clear();
  logMsg("[CFG] Falling back to backup: %s", backup);
  return loadJsonDocFromFile(doc, backup);
}

// ============================================================
// GLOBAL CONFIG
// Note: original uses lowercase "authcode" not "authCode"
// ============================================================

bool loadGlobalConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_GLOBAL, FILE_GLOBAL_BKP)) {
    logMsg("[CFG] Global config not found or invalid - using defaults");
    initDefaultGlobalConfig();
    return false;
  }

  strlcpy(g_globalConfig.authCode,    doc["authcode"]  | "", sizeof(g_globalConfig.authCode));
  g_globalConfig.unit           = doc["unit"]          | UNIT_CELSIUS;
  g_globalConfig.notifyOn       = doc["notifyon"]      | true;
  strlcpy(g_globalConfig.brewServiceId, doc["brewserviceid"] | "", sizeof(g_globalConfig.brewServiceId));
  g_globalConfig.brewService    = doc["brewservice"]   | BREW_SERVICE_NONE;
  g_globalConfig.migrated       = doc["migrated"]      | false;
  g_globalConfig.bleBaud        = doc["blebaud"]       | 9600;
  g_globalConfig.lastUptime     = doc["lastuptime"]    | 0;
  g_globalConfig.swNo           = doc["swno"]          | 0;
  g_globalConfig.sendToCloud    = doc["sendtocloud"]   | 0;
  g_globalConfig.globalSave     = doc["globalsave"]    | 0;
  strlcpy(g_globalConfig.myBrewBuddy, doc["mybrewbuddy"] | "", sizeof(g_globalConfig.myBrewBuddy));
  strlcpy(g_globalConfig.bSitterAuth, doc["bsitterauth"] | "", sizeof(g_globalConfig.bSitterAuth));
  g_globalConfig.babySitter     = doc["babysitter"]    | 0;
  g_globalConfig.plugCategory   = doc["plugcategory"]  | true;
  g_globalConfig.fNo            = doc["fno"]           | 1;
  g_globalConfig.mbbHardReset   = doc["mbbhardreset"]  | 0;
  g_globalConfig.tuningChartNo  = doc["tuning_chart_no"] | 0;
  g_globalConfig.resolution     = doc["resolution"]    | 11;

  return true;
}

bool saveGlobalConfig() {
  JsonDocument doc;
  doc["authcode"]        = g_globalConfig.authCode;
  doc["unit"]            = g_globalConfig.unit;
  doc["notifyon"]        = g_globalConfig.notifyOn;
  doc["brewserviceid"]   = g_globalConfig.brewServiceId;
  doc["brewservice"]     = g_globalConfig.brewService;
  doc["migrated"]        = g_globalConfig.migrated;
  doc["blebaud"]         = g_globalConfig.bleBaud;
  doc["lastuptime"]      = g_globalConfig.lastUptime;
  doc["swno"]            = g_globalConfig.swNo;
  doc["sendtocloud"]     = g_globalConfig.sendToCloud;
  doc["globalsave"]      = g_globalConfig.globalSave;
  doc["mybrewbuddy"]     = g_globalConfig.myBrewBuddy;
  doc["bsitterauth"]     = g_globalConfig.bSitterAuth;
  doc["babysitter"]      = g_globalConfig.babySitter;
  doc["plugcategory"]    = g_globalConfig.plugCategory;
  doc["fno"]             = g_globalConfig.fNo;
  doc["mbbhardreset"]    = g_globalConfig.mbbHardReset;
  doc["tuning_chart_no"] = g_globalConfig.tuningChartNo;
  doc["resolution"]      = g_globalConfig.resolution;

  return saveJsonDocSafe(doc, FILE_GLOBAL, FILE_GLOBAL_BKP);
}

// ============================================================
// FERMENTER CONFIG
// ============================================================

bool loadFermenterConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_FERMENTER, FILE_FERMENTER_BKP)) {
    logMsg("[CFG] Fermenter config not found or invalid - using defaults");
    initDefaultFermenterConfig();
    return false;
  }

  for (int i = 0; i < MAX_FERMENTERS; i++) {
    strlcpy(g_fermenters[i].fermenterName, doc["FermenterName"][i] | "Fermenter", sizeof(g_fermenters[i].fermenterName));
    strlcpy(g_fermenters[i].beerName,      doc["BeerName"][i]      | "Beer",      sizeof(g_fermenters[i].beerName));
    strlcpy(g_fermenters[i].yeastName,     doc["YeastName"][i]     | "Yeast",     sizeof(g_fermenters[i].yeastName));
    strlcpy(g_fermenters[i].bjcp,          doc["BJCP"][i]          | "BJCP",      sizeof(g_fermenters[i].bjcp));
    g_fermenters[i].ceilingTemp     = doc["CeilingTemp"][i]    | 22.0f;
    g_fermenters[i].floorTemp       = doc["FloorTemp"][i]      | 18.0f;
    g_fermenters[i].og              = doc["OG"][i]             | 1.050f;
    g_fermenters[i].tg              = doc["TG"][i]             | 1.010f;
    // Migrate old integer-format values (e.g. 1050 → 1.050)
    if (g_fermenters[i].og > 2.0f) g_fermenters[i].og /= 1000.0f;
    if (g_fermenters[i].tg > 2.0f) g_fermenters[i].tg /= 1000.0f;
    g_fermenters[i].hysteresis      = doc["Hysteresis"][i]     | 0.5f;
    g_fermenters[i].compressorDelay = doc["CompressorDelay"][i]| 10;
    g_fermenters[i].tempControl     = doc["TempControl"][i]    | true;
    g_fermenters[i].sgControl       = doc["SGControl"][i]      | false;
    g_fermenters[i].power           = doc["Power"][i]          | false;
    g_fermenters[i].alarmTolerance  = doc["AlarmTolerance"][i] | 0.0f;
    g_fermenters[i].ambientSG       = doc["AmbientSG"][i]      | 0.0f;
    g_fermenters[i].alarm           = doc["Alarm"][i]          | false;
    g_fermenters[i].profileNo       = doc["ProfileNo"][i]      | 0;
    g_fermenters[i].currentStep      = doc["CurrentStep"][i]    | 0;
    g_fermenters[i].currentHour     = doc["CurrentHour"][i]    | 0;
    g_fermenters[i].liveTest        = doc["LiveTest"][i]        | false;
    g_fermenters[i].status          = doc["Status"][i]         | 0;
    g_fermenters[i].profileRunning  = doc["ProfileRunning"][i] | false;
    g_fermenters[i].profilePaused   = doc["ProfilePaused"][i]  | false;
    // Backward compat: migrate old bool BrewServiceSend → bit 0 of new bitmask
    if (!doc["BrewServices"].isNull()) {
      g_fermenters[i].brewServices = doc["BrewServices"][i] | 0;
    } else {
      g_fermenters[i].brewServices = (doc["BrewServiceSend"][i] | 0) ? 1 : 0;
    }
    g_fermenters[i].psiCollect      = doc["PSI_Collect"][i]    | false;
    g_fermenters[i].function        = doc["Function"][i]       | 1;
    g_fermenters[i].series1         = doc["Series1"][i]        | 1;
    g_fermenters[i].series2         = doc["Series2"][i]        | 3;
    g_fermenters[i].series3         = doc["Series3"][i]        | 8;
    g_fermenters[i].series4         = doc["Series4"][i]        | 2;
    g_fermenters[i].sgCalibration   = doc["SGCalibration"][i]  | 0.0f;
    g_fermenters[i].mbbPsiCollect   = doc["MyBrewBuddyPSI_Colle"][i] | false;
    g_fermenters[i].startMillis     = doc["StartMillis"][i]    | 0;
  }

  // Validate temperature/hysteresis trio per fermenter; reset offending
  // fields to defaults so an invalid persisted config can't lock the user
  // out of the admin POST validation.
  bool corrected = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    float c = g_fermenters[i].ceilingTemp;
    float f = g_fermenters[i].floorTemp;
    float h = g_fermenters[i].hysteresis;
    bool invalid = (c < -20.0f || c > 50.0f) ||
                   (f < -20.0f || f > 50.0f) ||
                   (h <   0.0f || h > 10.0f) ||
                   (f >= c) ||
                   ((c - f) <  2.0f * h);
    if (invalid) {
      logMsg("[CFG] Fermenter %d: invalid temp/hyst trio (c=%.2f f=%.2f h=%.2f); reset to defaults", i, c, f, h);
      g_fermenters[i].ceilingTemp = 22.0f;
      g_fermenters[i].floorTemp   = 18.0f;
      g_fermenters[i].hysteresis  = 0.5f;
      corrected = true;
    }
  }
  if (corrected) saveFermenterConfig();
  return true;
}

bool saveFermenterConfig() {
  JsonDocument doc;

  JsonArray nameArr  = doc["FermenterName"].to<JsonArray>();
  JsonArray beerArr  = doc["BeerName"].to<JsonArray>();
  JsonArray yeastArr = doc["YeastName"].to<JsonArray>();
  JsonArray bjcpArr  = doc["BJCP"].to<JsonArray>();
  JsonArray ceilArr  = doc["CeilingTemp"].to<JsonArray>();
  JsonArray floorArr = doc["FloorTemp"].to<JsonArray>();
  JsonArray ogArr    = doc["OG"].to<JsonArray>();
  JsonArray tgArr    = doc["TG"].to<JsonArray>();
  JsonArray hystArr  = doc["Hysteresis"].to<JsonArray>();
  JsonArray compArr  = doc["CompressorDelay"].to<JsonArray>();
  JsonArray tcArr    = doc["TempControl"].to<JsonArray>();
  JsonArray sgcArr   = doc["SGControl"].to<JsonArray>();
  JsonArray pwrArr   = doc["Power"].to<JsonArray>();
  JsonArray alTolArr = doc["AlarmTolerance"].to<JsonArray>();
  JsonArray ambArr   = doc["AmbientSG"].to<JsonArray>();
  JsonArray almArr   = doc["Alarm"].to<JsonArray>();
  JsonArray profArr  = doc["ProfileNo"].to<JsonArray>();
  JsonArray csArr    = doc["CurrentStep"].to<JsonArray>();
  JsonArray hourArr  = doc["CurrentHour"].to<JsonArray>();
  JsonArray ltArr    = doc["LiveTest"].to<JsonArray>();
  JsonArray statArr  = doc["Status"].to<JsonArray>();
  JsonArray prRunArr = doc["ProfileRunning"].to<JsonArray>();
  JsonArray prPauArr = doc["ProfilePaused"].to<JsonArray>();
  JsonArray bsArr    = doc["BrewServices"].to<JsonArray>();
  JsonArray psiArr   = doc["PSI_Collect"].to<JsonArray>();
  JsonArray fnArr    = doc["Function"].to<JsonArray>();
  JsonArray s1Arr    = doc["Series1"].to<JsonArray>();
  JsonArray s2Arr    = doc["Series2"].to<JsonArray>();
  JsonArray s3Arr    = doc["Series3"].to<JsonArray>();
  JsonArray s4Arr    = doc["Series4"].to<JsonArray>();
  JsonArray sgcalArr = doc["SGCalibration"].to<JsonArray>();
  JsonArray mbbArr   = doc["MyBrewBuddyPSI_Colle"].to<JsonArray>();
  JsonArray smArr    = doc["StartMillis"].to<JsonArray>();

  for (int i = 0; i < MAX_FERMENTERS; i++) {
    nameArr.add(g_fermenters[i].fermenterName);
    beerArr.add(g_fermenters[i].beerName);
    yeastArr.add(g_fermenters[i].yeastName);
    bjcpArr.add(g_fermenters[i].bjcp);
    ceilArr.add(g_fermenters[i].ceilingTemp);
    floorArr.add(g_fermenters[i].floorTemp);
    ogArr.add(g_fermenters[i].og);
    tgArr.add(g_fermenters[i].tg);
    hystArr.add(g_fermenters[i].hysteresis);
    compArr.add(g_fermenters[i].compressorDelay);
    tcArr.add((bool)g_fermenters[i].tempControl);
    sgcArr.add((bool)g_fermenters[i].sgControl);
    pwrArr.add((bool)g_fermenters[i].power);
    alTolArr.add(g_fermenters[i].alarmTolerance);
    ambArr.add(g_fermenters[i].ambientSG);
    almArr.add((bool)g_fermenters[i].alarm);
    profArr.add(g_fermenters[i].profileNo);
    csArr.add(g_fermenters[i].currentStep);
    hourArr.add(g_fermenters[i].currentHour);
    ltArr.add((bool)g_fermenters[i].liveTest);
    statArr.add(g_fermenters[i].status);
    prRunArr.add((bool)g_fermenters[i].profileRunning);
    prPauArr.add((bool)g_fermenters[i].profilePaused);
    bsArr.add(g_fermenters[i].brewServices);
    psiArr.add((bool)g_fermenters[i].psiCollect);
    fnArr.add(g_fermenters[i].function);
    s1Arr.add(g_fermenters[i].series1);
    s2Arr.add(g_fermenters[i].series2);
    s3Arr.add(g_fermenters[i].series3);
    s4Arr.add(g_fermenters[i].series4);
    sgcalArr.add(g_fermenters[i].sgCalibration);
    mbbArr.add((bool)g_fermenters[i].mbbPsiCollect);
    smArr.add(g_fermenters[i].startMillis);
  }

  return saveJsonDocSafe(doc, FILE_FERMENTER, FILE_FERMENTER_BKP);
}

// ============================================================
// PROBE CONFIG
// ============================================================

bool loadProbeConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_PROBE, FILE_PROBE_BKP)) {
    initDefaultProbeConfig();
    return false;
  }

  for (int i = 0; i < MAX_PROBES; i++) {
    strlcpy(g_probes[i].probeName, doc["Probe_Name"][i] | "Probe",  sizeof(g_probes[i].probeName));
    strlcpy(g_probes[i].address,   doc["Address"][i]    | "",       sizeof(g_probes[i].address));
    g_probes[i].function    = doc["Function"][i]    | PROBE_UNASSIGNED;
    g_probes[i].fermenter   = doc["Fermenter"][i]   | PROBE_UNASSIGNED;
    g_probes[i].temperature = doc["Temperature"][i] | 0.0f;
    g_probes[i].mbb         = doc["MBB"][i]         | 0;
    g_probes[i].tempAdjust  = doc["Temp_Adjust"][i] | 0.0f;
    g_probes[i].sgAdjust    = doc["SG_Adjust"][i]   | 0.0f;
    g_probes[i].rawTemperature = g_probes[i].temperature;
  }
  return true;
}

bool saveProbeConfig() {
  JsonDocument doc;

  JsonArray nameArr = doc["Probe_Name"].to<JsonArray>();
  JsonArray addrArr = doc["Address"].to<JsonArray>();
  JsonArray fnArr   = doc["Function"].to<JsonArray>();
  JsonArray fermArr = doc["Fermenter"].to<JsonArray>();
  JsonArray tempArr = doc["Temperature"].to<JsonArray>();
  JsonArray mbbArr  = doc["MBB"].to<JsonArray>();
  JsonArray taArr   = doc["Temp_Adjust"].to<JsonArray>();
  JsonArray saArr   = doc["SG_Adjust"].to<JsonArray>();

  for (int i = 0; i < MAX_PROBES; i++) {
    nameArr.add(g_probes[i].probeName);
    addrArr.add(g_probes[i].address);
    fnArr.add(g_probes[i].function);
    fermArr.add(g_probes[i].fermenter);
    tempArr.add(g_probes[i].temperature);
    mbbArr.add(g_probes[i].mbb);
    taArr.add(g_probes[i].tempAdjust);
    saArr.add(g_probes[i].sgAdjust);
  }

  return saveJsonDocSafe(doc, FILE_PROBE, FILE_PROBE_BKP);
}

// ============================================================
// SMART PLUG CONFIG
// ============================================================

bool loadSmartPlugConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_SMARTPLUGS, FILE_SMARTPLUGS_BKP)) {
    initDefaultSmartPlugConfig();
    return false;
  }

  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    g_smartPlugs[i].type        = doc["Type"][i]        | 1;
    g_smartPlugs[i].codeset     = doc["Codeset"][i]     | 1;
    g_smartPlugs[i].protocol    = doc["Protocol"][i]    | 1;
    g_smartPlugs[i].bits        = doc["Bits"][i]        | 24;
    g_smartPlugs[i].delayLength = doc["DelayLength"][i] | 160;
    g_smartPlugs[i].function    = doc["Function"][i]    | PLUG_FN_UNASSIGNED;
    g_smartPlugs[i].fermenter   = doc["Fermenter"][i]   | PROBE_UNASSIGNED;
    g_smartPlugs[i].onCode      = doc["OnCode"][i]      | 0;
    g_smartPlugs[i].offCode     = doc["OffCode"][i]     | 0;
    strlcpy(g_smartPlugs[i].manufacturer, doc["Manufacturer"][i] | "Unknown", sizeof(g_smartPlugs[i].manufacturer));
    strlcpy(g_smartPlugs[i].model,        doc["Model"][i]        | "Model",   sizeof(g_smartPlugs[i].model));
    g_smartPlugs[i].plugNo      = doc["PlugNo"][i]      | i;
  }
  return true;
}

bool saveSmartPlugConfig() {
  JsonDocument doc;

  JsonArray typeArr  = doc["Type"].to<JsonArray>();
  JsonArray csArr    = doc["Codeset"].to<JsonArray>();
  JsonArray prArr    = doc["Protocol"].to<JsonArray>();
  JsonArray bitsArr  = doc["Bits"].to<JsonArray>();
  JsonArray dlArr    = doc["DelayLength"].to<JsonArray>();
  JsonArray fnArr    = doc["Function"].to<JsonArray>();
  JsonArray fermArr  = doc["Fermenter"].to<JsonArray>();
  JsonArray onArr    = doc["OnCode"].to<JsonArray>();
  JsonArray offArr   = doc["OffCode"].to<JsonArray>();
  JsonArray mfgArr   = doc["Manufacturer"].to<JsonArray>();
  JsonArray modArr   = doc["Model"].to<JsonArray>();
  JsonArray pnArr    = doc["PlugNo"].to<JsonArray>();

  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    typeArr.add(g_smartPlugs[i].type);
    csArr.add(g_smartPlugs[i].codeset);
    prArr.add(g_smartPlugs[i].protocol);
    bitsArr.add(g_smartPlugs[i].bits);
    dlArr.add(g_smartPlugs[i].delayLength);
    fnArr.add(g_smartPlugs[i].function);
    fermArr.add(g_smartPlugs[i].fermenter);
    onArr.add(g_smartPlugs[i].onCode);
    offArr.add(g_smartPlugs[i].offCode);
    mfgArr.add(g_smartPlugs[i].manufacturer);
    modArr.add(g_smartPlugs[i].model);
    pnArr.add(g_smartPlugs[i].plugNo);
  }

  return saveJsonDocSafe(doc, FILE_SMARTPLUGS, FILE_SMARTPLUGS_BKP);
}

// ============================================================
// PROFILE CONFIG + STEPS
// ============================================================

bool loadProfileConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_PROFILE, FILE_PROFILE_BKP)) {
    initDefaultProfileConfig();
    return false;
  }
  for (int i = 0; i < MAX_PROFILES; i++) {
    strlcpy(g_profiles[i].profileName, doc["ProfileName"][i] | "Empty Profile", sizeof(g_profiles[i].profileName));
  }
  return true;
}

bool saveProfileConfig() {
  JsonDocument doc;
  JsonArray nameArr = doc["ProfileName"].to<JsonArray>();
  for (int i = 0; i < MAX_PROFILES; i++) {
    nameArr.add(g_profiles[i].profileName);
  }
  return saveJsonDocSafe(doc, FILE_PROFILE, FILE_PROFILE_BKP);
}

bool loadProfileSteps() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_STEPS, FILE_STEPS_BKP)) return false;

  for (int i = 0; i < MAX_PROFILE_STEPS; i++) {
    g_profileSteps[i].stepNo    = doc["StepNo"][i]    | 0;
    g_profileSteps[i].stepType  = doc["StepType"][i]  | 0;
    g_profileSteps[i].startTemp = doc["StartTemp"][i] | 0.0f;
    g_profileSteps[i].endTemp   = doc["EndTemp"][i]   | 0.0f;
    g_profileSteps[i].sgTrigger = doc["SGTrigger"][i] | 0.0f;
    g_profileSteps[i].days      = doc["Days"][i]      | 0.0f;
  }
  return true;
}

bool saveProfileSteps() {
  JsonDocument doc;
  JsonArray snArr  = doc["StepNo"].to<JsonArray>();
  JsonArray stArr  = doc["StepType"].to<JsonArray>();
  JsonArray sTArr  = doc["StartTemp"].to<JsonArray>();
  JsonArray eTArr  = doc["EndTemp"].to<JsonArray>();
  JsonArray sgArr  = doc["SGTrigger"].to<JsonArray>();
  JsonArray dArr   = doc["Days"].to<JsonArray>();

  for (int i = 0; i < MAX_PROFILE_STEPS; i++) {
    snArr.add(g_profileSteps[i].stepNo);
    stArr.add(g_profileSteps[i].stepType);
    sTArr.add(g_profileSteps[i].startTemp);
    eTArr.add(g_profileSteps[i].endTemp);
    sgArr.add(g_profileSteps[i].sgTrigger);
    dArr.add(g_profileSteps[i].days);
  }
  return saveJsonDocSafe(doc, FILE_STEPS, FILE_STEPS_BKP);
}

// ============================================================
// iSPINDEL CONFIG
// ============================================================

bool loadiSpindelConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_ISPINDEL, FILE_ISPINDEL_BKP)) {
    initDefaultiSpindelConfig();
    return false;
  }
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    strlcpy(g_iSpindels[i].name, doc["iSpindelName"][i] | "None", sizeof(g_iSpindels[i].name));
    strlcpy(g_iSpindels[i].id, doc["ID"][i] | "", sizeof(g_iSpindels[i].id));
    g_iSpindels[i].collectData  = doc["iSpindelCollectData"][i] | false;
    g_iSpindels[i].fermenter    = doc["iSpindelFermenter"][i]   | PROBE_UNASSIGNED;
    g_iSpindels[i].unit         = doc["Unit"][i]                | 1;
    // Default to PROBE_FN_BEER for legacy configs without the field — preserves
    // existing behavior where iSpindel temperature flowed into the beer-temp chain.
    uint8_t fn                  = doc["Function"][i]            | PROBE_FN_BEER;
    if (fn != PROBE_FN_BEER) fn = PROBE_UNASSIGNED;
    g_iSpindels[i].function     = fn;
    g_iSpindels[i].tempAdjust   = doc["TempAdjust"][i]         | 0.0f;
    g_iSpindels[i].sgAdjust     = doc["SGAdjust"][i]           | 0.0f;
  }
  return true;
}

bool saveiSpindelConfig() {
  JsonDocument doc;
  JsonArray nameArr = doc["iSpindelName"].to<JsonArray>();
  JsonArray idArr   = doc["ID"].to<JsonArray>();
  JsonArray cdArr   = doc["iSpindelCollectData"].to<JsonArray>();
  JsonArray fiArr   = doc["iSpindelFermenter"].to<JsonArray>();
  JsonArray unArr   = doc["Unit"].to<JsonArray>();
  JsonArray fnArr   = doc["Function"].to<JsonArray>();
  JsonArray taArr   = doc["TempAdjust"].to<JsonArray>();
  JsonArray saArr   = doc["SGAdjust"].to<JsonArray>();

  for (int i = 0; i < MAX_ISPINDELS; i++) {
    nameArr.add(g_iSpindels[i].name);
    idArr.add(g_iSpindels[i].id);
    cdArr.add((bool)g_iSpindels[i].collectData);
    fiArr.add(g_iSpindels[i].fermenter);
    unArr.add(g_iSpindels[i].unit);
    fnArr.add(g_iSpindels[i].function);
    taArr.add(g_iSpindels[i].tempAdjust);
    saArr.add(g_iSpindels[i].sgAdjust);
  }
  return saveJsonDocSafe(doc, FILE_ISPINDEL, FILE_ISPINDEL_BKP);
}

// ============================================================
// PLAATO CONFIG
// ============================================================

bool loadPlaatoConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_PLAATO, FILE_PLAATO_BKP)) {
    initDefaultPlaatoConfig();
    return false;
  }
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    strlcpy(g_plaato[i].authCode, doc["AuthCode"][i] | "Plaato Authcode", sizeof(g_plaato[i].authCode));
    g_plaato[i].getData = doc["GetData"][i] | false;
  }
  return true;
}

bool savePlaatoConfig() {
  JsonDocument doc;
  JsonArray acArr = doc["AuthCode"].to<JsonArray>();
  JsonArray gdArr = doc["GetData"].to<JsonArray>();
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    acArr.add(g_plaato[i].authCode);
    gdArr.add((bool)g_plaato[i].getData);
  }
  return saveJsonDocSafe(doc, FILE_PLAATO, FILE_PLAATO_BKP);
}

// ============================================================
// TILT CONFIG
// 4-slot format matching original firmware jsonTilt.txt:
//   {"Address":[c,c,c,c],"Function":[...],"Fermenter":[...],
//    "Temp_Adjust":[...],"SG_Adjust":[...],"MBB":[...]}
// where Address = colour index (0-7) or 99 = unassigned slot
// ============================================================

bool loadTiltConfig() {
  initDefaultTiltConfig();  // start from known defaults

  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_TILT, FILE_TILT_BKP)) {
    logMsg("[CFG] Tilt config not found or invalid");
    return false;
  }

  // Each slot has a colour (Address). Populate the colour-indexed g_tilts entry.
  for (int i = 0; i < MAX_TILT_SLOTS; i++) {
    uint8_t colour = doc["Address"][i] | PROBE_UNASSIGNED;
    if (colour >= MAX_TILTS) continue;  // skip unassigned or out-of-range
    g_tilts[colour].colour     = colour;
    uint8_t fn                 = doc["Function"][i]    | PROBE_UNASSIGNED;
    // Migrate legacy values: only PROBE_FN_BEER means "provide beer temp" — everything
    // else (Ambient/Tilt-only/etc) collapses to Unassigned ("temperature reading not used").
    if (fn != PROBE_FN_BEER) fn = PROBE_UNASSIGNED;
    g_tilts[colour].function   = fn;
    g_tilts[colour].fermenter  = doc["Fermenter"][i]   | PROBE_UNASSIGNED;
    g_tilts[colour].tempAdjust = doc["Temp_Adjust"][i] | 0.0f;
    g_tilts[colour].sgAdjust   = doc["SG_Adjust"][i]   | 0.0f;
    g_tilts[colour].mbb        = doc["MBB"][i]         | 0;
  }
  return true;
}

bool saveTiltConfig() {
  JsonDocument doc;
  JsonArray addrArr = doc["Address"].to<JsonArray>();
  JsonArray fnArr   = doc["Function"].to<JsonArray>();
  JsonArray fermArr = doc["Fermenter"].to<JsonArray>();
  JsonArray taArr   = doc["Temp_Adjust"].to<JsonArray>();
  JsonArray saArr   = doc["SG_Adjust"].to<JsonArray>();
  JsonArray mbbArr  = doc["MBB"].to<JsonArray>();

  // Write configured colours (colour != PROBE_UNASSIGNED) first, pad to MAX_TILT_SLOTS
  int count = 0;
  for (int c = 0; c < MAX_TILTS && count < MAX_TILT_SLOTS; c++) {
    if (g_tilts[c].colour != PROBE_UNASSIGNED) {
      addrArr.add(g_tilts[c].colour);
      fnArr.add(g_tilts[c].function);
      fermArr.add(g_tilts[c].fermenter);
      taArr.add(g_tilts[c].tempAdjust);
      saArr.add(g_tilts[c].sgAdjust);
      mbbArr.add(g_tilts[c].mbb);
      count++;
    }
  }
  // Pad remaining slots with sentinel values
  for (; count < MAX_TILT_SLOTS; count++) {
    addrArr.add(PROBE_UNASSIGNED);
    fnArr.add(PROBE_UNASSIGNED);
    fermArr.add(PROBE_UNASSIGNED);
    taArr.add(0.0f);
    saArr.add(0.0f);
    mbbArr.add(0);
  }

  return saveJsonDocSafe(doc, FILE_TILT, FILE_TILT_BKP);
}

// ============================================================
// BREW SERVICE CONFIG (multiple service slots)
// ============================================================

bool loadBrewServiceConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_BREWSVC, FILE_BREWSVC_BKP)) {
    initDefaultBrewServiceConfig();
    // Migrate from old single-service global config — only when both files are truly
    // absent (first boot after upgrade), not on parse failure.
    if (!LittleFS.exists(FILE_BREWSVC) && !LittleFS.exists(FILE_BREWSVC_BKP)) {
      // Old service types: 1=BF, 2=Monitor Beer (removed), 3=Brewfather (now index 1)
      int legacySvc = g_globalConfig.brewService;
      int idx = (legacySvc == 1) ? 0 : (legacySvc == 3) ? 1 : -1;
      if (idx >= 0) {
        g_brewServices[idx].enabled = true;
        strlcpy(g_brewServices[idx].serviceId, g_globalConfig.brewServiceId, sizeof(g_brewServices[idx].serviceId));
        logMsg("[CFG] Migrated legacy brew service %d to slot %d", legacySvc, idx);
        saveBrewServiceConfig();
      }
    }
    return false;
  }

  // Detect old 3-slot config (BF, Monitor Beer, Brewfather) and remap to 2-slot
  if (doc["Enabled"].size() == 3) {
    g_brewServices[0].enabled = doc["Enabled"][0] | false;
    strlcpy(g_brewServices[0].serviceId, doc["ServiceId"][0] | "", sizeof(g_brewServices[0].serviceId));
    strlcpy(g_brewServices[0].deviceName, doc["DeviceName"][0] | "OurBrewbot", sizeof(g_brewServices[0].deviceName));
    // Old slot 1 was Monitor Beer (removed); old slot 2 was Brewfather → new slot 1
    g_brewServices[1].enabled = doc["Enabled"][2] | false;
    strlcpy(g_brewServices[1].serviceId, doc["ServiceId"][2] | "", sizeof(g_brewServices[1].serviceId));
    strlcpy(g_brewServices[1].deviceName, doc["DeviceName"][2] | "OurBrewbot", sizeof(g_brewServices[1].deviceName));
    logMsg("[CFG] Migrated 3-slot brew service config to 2-slot (Monitor Beer removed)");
    saveBrewServiceConfig();
  } else {
    for (int i = 0; i < MAX_BREW_SERVICES; i++) {
      g_brewServices[i].enabled = doc["Enabled"][i] | false;
      strlcpy(g_brewServices[i].serviceId, doc["ServiceId"][i] | "", sizeof(g_brewServices[i].serviceId));
      strlcpy(g_brewServices[i].deviceName, doc["DeviceName"][i] | "OurBrewbot", sizeof(g_brewServices[i].deviceName));
    }
  }
  return true;
}

bool saveBrewServiceConfig() {
  JsonDocument doc;
  JsonArray enArr = doc["Enabled"].to<JsonArray>();
  JsonArray idArr = doc["ServiceId"].to<JsonArray>();
  JsonArray dnArr = doc["DeviceName"].to<JsonArray>();
  for (int i = 0; i < MAX_BREW_SERVICES; i++) {
    enArr.add((bool)g_brewServices[i].enabled);
    idArr.add(g_brewServices[i].serviceId);
    dnArr.add(g_brewServices[i].deviceName);
  }
  return saveJsonDocSafe(doc, FILE_BREWSVC, FILE_BREWSVC_BKP);
}

// ============================================================
// MQTT CONFIG
// ============================================================

bool loadMqttConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_MQTT, FILE_MQTT_BKP)) {
    initDefaultMqttConfig();
    return false;
  }
  g_mqttConfig.enabled      = doc["enabled"]      | false;
  g_mqttConfig.haDiscovery  = doc["haDiscovery"]  | false;
  g_mqttConfig.allowControl = doc["allowControl"] | false;
  g_mqttConfig.logEnabled   = doc["logEnabled"]   | false;
  g_mqttConfig.port         = doc["port"]         | 1883;
  strlcpy(g_mqttConfig.host,      doc["host"]      | "", sizeof(g_mqttConfig.host));
  strlcpy(g_mqttConfig.username,  doc["username"]  | "", sizeof(g_mqttConfig.username));
  strlcpy(g_mqttConfig.password,  doc["password"]  | "", sizeof(g_mqttConfig.password));
  strlcpy(g_mqttConfig.baseTopic, doc["baseTopic"] | "ourbrewbot", sizeof(g_mqttConfig.baseTopic));
  return true;
}

bool saveMqttConfig() {
  JsonDocument doc;
  doc["enabled"]      = (bool)g_mqttConfig.enabled;
  doc["haDiscovery"]  = (bool)g_mqttConfig.haDiscovery;
  doc["allowControl"] = (bool)g_mqttConfig.allowControl;
  doc["logEnabled"]   = (bool)g_mqttConfig.logEnabled;
  doc["host"]         = g_mqttConfig.host;
  doc["port"]        = g_mqttConfig.port;
  doc["username"]    = g_mqttConfig.username;
  doc["password"]    = g_mqttConfig.password;
  doc["baseTopic"]   = g_mqttConfig.baseTopic;
  return saveJsonDocSafe(doc, FILE_MQTT, FILE_MQTT_BKP);
}

// ============================================================
// SYSLOG CONFIG
// ============================================================

bool loadSyslogConfig() {
  JsonDocument doc;
  if (!loadJsonDocSafe(doc, FILE_SYSLOG, FILE_SYSLOG_BKP)) {
    initDefaultSyslogConfig();
    return false;
  }
  g_syslogConfig.enabled  = doc["enabled"]  | false;
  g_syslogConfig.port     = doc["port"]     | 514;
  g_syslogConfig.facility = doc["facility"] | 16;
  g_syslogConfig.minLevel = doc["minLevel"] | 7;
  strlcpy(g_syslogConfig.host, doc["host"] | "", sizeof(g_syslogConfig.host));
  return true;
}

bool saveSyslogConfig() {
  JsonDocument doc;
  doc["enabled"]  = (bool)g_syslogConfig.enabled;
  doc["host"]     = g_syslogConfig.host;
  doc["port"]     = g_syslogConfig.port;
  doc["facility"] = g_syslogConfig.facility;
  doc["minLevel"] = g_syslogConfig.minLevel;
  return saveJsonDocSafe(doc, FILE_SYSLOG, FILE_SYSLOG_BKP);
}

// ============================================================
// LOAD ALL / SAVE ALL
// ============================================================

void loadAllConfig() {
  logMsg("[CFG] Loading all configuration...");
  loadGlobalConfig();
  loadFermenterConfig();
  loadProbeConfig();
  loadSmartPlugConfig();
  loadProfileConfig();
  loadProfileSteps();
  loadiSpindelConfig();
  loadPlaatoConfig();
  loadTiltConfig();
  loadBrewServiceConfig();
  loadMqttConfig();
  loadSyslogConfig();
  logMsg("[CFG] All configuration loaded");
}

void saveAllConfig() {
  saveGlobalConfig();
  saveFermenterConfig();
  saveProbeConfig();
  saveSmartPlugConfig();
  saveProfileConfig();
  saveProfileSteps();
  saveiSpindelConfig();
  savePlaatoConfig();
  saveBrewServiceConfig();
  saveMqttConfig();
  saveSyslogConfig();
}

// ============================================================
// DEFAULT INITIALISERS
// ============================================================

void initDefaultGlobalConfig() {
  memset(&g_globalConfig, 0, sizeof(g_globalConfig));
  g_globalConfig.unit       = UNIT_CELSIUS;
  g_globalConfig.notifyOn   = true;
  g_globalConfig.brewService= BREW_SERVICE_NONE;
  g_globalConfig.bleBaud    = 9600;
  g_globalConfig.swNo       = 20;
  g_globalConfig.plugCategory = true;
  g_globalConfig.fNo        = 1;
  g_globalConfig.resolution = 11;
}

void initDefaultFermenterConfig() {
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    snprintf(g_fermenters[i].fermenterName, sizeof(g_fermenters[i].fermenterName), "Fermenter %d", i+1);
    strlcpy(g_fermenters[i].beerName,  "Beer",  sizeof(g_fermenters[i].beerName));
    strlcpy(g_fermenters[i].yeastName, "Yeast", sizeof(g_fermenters[i].yeastName));
    strlcpy(g_fermenters[i].bjcp,      "BJCP",  sizeof(g_fermenters[i].bjcp));
    g_fermenters[i].ceilingTemp     = 22.0f;
    g_fermenters[i].floorTemp       = 18.0f;
    g_fermenters[i].og              = 1.050f;
    g_fermenters[i].tg              = 1.010f;
    g_fermenters[i].hysteresis      = 0.5f;
    g_fermenters[i].compressorDelay = 10;
    g_fermenters[i].tempControl     = true;
    g_fermenters[i].sgControl       = false;
    g_fermenters[i].power           = false;
    g_fermenters[i].series1 = 1; g_fermenters[i].series2 = 3;
    g_fermenters[i].series3 = 8; g_fermenters[i].series4 = 2;
  }
}

void initDefaultProbeConfig() {
  for (int i = 0; i < MAX_PROBES; i++) {
    strlcpy(g_probes[i].probeName, "Probe", sizeof(g_probes[i].probeName));
    g_probes[i].address[0] = '\0';
    g_probes[i].function  = PROBE_UNASSIGNED;
    g_probes[i].fermenter = PROBE_UNASSIGNED;
    g_probes[i].temperature = 0.0f;
    g_probes[i].mbb       = 0;
    g_probes[i].tempAdjust = 0.0f;
    g_probes[i].sgAdjust   = 0.0f;
  }
}

void initDefaultSmartPlugConfig() {
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    g_smartPlugs[i].type        = 1;
    g_smartPlugs[i].codeset     = 1;
    g_smartPlugs[i].protocol    = 1;
    g_smartPlugs[i].bits        = 24;
    g_smartPlugs[i].delayLength = 160;
    g_smartPlugs[i].function    = PLUG_FN_UNASSIGNED;
    g_smartPlugs[i].fermenter   = PROBE_UNASSIGNED;
    g_smartPlugs[i].onCode      = 0;
    g_smartPlugs[i].offCode     = 0;
    strlcpy(g_smartPlugs[i].manufacturer, "Manufacturer", sizeof(g_smartPlugs[i].manufacturer));
    strlcpy(g_smartPlugs[i].model,        "Model",        sizeof(g_smartPlugs[i].model));
    g_smartPlugs[i].plugNo      = i;
  }
}

void initDefaultProfileConfig() {
  for (int i = 0; i < MAX_PROFILES; i++) {
    strlcpy(g_profiles[i].profileName, "Empty Profile", sizeof(g_profiles[i].profileName));
  }
  memset(g_profileSteps, 0, sizeof(g_profileSteps));
}

void initDefaultTiltConfig() {
  for (int i = 0; i < MAX_TILTS; i++) {
    g_tilts[i].colour     = PROBE_UNASSIGNED;
    g_tilts[i].function   = PROBE_UNASSIGNED;
    g_tilts[i].fermenter  = PROBE_UNASSIGNED;
    g_tilts[i].tempAdjust = 0.0f;
    g_tilts[i].sgAdjust   = 0.0f;
    g_tilts[i].mbb        = 0;
    g_tilts[i].sg         = 0.0f;
    g_tilts[i].temperature = 0.0f;
    g_tilts[i].active     = false;
    g_tilts[i].lastSeen   = 0;
  }
}

void initDefaultiSpindelConfig() {
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    strlcpy(g_iSpindels[i].name, "None", sizeof(g_iSpindels[i].name));
    g_iSpindels[i].id[0]       = '\0';
    g_iSpindels[i].collectData = false;
    g_iSpindels[i].fermenter   = (i == 0) ? 0 : PROBE_UNASSIGNED;
    g_iSpindels[i].unit        = (i == 0) ? 1 : 0;
    g_iSpindels[i].function    = PROBE_FN_BEER;
  }
}

void initDefaultPlaatoConfig() {
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    strlcpy(g_plaato[i].authCode, "Plaato Authcode", sizeof(g_plaato[i].authCode));
    g_plaato[i].getData = false;
  }
}

void initDefaultBrewServiceConfig() {
  for (int i = 0; i < MAX_BREW_SERVICES; i++) {
    g_brewServices[i].enabled = false;
    g_brewServices[i].serviceId[0] = '\0';
    strlcpy(g_brewServices[i].deviceName, "OurBrewbot", sizeof(g_brewServices[i].deviceName));
  }
}

void initDefaultMqttConfig() {
  g_mqttConfig.enabled      = false;
  g_mqttConfig.haDiscovery  = false;
  g_mqttConfig.allowControl = false;
  g_mqttConfig.logEnabled   = false;
  g_mqttConfig.port         = 1883;
  g_mqttConfig.host[0]     = '\0';
  g_mqttConfig.username[0] = '\0';
  g_mqttConfig.password[0] = '\0';
  strlcpy(g_mqttConfig.baseTopic, "ourbrewbot", sizeof(g_mqttConfig.baseTopic));
}

void initDefaultSyslogConfig() {
  g_syslogConfig.enabled   = false;
  g_syslogConfig.host[0]   = '\0';
  g_syslogConfig.port      = 514;
  g_syslogConfig.facility  = 16;  // local0
  g_syslogConfig.minLevel  = 7;   // DEBUG
}

static void clearWiFiProvisioningArtifacts() {
  if (LittleFS.exists(FILE_CONFIG))     LittleFS.remove(FILE_CONFIG);
  if (LittleFS.exists(FILE_CONFIG_BKP)) LittleFS.remove(FILE_CONFIG_BKP);
  if (LittleFS.exists(FILE_DRD))        LittleFS.remove(FILE_DRD);
}

void resetWiFiConfig() {
  logMsg("[CFG] Resetting WiFi configuration");
  memset(&g_wifiConfig, 0, sizeof(g_wifiConfig));
  clearWiFiProvisioningArtifacts();
  logMsg("[CFG] WiFi reset complete - restart required");
}

// ============================================================
// FULL RESET
// ============================================================

void resetAllConfig() {
  logMsg("[CFG] Resetting all configuration to defaults");
  initDefaultGlobalConfig();
  initDefaultFermenterConfig();
  initDefaultProbeConfig();
  initDefaultSmartPlugConfig();
  initDefaultProfileConfig();
  initDefaultTiltConfig();
  initDefaultiSpindelConfig();
  initDefaultPlaatoConfig();
  initDefaultBrewServiceConfig();
  initDefaultMqttConfig();
  initDefaultSyslogConfig();
  saveAllConfig();

  // Remove WiFi config so WiFiManager re-runs the portal
  clearWiFiProvisioningArtifacts();

  logMsg("[CFG] Reset complete - restart required");
}

// ============================================================
// REBOOT LOGGING
// ============================================================

void recordReboot(const String& reason) {
  JsonDocument doc;
  String existing = loadJsonFile(FILE_REBOOT);
  if (existing.length() > 2) {
    deserializeJson(doc, existing);
  }

  // Keep a rolling log of last 10 reboots
  JsonArray log = !doc["log"].isNull() ? doc["log"].as<JsonArray>()
                                       : doc["log"].to<JsonArray>();

  struct rst_info *ri = ESP.getResetInfoPtr();

  JsonDocument entry;
  entry["reason"]   = reason;
  entry["uptime"]   = g_globalConfig.lastUptime;
  entry["heap"]     = ESP.getFreeHeap();
  entry["rsn_code"] = ri->reason;
  if (ri->reason == REASON_EXCEPTION_RST) {
    entry["exccause"]  = ri->exccause;
    entry["epc1"]      = ri->epc1;
    entry["excvaddr"]  = ri->excvaddr;
  }

  if (log.size() >= 10) log.remove(0);
  log.add(entry.as<JsonObject>());

  String json;
  serializeJson(doc, json);
  saveJsonFileSafe(FILE_REBOOT, FILE_REBOOT_BKP, json);
}
