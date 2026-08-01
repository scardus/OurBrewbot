/*
 * WebAPI.cpp — REST API and web server implementation
 */

#include "WebAPI.h"
#include "Log.h"
#include "Mqtt.h"
#include "Fermenter.h"
#include "Temperatures.h"
#include "Reports.h"
#include "iSpindel.h"
#include "SmartPlugs.h"
#include "Pins.h"
#include "Profile.h"
#include "Tilt.h"

// Forward refs to global server (defined in .ino)
extern ESP8266WebServer g_webServer;

// Forward declarations for handlers defined later in this file
void handleBLESniff(ESP8266WebServer& server);
void handleBLESniffPoll(ESP8266WebServer& server);
void handleBLESniffSend(ESP8266WebServer& server);
void handleSyslogConfig(ESP8266WebServer& server);
void handleSyslogConfigPost(ESP8266WebServer& server);

// ============================================================
// SERVER SETUP — register all routes
// ============================================================

static void logApiCall(ESP8266WebServer& server) {
  const char* method;
  switch (server.method()) {
    case HTTP_POST:   method = "POST";   break;
    case HTTP_PUT:    method = "PUT";    break;
    case HTTP_DELETE: method = "DELETE"; break;
    default:          method = "GET";    break;
  }
  WiFiClient c = server.client();
  String ip = c.connected() ? c.remoteIP().toString() : String("?");
  logMsg("[API] %s %s from %s", method, server.uri().c_str(), ip.c_str());
}

// ============================================================
// ROUTE TABLE
// One PROGMEM table drives all dispatch via onNotFound() — each server.on()
// registration heap-allocates a RequestHandler, two String URIs and a
// std::function (~4-5 KB total for the ~48 routes below).  Only the /update
// POST keeps a real registration: the two-callback upload form is the one
// thing onNotFound dispatch cannot provide.
// ============================================================

typedef void (*ApiHandler)(ESP8266WebServer&);

struct ApiRoute {
  char       path[20];   // longest: "/fermenter/profile" (18 + NUL)
  HTTPMethod method;
  ApiHandler handler;
  bool       log;        // false for high-frequency poll endpoints
};

static const ApiRoute kRoutes[] PROGMEM = {
  { "/",                 HTTP_GET,  handleRoot,               true  },
  { "/controller",       HTTP_GET,  handleController,         true  },
  { "/controller",       HTTP_POST, handleController,         true  },
  { "/fermenters",       HTTP_GET,  handleFermenters,         true  },
  { "/fermenter",        HTTP_GET,  handleFermenter,          true  },
  { "/fermenter",        HTTP_POST, handleFermenter,          true  },
  { "/board_info.json",  HTTP_GET,  handleBoardInfo,          true  },
  { "/reset",            HTTP_GET,  handleReset,              true  },
  { "/reboot",           HTTP_GET,  handleReboot,             true  },
  { "/update",           HTTP_GET,  handleOTAPage,            true  },
  { "/config",           HTTP_GET,  handleConfigPage,         true  },
  { "/wifi/reset",       HTTP_POST, handleWiFiReset,          true  },
  { "/WiFi",             HTTP_GET,  handleConfigPage,         true  },
  { "/iSpindel",         HTTP_POST, handleiSpindel,           true  },
  { "/ispindels",        HTTP_GET,  handleiSpindels,          true  },
  { "/ispindel/config",  HTTP_POST, handleiSpindelConfigPost, true  },
  { "/status",           HTTP_GET,  handleStatus,             true  },
  { "/probes",           HTTP_GET,  handleProbes,             true  },
  { "/probes",           HTTP_POST, handleProbePost,          true  },
  { "/health",           HTTP_GET,  handleHealth,             true  },
  { "/admin",            HTTP_GET,  handleAdmin,              true  },
  { "/smartplugs",       HTTP_GET,  handleSmartPlugs,         true  },
  { "/smartplug",        HTTP_POST, handleSmartPlugPost,      true  },
  { "/smartplug/test",   HTTP_POST, handleSmartPlugTest,      true  },
  { "/rf/sniff",         HTTP_GET,  handleRFSniff,            true  },
  { "/rf/sniff/poll",    HTTP_GET,  handleRFSniffPoll,        false },
  { "/ble/sniff",        HTTP_GET,  handleBLESniff,           true  },
  { "/ble/sniff/poll",   HTTP_GET,  handleBLESniffPoll,       false },
  { "/ble/sniff/send",   HTTP_POST, handleBLESniffSend,       true  },
  { "/brewservices",     HTTP_GET,  handleBrewServices,       true  },
  { "/brewservices",     HTTP_POST, handleBrewServicesPost,   true  },
  { "/brewservices/test",HTTP_POST, handleBrewServiceTest,    true  },
  { "/mqtt",             HTTP_GET,  handleMqttConfig,         true  },
  { "/mqtt",             HTTP_POST, handleMqttConfigPost,     true  },
  { "/mqtt/test",        HTTP_POST, handleMqttTest,           true  },
  { "/mqtt/discover",    HTTP_POST, handleMqttDiscover,       true  },
  { "/syslog",           HTTP_GET,  handleSyslogConfig,       true  },
  { "/syslog",           HTTP_POST, handleSyslogConfigPost,   true  },
  { "/debug",            HTTP_GET,  handleDebug,              true  },
  { "/debug",            HTTP_POST, handleDebug,              true  },
  { "/profiles",         HTTP_GET,  handleProfiles,           true  },
  { "/profile",          HTTP_POST, handleProfilePost,        true  },
  { "/fermenter/profile",HTTP_POST, handleFermenterProfile,   true  },
  { "/tilts",            HTTP_GET,  handleTilts,              true  },
  { "/tilt",             HTTP_POST, handleTiltPost,           true  },
  { "/fs/files",         HTTP_GET,  handleFsFiles,            true  },
  { "/fs/file",          HTTP_GET,  handleFsFile,             true  },
  { "/fs/save",          HTTP_POST, handleFsFileSave,         true  },
};

static void dispatchApiRequest(ESP8266WebServer& server) {
  HTTPMethod method = server.method();
  String uri = server.uri();  // uri() returns by value — keep it alive for the loop
  for (size_t i = 0; i < sizeof(kRoutes) / sizeof(kRoutes[0]); i++) {
    ApiRoute r;
    memcpy_P(&r, &kRoutes[i], sizeof(r));
    if (r.method == method && strcmp(uri.c_str(), r.path) == 0) {
      if (r.log) logApiCall(server);
      r.handler(server);
      return;
    }
  }
  logApiCall(server);
  handleNotFound(server);
}

void setupWebServer(ESP8266WebServer& server) {
  // OTA upload handler — must stay a real registration (see kRoutes note)
  server.on("/update", HTTP_POST,
    [&server]() {
      logApiCall(server);
      server.sendHeader("Connection", "close");
      if (Update.hasError()) {
        String err = "Update FAILED: ";
        err += Update.getError();
        server.send(500, "text/html", err);
      } else {
        server.send(200, "text/html",
          "<META http-equiv=\"refresh\" content=\"10;URL=/\">"
          "Update Success! Rebooting OurBrewbot Controller...");
        delay(500);
        ESP.restart();
      }
    },
    [&server]() { handleOTAUpload(server); }
  );

  server.onNotFound([&server]() { dispatchApiRequest(server); });
}

// ============================================================
// HELPERS
// ============================================================

void sendCORSHeaders(ESP8266WebServer& server) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendJsonResponse(ESP8266WebServer& server, const String& json, int code) {
  sendCORSHeaders(server);
  server.send(code, "application/json", json);
}

// Stream a JsonDocument straight to the client — no intermediate String.
// Uses measureJson + setContentLength so the response is not chunked.
void sendJsonDoc(ESP8266WebServer& server, JsonDocument& doc, int code) {
  sendCORSHeaders(server);
  server.setContentLength(measureJson(doc));
  server.send(code, "application/json", "");
  WiFiClient client = server.client();
  if (!client.connected()) return;  // guard: client may disconnect between header send and body write
  serializeJson(doc, client);
}

// The ubiquitous {"status":"...","msg":"..."} response, built from a flash
// string. Message must not contain JSON-special characters.
static void sendStatusMsg(ESP8266WebServer& server, int code, bool ok,
                          const __FlashStringHelper* msg) {
  char m[112];
  strncpy_P(m, (PGM_P)msg, sizeof(m) - 1);
  m[sizeof(m) - 1] = '\0';
  char buf[144];
  snprintf_P(buf, sizeof(buf), PSTR("{\"status\":\"%s\",\"msg\":\"%s\"}"),
             ok ? "ok" : "error", m);
  sendJsonResponse(server, buf, code);
}

static void sendOk(ESP8266WebServer& server, const __FlashStringHelper* msg) {
  sendStatusMsg(server, 200, true, msg);
}

static void sendErr(ESP8266WebServer& server, int code, const __FlashStringHelper* msg) {
  sendStatusMsg(server, code, false, msg);
}

// Parse the POST body into doc; sends the 400 error and returns false on
// malformed JSON.
static bool parseJsonBody(ESP8266WebServer& server, JsonDocument& doc) {
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendErr(server, 400, F("Invalid JSON"));
    return false;
  }
  return true;
}

// Validated integer field in [0, max) from a parsed body; sends the 400 error
// and returns -1 when the field is missing or out of range.
static int getValidIndex(ESP8266WebServer& server, const JsonDocument& doc,
                         const char* key, int max, const __FlashStringHelper* errMsg) {
  int idx = doc[key] | -1;
  if (idx < 0 || idx >= max) {
    sendErr(server, 400, errMsg);
    return -1;
  }
  return idx;
}

// ============================================================
// ROOT / HOME
// ============================================================


static const char ROOT_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
    <html><head><meta charset='utf-8'>
    <meta name='viewport' content='width=device-width,initial-scale=1'>
    <title>OurBrewbot</title>
    <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px}
    h2{color:#e94560;margin:0 0 16px}
    a{color:#53d8fb;text-decoration:none}
    a:hover{text-decoration:underline}
    .card{background:#16213e;border:1px solid #333;border-radius:6px;padding:16px;margin-bottom:12px}
    .card h3{color:#e94560;margin-bottom:10px;font-size:15px}
    .btn{display:inline-block;background:#e94560;color:#fff;padding:10px 24px;border-radius:4px;font-size:16px;font-weight:bold;margin-bottom:16px}
    .btn:hover{background:#c73650;text-decoration:none}
    ul{list-style:none;padding:0}
    li{padding:6px 0;border-bottom:1px solid #222;font-size:14px}
    li:last-child{border-bottom:none}
    li a{font-family:monospace;font-size:13px}
    .method{display:inline-block;width:40px;color:#e94560;font-weight:bold;font-family:monospace;font-size:13px}
    .desc{color:#aaa;margin-left:8px}
    </style></head><body>
    <h2>OurBrewbot</h2>
    <a class='btn' href='/admin'>Open Admin Dashboard</a>
    <div class='card'><h3>REST API</h3><ul>
    <li><span class='method'>GET</span><a href='/ble/sniff'>/ble/sniff</a><span class='desc'> &mdash; BLE sniff page</span></li>
    <li><span class='method'>GET</span><a href='/ble/sniff/poll'>/ble/sniff/poll</a><span class='desc'> &mdash; poll BLE sniff results</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/ble/sniff/send</span><span class='desc'> &mdash; send BLE AT command</span></li>
    <li><span class='method'>GET</span><a href='/board_info.json'>/board_info.json</a><span class='desc'> &mdash; board info</span></li>
    <li><span class='method'>GET</span><a href='/brewservices'>/brewservices</a><span class='desc'> &mdash; brew service config</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/brewservices</span><span class='desc'> &mdash; update brew service</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/brewservices/test</span><span class='desc'> &mdash; test brew service</span></li>
    <li><span class='method'>GET</span><a href='/config'>/config</a><span class='desc'> &mdash; WiFi reset page</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/wifi/reset</span><span class='desc'> &mdash; clear WiFi settings and reboot into setup portal</span></li>
    <li><span class='method'>GET</span><a href='/controller'>/controller</a><span class='desc'> &mdash; controller config</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/controller</span><span class='desc'> &mdash; update global config</span></li>
    <li><span class='method'>GET</span><a href='/debug'>/debug</a><span class='desc'> &mdash; debug config &amp; overrides</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/debug</span><span class='desc'> &mdash; update debug config</span></li>
    <li><span class='method'>GET</span><a href='/fermenters'>/fermenters</a><span class='desc'> &mdash; all fermenter data</span></li>
    <li><span class='method'>GET</span><a href='/fermenter?id=0'>/fermenter?id=N</a><span class='desc'> &mdash; single fermenter</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/fermenter</span><span class='desc'> &mdash; update fermenter config</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/fermenter/profile</span><span class='desc'> &mdash; profile control (start/stop/pause/resume/next/prev)</span></li>
    <li><span class='method'>GET</span><a href='/fs/files'>/fs/files</a><span class='desc'> &mdash; list LittleFS files</span></li>
    <li><span class='method'>GET</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/fs/file?name=...</span><span class='desc'> &mdash; read file content</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/fs/save</span><span class='desc'> &mdash; save config file to LittleFS</span></li>
    <li><span class='method'>GET</span><a href='/health'>/health</a><span class='desc'> &mdash; system health</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/iSpindel</span><span class='desc'> &mdash; iSpindel gravity data</span></li>
    <li><span class='method'>GET</span><a href='/ispindels'>/ispindels</a><span class='desc'> &mdash; iSpindel config &amp; live data</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/ispindel/config</span><span class='desc'> &mdash; update iSpindel config</span></li>
    <li><span class='method'>GET</span><a href='/mqtt'>/mqtt</a><span class='desc'> &mdash; MQTT config</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/mqtt</span><span class='desc'> &mdash; update MQTT config</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/mqtt/test</span><span class='desc'> &mdash; test MQTT connection</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/mqtt/discover</span><span class='desc'> &mdash; trigger HA discovery</span></li>
    <li><span class='method'>GET</span><a href='/probes'>/probes</a><span class='desc'> &mdash; temperature probes</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/probes</span><span class='desc'> &mdash; update probe config</span></li>
    <li><span class='method'>GET</span><a href='/profiles'>/profiles</a><span class='desc'> &mdash; fermentation profiles</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/profile</span><span class='desc'> &mdash; update profile</span></li>
    <li><span class='method'>GET</span><a href='/reboot'>/reboot</a><span class='desc'> &mdash; reboot device</span></li>
    <li><span class='method'>GET</span><a href='/reset'>/reset</a><span class='desc'> &mdash; reset configuration</span></li>
    <li><span class='method'>GET</span><a href='/rf/sniff'>/rf/sniff</a><span class='desc'> &mdash; RF sniff page</span></li>
    <li><span class='method'>GET</span><a href='/rf/sniff/poll'>/rf/sniff/poll</a><span class='desc'> &mdash; poll RF sniff results</span></li>
    <li><span class='method'>GET</span><a href='/smartplugs'>/smartplugs</a><span class='desc'> &mdash; smart plug config</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/smartplug</span><span class='desc'> &mdash; update smart plug</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/smartplug/test</span><span class='desc'> &mdash; test smart plug RF</span></li>
    <li><span class='method'>GET</span><a href='/status'>/status</a><span class='desc'> &mdash; quick status</span></li>
    <li><span class='method'>GET</span><a href='/syslog'>/syslog</a><span class='desc'> &mdash; syslog config</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/syslog</span><span class='desc'> &mdash; update syslog config</span></li>
    <li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/tilt</span><span class='desc'> &mdash; update Tilt config</span></li>
    <li><span class='method'>GET</span><a href='/tilts'>/tilts</a><span class='desc'> &mdash; Tilt hydrometer config &amp; live data</span></li>
    <li><span class='method'>GET</span><a href='/update'>/update</a><span class='desc'> &mdash; OTA firmware update</span></li>
    </ul></div>
    </body></html>)rawliteral";

void handleRoot(ESP8266WebServer& server) {
  server.setContentLength(strlen_P(ROOT_PAGE));
  server.send(200, "text/html", "");
  server.sendContent_P(ROOT_PAGE);
}

// ============================================================
// FERMENTERS — GET /fermenters
// ============================================================

void buildFermenterJson(JsonDocument& doc, uint8_t i) {
  doc["Fermenter"]       = i;
  doc["FermenterName"]   = g_fermenters[i].fermenterName;
  doc["BeerName"]        = g_fermenters[i].beerName;
  doc["YeastName"]       = g_fermenters[i].yeastName;
  // Setpoints leave in the user's display unit, exactly like the live readings
  // below. Ceiling/Floor are absolute temperatures (scale + 32 offset);
  // Hysteresis and AlarmTolerance are SPANS, so they scale only - a 0.5 C
  // hysteresis is 0.9 F, not 32.9 F.
  doc["CeilingTemp"]     = toDisplayTemp(g_fermenters[i].ceilingTemp);
  doc["FloorTemp"]       = toDisplayTemp(g_fermenters[i].floorTemp);
  doc["OG"]              = g_fermenters[i].og;
  doc["TG"]              = g_fermenters[i].tg;
  doc["Hysteresis"]      = toDisplayTempDelta(g_fermenters[i].hysteresis);
  doc["AlarmTolerance"]  = toDisplayTempDelta(g_fermenters[i].alarmTolerance);
  doc["CompressorDelay"] = g_fermenters[i].compressorDelay;
  doc["TempControl"]     = g_fermenters[i].tempControl;
  doc["SGControl"]       = g_fermenters[i].sgControl;
  doc["Power"]           = g_fermenters[i].power;
  doc["Status"]          = g_fermenters[i].status;
  doc["StatusStr"]       = getFermenterStatusStr(g_fermenters[i].status);
  doc["Alarm"]           = g_fermenters[i].alarm;
  doc["ProfileRunning"]  = g_fermenters[i].profileRunning;
  doc["ProfilePaused"]   = g_fermenters[i].profilePaused;
  doc["ProfileNo"]       = g_fermenters[i].profileNo;
  doc["CurrentStep"]     = g_fermenters[i].currentStep;
  doc["CurrentHour"]     = g_fermenters[i].currentHour;
  doc["LiveTest"]        = g_fermenters[i].liveTest;
  // Profile name: "Standard" for profileNo==0, else profile name
  if (g_fermenters[i].profileNo >= 1 && g_fermenters[i].profileNo <= MAX_PROFILES) {
    doc["ProfileName"] = g_profiles[g_fermenters[i].profileNo - 1].profileName;
    doc["TotalSteps"]  = countProfileSteps(g_fermenters[i].profileNo - 1);
  } else {
    doc["ProfileName"] = "Standard";
    doc["TotalSteps"]  = 0;
  }
  doc["SGCalibration"]   = g_fermenters[i].sgCalibration;
  doc["BrewServices"]    = g_fermenters[i].brewServices;
  doc["YeastName"]       = g_fermenters[i].yeastName;

  // Live readings
  float beerTemp    = getBeerTemp(i);
  float ambientTemp = getAmbientTemp(i);
  float sg          = getCurrentSG(i);

  doc["BeerTemp"]       = (beerTemp    > TEMP_VALID_MIN) ? toDisplayTemp(beerTemp)    : TEMP_NONE;
  doc["AmbientTemp"]    = (ambientTemp > TEMP_VALID_MIN) ? toDisplayTemp(ambientTemp) : TEMP_NONE;
  doc["SG"]             = sg;
  doc["Attenuation"]    = getAttenuation(i);
  doc["EstABV"]         = getEstABV(i);
  doc["TempUnit"]       = (g_globalConfig.unit == UNIT_CELSIUS) ? "C" : "F";
  doc["BeerTempSource"] = getBeerTempSource(i);
  doc["GravitySource"]  = getGravitySource(i);
}

void buildProfileJson(JsonDocument& doc, int p) {
  doc["index"] = p;
  doc["name"]  = g_profiles[p].profileName;
  JsonArray steps = doc["steps"].to<JsonArray>();
  uint8_t base = p * MAX_STEPS_PER_PROFILE;
  for (int s = 0; s < MAX_STEPS_PER_PROFILE; s++) {
    JsonObject st = steps.add<JsonObject>();
    const ProfileStep& step = g_profileSteps[base + s];
    st["stepType"]  = step.stepType;
    // An unused slot is all-zero, and countProfileSteps() / the WebUI both
    // detect it by testing those temperatures against 0. Converting them would
    // report 32 in Fahrenheit and the slot would stop looking empty, so emit
    // the sentinel verbatim and only convert real step temperatures.
    if (isStepEmpty(step)) {
      st["startTemp"] = 0.0f;
      st["endTemp"]   = 0.0f;
    } else {
      st["startTemp"] = toDisplayTemp(step.startTemp);
      st["endTemp"]   = toDisplayTemp(step.endTemp);
    }
    st["sgTrigger"] = step.sgTrigger;
    st["days"]      = step.days;
  }
}

void handleFermenters(ESP8266WebServer& server) {
  // Send fermenter array using chunked transfer to avoid building
  // one large String in RAM (each fermenter JSON is ~500 bytes)
  sendCORSHeaders(server);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (i > 0) server.sendContent(",");
    JsonDocument doc;
    buildFermenterJson(doc, i);
    char buf[768];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    server.sendContent(buf, n);
  }
  server.sendContent("]");
  server.sendContent("");  // end chunked transfer
}

void handleFermenter(ESP8266WebServer& server) {
  if (server.method() == HTTP_POST) {
    // Update fermenter config from POST body
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      int idx = doc["Fermenter"] | -1;
      if (idx >= 0 && idx < MAX_FERMENTERS) {
        // Every field below is applied to this local copy, and the live config
        // is only replaced once the entire body has passed validation. Writing
        // fields straight into g_fermenters[idx] as they were read meant a body
        // that failed a LATER check (OG, TG, CompressorDelay, AlarmTolerance)
        // returned 400 having already moved the ceiling/floor/hysteresis — and
        // the control loop reads those from RAM, so a rejected save really did
        // change how the fermenter was driven until the next config load.
        FermenterConfig fc = g_fermenters[idx];

        // Validated numeric fields — validateFermenterField() owns single-
        // field ranges; CeilingTemp / FloorTemp / Hysteresis are validated
        // holistically below against the would-be combined state so a save
        // can correct an invalid in-memory config.
        const char* valErr;
        char valJson[128];
        #define REJECT(msg) { \
            snprintf(valJson, sizeof(valJson), "{\"status\":\"error\",\"msg\":\"%s\"}", msg); \
            sendJsonResponse(server, valJson, 400); return; \
          }
        // validateFermenterField() is passed idx, not fc, deliberately: it
        // range-checks against the STORED config, which is the behaviour the
        // MQTT command path shares. None of the three keys used below read the
        // stored trio, so validating before the commit changes nothing.
        #define VALIDATE_AND_SET(jsonKey, valKey, member, cast) \
          if (!doc[#jsonKey].isNull()) { \
            float v = doc[#jsonKey]; \
            if (!validateFermenterField(idx, valKey, v, &valErr)) REJECT(valErr) \
            fc.member = (cast)v; \
          }

        // Holistic temp/hyst trio: take new value if present, else current.
        // Incoming values are in the user's display unit, so convert BEFORE the
        // range checks below - those limits are Celsius, and range-checking a
        // Fahrenheit number against them would reject valid setpoints and
        // accept invalid ones. Hysteresis is a span, so it uses the delta
        // helper (no 32 offset). Stored config stays Celsius throughout.
        float wbCeiling = doc["CeilingTemp"].isNull() ? fc.ceilingTemp : toCelsius((float)doc["CeilingTemp"]);
        float wbFloor   = doc["FloorTemp"].isNull()   ? fc.floorTemp   : toCelsius((float)doc["FloorTemp"]);
        float wbHyst    = doc["Hysteresis"].isNull()  ? fc.hysteresis  : toCelsiusTempDelta((float)doc["Hysteresis"]);
        if (wbCeiling < -20.0f || wbCeiling > 50.0f)  REJECT("ceiling temperature out of range (-20 to 50)")
        if (wbFloor   < -20.0f || wbFloor   > 50.0f)  REJECT("floor temperature out of range (-20 to 50)")
        if (wbHyst    <   0.0f || wbHyst    > 10.0f)  REJECT("hysteresis out of range (0 to 10)")
        if (wbFloor >= wbCeiling)                     REJECT("floor must be below ceiling")
        if ((wbCeiling - wbFloor) <  2.0f * wbHyst)   REJECT("safe zone must be at least 2x hysteresis")
        if (!doc["CeilingTemp"].isNull()) fc.ceilingTemp = wbCeiling;
        if (!doc["FloorTemp"].isNull())   fc.floorTemp   = wbFloor;
        if (!doc["Hysteresis"].isNull())  fc.hysteresis  = wbHyst;

        VALIDATE_AND_SET(CompressorDelay, "compressor_delay",    compressorDelay, uint16_t)
        VALIDATE_AND_SET(OG,              "og",                  og,              float)
        VALIDATE_AND_SET(TG,              "tg",                  tg,              float)
        #undef VALIDATE_AND_SET
        #undef REJECT

        if (!doc["AlarmTolerance"].isNull()) {
          float v = toCelsiusTempDelta((float)doc["AlarmTolerance"]);  // a span, and the range below is Celsius
          if (v < 0.0f || v > 10.0f) {
            sendErr(server, 400, F("alarm tolerance out of range (0 to 10)"));
            return;
          }
          fc.alarmTolerance = v;
        }

        if (!doc["Power"].isNull())           fc.power           = doc["Power"];
        if (!doc["TempControl"].isNull())     fc.tempControl     = doc["TempControl"];
        if (doc["BeerName"].is<const char*>())      strlcpy(fc.beerName,      doc["BeerName"].as<const char*>(),      sizeof(fc.beerName));
        if (doc["FermenterName"].is<const char*>()) strlcpy(fc.fermenterName, doc["FermenterName"].as<const char*>(), sizeof(fc.fermenterName));
        if (doc["YeastName"].is<const char*>())     strlcpy(fc.yeastName,     doc["YeastName"].as<const char*>(),     sizeof(fc.yeastName));
        if (!doc["BrewServices"].isNull())    fc.brewServices    = doc["BrewServices"];
        if (!doc["ProfileNo"].isNull())       { int v = doc["ProfileNo"]; if (v >= 0 && v <= MAX_PROFILES) fc.profileNo = (uint8_t)v; }
        if (!doc["LiveTest"].isNull())        fc.liveTest        = doc["LiveTest"];

        // Single commit point: nothing above this line is visible to the
        // control loop, so a rejected body leaves the fermenter untouched.
        g_fermenters[idx] = fc;

        saveFermenterConfig();
        sendOk(server, F("Configuration saved"));
        return;
      }
    }
    sendErr(server, 400, F("Configuration invalid"));
    return;
  }

  // GET single fermenter
  int idx = server.arg("id").toInt();
  if (idx < 0 || idx >= MAX_FERMENTERS) idx = 0;
  JsonDocument doc;
  buildFermenterJson(doc, idx);
  sendJsonDoc(server, doc);
}

// ============================================================
// DEBUG — GET /debug, POST /debug
// Runtime-only fermenter sensor overrides — never persisted.
// GET returns current debug mode and per-fermenter overrides
//     (temperatures in current display unit).
// POST accepts { "DebugMode": bool } and/or
//     { "Fermenter": n, "Enabled": bool, "BeerTemp": f,
//       "AmbientTemp": f, "SG": f } (temps in display unit).
// ============================================================

void handleDebug(ESP8266WebServer& server) {
  sendCORSHeaders(server);

  if (server.method() == HTTP_POST) {
    JsonDocument doc;
    if (!parseJsonBody(server, doc)) return;
    if (!doc["DebugMode"].isNull())
      g_fermenterDebugMode = doc["DebugMode"];
    if (!doc["Fermenter"].isNull()) {
      int idx = doc["Fermenter"];
      if (idx >= 0 && idx < MAX_FERMENTERS) {
        if (!doc["Enabled"].isNull())
          g_fermenterDebugOverrides[idx].enabled = doc["Enabled"];
        if (!doc["BeerTemp"].isNull())
          g_fermenterDebugOverrides[idx].beerTemp = toCelsius((float)doc["BeerTemp"]);
        if (!doc["AmbientTemp"].isNull())
          g_fermenterDebugOverrides[idx].ambientTemp = toCelsius((float)doc["AmbientTemp"]);
        if (!doc["SG"].isNull())
          g_fermenterDebugOverrides[idx].sg = doc["SG"];
      }
    }
    sendOk(server, F("Debug state updated"));
    return;
  }

  // GET — return current debug state, chunked per-fermenter (see handleFermenters)
  sendCORSHeaders(server);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char head[64];
  int hn = snprintf(head, sizeof(head), "{\"DebugMode\":%s,\"TempUnit\":\"%s\",\"Overrides\":[",
                    g_fermenterDebugMode ? "true" : "false",
                    (g_globalConfig.unit == UNIT_CELSIUS) ? "C" : "F");
  server.sendContent(head, hn);
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (i > 0) server.sendContent(",");
    JsonDocument ov;
    ov["Fermenter"]   = i;
    ov["Enabled"]     = g_fermenterDebugOverrides[i].enabled;
    ov["BeerTemp"]    = toDisplayTemp(g_fermenterDebugOverrides[i].beerTemp);
    ov["AmbientTemp"] = toDisplayTemp(g_fermenterDebugOverrides[i].ambientTemp);
    ov["SG"]          = g_fermenterDebugOverrides[i].sg;
    char buf[192];
    size_t n = serializeJson(ov, buf, sizeof(buf));
    server.sendContent(buf, n);
  }
  server.sendContent("]}");
  server.sendContent("");  // end chunked transfer
}

// ============================================================
// CONTROLLER — GET /controller
// Returns global config + smart plug config
// ============================================================

void buildControllerJson(JsonDocument& doc) {
  doc["AuthCode"]      = g_globalConfig.authCode;
  doc["Unit"]          = g_globalConfig.unit;
  doc["BrewService"]   = g_globalConfig.brewService;
  doc["BrewServiceId"] = g_globalConfig.brewServiceId;
  doc["NotifyOn"]      = g_globalConfig.notifyOn;
  doc["Resolution"]    = g_globalConfig.resolution;
  doc["AlarmDwellSec"] = g_globalConfig.alarmDwellSec;
  doc["MdnsEnabled"]   = g_globalConfig.mdnsEnabled;
  doc["FirmwareVersion"] = FW_VERSION;
  doc["ChipId"]        = String(ESP.getChipId(), HEX);
  doc["FreeHeap"]      = ESP.getFreeHeap();
  doc["Uptime"]        = (uint32_t)(millis() / 60000UL);
  doc["WiFiSSID"]      = WiFi.SSID();
  doc["IP"]            = WiFi.localIP().toString();
  doc["RSSI"]          = WiFi.RSSI();

  // mDNS hostname
  String mdnsName = "ourbrewbot-" + String(ESP.getChipId(), HEX);
  mdnsName.toLowerCase();
  doc["mDNSName"]      = mdnsName + ".local";

  // Smart plug summary
  JsonArray plugs = doc["SmartPlugs"].to<JsonArray>();
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    if (g_smartPlugs[i].onCode == 0) continue;
    JsonObject p = plugs.add<JsonObject>();
    p["PlugNo"]       = i;
    p["Function"]     = g_smartPlugs[i].function;
    p["Fermenter"]    = g_smartPlugs[i].fermenter;
    p["Manufacturer"] = g_smartPlugs[i].manufacturer;
    p["State"]        = getPlugState(i);
  }
}

void handleController(ESP8266WebServer& server) {
  if (server.method() == HTTP_POST) {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      uint8_t oldUnit = g_globalConfig.unit;
      if (!doc["Unit"].isNull())       { uint8_t v = doc["Unit"];       if (v == UNIT_CELSIUS || v == UNIT_FAHRENHEIT) g_globalConfig.unit       = v; }
      if (!doc["Resolution"].isNull()) { uint8_t v = doc["Resolution"]; if (v >= 9 && v <= 12)                        g_globalConfig.resolution = v; }
      if (!doc["AlarmDwellSec"].isNull()) { uint32_t v = doc["AlarmDwellSec"]; if (v <= 3600) g_globalConfig.alarmDwellSec = (uint16_t)v; }
      if (!doc["MdnsEnabled"].isNull())   g_globalConfig.mdnsEnabled   = doc["MdnsEnabled"];
      if (!doc["NotifyOn"].isNull())      g_globalConfig.notifyOn      = doc["NotifyOn"];
      if (!doc["BrewService"].isNull())   g_globalConfig.brewService   = doc["BrewService"];
      if (doc["BrewServiceId"].is<const char*>()) strlcpy(g_globalConfig.brewServiceId, doc["BrewServiceId"].as<const char*>(), sizeof(g_globalConfig.brewServiceId));
      saveGlobalConfig();
      // HA discovery advertises the temperature unit — republish so entities
      // pick up the new unit (retained topics are overwritten in place).
      if (g_globalConfig.unit != oldUnit) publishAllHaDiscovery();
      sendOk(server, F("Configuration saved"));
      return;
    }
    sendErr(server, 400, F("Configuration invalid"));
    return;
  }
  JsonDocument doc;
  buildControllerJson(doc);
  sendJsonDoc(server, doc);
}

// ============================================================
// BOARD INFO — GET /board_info.json
// ============================================================

void buildBoardInfoJson(JsonDocument& doc) {
  doc["chip_id"]     = String(ESP.getChipId(), HEX);
  doc["flash_size"]  = ESP.getFlashChipSize();
  doc["free_heap"]   = ESP.getFreeHeap();
  doc["sdk_version"] = ESP.getSdkVersion();
  doc["fw_version"]  = FW_VERSION;
  doc["fw_date"]     = FW_BUILD_DATE;
  doc["reset_reason"]= ESP.getResetReason();
}

void handleBoardInfo(ESP8266WebServer& server) {
  JsonDocument doc;
  buildBoardInfoJson(doc);
  sendJsonDoc(server, doc);
}

// ============================================================
// STATUS — GET /status (new, convenience endpoint)
// ============================================================

void handleStatus(ESP8266WebServer& server) {
  // Chunked per-fermenter streaming (same pattern as handleFermenters) —
  // avoids building the whole response in one elastic JsonDocument
  sendCORSHeaders(server);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"fermenters\":[");
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (i > 0) server.sendContent(",");
    JsonDocument doc;
    doc["id"]          = i;
    doc["name"]        = g_fermenters[i].fermenterName;
    doc["beer"]        = g_fermenters[i].beerName;
    doc["power"]       = g_fermenters[i].power;
    doc["status"]      = getFermenterStatusStr(g_fermenters[i].status);
    float bt = getBeerTemp(i);
    float at = getAmbientTemp(i);
    doc["beerTemp"]    = (bt > TEMP_VALID_MIN) ? toDisplayTemp(bt) : TEMP_NONE;
    doc["ambientTemp"] = (at > TEMP_VALID_MIN) ? toDisplayTemp(at) : TEMP_NONE;
    doc["sg"]          = getCurrentSG(i);
    doc["alarm"]       = g_fermenters[i].alarm;
    char buf[384];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    server.sendContent(buf, n);
  }
  char tail[96];
  int n = snprintf(tail, sizeof(tail), "],\"uptime\":%u,\"freeHeap\":%u,\"ip\":\"%s\"}",
                   (unsigned)(millis() / 60000UL), (unsigned)ESP.getFreeHeap(),
                   WiFi.localIP().toString().c_str());
  server.sendContent(tail, n);
  server.sendContent("");  // end chunked transfer
}

// ============================================================
// PROBES — GET /probes
// ============================================================

void handleProbes(ESP8266WebServer& server) {
  JsonDocument doc;
  JsonArray arr = doc["probes"].to<JsonArray>();
  for (int i = 0; i < MAX_PROBES; i++) {
    if (strlen(g_probes[i].address) == 0) continue;
    JsonObject p = arr.add<JsonObject>();
    p["index"]       = i;
    p["name"]        = g_probes[i].probeName;
    p["address"]     = g_probes[i].address;
    p["function"]    = g_probes[i].function;
    p["fermenter"]   = g_probes[i].fermenter;
    p["temperature"] = toDisplayTemp(g_probes[i].temperature);
    p["tempAdjust"]  = toDisplayTempDelta(g_probes[i].tempAdjust);  // a calibration offset: span, not absolute
  }
  sendJsonDoc(server, doc);
}

// ============================================================
// HEALTH — GET /health
// ============================================================

void handleHealth(ESP8266WebServer& server) {
  JsonDocument doc;
  doc["freeHeap"]    = ESP.getFreeHeap();
  doc["uptime"]      = (uint32_t)(millis() / 60000UL);
  doc["rssi"]        = WiFi.RSSI();
  doc["ssid"]        = WiFi.SSID();
  doc["ip"]          = WiFi.localIP().toString();
  doc["resetReason"] = ESP.getResetReason();
  sendJsonDoc(server, doc);
}

// ============================================================
// RESET — GET /reset
// ============================================================

void handleReset(ESP8266WebServer& server) {
  String target = server.arg("target");
  if (target == "config") {
    server.send(200, "application/json", F("{\"status\":\"ok\",\"msg\":\"Configuration reset\"}"));
    delay(200);
    resetAllConfig();
    ESP.restart();
  } else {
    // Show confirmation page
    server.send(200, "text/html",
      "<html><body><h3>Reset Configuration?</h3>"
      "<p>This will erase all settings.</p>"
      "<a href='/reset?target=config'>Confirm Reset</a> | <a href='/'>Cancel</a>"
      "</body></html>");
  }
}

// ============================================================
// REBOOT — GET /reboot
// ============================================================

void handleReboot(ESP8266WebServer& server) {
  server.send(200, "application/json", F("{\"status\":\"ok\",\"msg\":\"Rebooting\"}"));
  delay(500);
  ESP.restart();
}

// ============================================================
// OTA UPDATE — GET /update + POST /update
// ============================================================

// Served from PROGMEM via sendContent_P — server.send(F(...)) would copy the
// whole page into a heap String per request.
static const char OTA_PAGE[] PROGMEM =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OurBrewbot - Firmware Update</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px}"
    "h2{color:#e94560;margin:0 0 16px}"
    ".card{background:#16213e;border:1px solid #333;border-radius:6px;padding:16px;max-width:480px}"
    "p{margin-bottom:12px;font-size:14px;color:#aaa}"
    "input[type=file]{color:#e0e0e0;font-size:13px;margin-bottom:16px;display:block}"
    "input[type=submit]{background:#e94560;color:#fff;border:none;padding:6px 16px;border-radius:4px;cursor:pointer;font-size:13px}"
    "input[type=submit]:hover{background:#c73650}"
    "a{color:#53d8fb;text-decoration:none;font-size:13px}"
    "</style></head><body>"
    "<h2>Firmware Update</h2>"
    "<div class='card'>"
    "<p>Select a firmware .bin file and click Upload. The device will reboot automatically after a successful update.</p>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin'>"
    "<input type='submit' value='Upload Firmware'>"
    "</form></div>"
    "<p style='margin-top:12px'><a href='/'>&#8592; Cancel</a></p>"
    "</body></html>";

void handleOTAPage(ESP8266WebServer& server) {
  server.setContentLength(strlen_P(OTA_PAGE));
  server.send(200, "text/html", "");
  server.sendContent_P(OTA_PAGE);
}

void handleOTAUpload(ESP8266WebServer& server) {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    logMsgL(SYSLOG_NOTICE, "[OTA] Update started: %s", upload.filename.c_str());
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      // size_t is unsigned int on the ESP8266, so %u is right on the target -
      // but it is 64-bit on the host that builds the native tests, where the
      // same line warns. The cast makes it correct on both and changes
      // nothing in the firmware.
      logMsgL(SYSLOG_NOTICE, "[OTA] Update success: %u bytes", (unsigned)upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

// ============================================================
// ISPINDEL — POST /iSpindel (data reception from device)
// ============================================================

void handleiSpindel(ESP8266WebServer& server) {
  handleiSpindelPost(server.arg("plain"));
  sendJsonResponse(server, F("{\"status\":\"ok\"}"));
}

// ============================================================
// ISPINDELS — GET /ispindels (config + live data for admin UI)
// ============================================================

void handleiSpindels(ESP8266WebServer& server) {
  // Chunked per-item streaming (same pattern as handleFermenters)
  sendCORSHeaders(server);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"ispindels\":[");
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (i > 0) server.sendContent(",");
    JsonDocument s;
    s["index"]       = i;
    s["name"]        = g_iSpindels[i].name;
    s["id"]          = g_iSpindels[i].id;
    s["collectData"] = g_iSpindels[i].collectData;
    s["fermenter"]   = g_iSpindels[i].fermenter;
    s["unit"]        = g_iSpindels[i].unit;
    s["function"]    = g_iSpindels[i].function;
    s["tempAdjust"]  = toDisplayTempDelta(g_iSpindels[i].tempAdjust);  // a calibration offset: span, not absolute
    s["sgAdjust"]    = g_iSpindels[i].sgAdjust;
    s["sg"]          = g_iSpindels[i].sg;
    s["temperature"] = toDisplayTemp(g_iSpindels[i].temperature);
    s["battery"]     = g_iSpindels[i].battery;
    s["rssi"]        = g_iSpindels[i].rssi;
    s["angle"]       = g_iSpindels[i].angle;
    s["velocity"]    = g_iSpindels[i].velocity;
    s["corrGravity"]  = g_iSpindels[i].corrGravity;
    s["runTime"]      = g_iSpindels[i].runTime;
    s["gravityUnit"]  = g_iSpindels[i].gravityUnit;
    s["minutesSince"] = g_iSpindels[i].lastSeen == 0
                          ? 0xFFFF
                          : (uint32_t)(millis() - g_iSpindels[i].lastSeen) / 60000UL;
    char buf[512];
    size_t n = serializeJson(s, buf, sizeof(buf));
    server.sendContent(buf, n);
  }
  server.sendContent("]}");
  server.sendContent("");  // end chunked transfer
}

// ============================================================
// ISPINDEL CONFIG — POST /ispindel/config
// Body: {"index":0,"collectData":true,"fermenter":0}
// Clear: {"index":0,"_clear":true}
// ============================================================

void handleiSpindelConfigPost(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int idx = getValidIndex(server, doc, "index", MAX_ISPINDELS, F("Invalid index (0-3)"));
  if (idx < 0) return;

  if (doc["_clear"] | false) {
    strlcpy(g_iSpindels[idx].name, "None", sizeof(g_iSpindels[idx].name));
    g_iSpindels[idx].id[0]       = '\0';
    g_iSpindels[idx].collectData = false;
    g_iSpindels[idx].fermenter   = PROBE_UNASSIGNED;
    g_iSpindels[idx].unit        = 0;
    g_iSpindels[idx].function    = PROBE_FN_BEER;
    g_iSpindels[idx].tempAdjust  = 0.0f;
    g_iSpindels[idx].sgAdjust    = 0.0f;
    g_iSpindels[idx].sg             = 0.0f;
    g_iSpindels[idx].temperature    = 0.0f;
    g_iSpindels[idx].battery        = 0.0f;
    g_iSpindels[idx].rssi           = 0;
    g_iSpindels[idx].angle          = 0.0f;
    g_iSpindels[idx].velocity       = 0.0f;
    g_iSpindels[idx].corrGravity    = 0.0f;
    g_iSpindels[idx].runTime        = 0.0f;
    g_iSpindels[idx].gravityUnit[0] = '\0';
    saveiSpindelConfig();
    sendOk(server, F("iSpindel slot cleared"));
    return;
  }

  if (!doc["collectData"].isNull()) g_iSpindels[idx].collectData = doc["collectData"];
  if (!doc["fermenter"].isNull())   { uint8_t v = doc["fermenter"]; if (v < MAX_FERMENTERS || v == PROBE_UNASSIGNED) g_iSpindels[idx].fermenter = v; }
  if (!doc["unit"].isNull())        g_iSpindels[idx].unit        = doc["unit"];
  if (!doc["function"].isNull())    { uint8_t v = doc["function"]; g_iSpindels[idx].function = (v == PROBE_FN_BEER) ? PROBE_FN_BEER : PROBE_UNASSIGNED; }
  if (!doc["tempAdjust"].isNull())  g_iSpindels[idx].tempAdjust  = toCelsiusTempDelta((float)doc["tempAdjust"]);
  if (!doc["sgAdjust"].isNull())    g_iSpindels[idx].sgAdjust    = doc["sgAdjust"];
  // Normalize collectData from fermenter when client didn't send it explicitly.
  // Lets the UI omit the toggle entirely; legacy 'collectData=false + fermenter assigned'
  // configs self-heal on first UI save. External scripts that POST collectData win.
  if (doc["collectData"].isNull() && !doc["fermenter"].isNull()) {
    g_iSpindels[idx].collectData = (g_iSpindels[idx].fermenter != PROBE_UNASSIGNED);
  }
  saveiSpindelConfig();
  sendOk(server, F("iSpindel updated"));
}

// ============================================================
// WIFI RESET PAGE + ACTION
// ============================================================

// Served from PROGMEM via sendContent_P — see OTA_PAGE note above.
static const char CONFIG_PAGE[] PROGMEM =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OurBrewbot - Reset WiFi</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px}"
    "h2{color:#e94560;margin:0 0 16px}"
    ".card{background:#16213e;border:1px solid #333;border-radius:6px;padding:16px;max-width:480px}"
    "p{margin-bottom:12px;font-size:14px;color:#aaa;line-height:1.5}"
    ".warn{color:#ffd166}"
    ".btn{background:#e94560;color:#fff;border:none;padding:8px 16px;border-radius:4px;cursor:pointer;font-size:13px}"
    ".btn:hover{background:#c73650}"
    "a{color:#53d8fb;text-decoration:none;font-size:13px}"
    "</style></head><body>"
    "<h2>Reset WiFi Settings</h2>"
    "<div class='card'>"
    "<p>WiFi credentials are managed by WiFiManager. This action clears the saved WiFi settings, reboots the controller, and reopens the setup portal on the next boot.</p>"
    "<p class='warn'>Are you sure? The device will disconnect from the network until you reconnect it through the setup portal.</p>"
    "<button class='btn' onclick='resetWiFi()'>Reset WiFi And Reboot</button>"
    "</div>"
    "<p id='msg' style='margin-top:12px;color:#aaa'></p>"
    "<p style='margin-top:12px'><a href='/admin'>&#8592; Back to Admin</a></p>"
    "<script>"
    "function resetWiFi(){"
    "if(!confirm('Are you sure you want to clear WiFi settings and reboot into the setup portal?'))return;"
    "var msg=document.getElementById('msg');"
    "msg.textContent='Resetting WiFi settings and rebooting...';"
    "fetch('/wifi/reset',{method:'POST'})"
    ".then(function(r){return r.json()})"
    ".then(function(d){msg.textContent=d.msg||'Rebooting into setup portal...'})"
    ".catch(function(e){msg.textContent='Error: '+e});"
    "}"
    "</script>"
    "</body></html>";

void handleConfigPage(ESP8266WebServer& server) {
  server.setContentLength(strlen_P(CONFIG_PAGE));
  server.send(200, "text/html", "");
  server.sendContent_P(CONFIG_PAGE);
}

void handleWiFiReset(ESP8266WebServer& server) {
  sendOk(server, F("WiFi settings cleared. Rebooting into setup portal."));
  delay(250);
  WiFi.persistent(true);
  WiFi.disconnect(true);
  resetWiFiConfig();
  ESP.restart();
}

// ============================================================
// PROBE CONFIG — POST /probes
// ============================================================

void handleProbePost(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int idx = getValidIndex(server, doc, "index", MAX_PROBES, F("Invalid probe index"));
  if (idx < 0) return;
  if (strlen(g_probes[idx].address) == 0) {
    sendErr(server, 400, F("Invalid probe index"));
    return;
  }
  if (!doc["function"].isNull())   g_probes[idx].function   = doc["function"];
  if (!doc["fermenter"].isNull())  { uint8_t v = doc["fermenter"]; if (v < MAX_FERMENTERS || v == PROBE_UNASSIGNED) g_probes[idx].fermenter = v; }
  if (!doc["tempAdjust"].isNull()) g_probes[idx].tempAdjust = toCelsiusTempDelta((float)doc["tempAdjust"]);
  saveProbeConfig();
  sendOk(server, F("Probe updated"));
}

// ============================================================
// SMART PLUGS — GET /smartplugs (all 10 slots)
// ============================================================

void handleSmartPlugs(ESP8266WebServer& server) {
  // Chunked per-item streaming (same pattern as handleFermenters)
  sendCORSHeaders(server);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"plugs\":[");
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    if (i > 0) server.sendContent(",");
    JsonDocument p;
    p["index"]        = i;
    p["manufacturer"] = g_smartPlugs[i].manufacturer;
    p["model"]        = g_smartPlugs[i].model;
    p["onCode"]       = g_smartPlugs[i].onCode;
    p["offCode"]      = g_smartPlugs[i].offCode;
    p["protocol"]     = g_smartPlugs[i].protocol;
    p["bits"]         = g_smartPlugs[i].bits;
    p["delay"]        = g_smartPlugs[i].delayLength;
    p["codeset"]      = g_smartPlugs[i].codeset;
    p["function"]     = g_smartPlugs[i].function;
    p["fermenter"]    = g_smartPlugs[i].fermenter;
    p["state"]        = getPlugState(i);
    char buf[384];
    size_t n = serializeJson(p, buf, sizeof(buf));
    server.sendContent(buf, n);
  }
  server.sendContent("]}");
  server.sendContent("");  // end chunked transfer
}

// ============================================================
// SMART PLUG CONFIG — POST /smartplug
// ============================================================

void handleSmartPlugPost(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int idx = getValidIndex(server, doc, "index", MAX_SMART_PLUGS, F("Invalid plug index"));
  if (idx < 0) return;
  if (!doc["function"].isNull())     { uint8_t v = doc["function"];  if (v <= 9 || v == PLUG_FN_UNASSIGNED)           g_smartPlugs[idx].function    = v; }
  if (!doc["fermenter"].isNull())    { uint8_t v = doc["fermenter"]; if (v < MAX_FERMENTERS || v == PROBE_UNASSIGNED) g_smartPlugs[idx].fermenter   = v; }
  if (doc["manufacturer"].is<const char*>()) strlcpy(g_smartPlugs[idx].manufacturer, doc["manufacturer"].as<const char*>(), sizeof(g_smartPlugs[0].manufacturer));
  if (doc["model"].is<const char*>())        strlcpy(g_smartPlugs[idx].model,         doc["model"].as<const char*>(),         sizeof(g_smartPlugs[0].model));
  if (!doc["onCode"].isNull())       g_smartPlugs[idx].onCode      = doc["onCode"];
  if (!doc["offCode"].isNull())      g_smartPlugs[idx].offCode     = doc["offCode"];
  if (!doc["protocol"].isNull())     { uint8_t v = doc["protocol"]; if (v >= 1)              g_smartPlugs[idx].protocol    = v; }
  if (!doc["bits"].isNull())         { uint8_t v = doc["bits"];     if (v >= 1 && v <= 32)   g_smartPlugs[idx].bits        = v; }
  if (!doc["delay"].isNull())        g_smartPlugs[idx].delayLength = doc["delay"];
  if (!doc["codeset"].isNull())      g_smartPlugs[idx].codeset     = doc["codeset"];
  saveSmartPlugConfig();
  sendOk(server, F("Plug updated"));
}

// ============================================================
// SMART PLUG TEST — POST /smartplug/test
// ============================================================

void handleSmartPlugTest(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int idx = getValidIndex(server, doc, "index", MAX_SMART_PLUGS, F("Invalid plug index"));
  if (idx < 0) return;
  SmartPlugConfig& plug = g_smartPlugs[idx];
  bool on = (strcmp(doc["action"] | "on", "on") == 0);
  uint32_t code = on ? plug.onCode : plug.offCode;
  if (code == 0) {
    sendErr(server, 400, F("No code configured"));
    return;
  }
  logMsg("[PLUG] Test plug %d %s: code=%u (0x%06X), bits=%d, delay=%d, proto=%d",
    idx, on ? "ON" : "OFF", code, code, plug.bits, plug.delayLength, plug.protocol);

  ESP.wdtFeed();  // RF transmit bit-bangs for ~1-2 s inside this handler
  rfTransmit(code, plug.bits, plug.delayLength, plug.protocol);
  sendOk(server, F("RF code sent"));
}



// ============================================================
// RF SNIFFER — capture codes from remotes via MX-RM-5V receiver
// ============================================================

static bool s_sniffActive = false;

void handleRFSniffPoll(ESP8266WebServer& server) {
  // Start receiver if not already running
  if (!s_sniffActive) {
    g_rcSwitch.enableReceive(digitalPinToInterrupt(PIN_RF_RECEIVE));
    s_sniffActive = true;
  }

  JsonDocument doc;
  if (g_rcSwitch.available()) {
    unsigned long val = g_rcSwitch.getReceivedValue();
    doc["received"] = true;
    doc["code"] = val;
    char hex[12];
    snprintf(hex, sizeof(hex), "0x%06lX", val);
    doc["hex"] = hex;
    doc["bits"] = (int)g_rcSwitch.getReceivedBitlength();
    doc["delay"] = (int)g_rcSwitch.getReceivedDelay();
    doc["protocol"] = (int)g_rcSwitch.getReceivedProtocol();
    logMsg("[RF] Received: code=%lu (%s), %d-bit, delay=%d, proto=%d",
      val, hex, g_rcSwitch.getReceivedBitlength(),
      g_rcSwitch.getReceivedDelay(), g_rcSwitch.getReceivedProtocol());
    g_rcSwitch.resetAvailable();
  } else {
    doc["received"] = false;
  }
  sendJsonDoc(server, doc);
}

// Served from PROGMEM via sendContent_P — was a ~1.9 KB heap String per request.
static const char RF_SNIFF_PAGE[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>RF Sniffer</title>"
    "<style>body{background:#1a1a2e;color:#e0e0e0;font-family:system-ui,sans-serif;padding:16px}"
    "h2{color:#e94560}a{color:#53d8fb}"
    ".info{background:#16213e;padding:12px;border-radius:6px;margin:12px 0;font-size:14px}"
    ".log{background:#0a0a1a;border:1px solid #333;border-radius:6px;padding:12px;margin:12px 0;"
    "font-family:monospace;font-size:13px;min-height:200px;max-height:400px;overflow-y:auto}"
    ".entry{padding:4px 0;border-bottom:1px solid #222}"
    ".code{color:#53d8fb;font-weight:bold;font-size:15px}"
    ".meta{color:#888;font-size:12px}"
    ".btn{display:inline-block;background:#e94560;color:#fff;padding:8px 16px;border-radius:4px;"
    "cursor:pointer;border:none;font-size:14px;margin:4px}"
    ".btn:hover{background:#c73650}"
    ".btn-off{background:#333;color:#888}"
    ".status{font-size:14px;margin:8px 0}"
    ".on{color:#4f4}.off{color:#f44}"
    "</style></head><body>"
    "<h2>RF Code Sniffer</h2>"
    "<div class='info'>"
    "<p>Press buttons on your Dial remote. Received codes appear below.</p>"
    "<p>You can also press 'Test Transmit' to verify the transmitter — "
    "if working, the receiver will pick up the transmitted code.</p></div>"
    "<div class='status'>Receiver on GPIO14 (D5): <span id='st' class='on'>Listening...</span></div>"
    "<button class='btn' onclick='testTx()'>Test Transmit (GPIO4)</button>"
    "<button class='btn' onclick='clearLog()'>Clear Log</button>"
    "<div id='log' class='log'><div style='color:#888'>Waiting for RF signals...</div></div>"
    "<script>"
    "var log=document.getElementById('log'),first=true;"
    "function addEntry(d){"
    "if(first){log.innerHTML='';first=false;}"
    "var e=document.createElement('div');e.className='entry';"
    "e.innerHTML='<span class=\"code\">'+d.code+' ('+d.hex+')</span> '"
    "+'<span class=\"meta\">'+d.bits+'-bit, delay='+d.delay+'&mu;s, proto='+d.protocol+'</span>';"
    "log.insertBefore(e,log.firstChild);}"
    "function poll(){"
    "fetch('/rf/sniff/poll').then(function(r){return r.json()}).then(function(d){"
    "if(d.received)addEntry(d);"
    "}).catch(function(){});}"
    "function clearLog(){log.innerHTML='<div style=\"color:#888\">Waiting for RF signals...</div>';first=true;}"
    "setInterval(poll,500);"
    "</script>"
    "<br><a href='/admin'>Back to Admin</a>"
    "</body></html>";

void handleRFSniff(ESP8266WebServer& server) {
  // Stop receiver when leaving (will restart on poll)
  s_sniffActive = false;

  server.setContentLength(strlen_P(RF_SNIFF_PAGE));
  server.send(200, "text/html", "");
  server.sendContent_P(RF_SNIFF_PAGE);
}

// ============================================================
// BLE SNIFFER — AT command debug console for HM-10 module
// ============================================================

// Ring buffer for BLE serial data between polls
#define BLE_SNIFF_BUF_SIZE 512
static char  s_bleSniffBuf[BLE_SNIFF_BUF_SIZE];
static int   s_bleSniffLen = 0;
static unsigned long s_bleSniffLastPoll = 0;

// Called from main loop to auto-deactivate sniff mode after 10s of no polls
void checkBLESniffTimeout() {
  if (g_bleSniffActive && s_bleSniffLastPoll > 0 && millis() - s_bleSniffLastPoll > 10000) {
    g_bleSniffActive = false;
    s_bleSniffLastPoll = 0;
    s_bleSniffLen = 0;
  }
}

void handleBLESniffPoll(ESP8266WebServer& server) {
  // Activate sniff mode (pauses Tilt scanning)
  g_bleSniffActive = true;
  s_bleSniffLastPoll = millis();

  // Read any available bytes from BLE serial into ring buffer
  while (g_bleSerial.available() && s_bleSniffLen < BLE_SNIFF_BUF_SIZE - 1) {
    s_bleSniffBuf[s_bleSniffLen++] = (char)g_bleSerial.read();
  }
  s_bleSniffBuf[s_bleSniffLen] = '\0';

  JsonDocument doc;
  doc["data"] = s_bleSniffBuf;
  doc["len"]  = s_bleSniffLen;

  // Clear buffer after sending
  s_bleSniffLen = 0;

  sendJsonDoc(server, doc);
}

void handleBLESniffSend(ESP8266WebServer& server) {
  g_bleSniffActive = true;
  s_bleSniffLastPoll = millis();

  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  const char* cmd = doc["cmd"] | "";
  if (strlen(cmd) == 0) {
    sendErr(server, 400, F("Empty command"));
    return;
  }
  if (strlen(cmd) > 64) {
    sendErr(server, 400, F("Command too long (max 64 chars)"));
    return;
  }

  g_bleSerial.print(cmd);
  logMsg("[BLE SNIFF] Sent: %s", cmd);

  JsonDocument resp;
  resp["status"] = "ok";
  resp["cmd"]    = cmd;
  sendJsonDoc(server, resp);
}

// Served from PROGMEM via sendContent_P — was a ~2.7 KB heap String per request.
static const char BLE_SNIFF_PAGE[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>BLE AT Console</title>"
    "<style>body{background:#1a1a2e;color:#e0e0e0;font-family:system-ui,sans-serif;padding:16px}"
    "h2{color:#e94560}a{color:#53d8fb}"
    ".info{background:#16213e;padding:12px;border-radius:6px;margin:12px 0;font-size:14px}"
    ".log{background:#0a0a1a;border:1px solid #333;border-radius:6px;padding:12px;margin:12px 0;"
    "font-family:monospace;font-size:13px;min-height:200px;max-height:400px;overflow-y:auto;"
    "white-space:pre-wrap;word-break:break-all}"
    ".sent{color:#e94560;font-weight:bold}"
    ".recv{color:#53d8fb}"
    ".btn{display:inline-block;background:#e94560;color:#fff;padding:8px 16px;border-radius:4px;"
    "cursor:pointer;border:none;font-size:14px;margin:4px}"
    ".btn:hover{background:#c73650}"
    ".btn-at{background:#0f3460;color:#53d8fb;border:1px solid #53d8fb;padding:6px 12px;"
    "border-radius:4px;cursor:pointer;font-size:13px;margin:2px}"
    ".btn-at:hover{background:#1a4a8a}"
    ".cmd-row{display:flex;gap:8px;margin:12px 0;align-items:center}"
    ".cmd-row input{flex:1;background:#0f3460;border:1px solid #444;color:#e0e0e0;padding:8px 12px;"
    "border-radius:4px;font-family:monospace;font-size:14px}"
    ".status{font-size:14px;margin:8px 0}"
    ".on{color:#4f4}.off{color:#f44}"
    "</style></head><body>"
    "<h2>BLE AT Command Console</h2>"
    "<div class='info'>"
    "<p>Send AT commands to the HM-10 Bluetooth module. Tilt scanning is paused while this page is open.</p>"
    "<p>Common commands: AT (test), AT+VERS? (firmware), AT+ROLE? (role), AT+DISI? (iBeacon scan ~3s)</p></div>"
    "<div class='status'>HM-10 on GPIO12/13 (D6/D7): <span id='st' class='on'>Connected</span></div>"
    "<div style='margin:8px 0'>"
    "<button class='btn-at' onclick=\"sendCmd('AT')\">AT</button>"
    "<button class='btn-at' onclick=\"sendCmd('AT+VERS?')\">AT+VERS?</button>"
    "<button class='btn-at' onclick=\"sendCmd('AT+ROLE?')\">AT+ROLE?</button>"
    "<button class='btn-at' onclick=\"sendCmd('AT+IMME?')\">AT+IMME?</button>"
    "<button class='btn-at' onclick=\"sendCmd('AT+DISI?')\">AT+DISI?</button>"
    "<button class='btn-at' onclick=\"sendCmd('AT+ADDR?')\">AT+ADDR?</button>"
    "<button class='btn-at' onclick=\"sendCmd('AT+BAUD?')\">AT+BAUD?</button>"
    "</div>"
    "<div class='cmd-row'>"
    "<input type='text' id='cmd' placeholder='Type AT command...' onkeydown='if(event.key==\"Enter\")sendManual()'>"
    "<button class='btn' onclick='sendManual()'>Send</button>"
    "<button class='btn' onclick='clearLog()' style='background:#333'>Clear</button>"
    "</div>"
    "<div id='log' class='log'><span style='color:#888'>Waiting for data...</span></div>"
    "<script>"
    "var log=document.getElementById('log'),first=true;"
    "function appendLog(html){"
    "if(first){log.innerHTML='';first=false;}"
    "log.innerHTML+=html;log.scrollTop=log.scrollHeight;}"
    "function sendCmd(c){"
    "appendLog('<div class=\"sent\">&gt; '+c+'</div>');"
    "fetch('/ble/sniff/send',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({cmd:c})}).catch(function(e){appendLog('<div style=\"color:#f44\">Send error: '+e+'</div>')});}"
    "function sendManual(){var i=document.getElementById('cmd');if(i.value){sendCmd(i.value);i.value='';}}"
    "function poll(){"
    "fetch('/ble/sniff/poll').then(function(r){return r.json()}).then(function(d){"
    "if(d.len>0){appendLog('<span class=\"recv\">'+d.data.replace(/</g,'&lt;').replace(/>/g,'&gt;')+'</span>');}"
    "}).catch(function(){});}"
    "function clearLog(){log.innerHTML='<span style=\"color:#888\">Waiting for data...</span>';first=true;}"
    "setInterval(poll,300);"
    "</script>"
    "<br><a href='/admin'>Back to Admin</a>"
    "</body></html>";

void handleBLESniff(ESP8266WebServer& server) {
  // Reset sniff state
  s_bleSniffLen = 0;
  g_bleSniffActive = true;
  s_bleSniffLastPoll = millis();

  server.setContentLength(strlen_P(BLE_SNIFF_PAGE));
  server.send(200, "text/html", "");
  server.sendContent_P(BLE_SNIFF_PAGE);
}

// ============================================================
// BREW SERVICES — GET/POST /brewservices
// ============================================================

void handleBrewServices(ESP8266WebServer& server) {
  // Index: 0=Brewer's Friend, 1=Brewfather
  JsonDocument doc;
  JsonArray arr = doc["services"].to<JsonArray>();
  const char* names[] = {"Brewer's Friend", "Brewfather"};
  for (int i = 0; i < MAX_BREW_SERVICES; i++) {
    JsonObject s = arr.add<JsonObject>();
    s["index"]     = i;
    s["name"]      = names[i];
    s["enabled"]    = g_brewServices[i].enabled;
    s["serviceId"]  = g_brewServices[i].serviceId;
    s["deviceName"] = g_brewServices[i].deviceName;
  }
  sendJsonDoc(server, doc);
}

void handleBrewServicesPost(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int idx = getValidIndex(server, doc, "index", MAX_BREW_SERVICES, F("Invalid service index"));
  if (idx < 0) return;
  if (!doc["enabled"].isNull())    g_brewServices[idx].enabled = doc["enabled"];
  if (doc["serviceId"].is<const char*>())  strlcpy(g_brewServices[idx].serviceId,  doc["serviceId"].as<const char*>(),  sizeof(g_brewServices[0].serviceId));
  if (doc["deviceName"].is<const char*>()) strlcpy(g_brewServices[idx].deviceName, doc["deviceName"].as<const char*>(), sizeof(g_brewServices[0].deviceName));
  saveBrewServiceConfig();
  sendOk(server, F("Brew service saved"));
}

// ============================================================
// BREW SERVICE TEST — POST /brewservices/test
// ============================================================

void handleBrewServiceTest(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int idx = getValidIndex(server, doc, "index", MAX_BREW_SERVICES, F("Invalid service index"));
  if (idx < 0) return;
  if (strlen(g_brewServices[idx].serviceId) == 0) {
    sendErr(server, 400, F("No service ID configured"));
    return;
  }
  ESP.wdtFeed();  // synchronous outbound HTTP POST, bounded by the 5 s HTTP timeout
  int httpCode = testBrewService(idx);
  String resp = "{\"status\":";
  resp += (httpCode == 200) ? "\"ok\"" : "\"error\"";
  resp += ",\"msg\":\"HTTP ";
  resp += httpCode;
  resp += (httpCode == 200) ? " — OK" : " — Failed";
  resp += "\",\"httpCode\":";
  resp += httpCode;
  resp += "}";
  sendJsonResponse(server, resp);
}

// ============================================================
// MQTT CONFIG — GET/POST /mqtt, POST /mqtt/test
// ============================================================

void handleMqttConfig(ESP8266WebServer& server) {
  JsonDocument doc;
  doc["enabled"]      = g_mqttConfig.enabled;
  doc["haDiscovery"]  = g_mqttConfig.haDiscovery;
  doc["allowControl"] = g_mqttConfig.allowControl;
  doc["logEnabled"]   = g_mqttConfig.logEnabled;
  doc["host"]         = g_mqttConfig.host;
  doc["port"]        = g_mqttConfig.port;
  doc["username"]    = g_mqttConfig.username;
  doc["password"]    = g_mqttConfig.password;
  doc["baseTopic"]   = g_mqttConfig.baseTopic;
  sendJsonDoc(server, doc);
}

void handleMqttConfigPost(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  if (!doc["enabled"].isNull())          g_mqttConfig.enabled = doc["enabled"];
  if (doc["host"].is<const char*>())     strlcpy(g_mqttConfig.host,     doc["host"].as<const char*>(),     sizeof(g_mqttConfig.host));
  if (!doc["port"].isNull()) {
    uint32_t port = doc["port"];
    if (port < 1 || port > 65535) {
      sendErr(server, 400, F("port out of range (1-65535)"));
      return;
    }
    g_mqttConfig.port = (uint16_t)port;
  }
  if (doc["username"].is<const char*>()) strlcpy(g_mqttConfig.username, doc["username"].as<const char*>(), sizeof(g_mqttConfig.username));
  if (doc["password"].is<const char*>()) strlcpy(g_mqttConfig.password, doc["password"].as<const char*>(), sizeof(g_mqttConfig.password));
  if (doc["baseTopic"].is<const char*>()) {
    const char* bt = doc["baseTopic"].as<const char*>();
    if (bt[0] == '\0') {
      sendErr(server, 400, F("baseTopic cannot be empty"));
      return;
    }
    strlcpy(g_mqttConfig.baseTopic, bt, sizeof(g_mqttConfig.baseTopic));
  }
  if (!doc["haDiscovery"].isNull()) {
    bool newHa = doc["haDiscovery"];
    if (g_mqttConfig.haDiscovery && !newHa) cleanupAllHaDiscovery();
    g_mqttConfig.haDiscovery = newHa;
  }
  if (!doc["allowControl"].isNull()) {
    g_mqttConfig.allowControl = doc["allowControl"];
    mqttApplyControlSubscription();  // takes effect immediately on live connection
  }
  if (!doc["logEnabled"].isNull()) g_mqttConfig.logEnabled = doc["logEnabled"];
  saveMqttConfig();
  sendOk(server, F("MQTT config saved"));
}

void handleMqttTest(ESP8266WebServer& server) {
  ESP.wdtFeed();  // blocking broker connect, bounded by the 5 s socket timeout
  bool ok = testMqtt();
  if (ok) {
    sendOk(server, F("MQTT connected and test message published"));
  } else {
    sendErr(server, 200, F("MQTT connection failed — check host/port/credentials"));  // 200: JS reads d.status
  }
}

void handleMqttDiscover(ESP8266WebServer& server) {
  ESP.wdtFeed();  // discovery burst takes ~2-4 s (yields per entity)
  if (forcePublishAllHaDiscovery()) {
    sendOk(server, F("HA discovery published"));
  } else {
    sendErr(server, 200, F("MQTT not connected — check broker settings"));  // 200: JS reads d.status
  }
}

// ============================================================
// PROFILES — GET /profiles
// ============================================================

void handleProfiles(ESP8266WebServer& server) {
  // Stream profiles using chunked transfer to avoid a single ~8KB heap spike.
  // One profile at a time (~1536-byte doc) is built and freed each iteration.
  sendCORSHeaders(server);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"profiles\":[");
  for (int p = 0; p < MAX_PROFILES; p++) {
    if (p > 0) server.sendContent(",");
    JsonDocument doc;
    buildProfileJson(doc, p);
    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    server.sendContent(buf, n);
  }
  server.sendContent("],\"stepTypes\":[");
  for (int t = 0; t <= 9; t++) {
    if (t > 0) server.sendContent(",");
    JsonDocument tDoc;
    tDoc["id"]   = t;
    tDoc["name"] = getStepTypeDescription(t);
    char buf[96];
    size_t n = serializeJson(tDoc, buf, sizeof(buf));
    server.sendContent(buf, n);
  }
  server.sendContent("]}");
  server.sendContent("");  // end chunked transfer
}

// ============================================================
// PROFILE SAVE — POST /profile
// ============================================================

void handleProfilePost(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int idx = getValidIndex(server, doc, "index", MAX_PROFILES, F("Invalid profile index"));
  if (idx < 0) return;
  if (doc["name"].is<const char*>()) {
    strlcpy(g_profiles[idx].profileName, doc["name"].as<const char*>(), sizeof(g_profiles[0].profileName));
  }
  if (!doc["steps"].isNull()) {
    JsonArray steps = doc["steps"];
    uint8_t base = idx * MAX_STEPS_PER_PROFILE;
    for (int s = 0; s < MAX_STEPS_PER_PROFILE; s++) {
      if (s < (int)steps.size()) {
        JsonObject st = steps[s];
        uint8_t stepType  = st["stepType"] | 0;
        float   days      = st["days"]     | 0.0f;
        float   startTemp = st["startTemp"] | 0.0f;
        float   endTemp   = st["endTemp"]   | 0.0f;
        if (stepType > 9) stepType = 0;
        if (days < 0.0f)  days    = 0.0f;
        // Step temperatures arrive in the user's display unit, but the WebUI
        // pads unused slots with a literal all-zero step and both the firmware
        // (isStepEmpty) and the WebUI detect an unused slot by testing for
        // zero. Converting a padded zero would store -17.8 C, so the slot would
        // no longer count as empty and a running profile would never finish.
        // Store the sentinel verbatim; convert only real step temperatures.
        bool blankStep = (stepType == 0 && days == 0.0f && startTemp == 0.0f && endTemp == 0.0f);
        g_profileSteps[base + s].stepType  = stepType;
        g_profileSteps[base + s].startTemp = blankStep ? 0.0f : toCelsius(startTemp);
        g_profileSteps[base + s].endTemp   = blankStep ? 0.0f : toCelsius(endTemp);
        g_profileSteps[base + s].sgTrigger = st["sgTrigger"] | 0.0f;
        g_profileSteps[base + s].days      = days;
        g_profileSteps[base + s].stepNo    = s;
      } else {
        // Clear remaining steps
        memset(&g_profileSteps[base + s], 0, sizeof(ProfileStep));
      }
    }
  }
  saveProfileConfig();
  saveProfileSteps();
  sendOk(server, F("Profile saved"));
}

// ============================================================
// FERMENTER PROFILE CONTROL — POST /fermenter/profile
// Actions: start, stop, pause, next, prev
// ============================================================

void handleFermenterProfile(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int idx = getValidIndex(server, doc, "Fermenter", MAX_FERMENTERS, F("Invalid fermenter"));
  if (idx < 0) return;
  const char* action = doc["action"] | "";

  if (strcmp(action, "start") == 0) {
    int profIdx = doc["ProfileIndex"] | 0;
    if (profIdx < 1 || profIdx > MAX_PROFILES) {
      sendErr(server, 400, F("Invalid profile (1-4)"));
      return;
    }
    // Check profile has at least one step
    if (countProfileSteps(profIdx - 1) == 0) {
      sendErr(server, 400, F("Profile has no steps"));
      return;
    }
    startProfile(idx, profIdx);
    sendOk(server, F("Profile started"));
  } else if (strcmp(action, "stop") == 0) {
    stopProfile(idx);
    sendOk(server, F("Profile stopped"));
  } else if (strcmp(action, "pause") == 0) {
    pauseProfile(idx);
    sendOk(server, F("Profile paused"));
  } else if (strcmp(action, "resume") == 0) {
    if (g_fermenters[idx].profileNo == 0 || g_fermenters[idx].profileRunning) {
      sendErr(server, 400, F("No paused profile to resume"));
      return;
    }
    resumeProfile(idx);
    sendOk(server, F("Profile resumed"));
  } else if (strcmp(action, "next") == 0) {
    if (nextProfileStep(idx)) {
      sendOk(server, F("Advanced to next step"));
    } else if (!g_fermenters[idx].profileRunning && g_fermenters[idx].profileNo > 0) {
      sendOk(server, F("Profile complete"));
    } else {
      sendErr(server, 400, F("Profile not running"));
    }
  } else if (strcmp(action, "prev") == 0) {
    if (prevProfileStep(idx)) {
      sendOk(server, F("Moved to previous step"));
    } else {
      sendOk(server, F("Already on first step"));
    }
  } else {
    sendErr(server, 400, F("Unknown action"));
  }
}

// ============================================================
// FILESYSTEM BROWSER
// ============================================================

void handleFsFiles(ESP8266WebServer& server) {
  JsonDocument doc;
  JsonArray arr = doc["files"].to<JsonArray>();
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    JsonObject f = arr.add<JsonObject>();
    f["name"] = dir.fileName();
    f["size"] = (unsigned int)dir.fileSize();
  }
  sendJsonDoc(server, doc);
}

void handleFsFile(ESP8266WebServer& server) {
  String name = server.arg("name");
  // Path traversal guard
  if (name.length() == 0 || name.indexOf("..") >= 0) {
    server.send(400, "text/plain", "Bad request");
    return;
  }
  // Ensure path starts with /
  if (!name.startsWith("/")) {
    name = "/" + name;
  }
  if (!LittleFS.exists(name)) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  File f = LittleFS.open(name, "r");
  if (!f) {
    server.send(500, "text/plain", "Open failed");
    return;
  }
  sendCORSHeaders(server);
  // Stream straight from LittleFS — readString() held the whole file in heap twice
  server.streamFile(f, "text/plain");
  f.close();
}

void handleFsFileSave(ESP8266WebServer& server) {
  static const char* const ALLOWED[] = {
    "/jsonGlobal.txt", "/jsonFermenter.txt", "/jsonProbe.txt",
    "/jsonSmartPlugs.txt", "/jsonProfile.txt", "/jsonProfileSteps.txt",
    "/jsonTilt.txt", "/jsoniSpindel.txt",
    "/jsonBrewServices.txt", "/jsonMqtt.txt", "/jsonSyslog.txt"
  };
  String name = server.arg("name");
  bool ok = false;
  for (const char* allow : ALLOWED) { if (name == allow) { ok = true; break; } }
  if (!ok) { server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Not allowed\"}"); return; }

  String body = server.arg("plain");
  if (body.length() == 0) { server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Empty body\"}"); return; }

  // Validate JSON syntax before persisting — a corrupt file here is parsed at
  // next boot. The empty filter makes the parser walk the whole input without
  // materialising it, so even the largest file validates with ~no heap.
  {
    JsonDocument filter;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
      char msg[96];
      snprintf(msg, sizeof(msg), "{\"status\":\"error\",\"msg\":\"Invalid JSON: %s\"}", err.c_str());
      sendJsonResponse(server, msg, 400);
      return;
    }
  }

  File f = LittleFS.open(name, "w");
  if (!f) { server.send(500, "application/json", "{\"status\":\"error\",\"msg\":\"Write failed\"}"); return; }
  f.print(body);
  f.close();
  sendJsonResponse(server, "{\"status\":\"ok\"}");
}

// ============================================================
// TILTS — GET /tilts
// Returns all Tilt colour slots that are configured or have been seen.
// ============================================================

void handleTilts(ESP8266WebServer& server) {
  JsonDocument doc;
  JsonArray arr = doc["tilts"].to<JsonArray>();
  for (int i = 0; i < MAX_TILTS; i++) {
    // Include slots that are configured (colour set) or actively seen
    if (g_tilts[i].colour == PROBE_UNASSIGNED && !g_tilts[i].active) continue;
    JsonObject t = arr.add<JsonObject>();
    t["colour"]      = i;
    t["colourName"]  = getTiltColourName(i);
    t["function"]    = g_tilts[i].function;
    t["fermenter"]   = g_tilts[i].fermenter;
    t["tempAdjust"]  = toDisplayTempDelta(g_tilts[i].tempAdjust);  // a calibration offset: span, not absolute
    t["sgAdjust"]    = g_tilts[i].sgAdjust;
    t["mbb"]         = g_tilts[i].mbb;
    t["active"]      = g_tilts[i].active;
    t["isPro"]       = g_tilts[i].isPro;
    t["sg"]          = g_tilts[i].sg;
    t["temperature"] = toDisplayTemp(g_tilts[i].temperature);
  }
  sendJsonDoc(server, doc);
}

// ============================================================
// TILT CONFIG — POST /tilt
// Body: {"colour":0,"function":4,"fermenter":0,"tempAdjust":0.0,"sgAdjust":0.0}
// ============================================================

void handleTiltPost(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  int colour = getValidIndex(server, doc, "colour", MAX_TILTS, F("Invalid colour index (0-7)"));
  if (colour < 0) return;

  // Clear flag: remove this Tilt from persisted config
  if (doc["_clear"] | false) {
    g_tilts[colour].colour    = PROBE_UNASSIGNED;
    g_tilts[colour].function  = PROBE_UNASSIGNED;
    g_tilts[colour].fermenter = PROBE_UNASSIGNED;
    g_tilts[colour].tempAdjust = 0.0f;
    g_tilts[colour].sgAdjust   = 0.0f;
    g_tilts[colour].mbb        = 0;
    saveTiltConfig();
    sendOk(server, F("Tilt slot cleared"));
    return;
  }

  // Mark the slot as configured so saveTiltConfig() includes it
  g_tilts[colour].colour = (uint8_t)colour;
  if (!doc["function"].isNull())   g_tilts[colour].function   = doc["function"];
  if (!doc["fermenter"].isNull())  { uint8_t v = doc["fermenter"]; if (v < MAX_FERMENTERS || v == PROBE_UNASSIGNED) g_tilts[colour].fermenter = v; }
  if (!doc["tempAdjust"].isNull()) g_tilts[colour].tempAdjust = toCelsiusTempDelta((float)doc["tempAdjust"]);
  if (!doc["sgAdjust"].isNull())   g_tilts[colour].sgAdjust   = doc["sgAdjust"];
  saveTiltConfig();
  sendOk(server, F("Tilt updated"));
}

// ============================================================
// SYSLOG CONFIG — GET/POST /syslog
// ============================================================

void handleSyslogConfig(ESP8266WebServer& server) {
  JsonDocument doc;
  doc["enabled"]  = g_syslogConfig.enabled;
  doc["host"]     = g_syslogConfig.host;
  doc["port"]     = g_syslogConfig.port;
  doc["facility"] = g_syslogConfig.facility;
  doc["minLevel"] = g_syslogConfig.minLevel;
  sendJsonDoc(server, doc);
}

void handleSyslogConfigPost(ESP8266WebServer& server) {
  JsonDocument doc;
  if (!parseJsonBody(server, doc)) return;
  if (!doc["enabled"].isNull())  g_syslogConfig.enabled  = doc["enabled"];
  if (doc["host"].is<const char*>()) strlcpy(g_syslogConfig.host, doc["host"].as<const char*>(), sizeof(g_syslogConfig.host));
  if (!doc["port"].isNull()) {
    uint32_t port = doc["port"];
    if (port < 1 || port > 65535) {
      sendErr(server, 400, F("port out of range (1-65535)"));
      return;
    }
    g_syslogConfig.port = (uint16_t)port;
  }
  if (!doc["facility"].isNull()) { uint8_t v = doc["facility"]; if (v <= 23) g_syslogConfig.facility = v; }
  if (!doc["minLevel"].isNull()) { uint8_t v = doc["minLevel"]; if (v <= 7)  g_syslogConfig.minLevel = v; }
  saveSyslogConfig();
  logInit();  // re-resolve host with new config
  sendOk(server, F("Syslog config saved"));
}

// ============================================================
// 404 NOT FOUND
// ============================================================

void handleNotFound(ESP8266WebServer& server) {
  sendErr(server, 404, F("Not found"));
}
