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

// Try primary, fall back to backup if primary missing/corrupt
String loadJsonFileSafe(const char* primary, const char* backup) {
  String content = loadJsonFile(primary);
  if (content.length() > 2) return content;
  logMsg("[CFG] Falling back to backup: %s", backup);
  return loadJsonFile(backup);
}

// ============================================================
// GLOBAL CONFIG
// Note: original uses lowercase "authcode" not "authCode"
// ============================================================

bool loadGlobalConfig() {
  String json = loadJsonFileSafe(FILE_GLOBAL, FILE_GLOBAL_BKP);
  if (json.length() < 2) {
    logMsg("[CFG] Global config not found - using defaults");
    initDefaultGlobalConfig();
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    logMsg("[CFG] Global JSON parse error: %s", err.c_str());
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
  DynamicJsonDocument doc(1024);
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

  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_GLOBAL, FILE_GLOBAL_BKP, json);
}

// ============================================================
// FERMENTER CONFIG
// ============================================================

bool loadFermenterConfig() {
  String json = loadJsonFileSafe(FILE_FERMENTER, FILE_FERMENTER_BKP);
  if (json.length() < 2) {
    initDefaultFermenterConfig();
    return false;
  }

  DynamicJsonDocument doc(6144);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    logMsg("[CFG] Fermenter JSON error: %s", err.c_str());
    initDefaultFermenterConfig();
    return false;
  }

  for (int i = 0; i < MAX_FERMENTERS; i++) {
    strlcpy(g_fermenters[i].fermenterName, doc["FermenterName"][i] | "Fermenter", sizeof(g_fermenters[i].fermenterName));
    strlcpy(g_fermenters[i].beerName,      doc["BeerName"][i]      | "Beer",      sizeof(g_fermenters[i].beerName));
    strlcpy(g_fermenters[i].yeastName,     doc["YeastName"][i]     | "Yeast",     sizeof(g_fermenters[i].yeastName));
    strlcpy(g_fermenters[i].bjcp,          doc["BJCP"][i]          | "BJCP",      sizeof(g_fermenters[i].bjcp));
    g_fermenters[i].ceilingTemp     = doc["CeilingTemp"][i]    | 20.0f;
    g_fermenters[i].floorTemp       = doc["FloorTemp"][i]      | 20.0f;
    g_fermenters[i].og              = doc["OG"][i]             | 1050.0f;
    g_fermenters[i].tg              = doc["TG"][i]             | 1010.0f;
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
    // Backward compat: migrate old bool BrewServiceSend → bit 0 of new bitmask
    if (doc.containsKey("BrewServices")) {
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
  return true;
}

bool saveFermenterConfig() {
  DynamicJsonDocument doc(6144);

  JsonArray nameArr  = doc.createNestedArray("FermenterName");
  JsonArray beerArr  = doc.createNestedArray("BeerName");
  JsonArray yeastArr = doc.createNestedArray("YeastName");
  JsonArray bjcpArr  = doc.createNestedArray("BJCP");
  JsonArray ceilArr  = doc.createNestedArray("CeilingTemp");
  JsonArray floorArr = doc.createNestedArray("FloorTemp");
  JsonArray ogArr    = doc.createNestedArray("OG");
  JsonArray tgArr    = doc.createNestedArray("TG");
  JsonArray hystArr  = doc.createNestedArray("Hysteresis");
  JsonArray compArr  = doc.createNestedArray("CompressorDelay");
  JsonArray tcArr    = doc.createNestedArray("TempControl");
  JsonArray sgcArr   = doc.createNestedArray("SGControl");
  JsonArray pwrArr   = doc.createNestedArray("Power");
  JsonArray alTolArr = doc.createNestedArray("AlarmTolerance");
  JsonArray ambArr   = doc.createNestedArray("AmbientSG");
  JsonArray almArr   = doc.createNestedArray("Alarm");
  JsonArray profArr  = doc.createNestedArray("ProfileNo");
  JsonArray csArr    = doc.createNestedArray("CurrentStep");
  JsonArray hourArr  = doc.createNestedArray("CurrentHour");
  JsonArray ltArr    = doc.createNestedArray("LiveTest");
  JsonArray statArr  = doc.createNestedArray("Status");
  JsonArray prRunArr = doc.createNestedArray("ProfileRunning");
  JsonArray bsArr    = doc.createNestedArray("BrewServices");
  JsonArray psiArr   = doc.createNestedArray("PSI_Collect");
  JsonArray fnArr    = doc.createNestedArray("Function");
  JsonArray s1Arr    = doc.createNestedArray("Series1");
  JsonArray s2Arr    = doc.createNestedArray("Series2");
  JsonArray s3Arr    = doc.createNestedArray("Series3");
  JsonArray s4Arr    = doc.createNestedArray("Series4");
  JsonArray sgcalArr = doc.createNestedArray("SGCalibration");
  JsonArray mbbArr   = doc.createNestedArray("MyBrewBuddyPSI_Colle");
  JsonArray smArr    = doc.createNestedArray("StartMillis");

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

  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_FERMENTER, FILE_FERMENTER_BKP, json);
}

// ============================================================
// PROBE CONFIG
// ============================================================

bool loadProbeConfig() {
  String json = loadJsonFileSafe(FILE_PROBE, FILE_PROBE_BKP);
  if (json.length() < 2) {
    initDefaultProbeConfig();
    return false;
  }

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, json)) {
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
  DynamicJsonDocument doc(2048);

  JsonArray nameArr = doc.createNestedArray("Probe_Name");
  JsonArray addrArr = doc.createNestedArray("Address");
  JsonArray fnArr   = doc.createNestedArray("Function");
  JsonArray fermArr = doc.createNestedArray("Fermenter");
  JsonArray tempArr = doc.createNestedArray("Temperature");
  JsonArray mbbArr  = doc.createNestedArray("MBB");
  JsonArray taArr   = doc.createNestedArray("Temp_Adjust");
  JsonArray saArr   = doc.createNestedArray("SG_Adjust");

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

  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_PROBE, FILE_PROBE_BKP, json);
}

// ============================================================
// SMART PLUG CONFIG
// ============================================================

bool loadSmartPlugConfig() {
  String json = loadJsonFileSafe(FILE_SMARTPLUGS, FILE_SMARTPLUGS_BKP);
  if (json.length() < 2) {
    initDefaultSmartPlugConfig();
    return false;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, json)) {
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
  DynamicJsonDocument doc(4096);

  JsonArray typeArr  = doc.createNestedArray("Type");
  JsonArray csArr    = doc.createNestedArray("Codeset");
  JsonArray prArr    = doc.createNestedArray("Protocol");
  JsonArray bitsArr  = doc.createNestedArray("Bits");
  JsonArray dlArr    = doc.createNestedArray("DelayLength");
  JsonArray fnArr    = doc.createNestedArray("Function");
  JsonArray fermArr  = doc.createNestedArray("Fermenter");
  JsonArray onArr    = doc.createNestedArray("OnCode");
  JsonArray offArr   = doc.createNestedArray("OffCode");
  JsonArray mfgArr   = doc.createNestedArray("Manufacturer");
  JsonArray modArr   = doc.createNestedArray("Model");
  JsonArray pnArr    = doc.createNestedArray("PlugNo");

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

  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_SMARTPLUGS, FILE_SMARTPLUGS_BKP, json);
}

// ============================================================
// PROFILE CONFIG + STEPS
// ============================================================

bool loadProfileConfig() {
  String json = loadJsonFileSafe(FILE_PROFILE, FILE_PROFILE_BKP);
  if (json.length() < 2) {
    initDefaultProfileConfig();
    return false;
  }
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, json)) {
    initDefaultProfileConfig();
    return false;
  }
  for (int i = 0; i < MAX_PROFILES; i++) {
    strlcpy(g_profiles[i].profileName, doc["ProfileName"][i] | "Empty Profile", sizeof(g_profiles[i].profileName));
  }
  return true;
}

bool saveProfileConfig() {
  DynamicJsonDocument doc(512);
  JsonArray nameArr = doc.createNestedArray("ProfileName");
  for (int i = 0; i < MAX_PROFILES; i++) {
    nameArr.add(g_profiles[i].profileName);
  }
  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_PROFILE, FILE_PROFILE_BKP, json);
}

bool loadProfileSteps() {
  String json = loadJsonFileSafe(FILE_STEPS, FILE_STEPS_BKP);
  if (json.length() < 2) return false;

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, json)) return false;

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
  DynamicJsonDocument doc(8192);
  JsonArray snArr  = doc.createNestedArray("StepNo");
  JsonArray stArr  = doc.createNestedArray("StepType");
  JsonArray sTArr  = doc.createNestedArray("StartTemp");
  JsonArray eTArr  = doc.createNestedArray("EndTemp");
  JsonArray sgArr  = doc.createNestedArray("SGTrigger");
  JsonArray dArr   = doc.createNestedArray("Days");

  for (int i = 0; i < MAX_PROFILE_STEPS; i++) {
    snArr.add(g_profileSteps[i].stepNo);
    stArr.add(g_profileSteps[i].stepType);
    sTArr.add(g_profileSteps[i].startTemp);
    eTArr.add(g_profileSteps[i].endTemp);
    sgArr.add(g_profileSteps[i].sgTrigger);
    dArr.add(g_profileSteps[i].days);
  }
  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_STEPS, FILE_STEPS_BKP, json);
}

// ============================================================
// iSPINDEL CONFIG
// ============================================================

bool loadiSpindelConfig() {
  String json = loadJsonFileSafe(FILE_ISPINDEL, FILE_ISPINDEL_BKP);
  if (json.length() < 2) {
    initDefaultiSpindelConfig();
    return false;
  }
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, json)) {
    initDefaultiSpindelConfig();
    return false;
  }
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    strlcpy(g_iSpindels[i].name, doc["iSpindelName"][i] | "None", sizeof(g_iSpindels[i].name));
    g_iSpindels[i].id          = doc["ID"][i]                  | 0;
    g_iSpindels[i].collectData = doc["iSpindelCollectData"][i] | false;
    g_iSpindels[i].fermenter   = doc["iSpindelFermenter"][i]   | PROBE_UNASSIGNED;
    g_iSpindels[i].unit        = doc["Unit"][i]                | 1;
  }
  return true;
}

bool saveiSpindelConfig() {
  DynamicJsonDocument doc(1024);
  JsonArray nameArr = doc.createNestedArray("iSpindelName");
  JsonArray idArr   = doc.createNestedArray("ID");
  JsonArray cdArr   = doc.createNestedArray("iSpindelCollectData");
  JsonArray fiArr   = doc.createNestedArray("iSpindelFermenter");
  JsonArray unArr   = doc.createNestedArray("Unit");

  for (int i = 0; i < MAX_ISPINDELS; i++) {
    nameArr.add(g_iSpindels[i].name);
    idArr.add(g_iSpindels[i].id);
    cdArr.add((bool)g_iSpindels[i].collectData);
    fiArr.add(g_iSpindels[i].fermenter);
    unArr.add(g_iSpindels[i].unit);
  }
  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_ISPINDEL, FILE_ISPINDEL_BKP, json);
}

// ============================================================
// PLAATO CONFIG
// ============================================================

bool loadPlaatoConfig() {
  String json = loadJsonFileSafe(FILE_PLAATO, FILE_PLAATO_BKP);
  if (json.length() < 2) {
    initDefaultPlaatoConfig();
    return false;
  }
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, json)) {
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
  DynamicJsonDocument doc(1024);
  JsonArray acArr = doc.createNestedArray("AuthCode");
  JsonArray gdArr = doc.createNestedArray("GetData");
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    acArr.add(g_plaato[i].authCode);
    gdArr.add((bool)g_plaato[i].getData);
  }
  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_PLAATO, FILE_PLAATO_BKP, json);
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

  String json = loadJsonFileSafe(FILE_TILT, FILE_TILT_BKP);
  if (json.length() < 2) return false;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, json)) {
    logMsg("[CFG] Tilt JSON parse error");
    return false;
  }

  // Each slot has a colour (Address). Populate the colour-indexed g_tilts entry.
  for (int i = 0; i < MAX_TILT_SLOTS; i++) {
    uint8_t colour = doc["Address"][i] | PROBE_UNASSIGNED;
    if (colour >= MAX_TILTS) continue;  // skip unassigned or out-of-range
    g_tilts[colour].colour     = colour;
    g_tilts[colour].function   = doc["Function"][i]    | PROBE_UNASSIGNED;
    g_tilts[colour].fermenter  = doc["Fermenter"][i]   | PROBE_UNASSIGNED;
    g_tilts[colour].tempAdjust = doc["Temp_Adjust"][i] | 0.0f;
    g_tilts[colour].sgAdjust   = doc["SG_Adjust"][i]   | 0.0f;
    g_tilts[colour].mbb        = doc["MBB"][i]         | 0;
  }
  return true;
}

bool saveTiltConfig() {
  DynamicJsonDocument doc(512);
  JsonArray addrArr = doc.createNestedArray("Address");
  JsonArray fnArr   = doc.createNestedArray("Function");
  JsonArray fermArr = doc.createNestedArray("Fermenter");
  JsonArray taArr   = doc.createNestedArray("Temp_Adjust");
  JsonArray saArr   = doc.createNestedArray("SG_Adjust");
  JsonArray mbbArr  = doc.createNestedArray("MBB");

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

  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_TILT, FILE_TILT_BKP, json);
}

// ============================================================
// BREW SERVICE CONFIG (multiple service slots)
// ============================================================

bool loadBrewServiceConfig() {
  String json = loadJsonFileSafe(FILE_BREWSVC, FILE_BREWSVC_BKP);
  if (json.length() < 2) {
    // Migrate from old single-service global config
    // Old service types: 1=BF, 2=Monitor Beer (removed), 3=Brewfather (now index 1)
    initDefaultBrewServiceConfig();
    int legacySvc = g_globalConfig.brewService;
    int idx = (legacySvc == 1) ? 0 : (legacySvc == 3) ? 1 : -1;
    if (idx >= 0) {
      g_brewServices[idx].enabled = true;
      strlcpy(g_brewServices[idx].serviceId, g_globalConfig.brewServiceId, sizeof(g_brewServices[idx].serviceId));
      logMsg("[CFG] Migrated legacy brew service %d to slot %d", legacySvc, idx);
      saveBrewServiceConfig();
    }
    return false;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, json)) {
    initDefaultBrewServiceConfig();
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
  DynamicJsonDocument doc(1024);
  JsonArray enArr = doc.createNestedArray("Enabled");
  JsonArray idArr = doc.createNestedArray("ServiceId");
  JsonArray dnArr = doc.createNestedArray("DeviceName");
  for (int i = 0; i < MAX_BREW_SERVICES; i++) {
    enArr.add((bool)g_brewServices[i].enabled);
    idArr.add(g_brewServices[i].serviceId);
    dnArr.add(g_brewServices[i].deviceName);
  }
  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_BREWSVC, FILE_BREWSVC_BKP, json);
}

// ============================================================
// MQTT CONFIG
// ============================================================

bool loadMqttConfig() {
  String json = loadJsonFileSafe(FILE_MQTT, FILE_MQTT_BKP);
  if (json.length() < 2) {
    initDefaultMqttConfig();
    return false;
  }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, json)) {
    initDefaultMqttConfig();
    return false;
  }
  g_mqttConfig.enabled     = doc["enabled"]     | false;
  g_mqttConfig.haDiscovery = doc["haDiscovery"] | false;
  g_mqttConfig.port        = doc["port"]        | 1883;
  strlcpy(g_mqttConfig.host,      doc["host"]      | "", sizeof(g_mqttConfig.host));
  strlcpy(g_mqttConfig.username,  doc["username"]  | "", sizeof(g_mqttConfig.username));
  strlcpy(g_mqttConfig.password,  doc["password"]  | "", sizeof(g_mqttConfig.password));
  strlcpy(g_mqttConfig.baseTopic, doc["baseTopic"] | "ourbrewbot", sizeof(g_mqttConfig.baseTopic));
  return true;
}

bool saveMqttConfig() {
  DynamicJsonDocument doc(512);
  doc["enabled"]     = (bool)g_mqttConfig.enabled;
  doc["haDiscovery"] = (bool)g_mqttConfig.haDiscovery;
  doc["host"]        = g_mqttConfig.host;
  doc["port"]        = g_mqttConfig.port;
  doc["username"]    = g_mqttConfig.username;
  doc["password"]    = g_mqttConfig.password;
  doc["baseTopic"]   = g_mqttConfig.baseTopic;
  String json;
  serializeJson(doc, json);
  return saveJsonFileSafe(FILE_MQTT, FILE_MQTT_BKP, json);
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
    g_fermenters[i].ceilingTemp     = 20.0f;
    g_fermenters[i].floorTemp       = 20.0f;
    g_fermenters[i].og              = 1050.0f;
    g_fermenters[i].tg              = 1010.0f;
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
    g_iSpindels[i].id          = 0;
    g_iSpindels[i].collectData = false;
    g_iSpindels[i].fermenter   = (i == 0) ? 0 : PROBE_UNASSIGNED;
    g_iSpindels[i].unit        = (i == 0) ? 1 : 0;
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
  g_mqttConfig.enabled     = false;
  g_mqttConfig.haDiscovery = false;
  g_mqttConfig.port        = 1883;
  g_mqttConfig.host[0]     = '\0';
  g_mqttConfig.username[0] = '\0';
  g_mqttConfig.password[0] = '\0';
  strlcpy(g_mqttConfig.baseTopic, "ourbrewbot", sizeof(g_mqttConfig.baseTopic));
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
  saveAllConfig();

  // Remove WiFi config so WiFiManager re-runs the portal
  if (LittleFS.exists(FILE_CONFIG))     LittleFS.remove(FILE_CONFIG);
  if (LittleFS.exists(FILE_CONFIG_BKP)) LittleFS.remove(FILE_CONFIG_BKP);
  if (LittleFS.exists(FILE_DRD))        LittleFS.remove(FILE_DRD);

  logMsg("[CFG] Reset complete - restart required");
}

// ============================================================
// REBOOT LOGGING
// ============================================================

void recordReboot(const String& reason) {
  DynamicJsonDocument doc(1024);
  String existing = loadJsonFile(FILE_REBOOT);
  if (existing.length() > 2) {
    deserializeJson(doc, existing);
  }

  // Keep a rolling log of last 10 reboots
  JsonArray log = doc.containsKey("log") ? doc["log"].as<JsonArray>()
                                         : doc.createNestedArray("log");

  struct rst_info *ri = ESP.getResetInfoPtr();

  DynamicJsonDocument entry(256);
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
