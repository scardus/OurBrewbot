/*
 * WebAPI.cpp — REST API and web server implementation
 */

#include "WebAPI.h"
#include "Log.h"
#include "Mqtt.h"
#include "Fermenter.h"
#include "Temperatures.h"
#include "Reports.h"
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

// ============================================================
// SERVER SETUP — register all routes
// ============================================================

void setupWebServer(ESP8266WebServer& server) {
  server.on("/",              HTTP_GET,  [&server]() { handleRoot(server); });
  server.on("/controller",    HTTP_GET,  [&server]() { handleController(server); });
  server.on("/controller",    HTTP_POST, [&server]() { handleController(server); });
  server.on("/fermenters",    HTTP_GET,  [&server]() { handleFermenters(server); });
  server.on("/fermenter",     HTTP_GET,  [&server]() { handleFermenter(server); });
  server.on("/fermenter",     HTTP_POST, [&server]() { handleFermenter(server); });
  server.on("/board_info.json",HTTP_GET, [&server]() { handleBoardInfo(server); });
  server.on("/reset",         HTTP_GET,  [&server]() { handleReset(server); });
  server.on("/reboot",        HTTP_GET,  [&server]() { handleReboot(server); });
  server.on("/update",        HTTP_GET,  [&server]() { handleOTAPage(server); });
  server.on("/config",        HTTP_GET,  [&server]() { handleConfigPage(server); });
  server.on("/configMe",      HTTP_GET,  [&server]() { handleConfigMe(server); });
  server.on("/WiFi",          HTTP_GET,  [&server]() { handleConfigPage(server); });

  server.on("/iSpindel",      HTTP_POST, [&server]() { handleiSpindel(server); });

  // OTA upload handler
  server.on("/update", HTTP_POST,
    [&server]() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/html",
        "<META http-equiv=\"refresh\" content=\"15;URL=/\">"
        "Update Success! Rebooting OurBrewbot Controller...");
      delay(500);
      ESP.restart();
    },
    [&server]() { handleOTAUpload(server); }
  );

  // New convenience endpoints
  server.on("/status",        HTTP_GET,  [&server]() { handleStatus(server); });
  server.on("/probes",        HTTP_GET,  [&server]() { handleProbes(server); });
  server.on("/probes",        HTTP_POST, [&server]() { handleProbePost(server); });
  server.on("/health",        HTTP_GET,  [&server]() { handleHealth(server); });

  // Admin configuration page
  server.on("/admin",         HTTP_GET,  [&server]() { handleAdmin(server); });
  server.on("/smartplugs",    HTTP_GET,  [&server]() { handleSmartPlugs(server); });
  server.on("/smartplug",     HTTP_POST, [&server]() { handleSmartPlugPost(server); });
  server.on("/smartplug/test",HTTP_POST, [&server]() { handleSmartPlugTest(server); });
  server.on("/rf/sniff",     HTTP_GET,  [&server]() { handleRFSniff(server); });
  server.on("/rf/sniff/poll",HTTP_GET,  [&server]() { handleRFSniffPoll(server); });
  server.on("/ble/sniff",      HTTP_GET,  [&server]() { handleBLESniff(server); });
  server.on("/ble/sniff/poll", HTTP_GET,  [&server]() { handleBLESniffPoll(server); });
  server.on("/ble/sniff/send", HTTP_POST, [&server]() { handleBLESniffSend(server); });
  server.on("/brewservices",      HTTP_GET,  [&server]() { handleBrewServices(server); });
  server.on("/brewservices",      HTTP_POST, [&server]() { handleBrewServicesPost(server); });
  server.on("/brewservices/test", HTTP_POST, [&server]() { handleBrewServiceTest(server); });
  server.on("/mqtt",             HTTP_GET,  [&server]() { handleMqttConfig(server); });
  server.on("/mqtt",             HTTP_POST, [&server]() { handleMqttConfigPost(server); });
  server.on("/mqtt/test",        HTTP_POST, [&server]() { handleMqttTest(server); });
  server.on("/mqtt/discover",    HTTP_POST, [&server]() { handleMqttDiscover(server); });

  // Profile management
  server.on("/profiles",          HTTP_GET,  [&server]() { handleProfiles(server); });
  server.on("/profile",           HTTP_POST, [&server]() { handleProfilePost(server); });
  server.on("/fermenter/profile", HTTP_POST, [&server]() { handleFermenterProfile(server); });

  // Tilt hydrometer config
  server.on("/tilts", HTTP_GET,  [&server]() { handleTilts(server); });
  server.on("/tilt",  HTTP_POST, [&server]() { handleTiltPost(server); });

  // Filesystem browser
  server.on("/fs/files", HTTP_GET, [&server]() { handleFsFiles(server); });
  server.on("/fs/file",  HTTP_GET, [&server]() { handleFsFile(server); });

  server.onNotFound([&server]() { handleNotFound(server); });
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

// ============================================================
// ROOT / HOME
// ============================================================

void handleRoot(ESP8266WebServer& server) {
  String html = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OurBrewbot</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px}"
    "h2{color:#e94560;margin:0 0 16px}"
    "a{color:#53d8fb;text-decoration:none}"
    "a:hover{text-decoration:underline}"
    ".card{background:#16213e;border:1px solid #333;border-radius:6px;padding:16px;margin-bottom:12px}"
    ".card h3{color:#e94560;margin-bottom:10px;font-size:15px}"
    ".btn{display:inline-block;background:#e94560;color:#fff;padding:10px 24px;border-radius:4px;font-size:16px;font-weight:bold;margin-bottom:16px}"
    ".btn:hover{background:#c73650;text-decoration:none}"
    "ul{list-style:none;padding:0}"
    "li{padding:6px 0;border-bottom:1px solid #222;font-size:14px}"
    "li:last-child{border-bottom:none}"
    "li a{font-family:monospace;font-size:13px}"
    ".method{display:inline-block;width:40px;color:#e94560;font-weight:bold;font-family:monospace;font-size:13px}"
    ".desc{color:#aaa;margin-left:8px}"
    "</style></head><body>"
    "<h2>OurBrewbot</h2>"
    "<a class='btn' href='/admin'>Open Admin Dashboard</a>"
    "<div class='card'><h3>REST API</h3><ul>"
    "<li><span class='method'>GET</span><a href='/fermenters'>/fermenters</a><span class='desc'> &mdash; all fermenter data</span></li>"
    "<li><span class='method'>GET</span><a href='/fermenter?id=0'>/fermenter?id=N</a><span class='desc'> &mdash; single fermenter</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/fermenter</span><span class='desc'> &mdash; update fermenter config</span></li>"
    "<li><span class='method'>GET</span><a href='/controller'>/controller</a><span class='desc'> &mdash; controller config</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/controller</span><span class='desc'> &mdash; update global config</span></li>"
    "<li><span class='method'>GET</span><a href='/status'>/status</a><span class='desc'> &mdash; quick status</span></li>"
    "<li><span class='method'>GET</span><a href='/probes'>/probes</a><span class='desc'> &mdash; temperature probes</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/probes</span><span class='desc'> &mdash; update probe config</span></li>"
    "<li><span class='method'>GET</span><a href='/tilts'>/tilts</a><span class='desc'> &mdash; Tilt hydrometer config &amp; live data</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/tilt</span><span class='desc'> &mdash; update Tilt config</span></li>"
    "<li><span class='method'>GET</span><a href='/health'>/health</a><span class='desc'> &mdash; system health</span></li>"
    "<li><span class='method'>GET</span><a href='/smartplugs'>/smartplugs</a><span class='desc'> &mdash; smart plug config</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/smartplug</span><span class='desc'> &mdash; update smart plug</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/smartplug/test</span><span class='desc'> &mdash; test smart plug RF</span></li>"
    "<li><span class='method'>GET</span><a href='/rf/sniff'>/rf/sniff</a><span class='desc'> &mdash; RF sniff page</span></li>"
    "<li><span class='method'>GET</span><a href='/rf/sniff/poll'>/rf/sniff/poll</a><span class='desc'> &mdash; poll RF sniff results</span></li>"
    "<li><span class='method'>GET</span><a href='/brewservices'>/brewservices</a><span class='desc'> &mdash; brew service config</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/brewservices</span><span class='desc'> &mdash; update brew service</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/brewservices/test</span><span class='desc'> &mdash; test brew service</span></li>"
    "<li><span class='method'>GET</span><a href='/mqtt'>/mqtt</a><span class='desc'> &mdash; MQTT config</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/mqtt</span><span class='desc'> &mdash; update MQTT config</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/mqtt/test</span><span class='desc'> &mdash; test MQTT connection</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/mqtt/discover</span><span class='desc'> &mdash; trigger HA discovery</span></li>"
    "<li><span class='method'>GET</span><a href='/profiles'>/profiles</a><span class='desc'> &mdash; fermentation profiles</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/profile</span><span class='desc'> &mdash; update profile</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/fermenter/profile</span><span class='desc'> &mdash; profile control (start/stop/pause/next/prev)</span></li>"
    "<li><span class='method'>GET</span><a href='/board_info.json'>/board_info.json</a><span class='desc'> &mdash; board info</span></li>"
    "<li><span class='method'>POST</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/iSpindel</span><span class='desc'> &mdash; iSpindel gravity data</span></li>"
    "<li><span class='method'>GET</span><a href='/config'>/config</a><span class='desc'> &mdash; WiFi config page</span></li>"
    "<li><span class='method'>GET</span><a href='/update'>/update</a><span class='desc'> &mdash; OTA firmware update</span></li>"
    "<li><span class='method'>GET</span><a href='/reset'>/reset</a><span class='desc'> &mdash; reset configuration</span></li>"
    "<li><span class='method'>GET</span><a href='/reboot'>/reboot</a><span class='desc'> &mdash; reboot device</span></li>"
    "<li><span class='method'>GET</span><a href='/fs/files'>/fs/files</a><span class='desc'> &mdash; list LittleFS files</span></li>"
    "<li><span class='method'>GET</span><span style='color:#53d8fb;font-family:monospace;font-size:13px'>/fs/file?name=...</span><span class='desc'> &mdash; read file content</span></li>"
    "</ul></div>"
    "</body></html>"
  );
  server.send(200, "text/html", html);
}

// ============================================================
// FERMENTERS — GET /fermenters
// ============================================================

String buildFermenterJson(uint8_t i) {
  DynamicJsonDocument doc(1024);

  doc["Fermenter"]       = i;
  doc["FermenterName"]   = g_fermenters[i].fermenterName;
  doc["BeerName"]        = g_fermenters[i].beerName;
  doc["YeastName"]       = g_fermenters[i].yeastName;
  doc["CeilingTemp"]     = g_fermenters[i].ceilingTemp;
  doc["FloorTemp"]       = g_fermenters[i].floorTemp;
  doc["OG"]              = g_fermenters[i].og;
  doc["TG"]              = g_fermenters[i].tg;
  doc["Hysteresis"]      = g_fermenters[i].hysteresis;
  doc["CompressorDelay"] = g_fermenters[i].compressorDelay;
  doc["TempControl"]     = g_fermenters[i].tempControl;
  doc["SGControl"]       = g_fermenters[i].sgControl;
  doc["Power"]           = g_fermenters[i].power;
  doc["Status"]          = g_fermenters[i].status;
  doc["StatusStr"]       = getFermenterStatusStr(g_fermenters[i].status);
  doc["Alarm"]           = g_fermenters[i].alarm;
  doc["ProfileRunning"]  = g_fermenters[i].profileRunning;
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

  doc["BeerTemp"]    = (beerTemp    > -100.0f) ? toDisplayTemp(beerTemp)    : 0.0f;
  doc["AmbientTemp"] = (ambientTemp > -100.0f) ? toDisplayTemp(ambientTemp) : 0.0f;
  doc["SG"]          = sg;
  doc["Attenuation"] = getAttenuation(i);
  doc["TempUnit"]    = (g_globalConfig.unit == UNIT_CELSIUS) ? "C" : "F";

  String out;
  serializeJson(doc, out);
  return out;
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
    String fJson = buildFermenterJson(i);
    server.sendContent(fJson);
  }
  server.sendContent("]");
  server.sendContent("");  // end chunked transfer
}

void handleFermenter(ESP8266WebServer& server) {
  if (server.method() == HTTP_POST) {
    // Update fermenter config from POST body
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      int idx = doc["Fermenter"] | 0;
      if (idx >= 0 && idx < MAX_FERMENTERS) {
        if (doc.containsKey("CeilingTemp"))     g_fermenters[idx].ceilingTemp     = doc["CeilingTemp"];
        if (doc.containsKey("FloorTemp"))       g_fermenters[idx].floorTemp       = doc["FloorTemp"];
        if (doc.containsKey("Power"))           g_fermenters[idx].power           = doc["Power"];
        if (doc.containsKey("TempControl"))     g_fermenters[idx].tempControl     = doc["TempControl"];
        if (doc.containsKey("BeerName"))        strlcpy(g_fermenters[idx].beerName,      doc["BeerName"],      sizeof(g_fermenters[0].beerName));
        if (doc.containsKey("FermenterName"))   strlcpy(g_fermenters[idx].fermenterName, doc["FermenterName"], sizeof(g_fermenters[0].fermenterName));
        if (doc.containsKey("YeastName"))       strlcpy(g_fermenters[idx].yeastName,     doc["YeastName"],     sizeof(g_fermenters[0].yeastName));
        if (doc.containsKey("Hysteresis"))      g_fermenters[idx].hysteresis      = doc["Hysteresis"];
        if (doc.containsKey("CompressorDelay")) g_fermenters[idx].compressorDelay = doc["CompressorDelay"];
        if (doc.containsKey("BrewServices"))    g_fermenters[idx].brewServices    = doc["BrewServices"];
        if (doc.containsKey("OG"))              g_fermenters[idx].og              = doc["OG"];
        if (doc.containsKey("TG"))              g_fermenters[idx].tg              = doc["TG"];
        if (doc.containsKey("ProfileNo"))       g_fermenters[idx].profileNo       = doc["ProfileNo"];
        if (doc.containsKey("LiveTest"))        g_fermenters[idx].liveTest        = doc["LiveTest"];
        saveFermenterConfig();
        sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Configuration saved\"}"));
        return;
      }
    }
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Configuration invalid\"}"), 400);
    return;
  }

  // GET single fermenter
  int idx = server.arg("id").toInt();
  if (idx < 0 || idx >= MAX_FERMENTERS) idx = 0;
  sendJsonResponse(server, buildFermenterJson(idx));
}

// ============================================================
// CONTROLLER — GET /controller
// Returns global config + smart plug config
// ============================================================

String buildControllerJson() {
  DynamicJsonDocument doc(2048);

  doc["AuthCode"]      = g_globalConfig.authCode;
  doc["Unit"]          = g_globalConfig.unit;
  doc["BrewService"]   = g_globalConfig.brewService;
  doc["BrewServiceId"] = g_globalConfig.brewServiceId;
  doc["NotifyOn"]      = g_globalConfig.notifyOn;
  doc["Resolution"]    = g_globalConfig.resolution;
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
  JsonArray plugs = doc.createNestedArray("SmartPlugs");
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    if (g_smartPlugs[i].onCode == 0) continue;
    JsonObject p = plugs.createNestedObject();
    p["PlugNo"]       = i;
    p["Function"]     = g_smartPlugs[i].function;
    p["Fermenter"]    = g_smartPlugs[i].fermenter;
    p["Manufacturer"] = g_smartPlugs[i].manufacturer;
    p["State"]        = getPlugState(i);
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void handleController(ESP8266WebServer& server) {
  if (server.method() == HTTP_POST) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      if (doc.containsKey("Unit"))          g_globalConfig.unit          = doc["Unit"];
      if (doc.containsKey("Resolution"))    g_globalConfig.resolution    = doc["Resolution"];
      if (doc.containsKey("NotifyOn"))      g_globalConfig.notifyOn      = doc["NotifyOn"];
      if (doc.containsKey("BrewService"))   g_globalConfig.brewService   = doc["BrewService"];
      if (doc.containsKey("BrewServiceId")) strlcpy(g_globalConfig.brewServiceId, doc["BrewServiceId"], sizeof(g_globalConfig.brewServiceId));
      saveGlobalConfig();
      sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Configuration saved\"}"));
      return;
    }
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Configuration invalid\"}"), 400);
    return;
  }
  sendJsonResponse(server, buildControllerJson());
}

// ============================================================
// BOARD INFO — GET /board_info.json
// ============================================================

String buildBoardInfoJson() {
  DynamicJsonDocument doc(512);
  doc["chip_id"]     = String(ESP.getChipId(), HEX);
  doc["flash_size"]  = ESP.getFlashChipSize();
  doc["free_heap"]   = ESP.getFreeHeap();
  doc["sdk_version"] = ESP.getSdkVersion();
  doc["fw_version"]  = FW_VERSION;
  doc["fw_date"]     = FW_BUILD_DATE;
  doc["reset_reason"]= ESP.getResetReason();
  String out;
  serializeJson(doc, out);
  return out;
}

void handleBoardInfo(ESP8266WebServer& server) {
  sendJsonResponse(server, buildBoardInfoJson());
}

// ============================================================
// STATUS — GET /status (new, convenience endpoint)
// ============================================================

void handleStatus(ESP8266WebServer& server) {
  DynamicJsonDocument doc(1024);
  JsonArray fermenters = doc.createNestedArray("fermenters");
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    JsonObject f = fermenters.createNestedObject();
    f["id"]          = i;
    f["name"]        = g_fermenters[i].fermenterName;
    f["beer"]        = g_fermenters[i].beerName;
    f["power"]       = g_fermenters[i].power;
    f["status"]      = getFermenterStatusStr(g_fermenters[i].status);
    f["beerTemp"]    = toDisplayTemp(getBeerTemp(i));
    f["ambientTemp"] = toDisplayTemp(getAmbientTemp(i));
    f["sg"]          = getCurrentSG(i);
    f["alarm"]       = g_fermenters[i].alarm;
  }
  doc["uptime"]   = (uint32_t)(millis() / 60000UL);
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["ip"]       = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

// ============================================================
// PROBES — GET /probes
// ============================================================

void handleProbes(ESP8266WebServer& server) {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("probes");
  for (int i = 0; i < MAX_PROBES; i++) {
    if (strlen(g_probes[i].address) == 0) continue;
    JsonObject p = arr.createNestedObject();
    p["index"]       = i;
    p["name"]        = g_probes[i].probeName;
    p["address"]     = g_probes[i].address;
    p["function"]    = g_probes[i].function;
    p["fermenter"]   = g_probes[i].fermenter;
    p["temperature"] = toDisplayTemp(g_probes[i].temperature);
    p["tempAdjust"]  = g_probes[i].tempAdjust;
  }
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

// ============================================================
// HEALTH — GET /health
// ============================================================

void handleHealth(ESP8266WebServer& server) {
  DynamicJsonDocument doc(512);
  doc["freeHeap"]    = ESP.getFreeHeap();
  doc["uptime"]      = (uint32_t)(millis() / 60000UL);
  doc["rssi"]        = WiFi.RSSI();
  doc["ssid"]        = WiFi.SSID();
  doc["ip"]          = WiFi.localIP().toString();
  doc["resetReason"] = ESP.getResetReason();
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
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

void handleOTAPage(ESP8266WebServer& server) {
  server.send(200, "text/html", F(
    "<!DOCTYPE HTML><html><head><title>OTA Update</title></head><body>"
    "<h2>OurBrewbot Firmware Update</h2>"
    "<p>You are loading new firmware. Press confirm to proceed</p>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin'><br><br>"
    "<input type='submit' value='Upload Firmware'>"
    "</form>"
    "<p><a href='/'>Cancel</a></p>"
    "</body></html>"
  ));
}

void handleOTAUpload(ESP8266WebServer& server) {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    logMsg("[OTA] Update started: %s", upload.filename.c_str());
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
      logMsg("[OTA] Update success: %u bytes", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

// ============================================================
// ISPINDEL — POST /iSpindel
// ============================================================

void handleiSpindel(ESP8266WebServer& server) {
  handleiSpindelPost(server.arg("plain"));
  sendJsonResponse(server, F("{\"status\":\"ok\"}"));
}

// ============================================================
// WIFI CONFIG PAGES
// ============================================================

void handleConfigPage(ESP8266WebServer& server) {
  server.send(200, "text/html", F(
    "<!DOCTYPE HTML>\n<html>\n<head>\n  <title>WiFi setup</title>\n"
    "  <style>\n  body { background-color: #fcfcfc; box-sizing: border-box; }\n"
    "  body, input { font-family: Roboto, sans-serif; font-weight: 400; font-size: 16px; }\n"
    "  .centered { position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%);\n"
    "    padding: 20px; background-color: #ccc; border-radius: 4px; }\n"
    "  td { padding:0 0 0 5px; } label { white-space:nowrap; }\n"
    "  input { width: 20em; } input[name=\"port\"] { width: 5em; }\n"
    "  input[type=\"submit\"], img { margin: auto; display: block; width: 30%; }\n"
    "  </style>\n</head>\n<body>\n<div class=\"centered\">\n"
    "  <form method=\"get\" action=\"configMe\">\n    <table>\n"
    "    <tr><td><label for=\"ssid\">WiFi SSID:</label></td>"
    "  <td><input type=\"text\" name=\"ssid\" length=64 required=\"required\" ></td></tr>\n"
    "    <tr><td><label for=\"pass\">Password:</label></td>"
    "   <td><input type=\"text\" name=\"pass\" length=64 ></td></tr>\n"
    "    <tr><td><label for=\"blynk\">Auth token:</label></td>"
    "<td><input type=\"text\" name=\"blynk\" placeholder=\"a0b1c2d...\""
    "  maxlength=\"32\" required=\"required\" ></td></tr>\n"
    "    <tr><td><label for=\"host\">Host:</label></td>"
    "       <td><input type=\"text\" name=\"host\" value=\"104.248.10.162\" length=64></td></tr>\n"
    "    <tr><td><label for=\"port\">Port:</label></td>"
    "       <td><input type=\"number\" name=\"port\" value=\"8442\" min=\"1\" max=\"65535\"></td></tr>\n"
    "    </table><br/>\n    <input type=\"submit\" value=\"Apply\">\n"
    "  </form>\n</div>\n</body>\n</html>"
  ));
}

void handleConfigMe(ESP8266WebServer& server) {
  // Process GET params from the WiFi config form
  bool changed = false;

  if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
    // Note: changing WiFi requires restart — WiFiManager handles this
    changed = true;
  }
  if (server.hasArg("blynk")) {
    strlcpy(g_globalConfig.authCode, server.arg("blynk").c_str(), sizeof(g_globalConfig.authCode));
    changed = true;
  }

  if (changed) {
    saveGlobalConfig();
    server.send(200, "application/json",
      F("{\"status\":\"ok\",\"msg\":\"Configuration saved - Network Changed\"}"));
  } else {
    server.send(200, "application/json",
      F("{\"status\":\"ok\",\"msg\":\"No changes\"}"));
  }
}

// ============================================================
// PROBE CONFIG — POST /probes
// ============================================================

void handleProbePost(ESP8266WebServer& server) {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= MAX_PROBES || strlen(g_probes[idx].address) == 0) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid probe index\"}"), 400);
    return;
  }
  if (doc.containsKey("name"))       strlcpy(g_probes[idx].probeName, doc["name"], sizeof(g_probes[0].probeName));
  if (doc.containsKey("function"))   g_probes[idx].function   = doc["function"];
  if (doc.containsKey("fermenter"))  g_probes[idx].fermenter  = doc["fermenter"];
  if (doc.containsKey("tempAdjust")) g_probes[idx].tempAdjust = doc["tempAdjust"];
  saveProbeConfig();
  sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Probe updated\"}"));
}

// ============================================================
// SMART PLUGS — GET /smartplugs (all 10 slots)
// ============================================================

void handleSmartPlugs(ESP8266WebServer& server) {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("plugs");
  for (int i = 0; i < MAX_SMART_PLUGS; i++) {
    JsonObject p = arr.createNestedObject();
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
  }
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

// ============================================================
// SMART PLUG CONFIG — POST /smartplug
// ============================================================

void handleSmartPlugPost(ESP8266WebServer& server) {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= MAX_SMART_PLUGS) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid plug index\"}"), 400);
    return;
  }
  if (doc.containsKey("function"))     g_smartPlugs[idx].function    = doc["function"];
  if (doc.containsKey("fermenter"))    g_smartPlugs[idx].fermenter   = doc["fermenter"];
  if (doc.containsKey("manufacturer")) strlcpy(g_smartPlugs[idx].manufacturer, doc["manufacturer"], sizeof(g_smartPlugs[0].manufacturer));
  if (doc.containsKey("model"))        strlcpy(g_smartPlugs[idx].model, doc["model"], sizeof(g_smartPlugs[0].model));
  if (doc.containsKey("onCode"))       g_smartPlugs[idx].onCode      = doc["onCode"];
  if (doc.containsKey("offCode"))      g_smartPlugs[idx].offCode     = doc["offCode"];
  if (doc.containsKey("protocol"))     g_smartPlugs[idx].protocol    = doc["protocol"];
  if (doc.containsKey("bits"))         g_smartPlugs[idx].bits        = doc["bits"];
  if (doc.containsKey("delay"))        g_smartPlugs[idx].delayLength = doc["delay"];
  if (doc.containsKey("codeset"))      g_smartPlugs[idx].codeset     = doc["codeset"];
  saveSmartPlugConfig();
  sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Plug updated\"}"));
}

// ============================================================
// SMART PLUG TEST — POST /smartplug/test
// ============================================================

void handleSmartPlugTest(ESP8266WebServer& server) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= MAX_SMART_PLUGS) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid plug index\"}"), 400);
    return;
  }
  SmartPlugConfig& plug = g_smartPlugs[idx];
  bool on = (strcmp(doc["action"] | "on", "on") == 0);
  uint32_t code = on ? plug.onCode : plug.offCode;
  if (code == 0) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"No code configured\"}"), 400);
    return;
  }
  logMsg("[PLUG] Test plug %d %s: code=%u (0x%06X), bits=%d, delay=%d, proto=%d",
    idx, on ? "ON" : "OFF", code, code, plug.bits, plug.delayLength, plug.protocol);

  rfTransmit(code, plug.bits, plug.delayLength, plug.protocol);
  sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"RF code sent\"}"));
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

  DynamicJsonDocument doc(256);
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
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

void handleRFSniff(ESP8266WebServer& server) {
  // Stop receiver when leaving (will restart on poll)
  s_sniffActive = false;

  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
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
    "</body></html>");
  server.send(200, "text/html", html);
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

  DynamicJsonDocument doc(BLE_SNIFF_BUF_SIZE + 64);
  doc["data"] = s_bleSniffBuf;
  doc["len"]  = s_bleSniffLen;

  // Clear buffer after sending
  s_bleSniffLen = 0;

  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

void handleBLESniffSend(ESP8266WebServer& server) {
  g_bleSniffActive = true;
  s_bleSniffLastPoll = millis();

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  const char* cmd = doc["cmd"] | "";
  if (strlen(cmd) == 0) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Empty command\"}"), 400);
    return;
  }

  g_bleSerial.print(cmd);
  logMsg("[BLE SNIFF] Sent: %s", cmd);

  DynamicJsonDocument resp(128);
  resp["status"] = "ok";
  resp["cmd"]    = cmd;
  String out;
  serializeJson(resp, out);
  sendJsonResponse(server, out);
}

void handleBLESniff(ESP8266WebServer& server) {
  // Reset sniff state
  s_bleSniffLen = 0;
  g_bleSniffActive = true;
  s_bleSniffLastPoll = millis();

  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
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
    "</body></html>");
  server.send(200, "text/html", html);
}

// ============================================================
// BREW SERVICES — GET/POST /brewservices
// ============================================================

void handleBrewServices(ESP8266WebServer& server) {
  // Index: 0=Brewer's Friend, 1=Brewfather
  DynamicJsonDocument doc(512);
  JsonArray arr = doc.createNestedArray("services");
  const char* names[] = {"Brewer's Friend", "Brewfather"};
  for (int i = 0; i < MAX_BREW_SERVICES; i++) {
    JsonObject s = arr.createNestedObject();
    s["index"]     = i;
    s["name"]      = names[i];
    s["enabled"]    = g_brewServices[i].enabled;
    s["serviceId"]  = g_brewServices[i].serviceId;
    s["deviceName"] = g_brewServices[i].deviceName;
  }
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

void handleBrewServicesPost(ESP8266WebServer& server) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= MAX_BREW_SERVICES) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid service index\"}"), 400);
    return;
  }
  if (doc.containsKey("enabled"))    g_brewServices[idx].enabled = doc["enabled"];
  if (doc.containsKey("serviceId"))  strlcpy(g_brewServices[idx].serviceId, doc["serviceId"], sizeof(g_brewServices[0].serviceId));
  if (doc.containsKey("deviceName")) strlcpy(g_brewServices[idx].deviceName, doc["deviceName"], sizeof(g_brewServices[0].deviceName));
  saveBrewServiceConfig();
  sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Brew service saved\"}"));
}

// ============================================================
// BREW SERVICE TEST — POST /brewservices/test
// ============================================================

void handleBrewServiceTest(ESP8266WebServer& server) {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= MAX_BREW_SERVICES) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid service index\"}"), 400);
    return;
  }
  if (strlen(g_brewServices[idx].serviceId) == 0) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"No service ID configured\"}"), 400);
    return;
  }
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
  DynamicJsonDocument doc(256);
  doc["enabled"]     = g_mqttConfig.enabled;
  doc["haDiscovery"] = g_mqttConfig.haDiscovery;
  doc["host"]        = g_mqttConfig.host;
  doc["port"]        = g_mqttConfig.port;
  doc["username"]    = g_mqttConfig.username;
  doc["password"]    = g_mqttConfig.password;
  doc["baseTopic"]   = g_mqttConfig.baseTopic;
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

void handleMqttConfigPost(ESP8266WebServer& server) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  if (doc.containsKey("enabled"))   g_mqttConfig.enabled = doc["enabled"];
  if (doc.containsKey("host"))      strlcpy(g_mqttConfig.host,      doc["host"],      sizeof(g_mqttConfig.host));
  if (doc.containsKey("port"))      g_mqttConfig.port = doc["port"];
  if (doc.containsKey("username"))  strlcpy(g_mqttConfig.username,  doc["username"],  sizeof(g_mqttConfig.username));
  if (doc.containsKey("password"))  strlcpy(g_mqttConfig.password,  doc["password"],  sizeof(g_mqttConfig.password));
  if (doc.containsKey("baseTopic")) strlcpy(g_mqttConfig.baseTopic, doc["baseTopic"], sizeof(g_mqttConfig.baseTopic));
  if (doc.containsKey("haDiscovery")) {
    bool newHa = doc["haDiscovery"];
    if (g_mqttConfig.haDiscovery && !newHa) cleanupAllHaDiscovery();
    g_mqttConfig.haDiscovery = newHa;
  }
  saveMqttConfig();
  sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"MQTT config saved\"}"));
}

void handleMqttTest(ESP8266WebServer& server) {
  bool ok = testMqtt();
  if (ok) {
    sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"MQTT connected and test message published\"}"));
  } else {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"MQTT connection failed — check host/port/credentials\"}"));
  }
}

void handleMqttDiscover(ESP8266WebServer& server) {
  if (forcePublishAllHaDiscovery()) {
    sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"HA discovery published\"}"));
  } else {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"MQTT not connected — check broker settings\"}"));
  }
}

// ============================================================
// PROFILES — GET /profiles
// ============================================================

void handleProfiles(ESP8266WebServer& server) {
  DynamicJsonDocument doc(4096);
  JsonArray profiles = doc.createNestedArray("profiles");
  for (int p = 0; p < MAX_PROFILES; p++) {
    JsonObject prof = profiles.createNestedObject();
    prof["index"] = p;
    prof["name"]  = g_profiles[p].profileName;
    JsonArray steps = prof.createNestedArray("steps");
    uint8_t base = p * MAX_STEPS_PER_PROFILE;
    for (int s = 0; s < MAX_STEPS_PER_PROFILE; s++) {
      JsonObject st = steps.createNestedObject();
      st["stepType"]  = g_profileSteps[base + s].stepType;
      st["startTemp"] = g_profileSteps[base + s].startTemp;
      st["endTemp"]   = g_profileSteps[base + s].endTemp;
      st["sgTrigger"] = g_profileSteps[base + s].sgTrigger;
      st["days"]      = g_profileSteps[base + s].days;
    }
  }
  // Step type descriptions for UI dropdowns
  JsonArray types = doc.createNestedArray("stepTypes");
  for (int t = 0; t <= 9; t++) {
    JsonObject st = types.createNestedObject();
    st["id"]   = t;
    st["name"] = getStepTypeDescription(t);
  }
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

// ============================================================
// PROFILE SAVE — POST /profile
// ============================================================

void handleProfilePost(ESP8266WebServer& server) {
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  int idx = doc["index"] | -1;
  if (idx < 0 || idx >= MAX_PROFILES) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid profile index\"}"), 400);
    return;
  }
  if (doc.containsKey("name")) {
    strlcpy(g_profiles[idx].profileName, doc["name"] | "Empty Profile", sizeof(g_profiles[0].profileName));
  }
  if (doc.containsKey("steps")) {
    JsonArray steps = doc["steps"];
    uint8_t base = idx * MAX_STEPS_PER_PROFILE;
    for (int s = 0; s < MAX_STEPS_PER_PROFILE; s++) {
      if (s < (int)steps.size()) {
        JsonObject st = steps[s];
        g_profileSteps[base + s].stepType  = st["stepType"]  | 0;
        g_profileSteps[base + s].startTemp = st["startTemp"] | 0.0f;
        g_profileSteps[base + s].endTemp   = st["endTemp"]   | 0.0f;
        g_profileSteps[base + s].sgTrigger = st["sgTrigger"] | 0.0f;
        g_profileSteps[base + s].days      = st["days"]      | 0.0f;
        g_profileSteps[base + s].stepNo    = s;
      } else {
        // Clear remaining steps
        memset(&g_profileSteps[base + s], 0, sizeof(ProfileStep));
      }
    }
  }
  saveProfileConfig();
  saveProfileSteps();
  sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Profile saved\"}"));
}

// ============================================================
// FERMENTER PROFILE CONTROL — POST /fermenter/profile
// Actions: start, stop, pause, next, prev
// ============================================================

void handleFermenterProfile(ESP8266WebServer& server) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  int idx = doc["Fermenter"] | -1;
  if (idx < 0 || idx >= MAX_FERMENTERS) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid fermenter\"}"), 400);
    return;
  }
  const char* action = doc["action"] | "";

  if (strcmp(action, "start") == 0) {
    int profIdx = doc["ProfileIndex"] | 0;
    if (profIdx < 1 || profIdx > MAX_PROFILES) {
      sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid profile (1-4)\"}"), 400);
      return;
    }
    // Check profile has at least one step
    if (countProfileSteps(profIdx - 1) == 0) {
      sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Profile has no steps\"}"), 400);
      return;
    }
    startProfile(idx, profIdx);
    sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Profile started\"}"));
  } else if (strcmp(action, "stop") == 0) {
    stopProfile(idx);
    sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Profile stopped\"}"));
  } else if (strcmp(action, "pause") == 0) {
    pauseProfile(idx);
    sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Profile paused\"}"));
  } else if (strcmp(action, "next") == 0) {
    nextProfileStep(idx);
    sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Advanced to next step\"}"));
  } else if (strcmp(action, "prev") == 0) {
    prevProfileStep(idx);
    sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Moved to previous step\"}"));
  } else {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Unknown action\"}"), 400);
  }
}

// ============================================================
// FILESYSTEM BROWSER
// ============================================================

void handleFsFiles(ESP8266WebServer& server) {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("files");
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    JsonObject f = arr.createNestedObject();
    f["name"] = dir.fileName();
    f["size"] = (unsigned int)dir.fileSize();
  }
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
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
  server.send(200, "text/plain", f.readString());
  f.close();
}

// ============================================================
// TILTS — GET /tilts
// Returns all Tilt colour slots that are configured or have been seen.
// ============================================================

void handleTilts(ESP8266WebServer& server) {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("tilts");
  for (int i = 0; i < MAX_TILTS; i++) {
    // Include slots that are configured (colour set) or actively seen
    if (g_tilts[i].colour == PROBE_UNASSIGNED && !g_tilts[i].active) continue;
    JsonObject t = arr.createNestedObject();
    t["colour"]      = i;
    t["colourName"]  = getTiltColourName(i);
    t["function"]    = g_tilts[i].function;
    t["fermenter"]   = g_tilts[i].fermenter;
    t["tempAdjust"]  = g_tilts[i].tempAdjust;
    t["sgAdjust"]    = g_tilts[i].sgAdjust;
    t["mbb"]         = g_tilts[i].mbb;
    t["active"]      = g_tilts[i].active;
    t["sg"]          = g_tilts[i].sg;
    t["temperature"] = toDisplayTemp(g_tilts[i].temperature);
  }
  String out;
  serializeJson(doc, out);
  sendJsonResponse(server, out);
}

// ============================================================
// TILT CONFIG — POST /tilt
// Body: {"colour":0,"function":4,"fermenter":0,"tempAdjust":0.0,"sgAdjust":0.0}
// ============================================================

void handleTiltPost(ESP8266WebServer& server) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid JSON\"}"), 400);
    return;
  }
  int colour = doc["colour"] | -1;
  if (colour < 0 || colour >= MAX_TILTS) {
    sendJsonResponse(server, F("{\"status\":\"error\",\"msg\":\"Invalid colour index (0-7)\"}"), 400);
    return;
  }

  // Clear flag: remove this Tilt from persisted config
  if (doc["_clear"] | false) {
    g_tilts[colour].colour    = PROBE_UNASSIGNED;
    g_tilts[colour].function  = PROBE_UNASSIGNED;
    g_tilts[colour].fermenter = PROBE_UNASSIGNED;
    g_tilts[colour].tempAdjust = 0.0f;
    g_tilts[colour].sgAdjust   = 0.0f;
    g_tilts[colour].mbb        = 0;
    saveTiltConfig();
    sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Tilt slot cleared\"}"));
    return;
  }

  // Mark the slot as configured so saveTiltConfig() includes it
  g_tilts[colour].colour = (uint8_t)colour;
  if (doc.containsKey("function"))   g_tilts[colour].function   = doc["function"];
  if (doc.containsKey("fermenter"))  g_tilts[colour].fermenter  = doc["fermenter"];
  if (doc.containsKey("tempAdjust")) g_tilts[colour].tempAdjust = doc["tempAdjust"];
  if (doc.containsKey("sgAdjust"))   g_tilts[colour].sgAdjust   = doc["sgAdjust"];
  saveTiltConfig();
  sendJsonResponse(server, F("{\"status\":\"ok\",\"msg\":\"Tilt updated\"}"));
}

// ============================================================
// 404 NOT FOUND
// ============================================================

void handleNotFound(ESP8266WebServer& server) {
  sendJsonResponse(server,
    F("{\"status\":\"error\",\"msg\":\"Not found\"}"), 404);
}
