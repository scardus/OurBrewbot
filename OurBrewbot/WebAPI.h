#pragma once
/*
 * WebAPI.h — REST API server setup and route handlers
 *
 * Routes:
 *   GET  /fermenters       → fermenter status + temp data
 *   GET  /probes           → probe list
 *   GET  /smartplugs       → smart plug list
 *   POST /smartplug        → update a plug config
 *   POST /smartplug/test   → test RF transmission
 *   POST /update           → accept firmware binary upload
 *   POST /iSpindel         → receive iSpindel POST
 *   GET  /configMe         → save WiFi config (form GET)
 */

#include "Config.h"
#include "Version.h"
#include <ESP8266WebServer.h>
#include <ESP8266httpUpdate.h>

void setupWebServer(ESP8266WebServer& server);
void checkBLESniffTimeout();

// Route handlers (called by server)
void handleRoot(ESP8266WebServer& server);
void handleController(ESP8266WebServer& server);
void handleFermenters(ESP8266WebServer& server);
void handleFermenter(ESP8266WebServer& server);
void handleBoardInfo(ESP8266WebServer& server);
void handleStatus(ESP8266WebServer& server);
void handleProbes(ESP8266WebServer& server);
void handleHealth(ESP8266WebServer& server);
void handleReset(ESP8266WebServer& server);
void handleReboot(ESP8266WebServer& server);
void handleOTAPage(ESP8266WebServer& server);
void handleOTAUpload(ESP8266WebServer& server);
void handleiSpindel(ESP8266WebServer& server);
void handleConfigPage(ESP8266WebServer& server);
void handleConfigMe(ESP8266WebServer& server);
void handleNotFound(ESP8266WebServer& server);

// Admin page
void handleAdmin(ESP8266WebServer& server);
void handleProbePost(ESP8266WebServer& server);
void handleSmartPlugs(ESP8266WebServer& server);
void handleSmartPlugPost(ESP8266WebServer& server);
void handleSmartPlugTest(ESP8266WebServer& server);
void handleRFSniff(ESP8266WebServer& server);
void handleRFSniffPoll(ESP8266WebServer& server);
void handleBrewServices(ESP8266WebServer& server);
void handleBrewServicesPost(ESP8266WebServer& server);
void handleBrewServiceTest(ESP8266WebServer& server);
void handleMqttConfig(ESP8266WebServer& server);
void handleMqttConfigPost(ESP8266WebServer& server);
void handleMqttTest(ESP8266WebServer& server);
void handleMqttDiscover(ESP8266WebServer& server);

// Profile management
void handleProfiles(ESP8266WebServer& server);
void handleProfilePost(ESP8266WebServer& server);
void handleFermenterProfile(ESP8266WebServer& server);

// Filesystem browser
void handleFsFiles(ESP8266WebServer& server);
void handleFsFile(ESP8266WebServer& server);

// Tilt hydrometer config
void handleTilts(ESP8266WebServer& server);
void handleTiltPost(ESP8266WebServer& server);

// Helpers
void sendCORSHeaders(ESP8266WebServer& server);
void sendJsonResponse(ESP8266WebServer& server, const String& json, int code = 200);
String buildFermenterJson(uint8_t index);
String buildControllerJson();
String buildStatusJson();
String buildBoardInfoJson();
