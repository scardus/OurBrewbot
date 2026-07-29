/*
 * Mqtt.cpp -- MQTT client for publishing fermenter and device data
 *
 * Publishes each value on its own retained topic:
 *   <baseTopic>/Fermenter<N>/<key>   — per-fermenter data
 *   <baseTopic>/Device/<key>         — device-level diagnostics
 *
 * LWT: broker publishes <baseTopic>/availability = "offline" on unexpected disconnect.
 * On connect: publishes "online" to availability topic.
 *
 * HA Discovery: when haDiscovery is enabled, publishes Home Assistant MQTT discovery
 * config payloads on connect and whenever HA restarts (homeassistant/status = online).
 * A device-level HA device (ourbrewbot_{CHIPID}) is published alongside the per-fermenter
 * devices (ourbrewbot_{CHIPID}_f{N}), advertising firmware version, IP, mDNS name, WiFi
 * SSID, RSSI, free heap, uptime, chip ID, reboot reason, and reboot code.
 */

#include "Mqtt.h"
#include "MqttParse.h"
#include "Fermenter.h"
#include "Temperatures.h"
#include "Profile.h"
#include "Tilt.h"
#include "Version.h"
#include "Log.h"
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

static const char* probeFunctionName(uint8_t fn) {
  switch (fn) {
    case PROBE_FN_BEER:     return "beer";
    case PROBE_FN_AMBIENT:  return "ambient";
    case PROBE_FN_TILT:     return "tilt";
    case PROBE_FN_ISPINDEL: return "ispindel";
    case PROBE_FN_AIR:      return "air";
    case PROBE_FN_CONTROL:  return "control";
    default:                return "unassigned";
  }
}

extern String g_rebootReason;  // captured at boot in OurBrewbot.ino

static WiFiClient    g_mqttWifi;
static PubSubClient  g_mqtt(g_mqttWifi);
static bool          g_mqttWasConnected  = false;
static unsigned long g_mqttLastAttempt   = 0;
static unsigned long g_mqttBackoffMs     = 5000;  // start at 5s, doubles on failure
static bool          g_mqttPendingSave   = false;
static unsigned long g_mqttPendingSaveAt = 0;
#define MQTT_MAX_BACKOFF_MS  300000               // cap at 5 minutes

// ============================================================
// HELPERS — fixed char buffers to avoid heap fragmentation
// ============================================================

static char s_topicBuf[128];
static char s_availTopic[64];   // "<baseTopic>/availability", built on connect

// Throttled publish-failure logger — one syslog line per 30 s with a suppressed
// count, so a broker dropping mid-burst can't cause a log storm.
static void logPublishFailure(const char* topic) {
  static uint32_t s_failCount = 0;
  static unsigned long s_lastLogMs = 0;
  s_failCount++;
  if (millis() - s_lastLogMs >= 30000) {
    logMsgL(SYSLOG_WARNING, "[MQTT] publish failed: %s (%u failure(s)), state=%d",
            topic, (unsigned)s_failCount, g_mqtt.state());
    s_lastLogMs = millis();
    s_failCount = 0;
  }
}

static void publishValue(const char* base, const char* key, const char* value) {
  snprintf(s_topicBuf, sizeof(s_topicBuf), "%s/%s", base, key);
  if (!g_mqtt.publish(s_topicBuf, value, true))  // retained
    logPublishFailure(s_topicBuf);
}

static void publishFloat(const char* base, const char* key, float value, int decimals = 1) {
  char val[16];
  dtostrf(value, 1, decimals, val);
  publishValue(base, key, val);
}

static void publishInt(const char* base, const char* key, int value) {
  char val[12];
  snprintf(val, sizeof(val), "%d", value);
  publishValue(base, key, val);
}

static void publishBool(const char* base, const char* key, bool value) {
  publishValue(base, key, value ? "ON" : "OFF");
}

// HA discovery unit-of-measurement string for the configured display unit.
static const char* haTempUnit() {
  return (g_globalConfig.unit == UNIT_FAHRENHEIT) ? "\xC2\xB0""F" : "\xC2\xB0""C";
}

// ============================================================
// HOME ASSISTANT DISCOVERY
// ============================================================

// Fill common HA discovery fields (uniq_id, name, stat_t, avty_t, exp_aft, dev).
static void buildDiscoveryBase(JsonDocument& doc,
    const char* devId, const char* base, const char* devName,
    const char* objectId, const char* friendlyName,
    const char* stKey, const char* icon)
{
  char uid[56], stTopic[96];
  snprintf(uid,     sizeof(uid),     "%s_%s", devId, objectId);
  snprintf(stTopic, sizeof(stTopic), "%s/%s", base,  stKey);
  doc["uniq_id"] = uid;
  doc["name"]    = friendlyName;
  doc["stat_t"]  = stTopic;
  if (icon && icon[0]) doc["ic"] = icon;
  doc["avty_t"]  = s_availTopic;
  doc["exp_aft"] = 180;
  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray  ids = dev["ids"].to<JsonArray>();
  ids.add(devId);
  dev["name"] = devName;
  dev["mf"]   = "OurBrewbot";
  dev["mdl"]  = "ESP8266";
  dev["sw"]   = FW_VERSION;
}

// Serialize doc to the entity's discovery topic, publish retained, clear doc, yield.
// Serializes into a static buffer rather than a heap String — a discovery burst
// publishes ~70 entities back-to-back and the alloc/free churn was the main
// fragmentation source on this heap-constrained device.
static char s_discPayload[1024];  // sized to the PubSubClient buffer (setBufferSize)

static void publishAndReset(JsonDocument& doc,
    const char* component, const char* devId, const char* objectId)
{
  char discTopic[128];
  snprintf(discTopic, sizeof(discTopic), "homeassistant/%s/%s/%s/config",
    component, devId, objectId);
  const size_t len = measureJson(doc);
  if (len + 1 > sizeof(s_discPayload)) {
    logMsgL(SYSLOG_ERR, "[MQTT] discovery payload too large (%u B): %s — skipped",
            (unsigned)len, discTopic);
  } else {
    serializeJson(doc, s_discPayload, sizeof(s_discPayload));
    if (!g_mqtt.publish(discTopic, s_discPayload, true))
      logPublishFailure(discTopic);
  }
  doc.clear();
  yield();  // feed WDT between successive publishes
}

// Build and publish a single HA sensor/binary_sensor entity discovery config.
// Reuses the caller's JsonDocument (caller must call doc.clear() between calls).
static void publishOneEntity(
    JsonDocument& doc,
    const char* component,      // "sensor" or "binary_sensor"
    const char* devId,          // e.g. "ourbrewbot_ABCDEF_f0"
    const char* fermBase,       // e.g. "ourbrewbot/Fermenter0"
    const char* fermName,       // fermenter display name
    const char* objectId,       // entity slug == data topic key
    const char* name,           // entity friendly name
    const char* stKey,          // state topic key appended to fermBase
    const char* devClass,       // HA device_class, nullptr/""=omit
    const char* unit,           // unit_of_measurement, nullptr/""=omit
    const char* icon,           // mdi icon, nullptr/""=omit
    const char* entityCat,      // entity_category ("diagnostic"), nullptr/""=omit
    const char* stateClass      // state_class ("measurement"), nullptr/""=omit
) {
  buildDiscoveryBase(doc, devId, fermBase, fermName, objectId, name, stKey, icon);
  if (devClass   && devClass[0])   doc["dev_cla"]      = devClass;
  if (unit       && unit[0])       doc["unit_of_meas"] = unit;
  if (entityCat  && entityCat[0])  doc["ent_cat"]      = entityCat;
  if (stateClass && stateClass[0]) doc["stat_cla"]     = stateClass;
  publishAndReset(doc, component, devId, objectId);
}

// HA switch entity (ON/OFF command).
static void publishSwitchEntity(JsonDocument& doc,
    const char* devId, const char* base, const char* devName,
    const char* objectId, const char* name,
    const char* stKey, const char* cmdKey,
    const char* icon = nullptr)
{
  buildDiscoveryBase(doc, devId, base, devName, objectId, name, stKey, icon);
  char cmdTopic[128];
  snprintf(cmdTopic, sizeof(cmdTopic), "%s/%s", base, cmdKey);
  doc["cmd_t"]  = cmdTopic;
  doc["pl_on"]  = "ON";   // pl_on / pl_off are HA abbreviated names for payload_on / payload_off
  doc["pl_off"] = "OFF";
  publishAndReset(doc, "switch", devId, objectId);
}

// HA number entity (numeric slider/input).
static void publishNumberEntity(JsonDocument& doc,
    const char* devId, const char* base, const char* devName,
    const char* objectId, const char* name,
    const char* stKey, const char* cmdKey,
    float minVal, float maxVal, float step,
    const char* unit = nullptr, const char* devClass = nullptr,
    const char* icon = nullptr)
{
  buildDiscoveryBase(doc, devId, base, devName, objectId, name, stKey, icon);
  char cmdTopic[128];
  snprintf(cmdTopic, sizeof(cmdTopic), "%s/%s", base, cmdKey);
  doc["cmd_t"] = cmdTopic;
  doc["min"]   = minVal;
  doc["max"]   = maxVal;
  doc["step"]  = step;
  doc["mode"]  = "box";
  if (unit     && unit[0])     doc["unit_of_meas"] = unit;
  if (devClass && devClass[0]) doc["dev_cla"]      = devClass;
  publishAndReset(doc, "number", devId, objectId);
}

// HA select entity (dropdown from fixed options list).
static void publishSelectEntity(JsonDocument& doc,
    const char* devId, const char* base, const char* devName,
    const char* objectId, const char* name,
    const char* stKey, const char* cmdKey,
    const char** options, int optCount,
    const char* icon = nullptr)
{
  buildDiscoveryBase(doc, devId, base, devName, objectId, name, stKey, icon);
  char cmdTopic[128];
  snprintf(cmdTopic, sizeof(cmdTopic), "%s/%s", base, cmdKey);
  doc["cmd_t"] = cmdTopic;
  JsonArray opts = doc["ops"].to<JsonArray>();
  for (int i = 0; i < optCount; i++) opts.add(options[i]);
  publishAndReset(doc, "select", devId, objectId);
}

// HA text entity (free-text input).
static void publishTextEntity(JsonDocument& doc,
    const char* devId, const char* base, const char* devName,
    const char* objectId, const char* name,
    const char* stKey, const char* cmdKey,
    int maxLen = 31, const char* icon = nullptr)
{
  buildDiscoveryBase(doc, devId, base, devName, objectId, name, stKey, icon);
  char cmdTopic[128];
  snprintf(cmdTopic, sizeof(cmdTopic), "%s/%s", base, cmdKey);
  doc["cmd_t"] = cmdTopic;
  doc["max"]   = maxLen;
  publishAndReset(doc, "text", devId, objectId);
}

// HA button entity (press-only, no state topic).
static void publishButtonEntity(JsonDocument& doc,
    const char* devId, const char* base, const char* devName,
    const char* objectId, const char* name,
    const char* cmdKey, const char* icon = nullptr)
{
  char uid[56], cmdTopic[96];
  snprintf(uid,      sizeof(uid),      "%s_%s", devId, objectId);
  snprintf(cmdTopic, sizeof(cmdTopic), "%s/%s", base,  cmdKey);
  doc["uniq_id"] = uid;
  doc["name"]    = name;
  doc["cmd_t"]   = cmdTopic;
  doc["pl_prs"]  = "1";
  if (icon && icon[0]) doc["ic"] = icon;
  doc["avty_t"]  = s_availTopic;
  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray  ids = dev["ids"].to<JsonArray>();
  ids.add(devId);
  dev["name"] = devName;
  dev["mf"]   = "OurBrewbot";
  dev["mdl"]  = "ESP8266";
  dev["sw"]   = FW_VERSION;
  publishAndReset(doc, "button", devId, objectId);
}

// Echo a single fermenter field immediately after processing a /set command,
// so HA shows confirmed state without waiting for the 60 s periodic publish.
static void publishFermenterField(int i, const char* key) {
  char base[96];
  snprintf(base, sizeof(base), "%s/Fermenter%d", g_mqttConfig.baseTopic, i);
  if      (strcmp(key, "power")               == 0) publishBool (base, "power",               g_fermenters[i].power);
  else if (strcmp(key, "temp_control")        == 0) publishBool (base, "temp_control",         g_fermenters[i].tempControl);
  else if (strcmp(key, "profile_running")     == 0) publishBool (base, "profile_running",      g_fermenters[i].profileRunning);
  else if (strcmp(key, "ceiling_temperature") == 0) publishFloat(base, "ceiling_temperature",  toDisplayTemp(g_fermenters[i].ceilingTemp));
  else if (strcmp(key, "floor_temperature")   == 0) publishFloat(base, "floor_temperature",    toDisplayTemp(g_fermenters[i].floorTemp));
  else if (strcmp(key, "hysteresis")          == 0) publishFloat(base, "hysteresis",           toDisplayTempDelta(g_fermenters[i].hysteresis));
  else if (strcmp(key, "compressor_delay")    == 0) publishInt  (base, "compressor_delay",     g_fermenters[i].compressorDelay);
  else if (strcmp(key, "og")                  == 0) publishFloat(base, "og",                   g_fermenters[i].og, 4);
  else if (strcmp(key, "tg")                  == 0) publishFloat(base, "tg",                   g_fermenters[i].tg, 4);
  else if (strcmp(key, "name")                == 0) publishValue(base, "name",                 g_fermenters[i].fermenterName);
  else if (strcmp(key, "beer_name")           == 0) publishValue(base, "beer_name",            g_fermenters[i].beerName);
  else if (strcmp(key, "yeast")               == 0) publishValue(base, "yeast",                g_fermenters[i].yeastName);
  else if (strcmp(key, "profile_no")          == 0) publishInt  (base, "profile_no",           g_fermenters[i].profileNo);
}

// ============================================================
// HA ENTITY DESCRIPTOR TABLES
//
// One table per device scope drives BOTH discovery publish and removal, so
// an entity added to a table can never leak a retained HA config by being
// forgotten on the remove side. Tables live in PROGMEM (rows are memcpy_P'd
// to the stack before use); publish order matches table order.
// ============================================================

enum HaKind : uint8_t { HA_SENSOR, HA_BINARY, HA_NUMBER, HA_SWITCH, HA_SELECT, HA_TEXT, HA_BUTTON };

// Flags for discovery fields that depend on the configured temperature unit
#define HAF_TEMP_UNIT    0x01   // unit_of_meas = haTempUnit()
#define HAF_MINMAX_TEMP  0x02   // min/max converted via toDisplayTemp()
#define HAF_MAX_DELTA    0x04   // max converted via toDisplayTempDelta()

struct HaEntityDesc {
  uint8_t     kind;        // HaKind
  uint8_t     flags;
  const char* objectId;    // entity slug == state topic key; command topic = "<objectId>/set"
  const char* name;        // friendly name
  const char* devClass;    // HA device_class (nullptr = omit)
  const char* unit;        // unit_of_measurement (nullptr = omit or via HAF_TEMP_UNIT)
  const char* icon;        // mdi icon (nullptr = omit)
  const char* entityCat;   // entity_category (nullptr = omit)
  const char* stateClass;  // state_class (nullptr = omit)
  float       minVal, maxVal, step;  // HA_NUMBER only
};

#define HA_COUNT(t) (sizeof(t) / sizeof((t)[0]))

static const HaEntityDesc kDeviceEntities[] PROGMEM = {
  // Static text diagnostics
  { HA_SENSOR, 0, "firmware_version", "Firmware Version", nullptr, nullptr, "mdi:tag",           "diagnostic", nullptr },
  { HA_SENSOR, 0, "ip_address",       "IP Address",       nullptr, nullptr, "mdi:ip-network",    "diagnostic", nullptr },
  { HA_SENSOR, 0, "mdns_name",        "mDNS Name",        nullptr, nullptr, "mdi:lan",           "diagnostic", nullptr },
  { HA_SENSOR, 0, "wifi_ssid",        "WiFi SSID",        nullptr, nullptr, "mdi:wifi",          "diagnostic", nullptr },
  // Numeric diagnostics
  { HA_SENSOR, 0, "rssi",             "RSSI",             "signal_strength", "dBm", nullptr,     "diagnostic", "measurement" },
  { HA_SENSOR, 0, "free_heap",        "Free Heap",        nullptr, "B",       "mdi:memory",      "diagnostic", "measurement" },
  { HA_SENSOR, 0, "uptime",           "Uptime",           "duration", "min", "mdi:clock-outline","diagnostic", "measurement" },
  // Identity & reboot info
  { HA_SENSOR, 0, "chip_id",          "Chip ID",          nullptr, nullptr, "mdi:chip",          "diagnostic", nullptr },
  { HA_SENSOR, 0, "reboot_reason",    "Reboot Reason",    nullptr, nullptr, "mdi:restart",       "diagnostic", nullptr },
  { HA_SENSOR, 0, "reboot_code",      "Reboot Code",      nullptr, nullptr, "mdi:restart-alert", "diagnostic", "measurement" },
  // Device control buttons — always advertised; ignored by device when allowControl is off,
  // state topic publishes correct device state within 60s regardless
  { HA_BUTTON, 0, "reboot",           "Reboot",           nullptr, nullptr, "mdi:restart",       nullptr, nullptr },
  { HA_BUTTON, 0, "all_off",          "All Off",          nullptr, nullptr, "mdi:power-off",     nullptr, nullptr },
};

static const HaEntityDesc kFermenterEntities[] PROGMEM = {
  // Temperature sensors — numeric, use state_class measurement.
  // Entity names are the plain property names; HA prefixes them with dev.name ("OurBrewbot F0")
  // when displaying and when generating entity IDs (sensor.ourbrewbot_f0_beer_temperature).
  { HA_SENSOR, HAF_TEMP_UNIT, "beer_temperature",        "Beer Temperature",        "temperature", nullptr, nullptr, nullptr, "measurement" },
  { HA_SENSOR, 0,             "beer_temperature_source", "Beer Temperature Source", nullptr, nullptr, "mdi:information-outline", nullptr, nullptr },
  { HA_SENSOR, HAF_TEMP_UNIT, "ambient_temperature",     "Ambient Temperature",     "temperature", nullptr, nullptr, nullptr, "measurement" },
  // Setpoint numbers — always number entities; device ignores commands when allowControl is off,
  // and the 60s state publish corrects any HA UI changes within one interval
  { HA_NUMBER, HAF_TEMP_UNIT | HAF_MINMAX_TEMP, "ceiling_temperature", "Ceiling Temperature", "temperature", nullptr, nullptr, nullptr, nullptr, -20.0f, 50.0f, 0.1f },
  { HA_NUMBER, HAF_TEMP_UNIT | HAF_MINMAX_TEMP, "floor_temperature",   "Floor Temperature",   "temperature", nullptr, nullptr, nullptr, nullptr, -20.0f, 50.0f, 0.1f },
  { HA_SENSOR, 0,             "temperature_unit",        "Temperature Unit",        nullptr, nullptr, "mdi:thermometer", nullptr, nullptr },
  { HA_NUMBER, HAF_TEMP_UNIT | HAF_MAX_DELTA,   "hysteresis",          "Hysteresis",          nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f, 10.0f, 0.1f },
  { HA_NUMBER, 0,             "compressor_delay",        "Compressor Delay",        nullptr, "min", "mdi:timer-outline", nullptr, nullptr, 0.0f, 1440.0f, 1.0f },
  // Gravity — read-only sensors plus writable OG/TG numbers
  { HA_SENSOR, 0, "gravity",        "Gravity",        nullptr, "SG",    "mdi:test-tube",           nullptr, "measurement" },
  { HA_SENSOR, 0, "gravity_source", "Gravity Source", nullptr, nullptr, "mdi:information-outline", nullptr, nullptr },
  { HA_NUMBER, 0, "og",             "OG",             nullptr, "SG",    "mdi:test-tube",           nullptr, nullptr, 0.990f, 1.200f, 0.001f },
  { HA_NUMBER, 0, "tg",             "TG",             nullptr, "SG",    "mdi:test-tube",           nullptr, nullptr, 0.990f, 1.200f, 0.001f },
  { HA_SENSOR, 0, "attenuation",    "Attenuation",    nullptr, "%",     "mdi:percent",             nullptr, "measurement" },
  // Status — read-only text sensor
  { HA_SENSOR, 0, "status", "Status", nullptr, nullptr, "mdi:thermometer", nullptr, nullptr },
  // Alarm — over/under temperature beyond tolerance
  { HA_BINARY, 0, "alarm",  "Alarm",  "problem", nullptr, "mdi:alarm-light", nullptr, nullptr },
  // Text fields — always text entities; device ignores commands when allowControl is off
  { HA_TEXT, 0, "name",      "Name",      nullptr, nullptr, "mdi:label", nullptr, nullptr },
  { HA_TEXT, 0, "beer_name", "Beer Name", nullptr, nullptr, "mdi:beer",  nullptr, nullptr },
  { HA_TEXT, 0, "yeast",     "Yeast",     nullptr, nullptr, "mdi:flask", nullptr, nullptr },
  // ON/OFF state — always switches; device ignores commands when allowControl is off
  // and the 60s state publish corrects any HA UI changes within one interval
  { HA_SWITCH, 0, "power",           "Power",           nullptr, nullptr, "mdi:power",       nullptr, nullptr },
  { HA_SWITCH, 0, "temp_control",    "Temp Control",    nullptr, nullptr, "mdi:thermostat",  nullptr, nullptr },
  { HA_SWITCH, 0, "profile_running", "Profile Running", nullptr, nullptr, "mdi:play-circle", nullptr, nullptr },
  // Profile select + step progress (profile_no is the only HA_SELECT — its
  // options list lives in publishEntityFromDesc)
  { HA_SELECT, 0, "profile_no",    "Profile No",    nullptr, nullptr, "mdi:playlist-play", nullptr, nullptr },
  { HA_SENSOR, 0, "profile_step",  "Profile Step",  nullptr, "#", "mdi:counter", nullptr, "measurement" },
  { HA_SENSOR, 0, "profile_steps", "Profile Steps", nullptr, "#", "mdi:counter", nullptr, "measurement" },
};

static const HaEntityDesc kProbeEntities[] PROGMEM = {
  { HA_BINARY, 0,             "active",      "Active",      "connectivity", nullptr, nullptr,                "diagnostic", nullptr },
  { HA_SENSOR, HAF_TEMP_UNIT, "temperature", "Temperature", "temperature",  nullptr, nullptr,                nullptr, "measurement" },
  { HA_SENSOR, 0,             "name",        "Name",        nullptr, nullptr, "mdi:label",            "diagnostic", nullptr },
  { HA_SENSOR, 0,             "function",    "Function",    nullptr, nullptr, "mdi:function-variant", "diagnostic", nullptr },
  { HA_SENSOR, 0,             "fermenter",   "Fermenter",   nullptr, nullptr, "mdi:tank",             "diagnostic", nullptr },
};

static const HaEntityDesc kTiltEntities[] PROGMEM = {
  { HA_BINARY, 0,             "active",      "Active",              "connectivity", nullptr, nullptr,    "diagnostic", nullptr },
  { HA_SENSOR, HAF_TEMP_UNIT, "temperature", "Temperature",         "temperature",  nullptr, nullptr,    nullptr, "measurement" },
  { HA_SENSOR, 0,             "gravity",     "Gravity",             nullptr, "SG",    "mdi:test-tube",   nullptr, "measurement" },
  { HA_BINARY, 0,             "is_pro",      "Tilt Pro",            nullptr, nullptr, "mdi:bluetooth",   "diagnostic", nullptr },
  { HA_SENSOR, 0,             "fermenter",   "Fermenter",           nullptr, nullptr, "mdi:tank",        "diagnostic", nullptr },
  { HA_SENSOR, 0,             "function",    "Temperature Reading", nullptr, nullptr, "mdi:thermometer", "diagnostic", nullptr },
};

static const HaEntityDesc kIspindelEntities[] PROGMEM = {
  { HA_SENSOR, HAF_TEMP_UNIT, "temperature",       "Temperature",         "temperature", nullptr, nullptr,       nullptr, "measurement" },
  { HA_SENSOR, 0,             "gravity",           "Gravity",             nullptr, "SG",  "mdi:test-tube",       nullptr, "measurement" },
  { HA_SENSOR, 0,             "corrected_gravity", "Corrected Gravity",   nullptr, "SG",  "mdi:test-tube",       nullptr, "measurement" },
  { HA_SENSOR, 0,             "battery",           "Battery",             "voltage", "V", nullptr,               "diagnostic", "measurement" },
  { HA_SENSOR, 0,             "rssi",              "RSSI",                "signal_strength", "dBm", nullptr,     "diagnostic", "measurement" },
  { HA_SENSOR, 0,             "angle",             "Angle",               nullptr, "\xC2\xB0", "mdi:angle-acute","diagnostic", "measurement" },
  { HA_SENSOR, 0,             "velocity",          "Velocity",            nullptr, nullptr, "mdi:speedometer",   "diagnostic", "measurement" },
  { HA_SENSOR, 0,             "run_time",          "Run Time",            "duration", "s", "mdi:timer-outline",  "diagnostic", "measurement" },
  { HA_SENSOR, 0,             "name",              "Name",                nullptr, nullptr, "mdi:label",         "diagnostic", nullptr },
  { HA_SENSOR, 0,             "fermenter",         "Fermenter",           nullptr, nullptr, "mdi:tank",          "diagnostic", nullptr },
  { HA_SENSOR, 0,             "function",          "Temperature Reading", nullptr, nullptr, "mdi:thermometer",   "diagnostic", nullptr },
};

// Older releases published fermenter entities under different components /
// object ids. Removal cleans these up too, so upgrades don't leak retained
// configs in HA. (Publish never uses this list.)
struct HaLegacyEntity { const char* component; const char* objectId; };

static const HaLegacyEntity kFermenterLegacyEntities[] PROGMEM = {
  // v0.1.20 abbreviated IDs (wrong)
  { "sensor", "beer_temp" }, { "sensor", "ambient_temp" },
  { "sensor", "ceil_temp" }, { "sensor", "floor_temp" },
  // sensor versions of entities that are now number/text (pre-Patch 4/5)
  { "sensor", "ceiling_temperature" }, { "sensor", "floor_temperature" },
  { "sensor", "hysteresis" }, { "sensor", "og" }, { "sensor", "tg" },
  { "sensor", "name" }, { "sensor", "beer_name" }, { "sensor", "yeast" },
  { "sensor", "compressor_delay" },
  // per-fermenter diagnostics that moved to the device scope
  { "sensor", "rssi" }, { "sensor", "free_heap" },
  // binary_sensor versions from v0.1.22 — changed to sensor in v0.1.23
  { "binary_sensor", "power" }, { "binary_sensor", "temp_control" },
  { "binary_sensor", "profile_running" },
  // sensor versions of ON/OFF entities (pre-Patch 3 switches)
  { "sensor", "power" }, { "sensor", "temp_control" }, { "sensor", "profile_running" },
};

static const char* haComponentName(uint8_t kind) {
  switch (kind) {
    case HA_BINARY: return "binary_sensor";
    case HA_NUMBER: return "number";
    case HA_SWITCH: return "switch";
    case HA_SELECT: return "select";
    case HA_TEXT:   return "text";
    case HA_BUTTON: return "button";
    default:        return "sensor";
  }
}

static void removeOneEntity(const char* component, const char* devId, const char* objectId);

// Publish one entity from its descriptor row (copied out of PROGMEM).
static void publishEntityFromDesc(JsonDocument& doc, const HaEntityDesc* row,
    const char* devId, const char* base, const char* devName)
{
  HaEntityDesc d;
  memcpy_P(&d, row, sizeof(d));
  const char* unit = (d.flags & HAF_TEMP_UNIT) ? haTempUnit() : d.unit;
  char cmdKey[48];
  snprintf(cmdKey, sizeof(cmdKey), "%s/set", d.objectId);

  switch (d.kind) {
    case HA_SENSOR:
    case HA_BINARY:
      publishOneEntity(doc, haComponentName(d.kind), devId, base, devName,
        d.objectId, d.name, d.objectId, d.devClass, unit, d.icon, d.entityCat, d.stateClass);
      break;
    case HA_NUMBER: {
      float mn = d.minVal, mx = d.maxVal;
      if (d.flags & HAF_MINMAX_TEMP) { mn = toDisplayTemp(mn); mx = toDisplayTemp(mx); }
      if (d.flags & HAF_MAX_DELTA)   { mx = toDisplayTempDelta(mx); }
      publishNumberEntity(doc, devId, base, devName, d.objectId, d.name,
        d.objectId, cmdKey, mn, mx, d.step, unit, d.devClass, d.icon);
      break;
    }
    case HA_SWITCH:
      publishSwitchEntity(doc, devId, base, devName, d.objectId, d.name,
        d.objectId, cmdKey, d.icon);
      break;
    case HA_SELECT: {
      // profile_no is the only select entity: options 0..MAX_PROFILES
      static const char* profileOpts[] = {"0","1","2","3","4"};
      publishSelectEntity(doc, devId, base, devName, d.objectId, d.name,
        d.objectId, cmdKey, profileOpts, MAX_PROFILES + 1, d.icon);
      break;
    }
    case HA_TEXT:
      publishTextEntity(doc, devId, base, devName, d.objectId, d.name,
        d.objectId, cmdKey, 31, d.icon);
      break;
    case HA_BUTTON:
      publishButtonEntity(doc, devId, base, devName, d.objectId, d.name,
        cmdKey, d.icon);
      break;
  }
}

static void publishEntityTable(JsonDocument& doc, const HaEntityDesc* table, size_t n,
    const char* devId, const char* base, const char* devName)
{
  for (size_t k = 0; k < n; k++) {
    publishEntityFromDesc(doc, &table[k], devId, base, devName);
  }
}

// Remove every entity in a descriptor table (publishes empty retained configs).
static void removeEntityTable(const HaEntityDesc* table, size_t n, const char* devId) {
  for (size_t k = 0; k < n; k++) {
    HaEntityDesc d;
    memcpy_P(&d, &table[k], sizeof(d));
    removeOneEntity(haComponentName(d.kind), devId, d.objectId);
  }
}

static void removeLegacyEntityTable(const HaLegacyEntity* table, size_t n, const char* devId) {
  for (size_t k = 0; k < n; k++) {
    HaLegacyEntity e;
    memcpy_P(&e, &table[k], sizeof(e));
    removeOneEntity(e.component, devId, e.objectId);
  }
}

// Publish HA discovery entity configs for the device itself (not per-fermenter).
// Uses device ID ourbrewbot_{CHIPID} and base topic {baseTopic}/Device.
static void publishDeviceDiscovery() {
  if (!g_mqtt.connected()) return;

  char devId[24], devBase[64];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X",    ESP.getChipId());
  snprintf(devBase, sizeof(devBase), "%s/Device",           g_mqttConfig.baseTopic);

  JsonDocument doc;
  publishEntityTable(doc, kDeviceEntities, HA_COUNT(kDeviceEntities),
                     devId, devBase, "OurBrewbot");

  logMsg("[MQTT] HA discovery published for device: base=%s", devBase);
}

// Publish all HA discovery entity configs for one fermenter.
// objectId must exactly match the key used in the data topic so HA can find the state.
static void publishHaDiscovery(int i) {
  if (!g_mqtt.connected()) return;

  char devId[32], fermBase[96], fermLabel[24];
  snprintf(devId,     sizeof(devId),     "ourbrewbot_%06X_f%d", ESP.getChipId(), i);
  snprintf(fermBase,  sizeof(fermBase),  "%s/Fermenter%d", g_mqttConfig.baseTopic, i);
  snprintf(fermLabel, sizeof(fermLabel), "OurBrewbot F%d", i);  // stable — not user-editable name

  JsonDocument doc;
  publishEntityTable(doc, kFermenterEntities, HA_COUNT(kFermenterEntities),
                     devId, fermBase, fermLabel);

  logMsg("[MQTT] HA discovery published for F%d: base=%s", i, fermBase);
}

// Publish HA discovery for one Probe slot. Skips empty / unconfigured slots.
static void publishProbeDiscovery(int idx) {
  if (!g_mqtt.connected()) return;
  if (strlen(g_probes[idx].address) == 0) return;

  char devId[48], base[96], devName[48];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X_probe_%s",
    ESP.getChipId(), g_probes[idx].address);
  snprintf(base,    sizeof(base),    "%s/Probe/%s",
    g_mqttConfig.baseTopic, g_probes[idx].address);
  snprintf(devName, sizeof(devName), "OurBrewbot Probe %s",
    strlen(g_probes[idx].probeName) > 0 ? g_probes[idx].probeName : g_probes[idx].address);

  JsonDocument doc;
  publishEntityTable(doc, kProbeEntities, HA_COUNT(kProbeEntities), devId, base, devName);

  logMsg("[MQTT] HA discovery published for probe %s", g_probes[idx].address);
}

// Publish HA discovery for one Tilt colour slot. Skips unconfigured colours.
static void publishTiltDiscovery(int colour) {
  if (!g_mqtt.connected()) return;
  if (g_tilts[colour].colour == PROBE_UNASSIGNED) return;

  const char* colourName = getTiltColourName(colour);
  char devId[48], base[96], devName[48];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X_tilt_%s",
    ESP.getChipId(), colourName);
  snprintf(base,    sizeof(base),    "%s/Tilt/%s",
    g_mqttConfig.baseTopic, colourName);
  snprintf(devName, sizeof(devName), "OurBrewbot Tilt %s", colourName);

  JsonDocument doc;
  publishEntityTable(doc, kTiltEntities, HA_COUNT(kTiltEntities), devId, base, devName);

  logMsg("[MQTT] HA discovery published for tilt %s", colourName);
}

// Publish HA discovery for one iSpindel slot. Skips empty / "None" slots.
static void publishIspindelDiscovery(int idx) {
  if (!g_mqtt.connected()) return;
  if (strlen(g_iSpindels[idx].id) == 0) return;
  if (strcmp(g_iSpindels[idx].name, "None") == 0) return;

  char devId[48], base[96], devName[48];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X_ispindel_%s",
    ESP.getChipId(), g_iSpindels[idx].id);
  snprintf(base,    sizeof(base),    "%s/iSpindel/%s",
    g_mqttConfig.baseTopic, g_iSpindels[idx].id);
  snprintf(devName, sizeof(devName), "OurBrewbot iSpindel %s",
    strlen(g_iSpindels[idx].name) > 0 ? g_iSpindels[idx].name : g_iSpindels[idx].id);

  JsonDocument doc;
  publishEntityTable(doc, kIspindelEntities, HA_COUNT(kIspindelEntities), devId, base, devName);

  logMsg("[MQTT] HA discovery published for ispindel %s", g_iSpindels[idx].id);
}

// Remove HA discovery for one Probe by its OneWire address.
static void removeProbeDiscovery(const char* address) {
  if (!g_mqtt.connected()) return;
  if (strlen(address) == 0) return;
  char devId[48];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X_probe_%s", ESP.getChipId(), address);
  removeEntityTable(kProbeEntities, HA_COUNT(kProbeEntities), devId);
}

// Remove HA discovery for one Tilt colour.
static void removeTiltDiscovery(int colour) {
  if (!g_mqtt.connected()) return;
  const char* colourName = getTiltColourName(colour);
  char devId[48];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X_tilt_%s", ESP.getChipId(), colourName);
  removeEntityTable(kTiltEntities, HA_COUNT(kTiltEntities), devId);
}

// Remove HA discovery for one iSpindel by its hex id.
static void removeIspindelDiscovery(const char* id) {
  if (!g_mqtt.connected()) return;
  if (strlen(id) == 0) return;
  char devId[48];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X_ispindel_%s", ESP.getChipId(), id);
  removeEntityTable(kIspindelEntities, HA_COUNT(kIspindelEntities), devId);
}

// Remove one HA entity by publishing an empty retained payload to its discovery topic.
static void removeOneEntity(const char* component, const char* devId, const char* objectId) {
  char discTopic[128];
  snprintf(discTopic, sizeof(discTopic), "homeassistant/%s/%s/%s/config",
    component, devId, objectId);
  if (!g_mqtt.publish(discTopic, (const uint8_t*)"", 0, true))
    logPublishFailure(discTopic);
  yield();
}

// Remove all HA discovery entities for one fermenter — every entity in the
// current table plus the legacy IDs older releases published.
static void removeHaDiscovery(int i) {
  if (!g_mqtt.connected()) return;
  char devId[32];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X_f%d", ESP.getChipId(), i);
  removeLegacyEntityTable(kFermenterLegacyEntities, HA_COUNT(kFermenterLegacyEntities), devId);
  removeEntityTable(kFermenterEntities, HA_COUNT(kFermenterEntities), devId);
  logMsg("[MQTT] HA discovery removed for F%d", i);
}

// Remove all HA discovery entities for the device itself.
static void removeDeviceDiscovery() {
  if (!g_mqtt.connected()) return;
  char devId[24];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X", ESP.getChipId());
  removeEntityTable(kDeviceEntities, HA_COUNT(kDeviceEntities), devId);
  logMsg("[MQTT] HA discovery removed for device");
}

// Publish discovery for all MQTT-enabled fermenters and the device itself.
// Called on connect and whenever HA restarts.
void publishAllHaDiscovery() {
  if (!g_mqttConfig.haDiscovery) return;
  if (!g_mqtt.connected()) return;
  publishDeviceDiscovery();
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (g_fermenters[i].brewServices & (1 << MQTT_SERVICE_BIT)) {
      publishHaDiscovery(i);
    }
  }
  for (int i = 0; i < MAX_PROBES;    i++) publishProbeDiscovery(i);
  for (int i = 0; i < MAX_TILTS;     i++) publishTiltDiscovery(i);
  for (int i = 0; i < MAX_ISPINDELS; i++) publishIspindelDiscovery(i);
}

// Remove all HA discovery entities (called when haDiscovery is disabled).
void cleanupAllHaDiscovery() {
  if (!g_mqtt.connected()) return;
  removeDeviceDiscovery();
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    removeHaDiscovery(i);
  }
  for (int i = 0; i < MAX_PROBES; i++) {
    if (strlen(g_probes[i].address) > 0) removeProbeDiscovery(g_probes[i].address);
  }
  for (int i = 0; i < MAX_TILTS; i++) {
    if (g_tilts[i].colour != PROBE_UNASSIGNED) removeTiltDiscovery(i);
  }
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (strlen(g_iSpindels[i].id) > 0) removeIspindelDiscovery(g_iSpindels[i].id);
  }
}

// ============================================================
// MQTT CALLBACK — handles incoming subscribed messages
// ============================================================

static void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
  // HA birth message: re-publish discovery so entities appear after HA restart
  if (strcmp(topic, "homeassistant/status") == 0 &&
      length >= 6 && memcmp(payload, "online", 6) == 0) {
    publishAllHaDiscovery();
    return;
  }
  // Command dispatch: <baseTopic>/<scope>/<key>/set
  if (!g_mqttConfig.allowControl) return;

  // Parse scope and key from topic
  char scope[16];
  char key[32];
  if (!parseMqttCommandTopic(topic, g_mqttConfig.baseTopic, scope, sizeof(scope), key, sizeof(key))) return;

  // Copy payload (not null-terminated in PubSubClient callback)
  char pl[48];
  size_t n = length < sizeof(pl) - 1 ? length : sizeof(pl) - 1;
  memcpy(pl, payload, n);
  pl[n] = '\0';

  logMsg("[MQTT] cmd scope=%s key=%s pl=%s", scope, key, pl);

  // Device-scope commands
  if (strcmp(scope, "Device") == 0) {
    if (strcmp(key, "reboot") == 0) {
      recordReboot("MQTT command");
      ESP.restart();
    } else if (strcmp(key, "all_off") == 0) {
      switchOffAll();
    }
    return;
  }

  // Fermenter-scope commands: scope = "FermenterN"
  int idx = fermenterIndexFromScope(scope);
  if (idx < 0) return;

  bool on = (strcmp(pl, "ON") == 0);

  if (strcmp(key, "power") == 0) {
    setFermenterPower(idx, on);
  } else if (strcmp(key, "temp_control") == 0) {
    g_fermenters[idx].tempControl = on;
  } else if (strcmp(key, "profile_running") == 0) {
    if (on) startProfile(idx, g_fermenters[idx].profileNo);
    else    stopProfile(idx);
  } else if (strcmp(key, "ceiling_temperature") == 0 ||
             strcmp(key, "floor_temperature")   == 0 ||
             strcmp(key, "hysteresis")          == 0 ||
             strcmp(key, "compressor_delay")    == 0 ||
             strcmp(key, "og")                  == 0 ||
             strcmp(key, "tg")                  == 0) {
    const char* errMsg;
    float v = atof(pl);
    // Commands arrive in the configured display unit (HA discovery advertises it);
    // convert to internal Celsius before validation (Celsius ranges).
    if (strcmp(key, "ceiling_temperature") == 0 ||
        strcmp(key, "floor_temperature")   == 0) {
      v = toCelsius(v);
    } else if (strcmp(key, "hysteresis") == 0) {
      v = toCelsiusTempDelta(v);
    }
    if (!validateFermenterField(idx, key, v, &errMsg)) {
      logMsg("[MQTT] cmd rejected (%s): %s", key, errMsg);
      return;
    }
    if      (strcmp(key, "ceiling_temperature") == 0) g_fermenters[idx].ceilingTemp     = v;
    else if (strcmp(key, "floor_temperature")   == 0) g_fermenters[idx].floorTemp       = v;
    else if (strcmp(key, "hysteresis")          == 0) g_fermenters[idx].hysteresis      = v;
    else if (strcmp(key, "compressor_delay")    == 0) g_fermenters[idx].compressorDelay = (uint16_t)v;
    else if (strcmp(key, "og")                  == 0) g_fermenters[idx].og              = v;
    else if (strcmp(key, "tg")                  == 0) g_fermenters[idx].tg              = v;
  } else if (strcmp(key, "name") == 0) {
    strlcpy(g_fermenters[idx].fermenterName, pl, sizeof(g_fermenters[0].fermenterName));
  } else if (strcmp(key, "beer_name") == 0) {
    strlcpy(g_fermenters[idx].beerName, pl, sizeof(g_fermenters[0].beerName));
  } else if (strcmp(key, "yeast") == 0) {
    strlcpy(g_fermenters[idx].yeastName, pl, sizeof(g_fermenters[0].yeastName));
  } else if (strcmp(key, "profile_no") == 0) {
    int pno = atoi(pl);
    if (pno < 0 || pno > MAX_PROFILES) {
      logMsg("[MQTT] cmd rejected: profile_no %d out of range (0-%d)", pno, MAX_PROFILES);
      return;
    }
    g_fermenters[idx].profileNo = (uint8_t)pno;
  } else {
    return;  // unknown key — ignore silently
  }

  g_mqttPendingSave   = true;
  g_mqttPendingSaveAt = millis();
  publishFermenterField(idx, key);
}

// ============================================================
// CONNECT
// ============================================================

// Subscribe or unsubscribe the command wildcard based on current allowControl state.
// Safe to call on an already-connected client — used both at connect-time and when
// the setting is toggled at runtime via the admin UI.
void mqttApplyControlSubscription() {
  if (!g_mqtt.connected()) return;
  char cmdWildcard[48];
  snprintf(cmdWildcard, sizeof(cmdWildcard), "%s/+/+/set", g_mqttConfig.baseTopic);
  if (g_mqttConfig.allowControl) {
    g_mqtt.subscribe(cmdWildcard);
    logMsg("[MQTT] Subscribed to commands: %s/+/+/set", g_mqttConfig.baseTopic);
  } else {
    g_mqtt.unsubscribe(cmdWildcard);
    logMsg("[MQTT] Unsubscribed from commands: %s/+/+/set", g_mqttConfig.baseTopic);
  }

}

// Post-connect setup shared by mqttConnect() and testMqtt(): mark the device
// online, restore subscriptions and (re)publish HA discovery.  Keeping this in
// one place guarantees a test-initiated connection is indistinguishable from a
// normal reconnect.
static void onMqttConnected() {
  if (!g_mqttWasConnected) {
    logMsg("[MQTT] Connected to %s:%d", g_mqttConfig.host, g_mqttConfig.port);
  }
  g_mqttWasConnected = true;
  g_mqttBackoffMs = 5000;  // reset backoff on success

  // Mark device online
  if (!g_mqtt.publish(s_availTopic, "online", true))
    logPublishFailure(s_availTopic);

  // Subscribe to HA birth message to re-publish discovery after HA restarts
  g_mqtt.subscribe("homeassistant/status");

  // Subscribe/unsubscribe command wildcard based on current allowControl state
  mqttApplyControlSubscription();

  // Publish discovery configs for all MQTT-enabled fermenters
  if (g_mqttConfig.haDiscovery) {
    publishAllHaDiscovery();
  }
}

static bool mqttConnect() {
  if (!g_mqttConfig.enabled) return false;
  if (strlen(g_mqttConfig.host) == 0) return false;
  if (!WiFi.isConnected()) return false;

  // Exponential backoff — don't hammer the broker on repeated failures
  unsigned long now = millis();
  if (g_mqttLastAttempt > 0 && (now - g_mqttLastAttempt) < g_mqttBackoffMs) {
    return false;
  }
  g_mqttLastAttempt = now;

  g_mqtt.setServer(g_mqttConfig.host, g_mqttConfig.port);

  char clientId[24];
  snprintf(clientId, sizeof(clientId), "ourbrewbot-%06X", ESP.getChipId());

  // Availability topic: broker publishes "offline" as LWT on unexpected disconnect
  snprintf(s_availTopic, sizeof(s_availTopic), "%s/availability", g_mqttConfig.baseTopic);

  // Always set buffer and callback here (in case initMqtt() was skipped when MQTT was disabled at boot)
  g_mqtt.setBufferSize(1024);
  g_mqtt.setSocketTimeout(5);  // default 15 s connect stall starves the loop when broker is unreachable
  g_mqtt.setCallback(mqttMessageCallback);

  bool ok;
  if (strlen(g_mqttConfig.username) > 0) {
    ok = g_mqtt.connect(clientId,
                        g_mqttConfig.username, g_mqttConfig.password,
                        s_availTopic, 0, true, "offline");
  } else {
    ok = g_mqtt.connect(clientId,
                        nullptr, nullptr,
                        s_availTopic, 0, true, "offline");
  }

  if (ok) {
    onMqttConnected();
  } else {
    // Backoff is a floor, not a schedule: mqttEnsureConnected() is only reached
    // from reportMqtt(), so the next actual attempt is the next report tick
    // (60s) unless the backoff has grown past it.
    logMsg("[MQTT] Connection failed, rc=%d (backoff %lus, retry on next report)",
      g_mqtt.state(), g_mqttBackoffMs / 1000);
    g_mqttWasConnected = false;
    // Double backoff, capped
    if (g_mqttBackoffMs < MQTT_MAX_BACKOFF_MS) {
      g_mqttBackoffMs *= 2;
      if (g_mqttBackoffMs > MQTT_MAX_BACKOFF_MS) g_mqttBackoffMs = MQTT_MAX_BACKOFF_MS;
    }
  }
  return ok;
}

// Reconnect if the link has dropped, logging the drop before retrying.
// Clearing g_mqttWasConnected HERE — rather than only on a failed attempt — is
// what makes a clean drop-and-reconnect visible in the log. Without it the flag
// stays true, onMqttConnected() skips its "Connected" line, and a reconnect is
// only detectable from the re-subscribe it leaves behind.
static bool mqttEnsureConnected() {
  if (g_mqtt.connected()) return true;
  if (g_mqttWasConnected) {
    logMsg("[MQTT] Connection lost (rc=%d) - reconnecting", g_mqtt.state());
    g_mqttWasConnected = false;
  }
  return mqttConnect();
}

// ============================================================
// FORCE DISCOVER — manual trigger from admin UI button
// ============================================================

// Publish HA discovery for all MAX_FERMENTERS, ignoring the per-fermenter
// MQTT service-bit and the global haDiscovery flag.  Connects if needed.
// Returns true if at least one discovery payload was sent.
bool forcePublishAllHaDiscovery() {
  if (!g_mqttConfig.enabled || strlen(g_mqttConfig.host) == 0) {
    logMsg("[MQTT] Discover: MQTT not configured");
    return false;
  }
  if (!g_mqtt.connected()) {
    logMsg("[MQTT] Discover: not connected - attempting connect");
    if (!mqttEnsureConnected()) {
      logMsg("[MQTT] Discover: connect failed");
      return false;
    }
  }
  logMsg("[MQTT] Discover: publishing all fermenters to base=%s", g_mqttConfig.baseTopic);
  publishDeviceDiscovery();
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    publishHaDiscovery(i);
  }
  for (int i = 0; i < MAX_PROBES;    i++) publishProbeDiscovery(i);
  for (int i = 0; i < MAX_TILTS;     i++) publishTiltDiscovery(i);
  for (int i = 0; i < MAX_ISPINDELS; i++) publishIspindelDiscovery(i);
  return true;
}

// ============================================================
// DEFERRED SAVE — called from main loop, not from within the callback
// ============================================================

void mqttPendingSaveCheck() {
  if (!g_mqttPendingSave) return;
  if (millis() - g_mqttPendingSaveAt < 3000) return;
  saveFermenterConfig();
  g_mqttPendingSave = false;
  logMsg("[MQTT] Deferred config save complete");
}

// ============================================================
// LOG MIRROR — publish logMsg() lines to <baseTopic>/Device/log
// ============================================================
//
// Called from Log.cpp:logMsg() after the Serial/syslog fan-out. The re-entry
// guard prevents recursion if PubSubClient internals or our publish code ever
// triggers another logMsg() while we're mid-publish. We still publish lines
// that *describe* MQTT activity (e.g. "[MQTT] Published F0") — those are useful
// to see on the topic. The guard only prevents the publish-from-within-publish
// recursion that would form an infinite loop.

void mqttPublishLog(uint8_t level, const char* line) {
  static bool s_inLogPublish = false;
  if (s_inLogPublish) return;
  if (!g_mqttConfig.enabled || !g_mqttConfig.logEnabled) return;
  if (!g_mqtt.connected()) return;
  if (!line || !line[0]) return;

  static const char* const kSev[] = {
    "EMERG","ALERT","CRIT","ERR","WARNING","NOTICE","INFO","DEBUG"
  };
  const char* sev = (level < 8) ? kSev[level] : "INFO";

  // Escape " and \ so the JSON string is always valid.
  // Buffers are static — safe because s_inLogPublish prevents re-entry, keeping
  // ~490 bytes off the call stack per log call.
  static char safe[210];
  size_t j = 0;
  for (size_t i = 0; line[i] && j < sizeof(safe) - 2; ++i) {
    if (line[i] == '"' || line[i] == '\\') safe[j++] = '\\';
    safe[j++] = line[i];
  }
  safe[j] = '\0';

  static char payload[290];  // 40 JSON overhead + 210 safe + closing + margin
  snprintf(payload, sizeof(payload),
           "{\"level\":%u,\"severity\":\"%s\",\"msg\":\"%s\"}", level, sev, safe);

  s_inLogPublish = true;
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/Device/log", g_mqttConfig.baseTopic);
  // Deliberately unchecked: logging a failure here would re-enter the log
  // path this function mirrors (the s_inLogPublish guard would swallow it).
  g_mqtt.publish(topic, payload, false);  // non-retained
  s_inLogPublish = false;
}

// ============================================================
// INIT / LOOP
// ============================================================

void initMqtt() {
  if (!g_mqttConfig.enabled) return;
  g_mqtt.setServer(g_mqttConfig.host, g_mqttConfig.port);
  g_mqtt.setCallback(mqttMessageCallback);
  g_mqtt.setBufferSize(1024);  // writable-entity discovery payloads can reach ~900 bytes; measure in testing
  g_mqtt.setSocketTimeout(5);  // bound connect stalls (default 15 s)
  logMsg("[MQTT] Configured: %s:%d base=%s ha_discovery=%s",
    g_mqttConfig.host, g_mqttConfig.port, g_mqttConfig.baseTopic,
    g_mqttConfig.haDiscovery ? "on" : "off");
}

void mqttLoop() {
  if (!g_mqttConfig.enabled) return;
  if (!g_mqtt.connected()) return;
  g_mqtt.loop();
}

// ============================================================
// REPORT — publish all enabled fermenter data
// ============================================================

// Publish state for all configured Probes to {baseTopic}/Probe/{address}/{key}.
static void publishProbes() {
  char base[96];
  for (int i = 0; i < MAX_PROBES; i++) {
    if (strlen(g_probes[i].address) == 0) continue;
    snprintf(base, sizeof(base), "%s/Probe/%s",
      g_mqttConfig.baseTopic, g_probes[i].address);
    publishBool (base, "active",      g_probes[i].failCount < PROBE_FAIL_THRESHOLD);
    // Preserve the -127 "no reading" sentinel — only real readings are unit-converted
    float probeTemp = g_probes[i].temperature;
    publishFloat(base, "temperature", probeTemp > TEMP_VALID_MIN ? toDisplayTemp(probeTemp) : probeTemp);
    publishValue(base, "name",        g_probes[i].probeName);
    publishValue(base, "function",    probeFunctionName(g_probes[i].function));
    publishInt  (base, "fermenter",   g_probes[i].fermenter);
    yield();
  }
}

// Publish state for all configured Tilts to {baseTopic}/Tilt/{Colour}/{key}.
static void publishTilts() {
  char base[96];
  for (int i = 0; i < MAX_TILTS; i++) {
    if (g_tilts[i].colour == PROBE_UNASSIGNED) continue;
    snprintf(base, sizeof(base), "%s/Tilt/%s",
      g_mqttConfig.baseTopic, getTiltColourName(i));
    publishBool (base, "active",      g_tilts[i].active);
    publishFloat(base, "temperature", toDisplayTemp(g_tilts[i].temperature));
    publishFloat(base, "gravity",     g_tilts[i].sg, 4);
    publishBool (base, "is_pro",      g_tilts[i].isPro);
    publishInt  (base, "fermenter",   g_tilts[i].fermenter);
    publishValue(base, "function",    probeFunctionName(g_tilts[i].function));
    yield();
  }
}

// Publish state for all configured iSpindels to {baseTopic}/iSpindel/{id}/{key}.
static void publishIspindels() {
  char base[96];
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (strlen(g_iSpindels[i].id) == 0) continue;
    if (strcmp(g_iSpindels[i].name, "None") == 0) continue;
    snprintf(base, sizeof(base), "%s/iSpindel/%s",
      g_mqttConfig.baseTopic, g_iSpindels[i].id);
    publishFloat(base, "temperature",       toDisplayTemp(g_iSpindels[i].temperature));
    publishFloat(base, "gravity",           g_iSpindels[i].sg, 4);
    publishFloat(base, "corrected_gravity", g_iSpindels[i].corrGravity, 4);
    publishFloat(base, "battery",           g_iSpindels[i].battery, 3);
    publishInt  (base, "rssi",              g_iSpindels[i].rssi);
    publishFloat(base, "angle",             g_iSpindels[i].angle, 2);
    publishFloat(base, "velocity",          g_iSpindels[i].velocity, 4);
    publishFloat(base, "run_time",          g_iSpindels[i].runTime, 1);
    publishValue(base, "name",              g_iSpindels[i].name);
    publishInt  (base, "fermenter",         g_iSpindels[i].fermenter);
    publishValue(base, "function",          probeFunctionName(g_iSpindels[i].function));
    yield();
  }
}

// Publish current device-level diagnostics to {baseTopic}/Device/{key}.
static void publishDeviceReport() {
  char base[64];
  snprintf(base, sizeof(base), "%s/Device", g_mqttConfig.baseTopic);

  publishValue(base, "firmware_version", FW_VERSION);

  // IP address
  String ip = WiFi.localIP().toString();
  publishValue(base, "ip_address", ip.c_str());

  // mDNS name: ourbrewbot-{chipid_hex}.local
  char mdns[32];
  snprintf(mdns, sizeof(mdns), "ourbrewbot-%06x.local", ESP.getChipId());
  publishValue(base, "mdns_name", mdns);

  // WiFi SSID
  String ssid = WiFi.SSID();
  publishValue(base, "wifi_ssid", ssid.c_str());

  publishInt(base, "rssi",      WiFi.RSSI());
  publishInt(base, "free_heap", ESP.getFreeHeap());
  publishInt(base, "uptime",    (int)(millis() / 60000UL));

  // Chip ID as lowercase hex
  char chipId[8];
  snprintf(chipId, sizeof(chipId), "%06x", ESP.getChipId());
  publishValue(base, "chip_id", chipId);

  // Reboot info — captured once at boot, static for the session
  publishValue(base, "reboot_reason", g_rebootReason.c_str());
  struct rst_info* ri = ESP.getResetInfoPtr();
  publishInt(base, "reboot_code", ri->reason);

  logMsg("[MQTT] Published device report: base=%s", base);
}

void reportMqtt() {
  if (!g_mqttConfig.enabled) return;
  if (!WiFi.isConnected()) return;

  if (!mqttEnsureConnected()) return;

  // Base topic buffer: baseTopic(31) + "/" + "Fermenter" + "N" + NUL
  char base[96];

  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (!(g_fermenters[i].brewServices & (1 << MQTT_SERVICE_BIT))) continue;

    snprintf(base, sizeof(base), "%s/Fermenter%d", g_mqttConfig.baseTopic, i);

    // Identity
    publishValue(base, "name", g_fermenters[i].fermenterName);

    float beerTemp    = getBeerTemp(i);
    float ambientTemp = getAmbientTemp(i);
    float sg          = getCurrentSG(i);
    const char* unit  = (g_globalConfig.unit == UNIT_FAHRENHEIT) ? "F" : "C";

    // Temperatures — published in the configured display unit (internal values
    // are always Celsius; conversions are identity in Celsius mode)
    if (beerTemp > TEMP_VALID_MIN)
      publishFloat(base, "beer_temperature", toDisplayTemp(beerTemp));
    publishValue(base, "beer_temperature_source", getBeerTempSource(i));
    if (ambientTemp > TEMP_VALID_MIN)
      publishFloat(base, "ambient_temperature", toDisplayTemp(ambientTemp));
    publishFloat(base, "ceiling_temperature", toDisplayTemp(g_fermenters[i].ceilingTemp));
    publishFloat(base, "floor_temperature", toDisplayTemp(g_fermenters[i].floorTemp));
    publishValue(base, "temperature_unit", unit);
    publishFloat(base, "hysteresis", toDisplayTempDelta(g_fermenters[i].hysteresis));

    // Gravity
    if (sg > 0.0f)
      publishFloat(base, "gravity", sg, 4);
    publishValue(base, "gravity_source", getGravitySource(i));
    publishFloat(base, "og", g_fermenters[i].og, 4);
    publishFloat(base, "tg", g_fermenters[i].tg, 4);
    publishFloat(base, "attenuation", getAttenuation(i));

    // State
    publishBool(base, "power", g_fermenters[i].power);
    publishBool(base, "temp_control", g_fermenters[i].tempControl);
    publishBool(base, "alarm", g_fermenters[i].alarm);
    uint8_t st = g_fermenters[i].status;
    const char* state = (st == STATUS_HEATING) ? "heating" :
                        (st == STATUS_COOLING) ? "cooling" :
                        (st == STATUS_ALARM)   ? "alarm"   : "idle";
    publishValue(base, "status", state);

    // Info
    publishValue(base, "beer_name", g_fermenters[i].beerName);
    publishValue(base, "yeast", g_fermenters[i].yeastName);
    publishInt(base, "compressor_delay", g_fermenters[i].compressorDelay);
    publishInt (base, "profile_no",      g_fermenters[i].profileNo);
    publishBool(base, "profile_running", g_fermenters[i].profileRunning);
    if (g_fermenters[i].profileRunning) {
      publishInt(base, "profile_step",  g_fermenters[i].currentStep + 1);
      publishInt(base, "profile_steps", countProfileSteps(g_fermenters[i].profileNo - 1));
    } else {
      publishInt(base, "profile_step",  0);
      publishInt(base, "profile_steps", 0);
    }

    logMsg("[MQTT] Published F%d (%s)", i, g_fermenters[i].fermenterName);
  }

  publishProbes();
  publishTilts();
  publishIspindels();
  publishDeviceReport();
}

// ============================================================
// TEST — try connecting and publishing a test message
// ============================================================

bool testMqtt() {
  if (strlen(g_mqttConfig.host) == 0) {
    logMsg("[MQTT] Test: no host configured");
    return false;
  }

  // Connect only if needed, with the same LWT/availability arguments and
  // post-connect setup as the normal reconnect path — a test-initiated
  // connection is indistinguishable from a regular one.  When already
  // connected, connect() would be a no-op anyway; just publish on the
  // existing connection.
  if (!g_mqtt.connected()) {
    g_mqtt.setServer(g_mqttConfig.host, g_mqttConfig.port);
    g_mqtt.setBufferSize(1024);
    g_mqtt.setSocketTimeout(5);  // bound the blocking connect (default 15 s)
    g_mqtt.setCallback(mqttMessageCallback);

    char clientId[24];
    snprintf(clientId, sizeof(clientId), "ourbrewbot-%06X", ESP.getChipId());
    snprintf(s_availTopic, sizeof(s_availTopic), "%s/availability", g_mqttConfig.baseTopic);

    logMsg("[MQTT] Test connecting to %s:%d user=%s",
      g_mqttConfig.host, g_mqttConfig.port, g_mqttConfig.username);

    bool connected;
    if (strlen(g_mqttConfig.username) > 0) {
      connected = g_mqtt.connect(clientId,
                                 g_mqttConfig.username, g_mqttConfig.password,
                                 s_availTopic, 0, true, "offline");
    } else {
      connected = g_mqtt.connect(clientId,
                                 nullptr, nullptr,
                                 s_availTopic, 0, true, "offline");
    }

    if (!connected) {
      logMsg("[MQTT] Test failed, rc=%d", g_mqtt.state());
      return false;
    }
    onMqttConnected();
  }

  char topic[48];
  snprintf(topic, sizeof(topic), "%s/test", g_mqttConfig.baseTopic);
  bool ok = g_mqtt.publish(topic, "OurBrewbot MQTT test OK");
  logMsg("[MQTT] Test publish to %s: %s", topic, ok ? "OK" : "FAIL");

  return ok;
}
