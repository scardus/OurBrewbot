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
 * Config.cpp — Configuration persistence
 * Load/save all config to/from LittleFS.
 *
 * Most config files are driven by descriptor tables (see CONFIG FIELD TABLES
 * below): each persisted field is declared once with its JSON key, struct
 * member, type and load-default, and generic helpers perform both load and
 * save. This keeps the load/save/key lists from drifting apart. Migrations,
 * validation and index-dependent defaults remain explicit code next to the
 * loader they belong to. Tilt config keeps a fully custom load/save because
 * of its slot-to-colour mapping.
 */

#include "Config.h"
#include "Log.h"
#include "OriginalConfig.h"
#include "TextSafe.h"
#include "Uptime.h"
#include <user_interface.h>     // rst_info struct, REASON_EXCEPTION_RST
#include <cstddef>              // offsetof
#include <type_traits>

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
BrewServiceConfig g_brewServices[MAX_BREW_SERVICES];
MqttConfig        g_mqttConfig;
SyslogConfig      g_syslogConfig;

// ============================================================
// FILE UTILITIES
// ============================================================

// Stream-serialize a JsonDocument directly to a LittleFS file.
static bool saveJsonDocToFile(JsonDocument& doc, const char* path) {
  const size_t expected = measureJson(doc);
  File f = LittleFS.open(path, "w");
  if (!f) {
    logMsgL(SYSLOG_WARNING, "[CFG] Cannot write: %s", path);
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.close();
  if (written != expected) {
    // Partial write (e.g. filesystem full) — the file on disk is corrupt.
    logMsgL(SYSLOG_ERR, "[CFG] Truncated write: %s (%u/%u bytes)",
            path, (unsigned)written, (unsigned)expected);
    return false;
  }
  return true;
}

bool saveJsonDocSafe(JsonDocument& doc, const char* primary, const char* backup) {
  // Ran out of memory while the document was being filled in, so part of it
  // is missing. Keep the files already on flash rather than replace them.
  if (doc.overflowed()) {
    logMsgL(SYSLOG_ERR, "[CFG] Out of memory - not saved: %s", primary);
    return false;
  }
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
  logMsgL(SYSLOG_WARNING, "[CFG] Falling back to backup: %s", backup);
  return loadJsonDocFromFile(doc, backup);
}

// ============================================================
// UNSAVED FILES
//
// The 10-minute save used to rewrite every config file and its backup, 2,880
// file writes a day even when nothing had changed. Now it only writes the
// files marked here. A file is marked when something in memory changes
// without being saved straight away (profile progress, uptime, some of the
// corrections made while loading), when a save fails, or when a copy on flash
// was missing or damaged at boot.
// ============================================================

static uint16_t s_unsavedFiles = 0;   // one bit per ConfigFile

void configMarkUnsaved(ConfigFile file) {
  s_unsavedFiles |= (1u << file);
}

static bool isUnsaved(ConfigFile file) {
  return (s_unsavedFiles & (1u << file)) != 0;
}

// saveJsonDocSafe() for a config file: the mark is cleared once both copies
// are written, and set again if the save fails, so it is tried again later.
static bool saveConfigDoc(ConfigFile file, JsonDocument& doc,
                          const char* primary, const char* backup) {
  if (!saveJsonDocSafe(doc, primary, backup)) {
    configMarkUnsaved(file);
    return false;
  }
  s_unsavedFiles &= ~(1u << file);
  return true;
}

// loadJsonDocSafe() for a config file. The old 10-minute save also replaced a
// missing or damaged copy without meaning to, so now that unchanged files are
// left alone, a bad copy is marked unsaved here and the 10-minute save writes
// it again. The backup is checked first, on its own, so two files are never
// in memory at once.
static bool loadConfigDoc(ConfigFile file, JsonDocument& doc,
                          const char* primary, const char* backup) {
  bool backupOk = loadJsonDocFromFile(doc, backup);
  doc.clear();
  if (loadJsonDocFromFile(doc, primary)) {
    if (!backupOk) {
      logMsgL(SYSLOG_WARNING, "[CFG] Backup missing or damaged - it will be saved again: %s", backup);
      configMarkUnsaved(file);
    }
    return true;
  }
  doc.clear();
  configMarkUnsaved(file);
  if (!backupOk) return false;
  logMsgL(SYSLOG_WARNING, "[CFG] Falling back to backup: %s", backup);
  return loadJsonDocFromFile(doc, backup);
}

// ============================================================
// DESCRIPTOR-DRIVEN FIELD I/O
//
// A CfgField describes one persisted struct member. Tables of CfgField
// (one per config file, in JSON key order — table order defines save
// order, which must stay byte-identical to previous releases) drive the
// generic load/save helpers below. Tables and their key strings live in
// PROGMEM; rows are copied to the stack with memcpy_P before use.
// ============================================================

enum CfgType : uint8_t { CT_STR, CT_BOOL, CT_U8, CT_U16, CT_U32, CT_FLOAT };

struct CfgField {
  PGM_P       key;      // JSON key (PROGMEM string)
  uint16_t    offset;   // offsetof() the member within the struct
  CfgType     type;
  uint8_t     strSize;  // CT_STR only: sizeof() the char-array member
  float       defNum;   // load default for numeric/bool fields
  const char* defStr;   // load default for CT_STR fields
};

// Compile-time check that the declared CfgType matches the member's C++ type.
template <CfgType CT> struct CfgCppType            { using type = void;     };
template <> struct CfgCppType<CT_BOOL>             { using type = bool;     };
template <> struct CfgCppType<CT_U8>               { using type = uint8_t;  };
template <> struct CfgCppType<CT_U16>              { using type = uint16_t; };
template <> struct CfgCppType<CT_U32>              { using type = uint32_t; };
template <> struct CfgCppType<CT_FLOAT>            { using type = float;    };

template <typename M, CfgType CT>
constexpr bool cfgTypeOk() {
  return (CT == CT_STR) ? std::is_array<M>::value
                        : std::is_same<M, typename CfgCppType<CT>::type>::value;
}

// Copy one JSON value into a struct member, applying the load default when the
// value is missing or of the wrong type (same `doc[key] | default` semantics
// the hand-written loaders used).
static void cfgLoadField(void* obj, const CfgField& f, JsonVariantConst v) {
  void* p = (uint8_t*)obj + f.offset;
  switch (f.type) {
    case CT_STR: {
      // copyText() rather than strlcpy(): a name saved by older firmware may
      // end in half a character, and an imported file can hold anything, so
      // the text is made valid UTF-8 again as it is loaded.
      const char* s = v.as<const char*>();
      copyText((char*)p, s ? s : (f.defStr ? f.defStr : ""), f.strSize);
      break;
    }
    case CT_BOOL:  *(bool*)p     = v | (f.defNum != 0.0f);  break;
    case CT_U8:    *(uint8_t*)p  = v | (uint8_t)f.defNum;   break;
    case CT_U16:   *(uint16_t*)p = v | (uint16_t)f.defNum;  break;
    case CT_U32:   *(uint32_t*)p = v | (uint32_t)f.defNum;  break;
    case CT_FLOAT: *(float*)p    = v | f.defNum;            break;
  }
}

// Copy one struct member into a JSON slot (object member or array element).
static void cfgSaveField(const void* obj, const CfgField& f, JsonVariant v) {
  const void* p = (const uint8_t*)obj + f.offset;
  switch (f.type) {
    case CT_STR:   v.set((const char*)p);      break;
    case CT_BOOL:  v.set(*(const bool*)p);     break;
    case CT_U8:    v.set(*(const uint8_t*)p);  break;
    case CT_U16:   v.set(*(const uint16_t*)p); break;
    case CT_U32:   v.set(*(const uint32_t*)p); break;
    case CT_FLOAT: v.set(*(const float*)p);    break;
  }
}

// Scalar config files: { "key": value, ... }
static void cfgLoadScalar(JsonDocument& doc, void* obj,
                          const CfgField* table, size_t n) {
  for (size_t k = 0; k < n; k++) {
    CfgField f; memcpy_P(&f, &table[k], sizeof(f));
    cfgLoadField(obj, f, doc[FPSTR(f.key)]);
  }
}

static void cfgSaveScalar(JsonDocument& doc, const void* obj,
                          const CfgField* table, size_t n) {
  for (size_t k = 0; k < n; k++) {
    CfgField f; memcpy_P(&f, &table[k], sizeof(f));
    cfgSaveField(obj, f, doc[FPSTR(f.key)].to<JsonVariant>());
  }
}

// Array config files: { "key": [v0, v1, ...], ... } — one array per field,
// element i belongs to struct instance i (base + i * stride).
static void cfgLoadArray(JsonDocument& doc, void* base, size_t stride,
                         int count, const CfgField* table, size_t n) {
  for (size_t k = 0; k < n; k++) {
    CfgField f; memcpy_P(&f, &table[k], sizeof(f));
    for (int i = 0; i < count; i++) {
      cfgLoadField((uint8_t*)base + i * stride, f, doc[FPSTR(f.key)][i]);
    }
  }
}

static void cfgSaveArray(JsonDocument& doc, const void* base, size_t stride,
                         int count, const CfgField* table, size_t n) {
  for (size_t k = 0; k < n; k++) {
    CfgField f; memcpy_P(&f, &table[k], sizeof(f));
    JsonArray arr = doc[FPSTR(f.key)].to<JsonArray>();
    for (int i = 0; i < count; i++) {
      cfgSaveField((const uint8_t*)base + i * stride, f, arr.add<JsonVariant>());
    }
  }
}

#define CFG_COUNT(t) (sizeof(t) / sizeof((t)[0]))

// ============================================================
// CONFIG FIELD TABLES
//
// Each table is declared from an X-macro field list:
//   X(jsonKey, member, TYPE, loadDefault)
// expanded three times per struct: PROGMEM key strings, compile-time type
// checks, and the PROGMEM CfgField table itself. To add a persisted field,
// add one X(...) line — load and save both pick it up. Table order defines
// the JSON key order in saved files: append new fields at the end unless
// byte-identical output no longer matters.
// ============================================================

// Per-type helpers used by CFG_FIELD — size only applies to STR, and the
// default lands in defNum (numeric/bool) or defStr (string).
#define CFG_SIZE_STR(S, m)    (uint8_t)sizeof(((S*)nullptr)->m)
#define CFG_SIZE_BOOL(S, m)   0
#define CFG_SIZE_U8(S, m)     0
#define CFG_SIZE_U16(S, m)    0
#define CFG_SIZE_U32(S, m)    0
#define CFG_SIZE_FLOAT(S, m)  0
#define CFG_NUMDEF_STR(d)     0.0f
#define CFG_NUMDEF_BOOL(d)    ((d) ? 1.0f : 0.0f)
#define CFG_NUMDEF_U8(d)      (float)(d)
#define CFG_NUMDEF_U16(d)     (float)(d)
#define CFG_NUMDEF_U32(d)     (float)(d)
#define CFG_NUMDEF_FLOAT(d)   (d)
#define CFG_STRDEF_STR(d)     (d)
#define CFG_STRDEF_BOOL(d)    nullptr
#define CFG_STRDEF_U8(d)      nullptr
#define CFG_STRDEF_U16(d)     nullptr
#define CFG_STRDEF_U32(d)     nullptr
#define CFG_STRDEF_FLOAT(d)   nullptr

#define CFG_FIELD(S, keyArr, member, type, def) \
  { keyArr, (uint16_t)offsetof(S, member), CT_##type, \
    CFG_SIZE_##type(S, member), CFG_NUMDEF_##type(def), CFG_STRDEF_##type(def) },

#define CFG_ASSERT_TYPE(S, member, type) \
  static_assert(cfgTypeOk<decltype(((S*)nullptr)->member), CT_##type>(), \
                "CfgField type mismatch: " #S "::" #member);

// ---------- GlobalConfig (jsonGlobal.txt) ----------
// Note: original firmware uses lowercase "authcode" not "authCode"
#define GLOBAL_CONFIG_FIELDS(X) \
  X("authcode",        authCode,      STR,   "")                 \
  X("unit",            unit,          U8,    UNIT_CELSIUS)       \
  X("notifyon",        notifyOn,      BOOL,  true)               \
  X("brewserviceid",   brewServiceId, STR,   "")                 \
  X("brewservice",     brewService,   U8,    BREW_SERVICE_NONE)  \
  X("migrated",        migrated,      BOOL,  false)              \
  X("blebaud",         bleBaud,       U16,   9600)               \
  X("lastuptime",      lastUptime,    U32,   0)                  \
  X("swno",            swNo,          U8,    0)                  \
  X("sendtocloud",     sendToCloud,   U8,    0)                  \
  X("globalsave",      globalSave,    U8,    0)                  \
  X("mybrewbuddy",     myBrewBuddy,   STR,   "")                 \
  X("bsitterauth",     bSitterAuth,   STR,   "")                 \
  X("babysitter",      babySitter,    U8,    0)                  \
  X("plugcategory",    plugCategory,  BOOL,  true)               \
  X("fno",             fNo,           U8,    1)                  \
  X("mbbhardreset",    mbbHardReset,  U8,    0)                  \
  X("tuning_chart_no", tuningChartNo, U8,    0)                  \
  X("resolution",      resolution,    U8,    11)                 \
  X("alarm_dwell_sec", alarmDwellSec, U16,   600)                \
  X("mdns_enabled",    mdnsEnabled,   BOOL,  true)                \
  X("update_check",    updateCheck,   BOOL,  true)                \
  X("crash_reports",   crashReports,  BOOL,  true)

#define CFG_KEY(key, member, type, def) static const char kGKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(GlobalConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(GlobalConfig, kGKey_##member, member, type, def)
GLOBAL_CONFIG_FIELDS(CFG_KEY)
GLOBAL_CONFIG_FIELDS(CFG_CHK)
static const CfgField kGlobalFields[] PROGMEM = { GLOBAL_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- FermenterConfig (jsonFermenter.txt) ----------
#define FERMENTER_CONFIG_FIELDS(X) \
  X("FermenterName",        fermenterName,   STR,   "Fermenter")  \
  X("BeerName",             beerName,        STR,   "Beer")       \
  X("YeastName",            yeastName,       STR,   "Yeast")      \
  X("BJCP",                 bjcp,            STR,   "BJCP")       \
  X("CeilingTemp",          ceilingTemp,     FLOAT, 22.0f)        \
  X("FloorTemp",            floorTemp,       FLOAT, 18.0f)        \
  X("OG",                   og,              FLOAT, 1.050f)       \
  X("TG",                   tg,              FLOAT, 1.010f)       \
  X("Hysteresis",           hysteresis,      FLOAT, 0.5f)         \
  X("CompressorDelay",      compressorDelay, U16,   10)           \
  X("TempControl",          tempControl,     BOOL,  true)         \
  X("SGControl",            sgControl,       BOOL,  false)        \
  X("Power",                power,           BOOL,  false)        \
  X("AlarmTolerance",       alarmTolerance,  FLOAT, 3.0f)         \
  X("AmbientSG",            ambientSG,       FLOAT, 0.0f)         \
  X("Alarm",                alarm,           BOOL,  false)        \
  X("ProfileNo",            profileNo,       U8,    0)            \
  X("CurrentStep",          currentStep,     U8,    0)            \
  X("CurrentHour",          currentHour,     U16,   0)            \
  X("ProfileHours",         profileHours,    U16,   0)            \
  X("HourTicks",            hourTicks,       U8,    0)            \
  X("LiveTest",             liveTest,        BOOL,  false)        \
  X("Status",               status,          U8,    0)            \
  X("ProfileRunning",       profileRunning,  BOOL,  false)        \
  X("ProfilePaused",        profilePaused,   BOOL,  false)        \
  X("BrewServices",         brewServices,    U8,    0)            \
  X("PSI_Collect",          psiCollect,      BOOL,  false)        \
  X("Function",             function,        U8,    1)            \
  X("Series1",              series1,         U8,    1)            \
  X("Series2",              series2,         U8,    3)            \
  X("Series3",              series3,         U8,    8)            \
  X("Series4",              series4,         U8,    2)            \
  X("SGCalibration",        sgCalibration,   FLOAT, 0.0f)         \
  X("MyBrewBuddyPSI_Colle", mbbPsiCollect,   BOOL,  false)        \
  X("StartMillis",          startMillis,     U32,   0)

#define CFG_KEY(key, member, type, def) static const char kFKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(FermenterConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(FermenterConfig, kFKey_##member, member, type, def)
FERMENTER_CONFIG_FIELDS(CFG_KEY)
FERMENTER_CONFIG_FIELDS(CFG_CHK)
static const CfgField kFermenterFields[] PROGMEM = { FERMENTER_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- ProbeConfig (jsonProbe.txt) ----------
#define PROBE_CONFIG_FIELDS(X) \
  X("Probe_Name",  probeName,   STR,   "Probe")           \
  X("Address",     address,     STR,   "")                \
  X("Function",    function,    U8,    PROBE_UNASSIGNED)  \
  X("Fermenter",   fermenter,   U8,    PROBE_UNASSIGNED)  \
  X("Temperature", temperature, FLOAT, 0.0f)              \
  X("MBB",         mbb,         U8,    0)                 \
  X("Temp_Adjust", tempAdjust,  FLOAT, 0.0f)              \
  X("SG_Adjust",   sgAdjust,    FLOAT, 0.0f)

#define CFG_KEY(key, member, type, def) static const char kPKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(ProbeConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(ProbeConfig, kPKey_##member, member, type, def)
PROBE_CONFIG_FIELDS(CFG_KEY)
PROBE_CONFIG_FIELDS(CFG_CHK)
static const CfgField kProbeFields[] PROGMEM = { PROBE_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- SmartPlugConfig (jsonSmartPlugs.txt) ----------
// PlugNo's load default is the slot index — handled after the table load.
#define SMARTPLUG_CONFIG_FIELDS(X) \
  X("Type",         type,         U8,    1)                  \
  X("Codeset",      codeset,      U8,    1)                  \
  X("Protocol",     protocol,     U8,    1)                  \
  X("Bits",         bits,         U8,    24)                 \
  X("DelayLength",  delayLength,  U16,   160)                \
  X("Function",     function,     U8,    PLUG_FN_UNASSIGNED) \
  X("Fermenter",    fermenter,    U8,    PROBE_UNASSIGNED)   \
  X("OnCode",       onCode,       U32,   0)                  \
  X("OffCode",      offCode,      U32,   0)                  \
  X("Manufacturer", manufacturer, STR,   "Unknown")          \
  X("Model",        model,        STR,   "Model")            \
  X("PlugNo",       plugNo,       U8,    0)

#define CFG_KEY(key, member, type, def) static const char kSPKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(SmartPlugConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(SmartPlugConfig, kSPKey_##member, member, type, def)
SMARTPLUG_CONFIG_FIELDS(CFG_KEY)
SMARTPLUG_CONFIG_FIELDS(CFG_CHK)
static const CfgField kSmartPlugFields[] PROGMEM = { SMARTPLUG_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- ProfileConfig (jsonProfile.txt) ----------
#define PROFILE_CONFIG_FIELDS(X) \
  X("ProfileName", profileName, STR, "Empty Profile")

#define CFG_KEY(key, member, type, def) static const char kPRKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(ProfileConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(ProfileConfig, kPRKey_##member, member, type, def)
PROFILE_CONFIG_FIELDS(CFG_KEY)
PROFILE_CONFIG_FIELDS(CFG_CHK)
static const CfgField kProfileFields[] PROGMEM = { PROFILE_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- ProfileStep (jsonProfileSteps.txt) ----------
#define PROFILE_STEP_FIELDS(X) \
  X("StepNo",    stepNo,    U8,    0)     \
  X("StepType",  stepType,  U8,    0)     \
  X("StartTemp", startTemp, FLOAT, 0.0f)  \
  X("EndTemp",   endTemp,   FLOAT, 0.0f)  \
  X("SGTrigger", sgTrigger, FLOAT, 0.0f)  \
  X("Days",      days,      FLOAT, 0.0f)

#define CFG_KEY(key, member, type, def) static const char kSTKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(ProfileStep, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(ProfileStep, kSTKey_##member, member, type, def)
PROFILE_STEP_FIELDS(CFG_KEY)
PROFILE_STEP_FIELDS(CFG_CHK)
static const CfgField kProfileStepFields[] PROGMEM = { PROFILE_STEP_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- iSpindelConfig (jsoniSpindel.txt) ----------
// Function defaults to PROBE_FN_BEER for legacy configs without the field —
// preserves existing behavior where iSpindel temperature flowed into the
// beer-temp chain. Legacy value collapse happens after the table load.
// Unit defaults to ISPINDEL_UNIT_PLATO (1), which looks surprising but is
// deliberate: a device that declares no gravity unit is assumed to be sending
// Plato, and a device that does declare one overwrites this on its next POST.
#define ISPINDEL_CONFIG_FIELDS(X) \
  X("iSpindelName",        name,        STR,   "None")            \
  X("ID",                  id,          STR,   "")                \
  X("iSpindelCollectData", collectData, BOOL,  false)             \
  X("iSpindelFermenter",   fermenter,   U8,    PROBE_UNASSIGNED)  \
  X("Unit",                unit,        U8,    1)                 \
  X("Function",            function,    U8,    PROBE_FN_BEER)     \
  X("TempAdjust",          tempAdjust,  FLOAT, 0.0f)              \
  X("SGAdjust",            sgAdjust,    FLOAT, 0.0f)

#define CFG_KEY(key, member, type, def) static const char kISKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(iSpindelConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(iSpindelConfig, kISKey_##member, member, type, def)
ISPINDEL_CONFIG_FIELDS(CFG_KEY)
ISPINDEL_CONFIG_FIELDS(CFG_CHK)
static const CfgField kiSpindelFields[] PROGMEM = { ISPINDEL_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- BrewServiceConfig (jsonBrewServices.txt) ----------
#define BREWSVC_CONFIG_FIELDS(X) \
  X("Enabled",    enabled,    BOOL, false)         \
  X("ServiceId",  serviceId,  STR,  "")            \
  X("DeviceName", deviceName, STR,  "OurBrewbot")

#define CFG_KEY(key, member, type, def) static const char kBSKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(BrewServiceConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(BrewServiceConfig, kBSKey_##member, member, type, def)
BREWSVC_CONFIG_FIELDS(CFG_KEY)
BREWSVC_CONFIG_FIELDS(CFG_CHK)
static const CfgField kBrewSvcFields[] PROGMEM = { BREWSVC_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- MqttConfig (jsonMqtt.txt) ----------
#define MQTT_CONFIG_FIELDS(X) \
  X("enabled",      enabled,      BOOL, false)         \
  X("haDiscovery",  haDiscovery,  BOOL, false)         \
  X("allowControl", allowControl, BOOL, false)         \
  X("logEnabled",   logEnabled,   BOOL, false)         \
  X("host",         host,         STR,  "")            \
  X("port",         port,         U16,  1883)          \
  X("username",     username,     STR,  "")            \
  X("password",     password,     STR,  "")            \
  X("baseTopic",    baseTopic,    STR,  "ourbrewbot")

#define CFG_KEY(key, member, type, def) static const char kMQKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(MqttConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(MqttConfig, kMQKey_##member, member, type, def)
MQTT_CONFIG_FIELDS(CFG_KEY)
MQTT_CONFIG_FIELDS(CFG_CHK)
static const CfgField kMqttFields[] PROGMEM = { MQTT_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ---------- SyslogConfig (jsonSyslog.txt) ----------
#define SYSLOG_CONFIG_FIELDS(X) \
  X("enabled",  enabled,  BOOL, false)  \
  X("host",     host,     STR,  "")     \
  X("port",     port,     U16,  514)    \
  X("facility", facility, U8,   16)     \
  X("minLevel", minLevel, U8,   7)

#define CFG_KEY(key, member, type, def) static const char kSYKey_##member[] PROGMEM = key;
#define CFG_CHK(key, member, type, def) CFG_ASSERT_TYPE(SyslogConfig, member, type)
#define CFG_ROW(key, member, type, def) CFG_FIELD(SyslogConfig, kSYKey_##member, member, type, def)
SYSLOG_CONFIG_FIELDS(CFG_KEY)
SYSLOG_CONFIG_FIELDS(CFG_CHK)
static const CfgField kSyslogFields[] PROGMEM = { SYSLOG_CONFIG_FIELDS(CFG_ROW) };
#undef CFG_KEY
#undef CFG_CHK
#undef CFG_ROW

// ============================================================
// GLOBAL CONFIG
// ============================================================

bool loadGlobalConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_GLOBAL, doc, FILE_GLOBAL, FILE_GLOBAL_BKP)) {
    logMsg("[CFG] Global config not found or invalid - using defaults");
    initDefaultGlobalConfig();
    return false;
  }
  cfgLoadScalar(doc, &g_globalConfig, kGlobalFields, CFG_COUNT(kGlobalFields));

  // Range-check what the WebUI checks on save, as a bad or imported file can
  // hold anything. The probe wait is 94 << (resolution - 9) ms, which goes
  // wrong for anything outside 9-12 bits.
  if (g_globalConfig.resolution < 9 || g_globalConfig.resolution > 12) {
    logMsg("[CFG] Probe resolution %u is out of range (9-12) - using 11",
           g_globalConfig.resolution);
    g_globalConfig.resolution = 11;
    saveGlobalConfig();
  }
  return true;
}

bool saveGlobalConfig() {
  JsonDocument doc;
  cfgSaveScalar(doc, &g_globalConfig, kGlobalFields, CFG_COUNT(kGlobalFields));
  return saveConfigDoc(CFG_FILE_GLOBAL, doc, FILE_GLOBAL, FILE_GLOBAL_BKP);
}

// ============================================================
// FERMENTER CONFIG
// ============================================================

bool loadFermenterConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_FERMENTER, doc, FILE_FERMENTER, FILE_FERMENTER_BKP)) {
    logMsg("[CFG] Fermenter config not found or invalid - using defaults");
    initDefaultFermenterConfig();
    return false;
  }

  cfgLoadArray(doc, g_fermenters, sizeof(FermenterConfig), MAX_FERMENTERS,
               kFermenterFields, CFG_COUNT(kFermenterFields));

  // Migrate old integer-format gravity values (e.g. 1050 → 1.050)
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (g_fermenters[i].og > 2.0f || g_fermenters[i].tg > 2.0f) configMarkUnsaved(CFG_FILE_FERMENTER);
    if (g_fermenters[i].og > 2.0f) g_fermenters[i].og /= 1000.0f;
    if (g_fermenters[i].tg > 2.0f) g_fermenters[i].tg /= 1000.0f;
  }

  // Validate temperature/hysteresis trio per fermenter; reset offending
  // fields to defaults so an invalid persisted config can't lock the user
  // out of the admin POST validation.
  bool corrected = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    float c = g_fermenters[i].ceilingTemp;
    float f = g_fermenters[i].floorTemp;
    float h = g_fermenters[i].hysteresis;
    if (!checkTempBand(c, f, h, nullptr)) {
      logMsg("[CFG] Fermenter %d: invalid temp/hyst trio (c=%.2f f=%.2f h=%.2f); reset to defaults", i, c, f, h);
      g_fermenters[i].ceilingTemp = 22.0f;
      g_fermenters[i].floorTemp   = 18.0f;
      g_fermenters[i].hysteresis  = 0.5f;
      corrected = true;
    }
  }
  // Profile number and step index the profile steps array, so a bad value
  // would read past its end
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    FermenterConfig& f = g_fermenters[i];
    if (f.profileNo > MAX_PROFILES) {
      logMsg("[CFG] Fermenter %d: profile %u does not exist - profile stopped", i, f.profileNo);
      f.profileNo      = 0;
      f.profileRunning = false;
      f.profilePaused  = false;
      f.currentStep    = 0;
      f.currentHour    = 0;
      corrected = true;
    }
    if (f.currentStep >= MAX_STEPS_PER_PROFILE) {
      logMsg("[CFG] Fermenter %d: profile step %u is out of range - back to step 1", i, f.currentStep);
      f.currentStep = 0;
      f.currentHour = 0;
      f.hourTicks   = 0;
      corrected = true;
    }
    if (f.hourTicks >= 6) {   // six 10-minute ticks make an hour
      f.hourTicks = 0;
      corrected = true;
    }
  }
  if (corrected) saveFermenterConfig();
  return true;
}

bool saveFermenterConfig() {
  JsonDocument doc;
  cfgSaveArray(doc, g_fermenters, sizeof(FermenterConfig), MAX_FERMENTERS,
               kFermenterFields, CFG_COUNT(kFermenterFields));
  return saveConfigDoc(CFG_FILE_FERMENTER, doc, FILE_FERMENTER, FILE_FERMENTER_BKP);
}

// ============================================================
// PROBE CONFIG
// ============================================================

bool loadProbeConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_PROBE, doc, FILE_PROBE, FILE_PROBE_BKP)) {
    initDefaultProbeConfig();
    return false;
  }
  cfgLoadArray(doc, g_probes, sizeof(ProbeConfig), MAX_PROBES,
               kProbeFields, CFG_COUNT(kProbeFields));
  // No reading until each probe is read. The saved "Temperature" can be hours
  // old, and a probe missing at boot used to keep it for its first 6 failed
  // reads (~30 s), so control and alarms ran on it. The key stays in the file.
  for (int i = 0; i < MAX_PROBES; i++) {
    g_probes[i].temperature    = TEMP_NONE;
    g_probes[i].rawTemperature = TEMP_NONE;
  }
  return true;
}

bool saveProbeConfig() {
  JsonDocument doc;
  cfgSaveArray(doc, g_probes, sizeof(ProbeConfig), MAX_PROBES,
               kProbeFields, CFG_COUNT(kProbeFields));
  return saveConfigDoc(CFG_FILE_PROBE, doc, FILE_PROBE, FILE_PROBE_BKP);
}

// ============================================================
// SMART PLUG CONFIG
// ============================================================

bool loadSmartPlugConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_SMARTPLUGS, doc, FILE_SMARTPLUGS, FILE_SMARTPLUGS_BKP)) {
    initDefaultSmartPlugConfig();
    return false;
  }
  cfgLoadArray(doc, g_smartPlugs, sizeof(SmartPlugConfig), MAX_SMART_PLUGS,
               kSmartPlugFields, CFG_COUNT(kSmartPlugFields));
  // PlugNo defaults to the slot index when absent (index-dependent default)
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    g_smartPlugs[i].plugNo = doc["PlugNo"][i] | (uint8_t)i;
  }
  // The same 1-32 bit range the WebUI enforces: an RF code is at most 32 bits.
  // (The RF library already falls back to protocol 1 for a bad protocol.)
  bool corrected = false;
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    if (g_smartPlugs[i].bits < 1 || g_smartPlugs[i].bits > 32) {
      logMsg("[CFG] Plug %d: %u-bit code is out of range (1-32) - using 24", i, g_smartPlugs[i].bits);
      g_smartPlugs[i].bits = 24;
      corrected = true;
    }
  }
  if (corrected) saveSmartPlugConfig();
  // RF timing that would trip the watchdog is only logged: rfTransmit() won't
  // send it, and the file keeps the setting so it can be seen and corrected.
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    const SmartPlugConfig& p = g_smartPlugs[i];
    const char* rfErr = nullptr;
    if ((p.onCode != 0 || p.offCode != 0) &&
        !checkRfSettings(p.protocol, p.bits, p.delayLength, &rfErr)) {
      char why[64];
      strncpy_P(why, rfErr, sizeof(why) - 1);   // the reason is in flash
      why[sizeof(why) - 1] = '\0';
      logMsg("[CFG] Plug %d: %s - it won't be sent until this is corrected", i, why);
    }
  }
  return true;
}

bool saveSmartPlugConfig() {
  JsonDocument doc;
  cfgSaveArray(doc, g_smartPlugs, sizeof(SmartPlugConfig), MAX_SMART_PLUGS,
               kSmartPlugFields, CFG_COUNT(kSmartPlugFields));
  return saveConfigDoc(CFG_FILE_SMARTPLUGS, doc, FILE_SMARTPLUGS, FILE_SMARTPLUGS_BKP);
}

// ============================================================
// PROFILE CONFIG + STEPS
// ============================================================

bool loadProfileConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_PROFILE, doc, FILE_PROFILE, FILE_PROFILE_BKP)) {
    initDefaultProfileConfig();
    return false;
  }
  cfgLoadArray(doc, g_profiles, sizeof(ProfileConfig), MAX_PROFILES,
               kProfileFields, CFG_COUNT(kProfileFields));
  return true;
}

bool saveProfileConfig() {
  JsonDocument doc;
  cfgSaveArray(doc, g_profiles, sizeof(ProfileConfig), MAX_PROFILES,
               kProfileFields, CFG_COUNT(kProfileFields));
  return saveConfigDoc(CFG_FILE_PROFILE, doc, FILE_PROFILE, FILE_PROFILE_BKP);
}

bool loadProfileSteps() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_STEPS, doc, FILE_STEPS, FILE_STEPS_BKP)) return false;
  cfgLoadArray(doc, g_profileSteps, sizeof(ProfileStep), MAX_PROFILE_STEPS,
               kProfileStepFields, CFG_COUNT(kProfileStepFields));
  return true;
}

bool saveProfileSteps() {
  JsonDocument doc;
  cfgSaveArray(doc, g_profileSteps, sizeof(ProfileStep), MAX_PROFILE_STEPS,
               kProfileStepFields, CFG_COUNT(kProfileStepFields));
  return saveConfigDoc(CFG_FILE_STEPS, doc, FILE_STEPS, FILE_STEPS_BKP);
}

// ============================================================
// iSPINDEL CONFIG
// ============================================================

bool loadiSpindelConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_ISPINDEL, doc, FILE_ISPINDEL, FILE_ISPINDEL_BKP)) {
    initDefaultiSpindelConfig();
    return false;
  }
  cfgLoadArray(doc, g_iSpindels, sizeof(iSpindelConfig), MAX_ISPINDELS,
               kiSpindelFields, CFG_COUNT(kiSpindelFields));
  // No reading until each device reports (0 would be read as a real 0 C)
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    g_iSpindels[i].temperature = TEMP_NONE;
  }
  // Migrate legacy values: only PROBE_FN_BEER means "provide beer temp" —
  // everything else collapses to Unassigned ("temperature reading not used").
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (g_iSpindels[i].function != PROBE_FN_BEER && g_iSpindels[i].function != PROBE_UNASSIGNED) {
      g_iSpindels[i].function = PROBE_UNASSIGNED;
      configMarkUnsaved(CFG_FILE_ISPINDEL);
    }
  }
  // IDs from incoming readings have been made topic-safe since 0.5.1, but one
  // stored before that, or imported from a file, would be published under
  // until the device next reported. The same rule is applied here, so the
  // device's next reading still matches its slot.
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (makeIdTopicSafe(g_iSpindels[i].id)) {
      configMarkUnsaved(CFG_FILE_ISPINDEL);
      logMsg("[CFG] iSpindel slot %d: ID had characters MQTT cannot use - changed to %s", i, g_iSpindels[i].id);
    }
  }
  return true;
}

bool saveiSpindelConfig() {
  JsonDocument doc;
  cfgSaveArray(doc, g_iSpindels, sizeof(iSpindelConfig), MAX_ISPINDELS,
               kiSpindelFields, CFG_COUNT(kiSpindelFields));
  return saveConfigDoc(CFG_FILE_ISPINDEL, doc, FILE_ISPINDEL, FILE_ISPINDEL_BKP);
}

// ============================================================
// TILT CONFIG
// 4-slot format matching original firmware jsonTilt.txt:
//   {"Address":[c,c,c,c],"Function":[...],"Fermenter":[...],
//    "Temp_Adjust":[...],"SG_Adjust":[...],"MBB":[...]}
// where Address = colour index (0-7) or 99 = unassigned slot
//
// Custom load/save (not table-driven): slots map to colour-indexed
// g_tilts entries, so field order and struct offsets don't line up.
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

  return saveConfigDoc(CFG_FILE_TILT, doc, FILE_TILT, FILE_TILT_BKP);
}

// ============================================================
// BREW SERVICE CONFIG (multiple service slots)
// ============================================================

bool loadBrewServiceConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_BREWSVC, doc, FILE_BREWSVC, FILE_BREWSVC_BKP)) {
    // An original MyBrewbot's single global service was moved into this file
    // by OriginalConfig.cpp before this ran
    initDefaultBrewServiceConfig();
    return false;
  }

  // Detect old 3-slot config (BF, Monitor Beer, Brewfather) and remap to 2-slot
  if (doc["Enabled"].size() == 3) {
    g_brewServices[0].enabled = doc["Enabled"][0] | false;
    copyText(g_brewServices[0].serviceId, doc["ServiceId"][0] | "", sizeof(g_brewServices[0].serviceId));
    copyText(g_brewServices[0].deviceName, doc["DeviceName"][0] | "OurBrewbot", sizeof(g_brewServices[0].deviceName));
    // Old slot 1 was Monitor Beer (removed); old slot 2 was Brewfather → new slot 1
    g_brewServices[1].enabled = doc["Enabled"][2] | false;
    copyText(g_brewServices[1].serviceId, doc["ServiceId"][2] | "", sizeof(g_brewServices[1].serviceId));
    copyText(g_brewServices[1].deviceName, doc["DeviceName"][2] | "OurBrewbot", sizeof(g_brewServices[1].deviceName));
    logMsg("[CFG] Migrated 3-slot brew service config to 2-slot (Monitor Beer removed)");
    saveBrewServiceConfig();
  } else {
    cfgLoadArray(doc, g_brewServices, sizeof(BrewServiceConfig), MAX_BREW_SERVICES,
                 kBrewSvcFields, CFG_COUNT(kBrewSvcFields));
  }
  return true;
}

bool saveBrewServiceConfig() {
  JsonDocument doc;
  cfgSaveArray(doc, g_brewServices, sizeof(BrewServiceConfig), MAX_BREW_SERVICES,
               kBrewSvcFields, CFG_COUNT(kBrewSvcFields));
  return saveConfigDoc(CFG_FILE_BREWSVC, doc, FILE_BREWSVC, FILE_BREWSVC_BKP);
}

// ============================================================
// MQTT CONFIG
// ============================================================

bool loadMqttConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_MQTT, doc, FILE_MQTT, FILE_MQTT_BKP)) {
    initDefaultMqttConfig();
    return false;
  }
  cfgLoadScalar(doc, &g_mqttConfig, kMqttFields, CFG_COUNT(kMqttFields));
  // POST /mqtt now refuses a base topic the broker can't use, but one saved by
  // older firmware or imported from a file would make every publish fail, so
  // it is repaired here as well.
  if (g_mqttConfig.baseTopic[0] == '\0') {
    strlcpy(g_mqttConfig.baseTopic, "ourbrewbot", sizeof(g_mqttConfig.baseTopic));
    configMarkUnsaved(CFG_FILE_MQTT);
    logMsg("[CFG] MQTT base topic was empty - using ourbrewbot");
  } else if (makeBaseTopicSafe(g_mqttConfig.baseTopic)) {
    configMarkUnsaved(CFG_FILE_MQTT);
    logMsg("[CFG] MQTT base topic had characters MQTT cannot use - changed to %s", g_mqttConfig.baseTopic);
  }
  return true;
}

bool saveMqttConfig() {
  JsonDocument doc;
  cfgSaveScalar(doc, &g_mqttConfig, kMqttFields, CFG_COUNT(kMqttFields));
  return saveConfigDoc(CFG_FILE_MQTT, doc, FILE_MQTT, FILE_MQTT_BKP);
}

// ============================================================
// SYSLOG CONFIG
// ============================================================

bool loadSyslogConfig() {
  JsonDocument doc;
  if (!loadConfigDoc(CFG_FILE_SYSLOG, doc, FILE_SYSLOG, FILE_SYSLOG_BKP)) {
    initDefaultSyslogConfig();
    return false;
  }
  cfgLoadScalar(doc, &g_syslogConfig, kSyslogFields, CFG_COUNT(kSyslogFields));
  return true;
}

bool saveSyslogConfig() {
  JsonDocument doc;
  cfgSaveScalar(doc, &g_syslogConfig, kSyslogFields, CFG_COUNT(kSyslogFields));
  return saveConfigDoc(CFG_FILE_SYSLOG, doc, FILE_SYSLOG, FILE_SYSLOG_BKP);
}

// ============================================================
// MOUNT THE FILESYSTEM
// ============================================================

#define FS_MOUNT_ATTEMPTS 3

// What mountFileSystem() had to do at boot, for logFileSystemBootProblems()
static uint8_t s_fsMountAttempts = 0;
static bool    s_fsFormatted     = false;

bool mountFileSystem() {
  // LittleFS formats itself whenever a mount fails. One bad flash read at boot
  // would then erase every setting, the plugs' RF codes included, so that is
  // switched off and the mount is tried a few times first.
  LittleFS.setConfig(LittleFSConfig(false));
  for (uint8_t attempt = 1; attempt <= FS_MOUNT_ATTEMPTS; attempt++) {
    s_fsMountAttempts = attempt;
    if (LittleFS.begin()) {
      logMsg("[FS] LittleFS mounted OK (attempt %u)", attempt);
      return true;
    }
    logMsgL(SYSLOG_WARNING, "[FS] LittleFS mount failed (attempt %u of %u)",
            attempt, FS_MOUNT_ATTEMPTS);
    if (attempt < FS_MOUNT_ATTEMPTS) delay(100);
  }

  // Still can't be mounted: a new device with blank flash, or a damaged
  // filesystem. Format it so the controller can save settings again.
  s_fsFormatted = true;
  logMsgL(SYSLOG_ERR, "[FS] LittleFS can't be mounted - formatting it, any saved settings are lost");
  if (LittleFS.format() && LittleFS.begin()) {
    logMsg("[FS] LittleFS formatted and mounted");
    return true;
  }
  logMsgL(SYSLOG_ERR, "[FS] LittleFS unusable - running on defaults, nothing can be saved");
  return false;
}

void logFileSystemBootProblems() {
  if (s_fsFormatted) {
    logMsgL(SYSLOG_ERR, "DEFERRED [FS] LittleFS couldn't be mounted at boot and was formatted - saved settings were lost");
  } else if (s_fsMountAttempts > 1) {
    logMsgL(SYSLOG_WARNING, "DEFERRED [FS] LittleFS mounted at boot on attempt %u of %u",
            s_fsMountAttempts, FS_MOUNT_ATTEMPTS);
  }
}

// ============================================================
// LOAD ALL / SAVE ALL
// ============================================================

void loadAllConfig() {
  logMsg("[CFG] Loading all configuration...");
  // Before any loader: a loader that corrects a value saves the file at once,
  // which would hide the signs of an original config (see OriginalConfig.cpp)
  convertOriginalConfig();
  loadGlobalConfig();
  loadFermenterConfig();
  loadProbeConfig();
  loadSmartPlugConfig();
  loadProfileConfig();
  loadProfileSteps();
  loadiSpindelConfig();
  loadTiltConfig();
  loadBrewServiceConfig();
  loadMqttConfig();
  loadSyslogConfig();

  // One-shot migration: bump any AlarmTolerance==0 to 3.0 (the new default
  // for the severe-deviation escape hatch). Old configs never exposed this
  // field, so 0.0 was always an unset state. Guarded by g_globalConfig.migrated
  // so we don't re-bump if the user deliberately sets 0 later.
  if (!g_globalConfig.migrated) {
    bool changed = false;
    for (int i = 0; i < MAX_FERMENTERS; i++) {
      if (g_fermenters[i].alarmTolerance == 0.0f) {
        g_fermenters[i].alarmTolerance = 3.0f;
        changed = true;
      }
    }
    if (changed) saveFermenterConfig();
    g_globalConfig.migrated = true;
    saveGlobalConfig();
    logMsg("[CFG] One-shot migration applied: AlarmTolerance defaults set to 3.0");
  }

  logMsg("[CFG] All configuration loaded");
}

void saveAllConfig() {
  bool ok = true;
  ok &= saveGlobalConfig();
  ok &= saveFermenterConfig();
  ok &= saveProbeConfig();
  ok &= saveSmartPlugConfig();
  ok &= saveProfileConfig();
  ok &= saveProfileSteps();
  ok &= saveiSpindelConfig();
  ok &= saveBrewServiceConfig();
  ok &= saveMqttConfig();
  ok &= saveSyslogConfig();
  if (!ok)
    logMsgL(SYSLOG_ERR, "[CFG] One or more config saves failed");
}

// ============================================================
// CONFIG IMPORT
//
// POST /fs/save writes imported config files straight to flash. They only
// take effect at the next restart, so until then the settings in memory are
// the ones in use. Two things follow:
//  - the 10-minute save must not write memory back over the imported files
//    (it used to, when the WebUI's "Import complete" message was left open,
//    and the device then restarted on its old settings);
//  - if an import fails or is abandoned, writing memory back is exactly what
//    puts the previous settings back, so the device can't restart on a mix
//    of old and new files.
// An import never writes the backup copies itself: its first file saves the
// current settings to every file and its backup before anything is replaced.
// ============================================================

// How long an import may go without a new file before it counts as abandoned:
// far longer than the slowest upload (a few seconds), and short enough that
// the next 10-minute save puts things right.
#define CONFIG_IMPORT_IDLE_S 120

static bool     s_importActive    = false;
static uint32_t s_importLastWrite = 0;   // uptimeStamp() of the last imported file

// Write the settings in memory to every config file and its backup.
// saveAllConfig() leaves out the Tilt file, which is otherwise saved whenever a
// Tilt setting changes, but an import can include it.
static void saveSettingsInUse() {
  saveAllConfig();
  saveTiltConfig();
}

void configImportBeforeWrite() {
  if (!s_importActive) {
    logMsg("[CFG] Config import started - saving the current settings first");
    saveSettingsInUse();
    s_importActive = true;
  }
  s_importLastWrite = uptimeStamp();
}

void configImportRestore() {
  saveSettingsInUse();
  s_importActive = false;
  logMsgL(SYSLOG_NOTICE, "[CFG] Config import cancelled - the previous settings have been put back");
}

// Write each file marked unsaved. Unlike saveAllConfig() this includes the
// Tilt file, but only when a Tilt save has failed.
static void saveUnsavedConfig() {
  bool ok = true;
  if (isUnsaved(CFG_FILE_GLOBAL))     ok &= saveGlobalConfig();
  if (isUnsaved(CFG_FILE_FERMENTER))  ok &= saveFermenterConfig();
  if (isUnsaved(CFG_FILE_PROBE))      ok &= saveProbeConfig();
  if (isUnsaved(CFG_FILE_SMARTPLUGS)) ok &= saveSmartPlugConfig();
  if (isUnsaved(CFG_FILE_PROFILE))    ok &= saveProfileConfig();
  if (isUnsaved(CFG_FILE_STEPS))      ok &= saveProfileSteps();
  if (isUnsaved(CFG_FILE_TILT))       ok &= saveTiltConfig();
  if (isUnsaved(CFG_FILE_ISPINDEL))   ok &= saveiSpindelConfig();
  if (isUnsaved(CFG_FILE_BREWSVC))    ok &= saveBrewServiceConfig();
  if (isUnsaved(CFG_FILE_MQTT))       ok &= saveMqttConfig();
  if (isUnsaved(CFG_FILE_SYSLOG))     ok &= saveSyslogConfig();
  if (!ok)
    logMsgL(SYSLOG_ERR, "[CFG] One or more config saves failed - trying again in 10 minutes");
}

void configPeriodicSave() {
  if (!s_importActive) {
    saveUnsavedConfig();
    return;
  }
  if (uptimeSince(s_importLastWrite) < CONFIG_IMPORT_IDLE_S) {
    logMsg("[CFG] Config import in progress - not saving over the imported files");
    return;
  }
  // No file for 2 minutes and no restart: the import was abandoned part way
  saveSettingsInUse();
  s_importActive = false;
  logMsgL(SYSLOG_WARNING, "[CFG] A config import was not finished - the previous settings have been put back");
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
  g_globalConfig.alarmDwellSec = 600;
  g_globalConfig.mdnsEnabled = true;
  g_globalConfig.updateCheck = true;
  g_globalConfig.crashReports = true;
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
    g_fermenters[i].alarmTolerance = 3.0f;
  }
}

void initDefaultProbeConfig() {
  for (int i = 0; i < MAX_PROBES; i++) {
    strlcpy(g_probes[i].probeName, "Probe", sizeof(g_probes[i].probeName));
    g_probes[i].address[0] = '\0';
    g_probes[i].function  = PROBE_UNASSIGNED;
    g_probes[i].fermenter = PROBE_UNASSIGNED;
    g_probes[i].temperature    = TEMP_NONE;   // no reading yet (0 would look like a real 0 C)
    g_probes[i].rawTemperature = TEMP_NONE;
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
    g_iSpindels[i].fermenter   = PROBE_UNASSIGNED;   // assigned in the admin page
    // Plato for every slot: it is what an iSpindel that declares no unit is
    // assumed to be sending. A device that does declare one overwrites this
    // when it registers (see iSpindel.h).
    g_iSpindels[i].unit        = ISPINDEL_UNIT_PLATO;
    g_iSpindels[i].function    = PROBE_FN_BEER;
    g_iSpindels[i].temperature = TEMP_NONE;   // no reading yet
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
  logMsgL(SYSLOG_NOTICE, "[CFG] Resetting WiFi configuration");
  clearWiFiProvisioningArtifacts();
  logMsg("[CFG] WiFi reset complete - restart required");
}

// ============================================================
// FULL RESET
// ============================================================

void resetAllConfig() {
  logMsgL(SYSLOG_NOTICE, "[CFG] Resetting all configuration to defaults");
  initDefaultGlobalConfig();
  initDefaultFermenterConfig();
  initDefaultProbeConfig();
  initDefaultSmartPlugConfig();
  initDefaultProfileConfig();
  initDefaultTiltConfig();
  initDefaultiSpindelConfig();
  initDefaultBrewServiceConfig();
  initDefaultMqttConfig();
  initDefaultSyslogConfig();
  // Not saveAllConfig(): it leaves out the Tilt file, so the old Tilt settings
  // would be loaded again after the restart
  saveSettingsInUse();

  // Remove WiFi config so WiFiManager re-runs the portal
  clearWiFiProvisioningArtifacts();

  // ...and any crash reports still waiting to be sent
  if (LittleFS.exists(FILE_CRASH_QUEUE)) LittleFS.remove(FILE_CRASH_QUEUE);

  logMsg("[CFG] Reset complete - restart required");
}

// ============================================================
// REBOOT LOGGING
// ============================================================

void recordReboot(const String& reason) {
  // Single streaming doc — load existing reboot log directly from File, append
  // the new entry, write back. Avoids holding a loaded-String + two JsonDocuments
  // + a serialized-String all at once (the worst single peak in the codebase).
  JsonDocument doc;
  loadJsonDocSafe(doc, FILE_REBOOT, FILE_REBOOT_BKP);  // ok if file missing

  JsonArray log = !doc["log"].isNull() ? doc["log"].as<JsonArray>()
                                       : doc["log"].to<JsonArray>();

  struct rst_info *ri = ESP.getResetInfoPtr();

  JsonObject entry = log.add<JsonObject>();
  entry["reason"]   = reason;
  entry["uptime"]   = g_globalConfig.lastUptime;
  entry["heap"]     = ESP.getFreeHeap();
  entry["rsn_code"] = ri->reason;
  if (ri->reason == REASON_EXCEPTION_RST) {
    entry["exccause"]  = ri->exccause;
    entry["epc1"]      = ri->epc1;
    entry["excvaddr"]  = ri->excvaddr;
  }

  // Trim to last 10 entries
  while (log.size() > 10) log.remove(0);

  saveJsonDocSafe(doc, FILE_REBOOT, FILE_REBOOT_BKP);
}
