/*
 * Reports.cpp — Third-party brew service integrations
 */

#include "Reports.h"
#include "Fermenter.h"
#include "Temperatures.h"
#include "Log.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

#define HTTP_TIMEOUT_MS  10000  // 10 second timeout for all HTTP requests

// ============================================================
// MAIN REPORT DISPATCHER
// ============================================================

void sendReports() {
  if (!WiFi.isConnected()) return;

  // Index 0=Brewer's Friend, 1=Brewfather
  for (int s = 0; s < MAX_BREW_SERVICES; s++) {
    if (!g_brewServices[s].enabled) continue;
    if (strlen(g_brewServices[s].serviceId) == 0) continue;

    for (int i = 0; i < MAX_FERMENTERS; i++) {
      if (!g_fermenters[i].power) continue;
      if (!(g_fermenters[i].brewServices & (1 << s))) continue;

      switch (s + 1) {  // index+1 = BREW_SERVICE_xxx
        case BREW_SERVICE_BREWERS_FRIEND:
          reportBrewersFriend(i, s);
          break;
        case BREW_SERVICE_BREWFATHER:
          reportBrewfather(i, s);
          break;
      }
    }
  }
}

// ============================================================
// BREWFATHER — Custom Stream API
// Docs: https://docs.brewfather.app/integrations/custom-stream
// URL:  POST https://log.brewfather.net/stream?id=<stream-id>
// Rate: max once per 15 min per device name
// ============================================================

void reportBrewfather(uint8_t i, uint8_t svcIndex) {
  if (strlen(g_brewServices[svcIndex].serviceId) == 0) return;

  WiFiClient client;
  HTTPClient http;

  char url[128];
  snprintf(url, sizeof(url), "http://log.brewfather.net/stream?id=%s",
    g_brewServices[svcIndex].serviceId);

  http.begin(client, url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  float beerTemp    = getBeerTemp(i);
  float ambientTemp = getAmbientTemp(i);
  float sg          = getCurrentSG(i);

  DynamicJsonDocument doc(512);
  // "name" identifies the device in Brewfather (rate-limited per name)
  // Each fermenter gets its own identity; deviceName goes to device_source
  doc["name"]           = g_fermenters[i].fermenterName;
  doc["device_source"]  = g_brewServices[svcIndex].deviceName;
  if (beerTemp > -100.0f)    doc["temp"]          = toDisplayTemp(beerTemp);
  if (ambientTemp > -100.0f) doc["aux_temp"]      = toDisplayTemp(ambientTemp);
  doc["temp_unit"]      = (g_globalConfig.unit == UNIT_CELSIUS) ? "C" : "F";
  doc["temp_target"]    = g_fermenters[i].ceilingTemp;
  if (sg > 0.0f)             doc["gravity"]       = sg / 1000.0f;
  doc["gravity_unit"]   = "G";
  if (g_fermenters[i].tg > 0.0f) doc["gravity_target"] = g_fermenters[i].tg / 1000.0f;
  doc["beer"]           = g_fermenters[i].beerName;
  doc["comment"]        = g_fermenters[i].yeastName;
  doc["hysteresis"]     = g_fermenters[i].hysteresis;
  doc["rssi"]           = WiFi.RSSI();
  uint8_t st = g_fermenters[i].status;
  doc["device_state"]   = (st == STATUS_HEATING) ? "heating" :
                          (st == STATUS_COOLING) ? "cooling" : "on";

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code > 0) {
    logMsg("[RPT] Brewfather F%d (%s): HTTP %d", i, g_fermenters[i].fermenterName, code);
  } else {
    logMsg("[RPT] Brewfather F%d (%s): Error %s", i, g_fermenters[i].fermenterName, http.errorToString(code).c_str());
  }
  http.end();
}

// ============================================================
// BREWER'S FRIEND — Custom Stream API
// Docs: https://docs.brewersfriend.com/api/stream
// URL:  POST https://log.brewersfriend.com/stream/<api_key>
// Rate: max once per 15 min per session
// ============================================================

void reportBrewersFriend(uint8_t i, uint8_t svcIndex) {
  if (strlen(g_brewServices[svcIndex].serviceId) == 0) return;

  WiFiClient client;
  HTTPClient http;

  char url[128];
  snprintf(url, sizeof(url), "http://log.brewersfriend.com/stream/%s",
    g_brewServices[svcIndex].serviceId);

  http.begin(client, url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  float beerTemp    = getBeerTemp(i);
  float ambientTemp = getAmbientTemp(i);
  float sg          = getCurrentSG(i);

  DynamicJsonDocument doc(512);
  // "name" identifies the device in Brewer's Friend (rate-limited per name)
  doc["name"]           = g_fermenters[i].fermenterName;
  doc["device_source"]  = g_brewServices[svcIndex].deviceName;
  if (beerTemp > -100.0f)    doc["temp"]          = toDisplayTemp(beerTemp);
  if (ambientTemp > -100.0f) doc["ambient"]        = toDisplayTemp(ambientTemp);
  doc["temp_unit"]      = (g_globalConfig.unit == UNIT_CELSIUS) ? "C" : "F";
  doc["temp_target"]    = g_fermenters[i].ceilingTemp;
  if (sg > 0.0f)             doc["gravity"]       = sg / 1000.0f;
  doc["gravity_unit"]   = "G";
  if (g_fermenters[i].tg > 0.0f) doc["gravity_target"] = g_fermenters[i].tg / 1000.0f;
  if (g_fermenters[i].og > 0.0f) doc["og"]             = g_fermenters[i].og / 1000.0f;
  doc["beer"]           = g_fermenters[i].beerName;
  doc["comment"]        = g_fermenters[i].yeastName;
  doc["hysteresis"]     = g_fermenters[i].hysteresis;
  doc["RSSI"]           = WiFi.RSSI();
  uint8_t st = g_fermenters[i].status;
  doc["heat_state"]     = (st == STATUS_HEATING) ? "heating" :
                          (st == STATUS_COOLING) ? "cooling" : "off";

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code > 0) {
    logMsg("[RPT] BrewersFriend F%d (%s): HTTP %d", i, g_fermenters[i].fermenterName, code);
  } else {
    logMsg("[RPT] BrewersFriend F%d (%s): Error %s", i, g_fermenters[i].fermenterName, http.errorToString(code).c_str());
  }
  http.end();
}

// ============================================================
// BREW SERVICE TEST — sends a minimal test payload
// Returns HTTP status code, or negative on error
// ============================================================

int testBrewService(uint8_t svcIndex) {
  if (svcIndex >= MAX_BREW_SERVICES) return -1;
  if (strlen(g_brewServices[svcIndex].serviceId) == 0) return -2;

  int svcType = svcIndex + 1;  // index+1 = BREW_SERVICE_xxx

  DynamicJsonDocument doc(256);
  doc["name"] = "OurBrewbot Test";
  doc["device_source"] = strlen(g_brewServices[svcIndex].deviceName) > 0
    ? (const char*)g_brewServices[svcIndex].deviceName : "OurBrewbot";

  String body;
  serializeJson(doc, body);
  int result = -1;

  char url[128];
  String response;

  WiFiClient client;
  HTTPClient http;

  if (svcType == BREW_SERVICE_BREWFATHER) {
    snprintf(url, sizeof(url), "http://log.brewfather.net/stream?id=%s",
      g_brewServices[svcIndex].serviceId);
    http.begin(client, url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    logMsg("[RPT] Test POST %s", url);
    logMsg("[RPT] Body: %s", body.c_str());
    result = http.POST(body);
    response = http.getString();
    http.end();
  } else if (svcType == BREW_SERVICE_BREWERS_FRIEND) {
    snprintf(url, sizeof(url), "http://log.brewersfriend.com/stream/%s",
      g_brewServices[svcIndex].serviceId);
    http.begin(client, url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    logMsg("[RPT] Test POST %s", url);
    logMsg("[RPT] Body: %s", body.c_str());
    result = http.POST(body);
    response = http.getString();
    http.end();
  }

  logMsg("[RPT] Test service %d: HTTP %d", svcIndex, result);
  logMsg("[RPT] Response: %s", response.c_str());
  return result;
}