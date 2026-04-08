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
#include "Version.h"
#include "Log.h"
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

extern String g_rebootReason;  // captured at boot in OurBrewbot.ino

static WiFiClient   g_mqttWifi;
static PubSubClient g_mqtt(g_mqttWifi);
static bool         g_mqttWasConnected = false;
static unsigned long g_mqttLastAttempt  = 0;
static unsigned long g_mqttBackoffMs    = 5000;   // start at 5s, doubles on failure
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

// Build and publish a single HA entity discovery config.
// Reuses the caller's JsonDocument (caller must call doc.clear() between calls).
static void publishOneEntity(
    DynamicJsonDocument& doc,
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
  char uid[56], stTopic[96], discTopic[128];
  snprintf(uid,       sizeof(uid),       "%s_%s",  devId, objectId);
  snprintf(stTopic,   sizeof(stTopic),   "%s/%s",  fermBase, stKey);
  snprintf(discTopic, sizeof(discTopic), "homeassistant/%s/%s/%s/config",
    component, devId, objectId);

  doc["uniq_id"] = uid;
  doc["name"]    = name;
  doc["stat_t"]  = stTopic;
  if (devClass   && devClass[0])   doc["dev_cla"]      = devClass;
  if (unit       && unit[0])       doc["unit_of_meas"] = unit;
  if (icon       && icon[0])       doc["ic"]           = icon;
  if (entityCat  && entityCat[0])  doc["ent_cat"]      = entityCat;
  if (stateClass && stateClass[0]) doc["stat_cla"]     = stateClass;
  doc["avty_t"]  = s_availTopic;
  doc["exp_aft"] = 180;   // entity goes unavailable after 3× the 60 s publish interval

  // ArduinoJson 7 API for nested objects
  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray  ids = dev["ids"].to<JsonArray>();
  ids.add(devId);
  dev["name"] = fermName;
  dev["mf"]   = "OurBrewbot";
  dev["mdl"]  = "ESP8266";
  dev["sw"]   = FW_VERSION;

  String payload;
  serializeJson(doc, payload);
  g_mqtt.publish(discTopic, payload.c_str(), true);
  doc.clear();
  yield();  // feed WDT between successive publishes
}

// Publish HA discovery entity configs for the device itself (not per-fermenter).
// Uses device ID ourbrewbot_{CHIPID} and base topic {baseTopic}/Device.
static void publishDeviceDiscovery() {
  if (!g_mqtt.connected()) return;

  char devId[24], devBase[64];
  snprintf(devId,   sizeof(devId),   "ourbrewbot_%06X",    ESP.getChipId());
  snprintf(devBase, sizeof(devBase), "%s/Device",           g_mqttConfig.baseTopic);

  DynamicJsonDocument doc(640);

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
  const char* tempUnit = (g_globalConfig.unit == UNIT_CELSIUS) ? "\xC2\xB0""C" : "\xC2\xB0""F";  // °C / °F

  DynamicJsonDocument doc(640);

  // Temperature sensors — numeric, use state_class measurement
  // Entity names are the plain property names; HA prefixes them with dev.name ("OurBrewbot F0")
  // when displaying and when generating entity IDs (sensor.ourbrewbot_f0_beer_temperature).
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "beer_temperature",    "Beer Temperature",    "beer_temperature",    "temperature", tempUnit, nullptr,           nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "ambient_temperature", "Ambient Temperature", "ambient_temperature", "temperature", tempUnit, nullptr,           nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "ceiling_temperature", "Ceiling Temperature", "ceiling_temperature", "temperature", tempUnit, nullptr,           nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "floor_temperature",   "Floor Temperature",   "floor_temperature",   "temperature", tempUnit, nullptr,           nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "temperature_unit",    "Temperature Unit",    "temperature_unit",    nullptr,       nullptr,  "mdi:thermometer", nullptr, nullptr);
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "hysteresis",          "Hysteresis",          "hysteresis",          nullptr,       tempUnit, nullptr,           nullptr, "measurement");

  // Gravity sensors — numeric
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "gravity",     "Gravity",     "gravity",     nullptr, "SG", "mdi:test-tube", nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "og",          "OG",          "og",          nullptr, "SG", "mdi:test-tube", nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "tg",          "TG",          "tg",          nullptr, "SG", "mdi:test-tube", nullptr, "measurement");
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "attenuation", "Attenuation", "attenuation", nullptr, "%",  "mdi:percent",   nullptr, "measurement");

  // Status and info — text, no state_class
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "status",           "Status",           "status",           nullptr, nullptr, "mdi:thermometer",   nullptr, nullptr);
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "name",             "Name",             "name",             nullptr, nullptr, "mdi:label",         nullptr, nullptr);
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "beer_name",        "Beer Name",        "beer_name",        nullptr, nullptr, "mdi:beer",          nullptr, nullptr);
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "yeast",            "Yeast",            "yeast",            nullptr, nullptr, "mdi:flask",         nullptr, nullptr);
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "compressor_delay", "Compressor Delay", "compressor_delay", nullptr, "min",   "mdi:timer-outline", nullptr, "measurement");

  // ON/OFF state sensors
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "power",           "Power",           "power",           nullptr, nullptr, "mdi:power",       nullptr, nullptr);
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "temp_control",    "Temp Control",    "temp_control",    nullptr, nullptr, "mdi:thermostat",  nullptr, nullptr);
  publishOneEntity(doc, "sensor", devId, fermBase, fermLabel,
    "profile_running", "Profile Running", "profile_running", nullptr, nullptr, "mdi:play-circle", nullptr, nullptr);

  logMsg("[MQTT] HA discovery published for F%d: base=%s", i, fermBase);
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
  removeOneEntity("sensor", devId, "ambient_temperature");
  removeOneEntity("sensor", devId, "ceiling_temperature");
  removeOneEntity("sensor", devId, "floor_temperature");
  removeOneEntity("sensor", devId, "temperature_unit");
  removeOneEntity("sensor", devId, "hysteresis");
  removeOneEntity("sensor", devId, "gravity");
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

  // Current sensor versions of ON/OFF entities
  removeOneEntity("sensor", devId, "power");
  removeOneEntity("sensor", devId, "temp_control");
  removeOneEntity("sensor", devId, "profile_running");

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
}

// Remove all HA discovery entities (called when haDiscovery is disabled).
void cleanupAllHaDiscovery() {
  if (!g_mqtt.connected()) return;
  removeDeviceDiscovery();
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    removeHaDiscovery(i);
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
  }
}

// ============================================================
// CONNECT
// ============================================================

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
  g_mqtt.setBufferSize(700);
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
  return true;
}

// ============================================================
// INIT / LOOP
// ============================================================

void initMqtt() {
  if (!g_mqttConfig.enabled) return;
  g_mqtt.setServer(g_mqttConfig.host, g_mqttConfig.port);
  g_mqtt.setCallback(mqttMessageCallback);
  g_mqtt.setBufferSize(700);  // discovery payloads are ~500 bytes; default 256 is too small
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
    const char* unit  = (g_globalConfig.unit == UNIT_CELSIUS) ? "C" : "F";

    // Temperatures
    if (beerTemp > -100.0f)
      publishFloat(base, "beer_temperature", toDisplayTemp(beerTemp));
    if (ambientTemp > -100.0f)
      publishFloat(base, "ambient_temperature", toDisplayTemp(ambientTemp));
    publishFloat(base, "ceiling_temperature", g_fermenters[i].ceilingTemp);
    publishFloat(base, "floor_temperature", g_fermenters[i].floorTemp);
    publishValue(base, "temperature_unit", unit);
    publishFloat(base, "hysteresis", g_fermenters[i].hysteresis);

    // Gravity
    if (sg > 0.0f)
      publishFloat(base, "gravity", sg / 1000.0f, 4);
    publishFloat(base, "og", g_fermenters[i].og / 1000.0f, 4);
    publishFloat(base, "tg", g_fermenters[i].tg / 1000.0f, 4);
    publishFloat(base, "attenuation", getAttenuation(i));

    // State
    publishBool(base, "power", g_fermenters[i].power);
    publishBool(base, "temp_control", g_fermenters[i].tempControl);
    uint8_t st = g_fermenters[i].status;
    const char* state = (st == STATUS_HEATING) ? "heating" :
                        (st == STATUS_COOLING) ? "cooling" :
                        (st == STATUS_ALARM)   ? "alarm"   : "idle";
    publishValue(base, "status", state);

    // Info
    publishValue(base, "beer_name", g_fermenters[i].beerName);
    publishValue(base, "yeast", g_fermenters[i].yeastName);
    publishInt(base, "compressor_delay", g_fermenters[i].compressorDelay);
    publishBool(base, "profile_running", g_fermenters[i].profileRunning);

    logMsg("[MQTT] Published F%d (%s)", i, g_fermenters[i].fermenterName);
  }

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
