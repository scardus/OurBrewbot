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
 * OurBrewbot — ESP8266 fermentation controller firmware
 * Targets: NodeMCU / Wemos D1 Mini (ESP8266)
 *
 * Built with PlatformIO. The libraries below are fetched automatically from
 * lib_deps in platformio.ini, which holds the exact versions - nothing needs
 * installing by hand.
 *  - ESP8266WiFi, ESP8266WebServer, ESP8266HTTPClient, ESP8266httpUpdate,
 *    LittleFS, SoftwareSerial   (bundled with the ESP8266 Arduino core)
 *  - ArduinoJson         (Benoit Blanchon)
 *  - DallasTemperature   (Miles Burton)
 *  - OneWire             (Paul Stoffregen)
 *  - WiFiManager         (tzapu)
 *  - rc-switch           (sui77)
 *  - PubSubClient3       (hmueller01)
 *  - MicroMDNS           (scardus)
 */

// ============================================================
// INCLUDES
// ============================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
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
#include "iSpindel.h"
#include "Reports.h"
#include "Mqtt.h"
#include "WebAPI.h"
#include "StackProbe.h"
#include "Crash.h"
#include "FirstFault.h"
#include "UpdateCheck.h"
#include "CrashReport.h"
#include "TcpCleanup.h"
#include "StackCheck.h"
#include "Uptime.h"
#include <MicroMDNS.h>

// ============================================================
// TIMING CONSTANTS (milliseconds)
// ============================================================
#define INTERVAL_TEMPS_MS       5000    // Temperature polling
#define INTERVAL_FERMENTER_MS   10000   // Fermenter control loop
#define INTERVAL_TILT_MS        5000   // Tilt BLE scan interval
#define INTERVAL_CLOUD_MS       900000  // Cloud/service reporting (15 min)
#define INTERVAL_MQTT_MS        60000   // MQTT publish interval (60s)
#define INTERVAL_TEN_MIN_MS     600000  // 10-minute periodic tasks
#define INTERVAL_UPTIME_MS      60000   // Uptime counter increment
#define INTERVAL_PROBE_SCAN_MS  30000   // Probe hot-plug scanning

// WiFi at boot and while offline (see setupWiFi() and wifiLoop())
#define WIFI_BOOT_WAIT_MS       10000   // wait this long for the saved network at boot
#define WIFI_OFFLINE_MS         60000   // offline this long -> open the setup hotspot
#define WIFI_HOTSPOT_MS         120000  // hotspot stays up this long, then reconnecting resumes
#define WIFI_HOTSPOT_MAX_MS     300000  // ...or up to this long while a phone is joined to it
#define WIFI_DROP_MS            300000  // WiFi lost after connecting: wait this long before the offline cycle

// ============================================================
// GLOBAL STATE
// ============================================================
unsigned long g_lastTempTime    = 0;
unsigned long g_lastFermTime    = 0;
unsigned long g_lastCloudTime   = 0;
unsigned long g_lastMqttTime    = 0;
unsigned long g_lastTenMinTime  = 0;
unsigned long g_lastUptimeTime  = 0;
unsigned long g_lastProbeScanTime = 0;
unsigned long g_lastTiltTime    = 0;

// Async temperature conversion state
bool          g_tempConversionPending = false;
unsigned long g_tempRequestTime       = 0;
bool g_wifiConnected = false;
String g_rebootReason = ESP.getResetReason();

bool g_fermenterDebugMode = false;
FermenterDebugOverride g_fermenterDebugOverrides[MAX_FERMENTERS];

// Hardware objects
ESP8266WebServer g_webServer(80);

// Functions
void setupWiFi();
void wifiLoop();
void onTenMinuteTimer();

// ============================================================
// SETUP
// ============================================================
void setup() {
  // First, so even a crash during start-up has its first exception recorded
  firstFaultInstall();

  Serial.begin(115200);
  delay(100);

  Serial.print("\r\n");
  logMsg("=================================");
  logMsg("OurBrewbot Firmware %s - Built: %s", FW_VERSION, FW_BUILD_DATE);
  logMsg("=================================");

  // Log reboot reason with full rst_info detail
  {
    struct rst_info *ri = ESP.getResetInfoPtr();
    logMsgL(SYSLOG_NOTICE, "[SYS] Reset reason: %s (code %u)", ESP.getResetReason().c_str(), ri->reason);
    if (ri->reason == REASON_EXCEPTION_RST) {
      logMsgL(SYSLOG_ERR, "[SYS] Exception cause: %u, EPC1: 0x%08x, EXCVADDR: 0x%08x",
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

  // Record startup (must be after loadAllConfig so lastUptime is available)
  recordReboot(g_rebootReason);

  // Reset uptime counter — this tracks minutes since boot, not cumulative
  g_globalConfig.lastUptime = 0;

  // Start temperature sensors and register probes
  g_sensors1.begin();
  g_sensors1.setResolution(g_globalConfig.resolution);

  g_sensors2.begin();
  g_sensors2.setResolution(g_globalConfig.resolution);

  logMsg("[TEMP] Bus1: %d probes, Bus2: %d probes",
    g_sensors1.getDeviceCount(), g_sensors2.getDeviceCount());

  scanBuses();

  // Clean up any duplicate probes from old truncated addresses
  cleanupDuplicateProbes();

  // Read every probe once now, so the first control pass uses live readings
  readTemperaturesNow();
  saveProbeConfig();

  // BLE module setup (KeyeStudio BT 4.0 v2 / HM-10)
  initBLE();

  // WiFi setup via WiFiManager
  setupWiFi();

  // Start web server
  setupWebServer(g_webServer);
  g_webServer.begin();
  logMsg("[WEB] Server started on port 80");

  // Re-send boot messages via syslog now that WiFi (and syslog host) is available.
  // Severity here mirrors the original pre-WiFi logMsgL calls so syslog
  // filtering treats them consistently. Skipped if WiFi was down at boot - there
  // is no syslog to send them to.
  if (g_syslogConfig.enabled && g_wifiConnected) {
    struct rst_info *ri = ESP.getResetInfoPtr();
    logMsg("DEFERRED [SYS] OurBrewbot Firmware %s - Built: %s", FW_VERSION, FW_BUILD_DATE);
    logMsgL(SYSLOG_NOTICE, "DEFERRED [SYS] Reset reason: %s (code %u)", ESP.getResetReason().c_str(), ri->reason);
    if (ri->reason == REASON_EXCEPTION_RST) {
      logMsgL(SYSLOG_ERR, "DEFERRED [SYS] Exception cause: %u, EPC1: 0x%08x, EXCVADDR: 0x%08x",
        ri->exccause, ri->epc1, ri->excvaddr);
    }
    crashLogPendingDeferred();
    logMsgL(SYSLOG_NOTICE, "DEFERRED [WIFI] Connected. IP: %s", WiFi.localIP().toString().c_str());
  } else {
    // No syslog, but still read (and clear) any crash record so the crash
    // report (CrashReport.cpp) can send it
    crashLogPendingDeferred();
  }

  // MQTT client setup
  initMqtt();

  logMsg("[INIT] Setup complete");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  uptimeTick();   // first, every pass - keeps uptimeSeconds() counting past the millis() wrap
  unsigned long now = millis();

  // Handle web server requests, BLE sniff mode timeout, mDNS, and MQTT client loop.
  // checkpoint() drops a one-byte breadcrumb to RTC so a hardware-watchdog reset
  // (which bypasses custom_crash_callback) still tells us which subsystem hung.
  checkpoint(CP_WEB);          g_webServer.handleClient();
                               wifiLoop();   // only does anything while WiFi is down
  tcpClearTimeWait();   // must run every pass - see TcpCleanup.cpp
  STACK_PROBE_IRQ_SAMPLE();   // diagnostic build only - see StackProbe.cpp
  checkpoint(CP_BLE);          checkBLESniffTimeout();
                               checkRFSniffTimeout();
                               firstFaultLoop();   // logs a CPU exception the device survived
  checkpoint(CP_TILT);         serviceTilt();   // drain any in-flight BLE scan every pass
  // mDNS runs entirely from here now (see MicroMDNS.h). The heap gate and
  // ESP.wdtFeed() that used to wrap this call were both dead weight: the old
  // library did its parsing in the WiFi stack's receive callback, not in the
  // call we were guarding, and ESP.wdtFeed() only feeds the *software*
  // watchdog. mdnsLoop() allocates nothing and services at most a few packets
  // per pass, so it needs neither.
  checkpoint(CP_MDNS);
  if (g_globalConfig.mdnsEnabled) {
    mdnsLoop();
  }
  checkpoint(CP_MQTT);         mqttLoop();
  checkpoint(CP_MQTT_PEND);    mqttPendingSaveCheck();

  // LED: double-blink every 5s when WiFi connected, fast-flash when disconnected (active low)
  if (WiFi.isConnected()) {
    uint32_t phase = now % 5000;
    bool ledOn = (phase >= 100 && phase < 200) || (phase >= 300);
    digitalWrite(PIN_LED, ledOn ? LOW : HIGH);
  } else {
    digitalWrite(PIN_LED, (now / 500) & 1);  // toggle every 500ms
  }

  // Temperature processing — async two-phase to avoid blocking the loop.
  // Phase 1: fire conversion request and return immediately.
  if (now - g_lastTempTime >= INTERVAL_TEMPS_MS) {
    g_lastTempTime = now;
    checkpoint(CP_TEMP_REQ);
    requestTempConversion();
    g_tempConversionPending = true;
    g_tempRequestTime = now;
  }

  // Phase 2: read results once the conversion time has elapsed.
  // Resolution 9=94ms, 10=188ms, 11=375ms, 12=750ms
  if (g_tempConversionPending &&
      now - g_tempRequestTime >= (94u << (g_globalConfig.resolution - 9))) {
    g_tempConversionPending = false;
    checkpoint(CP_TEMP_READ);
    readTempResults();

    // Print probe temperatures to serial console
    for (int i = 0; i < MAX_PROBES; i++) {
      if (strlen(g_probes[i].address) == 0) continue;
      logMsg("[TEMP] %s (%s): %.1fC (raw %.1f, adj %.1f)",
        g_probes[i].probeName, g_probes[i].address,
        g_probes[i].temperature, g_probes[i].rawTemperature,
        g_probes[i].tempAdjust);
    }
  }

  // Periodic probe scanning (hot-plug support).
  // Guard: never run while a conversion is pending — scanBuses() calls begin()
  // which sends a OneWire reset that cancels the in-progress conversion.
  if (!g_tempConversionPending && now - g_lastProbeScanTime >= INTERVAL_PROBE_SCAN_MS) {
    g_lastProbeScanTime = now;
    checkpoint(CP_PROBE_SCAN);
    periodicProbeScan();
  }

  // Tilt BLE scanning — the tick only STARTS a scan; serviceTilt() at the
  // top of loop() drains it across passes (was a 4 s busy-wait here).
  if (now - g_lastTiltTime >= INTERVAL_TILT_MS) {
    g_lastTiltTime = now;
    checkpoint(CP_TILT);
    startTiltScan();
  }

  // Fermenter control loop
  if (now - g_lastFermTime >= INTERVAL_FERMENTER_MS) {
    g_lastFermTime = now;
    checkpoint(CP_FERM);
    // LiveTest mode: advance 1 hour per 10-second tick
    for (int i = 0; i < MAX_FERMENTERS; i++) {
      if (g_fermenters[i].liveTest && g_fermenters[i].profileRunning && g_fermenters[i].power) {
        profileAddHour(i);
      }
    }
    processFermenters();
    iSpindelCheckFreshness();   // logs an iSpindel going stale or coming back
  }

  // Cloud / service reporting (HTTP services only). The 15-min tick only
  // queues work; ONE report is POSTed per loop pass so the loop never stalls
  // longer than a single 5 s HTTP timeout (was: up to services × fermenters
  // × 5 s in one pass).
  if (now - g_lastCloudTime >= INTERVAL_CLOUD_MS) {
    g_lastCloudTime = now;
    queueReports();
  }
  if (reportsPending()) {
    checkpoint(CP_CLOUD);
    processReportQueue();
  }

  // MQTT publish (separate timer, more frequent)
  if (now - g_lastMqttTime >= INTERVAL_MQTT_MS) {
    g_lastMqttTime = now;
    checkpoint(CP_MQTT_PUB);
    reportMqtt();
  }

  // Ten-minute periodic tasks
  if (now - g_lastTenMinTime >= INTERVAL_TEN_MIN_MS) {
    g_lastTenMinTime = now;
    checkpoint(CP_TEN_MIN);
    onTenMinuteTimer();
  }

  // Daily firmware update check - returns straight away until it's due
  updateCheckLoop();

  // Send the crash report once after a crash - does nothing otherwise
  crashReportLoop();

  // Uptime in minutes, saved with the global config for the reboot log.
  // Taken from the uptime clock rather than counted up here, so it can't
  // drift when a loop pass runs long, and there's one uptime, not two.
  if (now - g_lastUptimeTime >= INTERVAL_UPTIME_MS) {
    g_lastUptimeTime = now;
    g_globalConfig.lastUptime = uptimeSeconds() / 60;
    // WiFi signal for the crash record (see crashNoteRssi in Crash.h)
    crashNoteRssi(WiFi.isConnected() ? WiFi.RSSI() : 0);
  }
}

// ============================================================
// WIFI SETUP
// ============================================================
// Config-portal dark theme. PROGMEM: WiFiManager stores the pointer and
// byte-reads it when serving the portal — works from flash on core 3.x.
static const char WM_PORTAL_CSS[] PROGMEM =
    "<style>"
    "body{background:#1a1a2e;color:#e0e0e0;font-family:system-ui,sans-serif}"
    "h1,h2,h3{color:#e94560}"
    "input[type=text],input[type=password]{"
      "background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:4px;padding:8px}"
    "input[type=submit]{"
      "background:#e94560;color:#fff;border:none;padding:10px 24px;"
      "border-radius:4px;font-weight:bold;cursor:pointer}"
    "input[type=submit]:hover{background:#c73650}"
    ".wrap{background:#16213e;border:1px solid #333;border-radius:6px}"
    "a{color:#e0e0e0}.q a{color:#fff}"
    "</style>";

// MicroMDNS hands over finished lines with no prefix; tag them so they read
// like every other subsystem in the syslog.
static void mdnsLog(const char* message) {
  logMsg("[MDNS] %s", message);
}

// See Config.h. mdnsEnd() does nothing if mDNS is disabled or never started,
// so this is safe from every restart path, including a failed WiFi setup.
void restartDevice(bool forgetWiFi) {
  mdnsEnd();

  if (forgetWiFi) {
    WiFi.persistent(true);
    WiFi.disconnect(true);
  }
  ESP.restart();
}

// The setup hotspot's name, e.g. "OurBrewbot-2924FA" (chip ID makes it unique)
static String setupHotspotName() {
  String apName = "OurBrewbot-" + String(ESP.getChipId(), HEX);
  apName.toUpperCase();
  return apName;
}

// Settings shared by the boot-time and the offline setup hotspot
static void configureWiFiManager(WiFiManager& wifiManager) {
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setTitle("OurBrewbot Setup");
  wifiManager.setCustomHeadElement(WM_PORTAL_CSS);
}

// Everything that needs a WiFi connection, run the first time WiFi connects -
// during setup() normally, or later from wifiLoop() if the device booted while
// its network was down. wifiLoop() runs it again when WiFi comes back after a
// long drop, so it must be safe to call more than once.
static void onWiFiConnected() {
  // Modem sleep's periodic DTIM beacon wake/parse cycle is the known trigger
  // for a null-pointer crash inside the closed-source SDK's beacon parser
  // (scan_parse_beacon/cnx_update_bss_more, cause 29). Disabling sleep avoids
  // that code path; cost is higher idle draw, irrelevant on mains power.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  g_wifiConnected = true;
  logMsgL(SYSLOG_NOTICE, "[WIFI] Connected. IP: %s", WiFi.localIP().toString().c_str());

  // Re-initialise log system now that WiFi is up so syslog host can be resolved
  logInit();

  // mDNS: register as ourbrewbot-CHIPID.local (only if enabled in settings)
  if (g_globalConfig.mdnsEnabled) {
    String mdnsName = "ourbrewbot-" + String(ESP.getChipId(), HEX);
    mdnsName.toLowerCase();
    mdnsSetLogger(mdnsLog);
    mdnsBegin(mdnsName.c_str());
    mdnsAddService("http", "tcp", 80);
  } else {
    logMsg("[MDNS] Disabled");
  }
}

void setupWiFi() {
  WiFiManager wifiManager;
  configureWiFiManager(wifiManager);
  String apName = setupHotspotName();

  if (!wifiManager.getWiFiIsSaved()) {
    // A brand-new device with no WiFi saved: the setup hotspot, waiting until
    // it is set up, as before. There is nothing to control yet.
    logMsg("[WIFI] No WiFi saved - starting setup hotspot %s", apName.c_str());
    if (!wifiManager.autoConnect(apName.c_str())) {
      logMsgL(SYSLOG_ERR, "[WIFI] Failed to connect - restarting");
      delay(3000);
      restartDevice();
    }
  } else {
    // WiFi saved: give it a few seconds, then carry on either way. The
    // fermenters are controlled whether WiFi is up or not - before 0.6.4 the
    // device sat in the setup hotspot and restarted until WiFi came back,
    // controlling nothing (and leaving the plugs as they were) all that time.
    logMsg("[WIFI] Connecting to %s...", WiFi.SSID().c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin();
    unsigned long start = millis();
    while (!WiFi.isConnected() && millis() - start < WIFI_BOOT_WAIT_MS) {
      delay(100);
    }
  }

  if (WiFi.isConnected()) {
    onWiFiConnected();
  } else {
    logMsgL(SYSLOG_WARNING, "[WIFI] Not connected - controlling offline and retrying in the background");
  }
}

// ============================================================
// WIFI WHILE OFFLINE
// ============================================================
// Used when the device booted without reaching its saved network, or when
// WiFi has been lost for 5 minutes after connecting (e.g. a new router, or a
// WiFi password change). It keeps trying in the background, and takes turns
// with the setup hotspot, so new WiFi details can be entered from a phone.
//
// The hotspot and reconnecting take turns: 1 minute reconnecting, then 2
// minutes of hotspot - kept open for up to 5 minutes while a phone is joined
// to it, so nobody is cut off while typing in new WiFi details. The WiFi
// library switches reconnecting off when the hotspot opens (running both at
// once makes the hotspot unreliable), but the device can still rejoin its
// network while the hotspot is up - wifiLoop() then closes the hotspot at
// once. Our web server is stopped while the hotspot is up, as both use port 80.
//
// Drops shorter than 5 minutes (e.g. a router reboot) are left to the SDK,
// which reconnects by itself.

static WiFiManager*  s_hotspot         = nullptr;   // non-null while the hotspot is up
static unsigned long s_wifiPhaseStart  = 0;         // when reconnecting / the hotspot started
static bool          s_offlineCycle    = false;     // true while a long drop is running the offline cycle
static bool          s_wifiDropped     = false;     // WiFi lost after connecting, not back yet
static unsigned long s_wifiDroppedAt   = 0;         // when it was lost

static void openSetupHotspot() {
  String apName = setupHotspotName();
  logMsgL(SYSLOG_WARNING, "[WIFI] Still not connected - opening setup hotspot %s for 2 min", apName.c_str());
  g_webServer.stop();
  s_hotspot = new WiFiManager();
  configureWiFiManager(*s_hotspot);
  s_hotspot->setConfigPortalTimeout(0);   // no library timeout - wifiLoop() decides when to close it
  s_hotspot->setConfigPortalBlocking(false);
  s_hotspot->startConfigPortal(apName.c_str());
}

static void closeSetupHotspot() {
  // The library closes the hotspot by itself once new WiFi details have been
  // saved. Closing it a second time crashes (its web server is already gone).
  if (s_hotspot->getConfigPortalActive()) {
    s_hotspot->stopConfigPortal();
  }
  delete s_hotspot;
  s_hotspot = nullptr;
  g_webServer.begin();
  if (!WiFi.isConnected()) {
    logMsg("[WIFI] Setup hotspot closed - trying the saved network again");
    WiFi.mode(WIFI_STA);
    WiFi.begin();
  }
}

// The hotspot's turn is over after 2 minutes - or, while a phone is joined
// to it, after 5 minutes (someone may be typing in new WiFi details).
static bool hotspotTimeIsUp(unsigned long now) {
  unsigned long upFor = now - s_wifiPhaseStart;
  if (WiFi.softAPgetStationNum() > 0) {
    return upFor >= WIFI_HOTSPOT_MAX_MS;
  }
  return upFor >= WIFI_HOTSPOT_MS;
}

// Called every pass once WiFi has connected, while the SDK looks after any
// drop by itself. True once WiFi has been down for WIFI_DROP_MS, so the
// offline cycle should start.
static bool wifiDownTooLong(unsigned long now) {
  if (WiFi.isConnected()) {
    if (s_wifiDropped) {
      logMsgL(SYSLOG_NOTICE, "[WIFI] Reconnected after %lu s", (now - s_wifiDroppedAt) / 1000);
      s_wifiDropped = false;
    }
    return false;
  }
  if (!s_wifiDropped) {
    s_wifiDropped   = true;
    s_wifiDroppedAt = now;
  }
  return now - s_wifiDroppedAt >= WIFI_DROP_MS;
}

void wifiLoop() {
  unsigned long now = millis();

  // Connected at some point since boot: short drops are left to the SDK. A
  // long one starts the offline cycle, hotspot first - the SDK has already had
  // 5 minutes to reconnect.
  if (g_wifiConnected && !s_offlineCycle) {
    if (!wifiDownTooLong(now)) return;
    logMsgL(SYSLOG_WARNING, "[WIFI] Lost for %u min - starting the offline cycle", WIFI_DROP_MS / 60000);
    s_offlineCycle = true;
    openSetupHotspot();
    s_wifiPhaseStart = now;
    return;
  }

  if (s_hotspot != nullptr) {
    s_hotspot->process();            // serves the hotspot's pages
    if (WiFi.isConnected() || !s_hotspot->getConfigPortalActive() || hotspotTimeIsUp(now)) {
      closeSetupHotspot();
      s_wifiPhaseStart = now;
    }
  } else if (!WiFi.isConnected() && now - s_wifiPhaseStart >= WIFI_OFFLINE_MS) {
    openSetupHotspot();
    s_wifiPhaseStart = now;
  }

  if (s_hotspot == nullptr && WiFi.isConnected()) {
    // After a long drop this runs again: it finds the syslog host and restarts
    // mDNS, in case the device is now on a new network
    bool firstConnect = !g_wifiConnected;
    onWiFiConnected();
    if (firstConnect) {
      logMsgL(SYSLOG_NOTICE, "[WIFI] Connected %u min after boot - the fermenters were controlled offline until then",
        (unsigned)(uptimeSeconds() / 60));
    } else {
      logMsgL(SYSLOG_NOTICE, "[WIFI] Back after %lu min offline - the fermenters were controlled offline until then",
        (now - s_wifiDroppedAt) / 60000);
    }
    s_offlineCycle = false;
    s_wifiDropped  = false;
  }
}

// ============================================================
// TEN MINUTE TIMER TASKS
// ============================================================
void onTenMinuteTimer() {
  // First, so the health line below already reaches syslog if its host has
  // only just become reachable
  logRetrySyslog();

  // Health report. "Stack free" is the lowest amount of the 4 KB loop stack
  // that has been left unused since boot (a high-water mark). If it ever
  // reaches 0 the loop stack has overflowed into the WiFi SDK's stack below it.
  logMsg("[HEALTH] Free heap: %u bytes, Largest contiguous: %u bytes, Fragmentation: %u%% | Stack free: %u bytes | Uptime: %u min | WiFi RSSI: %d dBm",
    ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation(), ESP.getFreeContStack(), g_globalConfig.lastUptime, WiFi.RSSI());
  uint32_t    stackFree;
  uint8_t     stackModule;
  const char* stackWhere;
  if (stackTakeNewLow(stackFree, stackModule, stackWhere)) {
    logMsg("[STACK] New deepest point: %u bytes of loop stack left, after %s %s",
           stackFree, stackModule == 0xFF ? "setup" : checkpointName(stackModule), stackWhere);
  }
  STACK_PROBE_LOG_NEW();   // diagnostic build only - see StackProbe.cpp
  uint32_t cleared = tcpTakeClearedCount();
  if (cleared > 0) {
    logMsg("[TCP] Cleared %u closed connections from TIME_WAIT in the last 10 min", cleared);
  }

  // Count profile time for running profiles (6 calls × 10 min = 1 hour). The
  // part-hour is kept while a profile is paused or powered off, and is saved
  // with the config just below, so it survives a reboot.
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (g_fermenters[i].profileRunning && g_fermenters[i].power && !g_fermenters[i].liveTest) {
      profileAddTenMinutes(i);
    }
  }

  // Save all config as periodic backup - held off while a config import is
  // part way through (see configPeriodicSave() in Config.cpp)
  configPeriodicSave();
}
