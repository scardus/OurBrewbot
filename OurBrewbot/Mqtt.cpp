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

static void publishValue(const char* base, const char* key, const char* value) {
  snprintf(s_topicBuf, sizeof(s_topicBuf), "%s/%s", base, key);
  g_mqtt.publish(s_topicBuf, value, true);  // retained
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
static void publishAndReset(JsonDocument& doc,
    const char* component, const char* devId, const char* objectId)
{
  char discTopic[128];
  snprintf(discTopic, sizeof(discTopic), "homeassistant/%s/%s/%s/config",
    component, devId, objectId);
  String payload;
  serializeJson(doc, payload);
  g_mqtt.publish(discTopic, payload.c_str(), true);
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
  else if (strcmp(key, "ceiling_temperature") == 0) publishFloat(base, "ceiling_temperature",  g_fermenters[i].ceilingTemp);
  else if (strcmp(key, "floor_temperature")   == 0) publishFloat(base, "floor_temperature",    g_fermenters[i].floorTemp);
  else if (strcmp(key, "hysteresis")          == 0) publishFloat(base, "hysteresis",           g_fermenters[i].hysteresis);
  else if (strcmp(key, "compressor_delay")    == 0) publishInt  (base, "compressor_delay",     g_fermenters[i].compressorDelay);
  else if (strcmp(key, "og")                  == 0) publishFloat(base, "og",                   g_fermenters[i].og, 4);
  else if (strcmp(key, "tg")                  == 0) publishFloat(base, "tg",                   g_fermenters[i].tg, 4);
  else if (strcmp(key, "name")                == 0) publishValue(base, "name",                 g_fermenters[i].fermenterName);
  else if (strcmp(key, "beer_name")           == 0) publishValue(base, "beer_name",            g_fermenters[i].beerName);
  else if (strcmp(key, "yeast")               == 0) publishValue(base, "yeast",                g_fermenters[i].yeastName);
  else if (strcmp(key, "profile_no")          == 0) publishInt  (base, "profile_no",           g_fermenters[i].profileNo);
}

// Publish HA discovery entity configs for the device itself (not per-fermenter).
// Uses device ID ourbrewbot_{CHIPID} and base topic {baseTopic}/Device.
static void publishDeviceDiscovery() {
  if (!g_mqtt.connected()) return;

  char devId[24], devBase[64];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X",    ESP.getChipId());
  snprintf(devBase, sizeof(devBase), "%s/Device",           g_mqttConfig.baseTopic);

  JsonDocument doc;

  // Static text diagnostics
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "firmware_version", "Firmware Version", "firmware_version",
    nullptr, nullptr, "mdi:tag", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "ip_address", "IP Address", "ip_address",
    nullptr, nullptr, "mdi:ip-network", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "mdns_name", "mDNS Name", "mdns_name",
    nullptr, nullptr, "mdi:lan", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "wifi_ssid", "WiFi SSID", "wifi_ssid",
    nullptr, nullptr, "mdi:wifi", "diagnostic", nullptr);

  // Numeric diagnostics
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "rssi",      "RSSI",      "rssi",
    "signal_strength", "dBm", nullptr, "diagnostic", "measurement");
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "free_heap", "Free Heap", "free_heap",
    nullptr, "B", "mdi:memory", "diagnostic", "measurement");
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "uptime",    "Uptime",    "uptime",
    "duration", "min", "mdi:clock-outline", "diagnostic", "measurement");

  // Identity & reboot info
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "chip_id", "Chip ID", "chip_id",
    nullptr, nullptr, "mdi:chip", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "reboot_reason", "Reboot Reason", "reboot_reason",
    nullptr, nullptr, "mdi:restart", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, devBase, "OurBrewbot",
    "reboot_code", "Reboot Code", "reboot_code",
    nullptr, nullptr, "mdi:restart-alert", "diagnostic", "measurement");

  // Device control buttons — always advertised; ignored by device when allowControl is off,
  // state topic publishes correct device state within 60s regardless
  publishButtonEntity(doc, devId, devBase, "OurBrewbot",
    "reboot",  "Reboot",  "reboot/set",  "mdi:restart");
  publishButtonEntity(doc, devId, devBase, "OurBrewbot",
    "all_off", "All Off", "all_off/set", "mdi:power-off");

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
  const char* tempUnit = "\xC2\xB0""C";

  JsonDocument doc;

  // Temperature sensors — numeric, use state_class measurement
  // Entity names are the plain property names; HA prefixes them with dev.name ("OurBrewbot F0")
  // when displaying and when generating entity IDs (sensor.ourbrewbot_f0_beer_temperature).
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "beer_temperature",    "Beer Temperature",    "beer_temperature",    "temperature", tempUnit, nullptr,           nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "beer_temperature_source", "Beer Temperature Source", "beer_temperature_source", nullptr, nullptr, "mdi:information-outline", nullptr, nullptr);
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "ambient_temperature", "Ambient Temperature", "ambient_temperature", "temperature", tempUnit, nullptr,           nullptr, "measurement");
  // Setpoint numbers — always number entities; device ignores commands when allowControl is off,
  // and the 60s state publish corrects any HA UI changes within one interval
  publishNumberEntity(doc, devId, fermBase, fermLabel,
    "ceiling_temperature", "Ceiling Temperature",
    "ceiling_temperature", "ceiling_temperature/set",
    -20.0f, 50.0f, 0.1f, tempUnit, "temperature");
  publishNumberEntity(doc, devId, fermBase, fermLabel,
    "floor_temperature", "Floor Temperature",
    "floor_temperature", "floor_temperature/set",
    -20.0f, 50.0f, 0.1f, tempUnit, "temperature");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "temperature_unit", "Temperature Unit", "temperature_unit", nullptr, nullptr, "mdi:thermometer", nullptr, nullptr);
  publishNumberEntity(doc, devId, fermBase, fermLabel,
    "hysteresis", "Hysteresis",
    "hysteresis", "hysteresis/set",
    0.0f, 10.0f, 0.1f, tempUnit);
  publishNumberEntity(doc, devId, fermBase, fermLabel,
    "compressor_delay", "Compressor Delay",
    "compressor_delay", "compressor_delay/set",
    0.0f, 1440.0f, 1.0f, "min", nullptr, "mdi:timer-outline");

  // Gravity sensors — read-only numeric sensors
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "gravity",     "Gravity",     "gravity",     nullptr, "SG", "mdi:test-tube", nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "gravity_source", "Gravity Source", "gravity_source", nullptr, nullptr, "mdi:information-outline", nullptr, nullptr);
  publishNumberEntity(doc, devId, fermBase, fermLabel,
    "og", "OG",
    "og", "og/set",
    0.990f, 1.200f, 0.001f, "SG", nullptr, "mdi:test-tube");
  publishNumberEntity(doc, devId, fermBase, fermLabel,
    "tg", "TG",
    "tg", "tg/set",
    0.990f, 1.200f, 0.001f, "SG", nullptr, "mdi:test-tube");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "attenuation", "Attenuation", "attenuation", nullptr, "%",  "mdi:percent",   nullptr, "measurement");

  // Status — read-only text sensor
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "status", "Status", "status", nullptr, nullptr, "mdi:thermometer", nullptr, nullptr);

  // Alarm — over/under temperature beyond tolerance
  publishOneEntity(doc, "binary_sensor", devId, fermBase, fermLabel,
    "alarm", "Alarm", "alarm", "problem", nullptr, "mdi:alarm-light", nullptr, nullptr);

  // Text fields — always text entities; device ignores commands when allowControl is off
  publishTextEntity(doc, devId, fermBase, fermLabel,
    "name",      "Name",      "name",      "name/set",      31, "mdi:label");
  publishTextEntity(doc, devId, fermBase, fermLabel,
    "beer_name", "Beer Name", "beer_name", "beer_name/set", 31, "mdi:beer");
  publishTextEntity(doc, devId, fermBase, fermLabel,
    "yeast",     "Yeast",     "yeast",     "yeast/set",     31, "mdi:flask");

  // ON/OFF state — always switches; device ignores commands when allowControl is off
  // and the 60s state publish corrects any HA UI changes within one interval
  publishSwitchEntity(doc, devId, fermBase, fermLabel,
    "power",           "Power",           "power",           "power/set",           "mdi:power");
  publishSwitchEntity(doc, devId, fermBase, fermLabel,
    "temp_control",    "Temp Control",    "temp_control",    "temp_control/set",    "mdi:thermostat");
  publishSwitchEntity(doc, devId, fermBase, fermLabel,
    "profile_running", "Profile Running", "profile_running", "profile_running/set", "mdi:play-circle");

  // Profile select + step progress
  static const char* profileOpts[] = {"0","1","2","3","4"};
  publishSelectEntity(doc, devId, fermBase, fermLabel,
    "profile_no", "Profile No", "profile_no", "profile_no/set",
    profileOpts, MAX_PROFILES + 1, "mdi:playlist-play");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "profile_step",  "Profile Step",  "profile_step",  nullptr, "#", "mdi:counter", nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "profile_steps", "Profile Steps", "profile_steps", nullptr, "#", "mdi:counter", nullptr, "measurement");

  logMsg("[MQTT] HA discovery published for F%d: base=%s", i, fermBase);
}

// Publish HA discovery for one Probe slot. Skips empty / unconfigured slots.
static void publishProbeDiscovery(int idx) {
  if (!g_mqtt.connected()) return;
  if (strlen(g_probes[idx].address) == 0) return;

  const char* tempUnit = "\xC2\xB0""C";
  char devId[48], base[96], devName[48];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X_probe_%s",
    ESP.getChipId(), g_probes[idx].address);
  snprintf(base,    sizeof(base),    "%s/Probe/%s",
    g_mqttConfig.baseTopic, g_probes[idx].address);
  snprintf(devName, sizeof(devName), "OurBrewbot Probe %s",
    strlen(g_probes[idx].probeName) > 0 ? g_probes[idx].probeName : g_probes[idx].address);

  JsonDocument doc;
  publishOneEntity(doc, "binary_sensor", devId, base, devName,
    "active", "Active", "active",
    "connectivity", nullptr, nullptr, "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, base, devName,
    "temperature", "Temperature", "temperature",
    "temperature", tempUnit, nullptr, nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "name", "Name", "name",
    nullptr, nullptr, "mdi:label", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, base, devName,
    "function", "Function", "function",
    nullptr, nullptr, "mdi:function-variant", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, base, devName,
    "fermenter", "Fermenter", "fermenter",
    nullptr, nullptr, "mdi:tank", "diagnostic", nullptr);

  logMsg("[MQTT] HA discovery published for probe %s", g_probes[idx].address);
}

// Publish HA discovery for one Tilt colour slot. Skips unconfigured colours.
static void publishTiltDiscovery(int colour) {
  if (!g_mqtt.connected()) return;
  if (g_tilts[colour].colour == PROBE_UNASSIGNED) return;

  const char* tempUnit = "\xC2\xB0""C";
  const char* colourName = getTiltColourName(colour);
  char devId[48], base[96], devName[48];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X_tilt_%s",
    ESP.getChipId(), colourName);
  snprintf(base,    sizeof(base),    "%s/Tilt/%s",
    g_mqttConfig.baseTopic, colourName);
  snprintf(devName, sizeof(devName), "OurBrewbot Tilt %s", colourName);

  JsonDocument doc;
  publishOneEntity(doc, "binary_sensor", devId, base, devName,
    "active", "Active", "active",
    "connectivity", nullptr, nullptr, "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, base, devName,
    "temperature", "Temperature", "temperature",
    "temperature", tempUnit, nullptr, nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "gravity", "Gravity", "gravity",
    nullptr, "SG", "mdi:test-tube", nullptr, "measurement");
  publishOneEntity(doc, "binary_sensor", devId, base, devName,
    "is_pro", "Tilt Pro", "is_pro",
    nullptr, nullptr, "mdi:bluetooth", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, base, devName,
    "fermenter", "Fermenter", "fermenter",
    nullptr, nullptr, "mdi:tank", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, base, devName,
    "function", "Temperature Reading", "function",
    nullptr, nullptr, "mdi:thermometer", "diagnostic", nullptr);

  logMsg("[MQTT] HA discovery published for tilt %s", colourName);
}

// Publish HA discovery for one iSpindel slot. Skips empty / "None" slots.
static void publishIspindelDiscovery(int idx) {
  if (!g_mqtt.connected()) return;
  if (strlen(g_iSpindels[idx].id) == 0) return;
  if (strcmp(g_iSpindels[idx].name, "None") == 0) return;

  const char* tempUnit = "\xC2\xB0""C";
  char devId[48], base[96], devName[48];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X_ispindel_%s",
    ESP.getChipId(), g_iSpindels[idx].id);
  snprintf(base,    sizeof(base),    "%s/iSpindel/%s",
    g_mqttConfig.baseTopic, g_iSpindels[idx].id);
  snprintf(devName, sizeof(devName), "OurBrewbot iSpindel %s",
    strlen(g_iSpindels[idx].name) > 0 ? g_iSpindels[idx].name : g_iSpindels[idx].id);

  JsonDocument doc;
  publishOneEntity(doc, "sensor", devId, base, devName,
    "temperature", "Temperature", "temperature",
    "temperature", tempUnit, nullptr, nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "gravity", "Gravity", "gravity",
    nullptr, "SG", "mdi:test-tube", nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "corrected_gravity", "Corrected Gravity", "corrected_gravity",
    nullptr, "SG", "mdi:test-tube", nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "battery", "Battery", "battery",
    "voltage", "V", nullptr, "diagnostic", "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "rssi", "RSSI", "rssi",
    "signal_strength", "dBm", nullptr, "diagnostic", "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "angle", "Angle", "angle",
    nullptr, "\xC2\xB0", "mdi:angle-acute", "diagnostic", "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "velocity", "Velocity", "velocity",
    nullptr, nullptr, "mdi:speedometer", "diagnostic", "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "run_time", "Run Time", "run_time",
    "duration", "s", "mdi:timer-outline", "diagnostic", "measurement");
  publishOneEntity(doc, "sensor", devId, base, devName,
    "name", "Name", "name",
    nullptr, nullptr, "mdi:label", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, base, devName,
    "fermenter", "Fermenter", "fermenter",
    nullptr, nullptr, "mdi:tank", "diagnostic", nullptr);
  publishOneEntity(doc, "sensor", devId, base, devName,
    "function", "Temperature Reading", "function",
    nullptr, nullptr, "mdi:thermometer", "diagnostic", nullptr);

  logMsg("[MQTT] HA discovery published for ispindel %s", g_iSpindels[idx].id);
}

static void removeOneEntity(const char* component, const char* devId, const char* objectId);

// Remove HA discovery for one Probe by its OneWire address.
static void removeProbeDiscovery(const char* address) {
  if (!g_mqtt.connected()) return;
  if (strlen(address) == 0) return;
  char devId[48];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X_probe_%s", ESP.getChipId(), address);
  removeOneEntity("binary_sensor", devId, "active");
  removeOneEntity("sensor", devId, "temperature");
  removeOneEntity("sensor", devId, "name");
  removeOneEntity("sensor", devId, "function");
  removeOneEntity("sensor", devId, "fermenter");
}

// Remove HA discovery for one Tilt colour.
static void removeTiltDiscovery(int colour) {
  if (!g_mqtt.connected()) return;
  const char* colourName = getTiltColourName(colour);
  char devId[48];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X_tilt_%s", ESP.getChipId(), colourName);
  removeOneEntity("binary_sensor", devId, "active");
  removeOneEntity("sensor", devId, "temperature");
  removeOneEntity("sensor", devId, "gravity");
  removeOneEntity("binary_sensor", devId, "is_pro");
  removeOneEntity("sensor", devId, "fermenter");
  removeOneEntity("sensor", devId, "function");
}

// Remove HA discovery for one iSpindel by its hex id.
static void removeIspindelDiscovery(const char* id) {
  if (!g_mqtt.connected()) return;
  if (strlen(id) == 0) return;
  char devId[48];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X_ispindel_%s", ESP.getChipId(), id);
  removeOneEntity("sensor", devId, "temperature");
  removeOneEntity("sensor", devId, "gravity");
  removeOneEntity("sensor", devId, "corrected_gravity");
  removeOneEntity("sensor", devId, "battery");
  removeOneEntity("sensor", devId, "rssi");
  removeOneEntity("sensor", devId, "angle");
  removeOneEntity("sensor", devId, "velocity");
  removeOneEntity("sensor", devId, "run_time");
  removeOneEntity("sensor", devId, "name");
  removeOneEntity("sensor", devId, "fermenter");
  removeOneEntity("sensor", devId, "function");
}

// Remove one HA entity by publishing an empty retained payload to its discovery topic.
static void removeOneEntity(const char* component, const char* devId, const char* objectId) {
  char discTopic[128];
  snprintf(discTopic, sizeof(discTopic), "homeassistant/%s/%s/%s/config",
    component, devId, objectId);
  g_mqtt.publish(discTopic, (const uint8_t*)"", 0, true);
  yield();
}

// Remove all HA discovery entities for one fermenter.
// Includes old abbreviated IDs from v0.1.20 to ensure full cleanup on upgrade.
static void removeHaDiscovery(int i) {
  if (!g_mqtt.connected()) return;
  char devId[32];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X_f%d", ESP.getChipId(), i);

  // v0.1.20 abbreviated IDs (wrong) — remove so HA doesn't keep stale entities
  removeOneEntity("sensor", devId, "beer_temp");
  removeOneEntity("sensor", devId, "ambient_temp");
  removeOneEntity("sensor", devId, "ceil_temp");
  removeOneEntity("sensor", devId, "floor_temp");

  // Current full-name sensor IDs
  removeOneEntity("sensor", devId, "beer_temperature");
  removeOneEntity("sensor", devId, "beer_temperature_source");
  removeOneEntity("sensor", devId, "ambient_temperature");
  removeOneEntity("sensor", devId, "ceiling_temperature");
  removeOneEntity("sensor", devId, "floor_temperature");
  removeOneEntity("sensor", devId, "temperature_unit");
  removeOneEntity("sensor", devId, "hysteresis");
  removeOneEntity("sensor", devId, "gravity");
  removeOneEntity("sensor", devId, "gravity_source");
  removeOneEntity("sensor", devId, "og");
  removeOneEntity("sensor", devId, "tg");
  removeOneEntity("sensor", devId, "attenuation");
  removeOneEntity("sensor", devId, "status");
  removeOneEntity("sensor", devId, "name");
  removeOneEntity("sensor", devId, "beer_name");
  removeOneEntity("sensor", devId, "yeast");
  removeOneEntity("sensor", devId, "compressor_delay");
  removeOneEntity("sensor", devId, "rssi");
  removeOneEntity("sensor", devId, "free_heap");

  // Binary sensor versions from v0.1.22 — changed to sensor in v0.1.23, clean up old topics
  removeOneEntity("binary_sensor", devId, "power");
  removeOneEntity("binary_sensor", devId, "temp_control");
  removeOneEntity("binary_sensor", devId, "profile_running");

  // Alarm binary_sensor (added v0.1.84)
  removeOneEntity("binary_sensor", devId, "alarm");

  // Current sensor versions of ON/OFF entities
  removeOneEntity("sensor", devId, "power");
  removeOneEntity("sensor", devId, "temp_control");
  removeOneEntity("sensor", devId, "profile_running");
  removeOneEntity("sensor", devId, "profile_step");
  removeOneEntity("sensor", devId, "profile_steps");

  // Switch versions (present from Patch 3+)
  removeOneEntity("switch", devId, "power");
  removeOneEntity("switch", devId, "temp_control");
  removeOneEntity("switch", devId, "profile_running");

  // Number versions (present from Patch 4+)
  removeOneEntity("number", devId, "ceiling_temperature");
  removeOneEntity("number", devId, "floor_temperature");
  removeOneEntity("number", devId, "hysteresis");
  removeOneEntity("number", devId, "compressor_delay");
  removeOneEntity("number", devId, "og");
  removeOneEntity("number", devId, "tg");

  // Text versions (present from Patch 5+)
  removeOneEntity("text",   devId, "name");
  removeOneEntity("text",   devId, "beer_name");
  removeOneEntity("text",   devId, "yeast");

  // Select versions (present from Patch 5+)
  removeOneEntity("select", devId, "profile_no");

  logMsg("[MQTT] HA discovery removed for F%d", i);
}

// Remove all HA discovery entities for the device itself.
static void removeDeviceDiscovery() {
  if (!g_mqtt.connected()) return;
  char devId[24];
  snprintf(devId, sizeof(devId), "ourbrewbot_%06X", ESP.getChipId());
  removeOneEntity("sensor", devId, "firmware_version");
  removeOneEntity("sensor", devId, "ip_address");
  removeOneEntity("sensor", devId, "mdns_name");
  removeOneEntity("sensor", devId, "wifi_ssid");
  removeOneEntity("sensor", devId, "rssi");
  removeOneEntity("sensor", devId, "free_heap");
  removeOneEntity("sensor", devId, "uptime");
  removeOneEntity("sensor", devId, "chip_id");
  removeOneEntity("sensor", devId, "reboot_reason");
  removeOneEntity("sensor", devId, "reboot_code");
  removeOneEntity("button", devId, "reboot");
  removeOneEntity("button", devId, "all_off");
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
  size_t baseLen = strlen(g_mqttConfig.baseTopic);
  if (strncmp(topic, g_mqttConfig.baseTopic, baseLen) != 0 || topic[baseLen] != '/') return;
  const char* rest   = topic + baseLen + 1;
  const char* slash1 = strchr(rest, '/');
  if (!slash1) return;
  char scope[16];
  size_t scopeLen = slash1 - rest;
  if (scopeLen == 0 || scopeLen >= sizeof(scope)) return;
  memcpy(scope, rest, scopeLen);
  scope[scopeLen] = '\0';

  const char* keyStart = slash1 + 1;
  const char* slash2   = strchr(keyStart, '/');
  if (!slash2 || strcmp(slash2, "/set") != 0) return;
  char key[32];
  size_t keyLen = slash2 - keyStart;
  if (keyLen == 0 || keyLen >= sizeof(key)) return;
  memcpy(key, keyStart, keyLen);
  key[keyLen] = '\0';

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
  if (strncmp(scope, "Fermenter", 9) != 0) return;
  int idx = atoi(scope + 9);
  if (idx < 0 || idx >= MAX_FERMENTERS) return;

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
    if (!g_mqttWasConnected) {
      logMsg("[MQTT] Connected to %s:%d", g_mqttConfig.host, g_mqttConfig.port);
    }
    g_mqttWasConnected = true;
    g_mqttBackoffMs = 5000;  // reset backoff on success

    // Mark device online
    g_mqtt.publish(s_availTopic, "online", true);

    // Subscribe to HA birth message to re-publish discovery after HA restarts
    g_mqtt.subscribe("homeassistant/status");

    // Subscribe/unsubscribe command wildcard based on current allowControl state
    mqttApplyControlSubscription();

    // Publish discovery configs for all MQTT-enabled fermenters
    if (g_mqttConfig.haDiscovery) {
      publishAllHaDiscovery();
    }
  } else {
    logMsg("[MQTT] Connection failed, rc=%d (retry in %lus)",
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
    logMsg("[MQTT] Discover: not connected — attempting connect");
    if (!mqttConnect()) {
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
// INIT / LOOP
// ============================================================

void initMqtt() {
  if (!g_mqttConfig.enabled) return;
  g_mqtt.setServer(g_mqttConfig.host, g_mqttConfig.port);
  g_mqtt.setCallback(mqttMessageCallback);
  g_mqtt.setBufferSize(1024);  // writable-entity discovery payloads can reach ~900 bytes; measure in testing
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
    publishFloat(base, "temperature", g_probes[i].temperature);
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
    publishFloat(base, "temperature", g_tilts[i].temperature);
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
    publishFloat(base, "temperature",       g_iSpindels[i].temperature);
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

  if (!g_mqtt.connected()) {
    if (!mqttConnect()) return;
  }

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
    const char* unit  = "C";

    // Temperatures
    if (beerTemp > -100.0f)
      publishFloat(base, "beer_temperature", beerTemp);
    publishValue(base, "beer_temperature_source", getBeerTempSource(i));
    if (ambientTemp > -100.0f)
      publishFloat(base, "ambient_temperature", ambientTemp);
    publishFloat(base, "ceiling_temperature", g_fermenters[i].ceilingTemp);
    publishFloat(base, "floor_temperature", g_fermenters[i].floorTemp);
    publishValue(base, "temperature_unit", unit);
    publishFloat(base, "hysteresis", g_fermenters[i].hysteresis);

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

  g_mqtt.setServer(g_mqttConfig.host, g_mqttConfig.port);

  char clientId[24];
  snprintf(clientId, sizeof(clientId), "ourbrewbot-%06X", ESP.getChipId());

  logMsg("[MQTT] Test connecting to %s:%d user=%s",
    g_mqttConfig.host, g_mqttConfig.port, g_mqttConfig.username);

  bool ok;
  if (strlen(g_mqttConfig.username) > 0) {
    ok = g_mqtt.connect(clientId, g_mqttConfig.username, g_mqttConfig.password);
  } else {
    ok = g_mqtt.connect(clientId);
  }

  if (!ok) {
    logMsg("[MQTT] Test failed, rc=%d", g_mqtt.state());
    return false;
  }

  char topic[48];
  snprintf(topic, sizeof(topic), "%s/test", g_mqttConfig.baseTopic);
  ok = g_mqtt.publish(topic, "OurBrewbot MQTT test OK");
  logMsg("[MQTT] Test publish to %s: %s", topic, ok ? "OK" : "FAIL");

  return ok;
}
