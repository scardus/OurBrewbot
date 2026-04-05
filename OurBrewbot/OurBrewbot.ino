/*
 * OurBrewbot — ESP8266 fermentation controller firmware
 * Targets: NodeMCU / Wemos D1 Mini (ESP8266)
 *
 * Libraries required (install via Arduino Library Manager):
 *  - ESP8266WiFi          (bundled with ESP8266 Arduino core)
 *  - ESP8266WebServer     (bundled)
 *  - ESP8266HTTPClient    (bundled)
 *  - ESP8266httpUpdate    (bundled)
 *  - LittleFS             (bundled)
 *  - ArduinoJson          v6.x (search "ArduinoJson" by Benoit Blanchon)
 *  - DallasTemperature    (search "DallasTemperature" by Miles Burton)
 *  - OneWire              (search "OneWire" by Jim Studt)
 *  - WiFiManager          (search "WiFiManager" by tzapu)
 *  - SoftwareSerial       (bundled with ESP8266 Arduino core)
 *  - rc-switch            v2.6.4 (search "rc-switch" by sui77)
 *  - PubSubClient         v2.8 (search "PubSubClient" by Nick O'Leary)
 */

// ============================================================
// INCLUDES
// ============================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <WiFiManager.h>

// Local module headers — these pull in all other dependencies
#include "Pins.h"
#include "Version.h"
#include "Config.h"
#include "Log.h"
#include "Fermenter.h"
#include "Profile.h"
#include "Temperatures.h"
#include "SmartPlugs.h"
#include "Tilt.h"
#include "Reports.h"
#include "Mqtt.h"
#include "WebAPI.h"

// ============================================================
// TIMING CONSTANTS (milliseconds)
// ============================================================
#define INTERVAL_TEMPS_MS       5000    // Temperature polling
#define INTERVAL_FERMENTER_MS   10000   // Fermenter control loop
#define INTERVAL_TILT_MS        30000   // Tilt BLE scan interval
#define INTERVAL_CLOUD_MS       900000  // Cloud/service reporting (15 min)
#define INTERVAL_MQTT_MS        60000   // MQTT publish interval (60s)
#define INTERVAL_TEN_MIN_MS     600000  // 10-minute periodic tasks
#define INTERVAL_UPTIME_MS      60000   // Uptime counter increment
#define INTERVAL_PROBE_SCAN_MS  30000   // Probe hot-plug scanning

// ============================================================
// CONTROLLER STATE MACHINE
// ============================================================
enum ControllerState {
  WAIT_CONFIG,        // Unconfigured, hosting AP portal
  CONFIGURING,        // Saving WiFi config
  CONNECTING_NET,     // Attempting WiFi connection
  RUNNING,            // Normal operation
  OTA_UPGRADE,        // Performing OTA firmware update
  RESET_CONFIG,       // Resetting all config to defaults
  ERROR               // Error state
};

// ============================================================
// GLOBAL STATE
// ============================================================
ControllerState g_state = WAIT_CONFIG;
unsigned long g_lastTempTime    = 0;
unsigned long g_lastFermTime    = 0;
unsigned long g_lastCloudTime   = 0;
unsigned long g_lastMqttTime    = 0;
unsigned long g_lastTenMinTime  = 0;
unsigned long g_lastUptimeTime  = 0;
unsigned long g_lastProbeScanTime = 0;
unsigned long g_lastTiltTime    = 0;
bool g_wifiConnected = false;
String g_rebootReason = ESP.getResetReason();

// Hardware objects
ESP8266WebServer g_webServer(80);

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.print("\r\n");
  logMsg("=================================");
  logMsg("OurBrewbot Firmware %s", FW_VERSION);
  logMsg("=================================");

  // Log reboot reason with full rst_info detail
  {
    struct rst_info *ri = ESP.getResetInfoPtr();
    logMsg("[SYS] Reset reason: %s (code %u)", ESP.getResetReason().c_str(), ri->reason);
    if (ri->reason == REASON_EXCEPTION_RST) {
      logMsg("[SYS] Exception cause: %u, EPC1: 0x%08x, EXCVADDR: 0x%08x",
        ri->exccause, ri->epc1, ri->excvaddr);
    }
  }

  // LED setup
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH); // active low

  // RF transmitter setup via RCSwitch
  g_rcSwitch.enableTransmit(PIN_RF_TRANSMIT);

  // Mount filesystem
  if (!LittleFS.begin()) {
    logMsg("[FS] LittleFS mount failed - formatting");
    LittleFS.format();
    LittleFS.begin();
  }
  logMsg("[FS] LittleFS mounted OK");

  // Load all config from flash
  loadAllConfig();

  // Reset uptime counter — this tracks minutes since boot, not cumulative
  g_globalConfig.lastUptime = 0;

  // Start temperature sensors and register probes
  g_sensors1.begin();
  g_sensors1.setResolution(g_globalConfig.resolution);
#ifdef ENABLE_BUS2
  g_sensors2.begin();
  g_sensors2.setResolution(g_globalConfig.resolution);
  logMsg("[TEMP] Bus1: %d probes, Bus2: %d probes",
    g_sensors1.getDeviceCount(), g_sensors2.getDeviceCount());
#else
  logMsg("[TEMP] Bus1: %d probes, Bus2: disabled",
    g_sensors1.getDeviceCount());
#endif
  scanBuses();

  // Clean up any duplicate probes from old truncated addresses
  cleanupDuplicateProbes();
  saveProbeConfig();

  // BLE module setup (KeyeStudio BT 4.0 v2 / HM-10)
  initBLE();

  // WiFi setup via WiFiManager
  setupWiFi();

  // Start web server
  setupWebServer(g_webServer);
  g_webServer.begin();
  logMsg("[WEB] Server started on port 80");

  // Record startup
  recordReboot(g_rebootReason);

  // MQTT client setup
  initMqtt();

  g_state = RUNNING;
  logMsg("[INIT] Setup complete - entering RUNNING state");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  g_webServer.handleClient();
  checkBLESniffTimeout();
  MDNS.update();
  mqttLoop();

  unsigned long now = millis();

  // LED: steady ON when WiFi connected, flashing when disconnected (active low)
  if (WiFi.isConnected()) {
    digitalWrite(PIN_LED, LOW);
  } else {
    digitalWrite(PIN_LED, (now / 500) & 1);  // toggle every 500ms
  }

  switch (g_state) {
    case RUNNING:
      // Temperature processing
      if (now - g_lastTempTime >= INTERVAL_TEMPS_MS) {
        g_lastTempTime = now;
        pollTemperatures();
        allocateProbeTemperatures();

        // Print probe temperatures to serial console
        for (int i = 0; i < MAX_PROBES; i++) {
          if (strlen(g_probes[i].address) == 0) continue;
          logMsg("[TEMP] %s (%s): %.1fC (raw %.1f, adj %.1f)",
            g_probes[i].probeName, g_probes[i].address,
            g_probes[i].temperature, g_probes[i].rawTemperature,
            g_probes[i].tempAdjust);
        }
      }

      // Periodic probe scanning (hot-plug support)
      if (now - g_lastProbeScanTime >= INTERVAL_PROBE_SCAN_MS) {
        g_lastProbeScanTime = now;
        periodicProbeScan();
      }

      // Tilt BLE scanning
      if (now - g_lastTiltTime >= INTERVAL_TILT_MS) {
        g_lastTiltTime = now;
        checkTilt();
      }

      // Fermenter control loop
      if (now - g_lastFermTime >= INTERVAL_FERMENTER_MS) {
        g_lastFermTime = now;
        // LiveTest mode: advance 1 hour per 10-second tick
        for (int i = 0; i < MAX_FERMENTERS; i++) {
          if (g_fermenters[i].liveTest && g_fermenters[i].profileRunning && g_fermenters[i].power) {
            g_fermenters[i].currentHour++;
          }
        }
        processFermenters();
      }

      // Cloud / service reporting (HTTP services only)
      if (now - g_lastCloudTime >= INTERVAL_CLOUD_MS) {
        g_lastCloudTime = now;
        sendReports();
      }

      // MQTT publish (separate timer, more frequent)
      if (now - g_lastMqttTime >= INTERVAL_MQTT_MS) {
        g_lastMqttTime = now;
        reportMqtt();
      }

      // Ten-minute periodic tasks
      if (now - g_lastTenMinTime >= INTERVAL_TEN_MIN_MS) {
        g_lastTenMinTime = now;
        onTenMinuteTimer();
      }

      // Uptime counter (incremented every minute, saved to global config)
      if (now - g_lastUptimeTime >= INTERVAL_UPTIME_MS) {
        g_lastUptimeTime = now;
        g_globalConfig.lastUptime++;
        saveGlobalConfig();
      }
      break;

    case RESET_CONFIG:
      logMsg("[SYS] Resetting all config...");
      resetAllConfig();
      g_state = WAIT_CONFIG;
      ESP.restart();
      break;

    case OTA_UPGRADE:
      // Handled by web server POST /update handler
      break;

    case ERROR:
      // Blink LED to indicate error
      digitalWrite(PIN_LED, (millis() / 500) % 2);
      break;

    default:
      break;
  }
}

// ============================================================
// WIFI SETUP
// ============================================================
void setupWiFi() {
  WiFiManager wifiManager;
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(180);

  // Use chip ID in AP name to make it unique per device
  String apName = "OurBrewbot-" + String(ESP.getChipId(), HEX);
  apName.toUpperCase();

  logMsg("[WIFI] Connecting... AP name if needed: %s", apName.c_str());

  if (!wifiManager.autoConnect(apName.c_str())) {
    logMsg("[WIFI] Failed to connect - restarting");
    delay(3000);
    ESP.restart();
  }

  g_wifiConnected = true;
  logMsg("[WIFI] Connected. IP: %s", WiFi.localIP().toString().c_str());

  // mDNS: register as ourbrewbot-CHIPID.local
  String mdnsName = "ourbrewbot-" + String(ESP.getChipId(), HEX);
  mdnsName.toLowerCase();
  if (MDNS.begin(mdnsName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    logMsg("[MDNS] Registered as %s.local", mdnsName.c_str());
  } else {
    logMsg("[MDNS] Failed to start");
  }
}

// ============================================================
// TEN MINUTE TIMER TASKS
// ============================================================
void onTenMinuteTimer() {
  // OTA auto-update disabled by default — enable and set your own OTA URL if needed
  // checkForFirmwareUpdate();

  // Health report to serial
  logMsg("[HEALTH] Free heap: %u bytes, Uptime: %lu min",
    ESP.getFreeHeap(), g_globalConfig.lastUptime);

  // Increment currentHour for active profile steps (6 calls × 10 min = 1 hour)
  static uint8_t s_hourTick[MAX_FERMENTERS] = {0};
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (g_fermenters[i].profileRunning && g_fermenters[i].power && !g_fermenters[i].liveTest) {
      s_hourTick[i]++;
      if (s_hourTick[i] >= 6) {
        s_hourTick[i] = 0;
        g_fermenters[i].currentHour++;
      }
    } else {
      s_hourTick[i] = 0;
    }
  }

  // Save all config as periodic backup
  saveAllConfig();
}
