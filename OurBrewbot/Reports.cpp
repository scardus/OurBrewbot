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

#define HTTP_TIMEOUT_MS  5000   // 5 second timeout — brew services respond in <1 s normally

// ============================================================
// SERVICE DEFINITIONS
//
// Both services take the same custom-stream style POST; the per-service
// differences are pure data (URL and a few JSON key names), so one table
// row per service drives a single reporter.
//   Brewfather:      https://docs.brewfather.app/integrations/custom-stream
//                    POST http://log.brewfather.net/stream?id=<stream-id>
//                    Rate: max once per 15 min per device name
//   Brewer's Friend: https://docs.brewersfriend.com/api/stream
//                    POST http://log.brewersfriend.com/stream/<api_key>
//                    Rate: max once per 15 min per session
// ============================================================

struct BrewServiceDef {
  const char* label;       // log label
  const char* urlFormat;   // snprintf format, %s = serviceId
  const char* ambientKey;  // JSON key for the ambient temperature
  const char* rssiKey;     // JSON key for WiFi RSSI
  const char* stateKey;    // JSON key for the heating/cooling state
  const char* idleState;   // state value when neither heating nor cooling
  bool        sendOg;      // include the OG field (Brewer's Friend only)
};

// Indexed by service slot: 0=Brewer's Friend, 1=Brewfather (BREW_SERVICE_xxx - 1).
// The differing idle-state values ("off" vs "on") are long-standing behavior,
// preserved as-is.
static const BrewServiceDef kBrewServiceDefs[MAX_BREW_SERVICES] = {
  { "BrewersFriend", "http://log.brewersfriend.com/stream/%s",
    "ambient",  "RSSI", "heat_state",   "off", true  },
  { "Brewfather",    "http://log.brewfather.net/stream?id=%s",
    "aux_temp", "rssi", "device_state", "on",  false },
};

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
      reportBrewService(i, s);
    }
  }
}

// ============================================================
// BREW SERVICE REPORTER — one fermenter to one service slot
// ============================================================

void reportBrewService(uint8_t i, uint8_t svcIndex) {
  if (svcIndex >= MAX_BREW_SERVICES) return;
  if (strlen(g_brewServices[svcIndex].serviceId) == 0) return;
  const BrewServiceDef& svc = kBrewServiceDefs[svcIndex];

  WiFiClient client;
  HTTPClient http;

  char url[128];
  snprintf(url, sizeof(url), svc.urlFormat, g_brewServices[svcIndex].serviceId);

  http.begin(client, url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  float beerTemp    = getBeerTemp(i);
  float ambientTemp = getAmbientTemp(i);
  float sg          = getCurrentSG(i);

  JsonDocument doc;
  // "name" identifies the device to the service (rate-limited per name)
  // Each fermenter gets its own identity; deviceName goes to device_source
  doc["name"]           = g_fermenters[i].fermenterName;
  doc["device_source"]  = g_brewServices[svcIndex].deviceName;
  if (beerTemp > TEMP_VALID_MIN)    doc["temp"]           = toDisplayTemp(beerTemp);
  if (ambientTemp > TEMP_VALID_MIN) doc[svc.ambientKey]   = toDisplayTemp(ambientTemp);
  doc["temp_unit"]      = (g_globalConfig.unit == UNIT_CELSIUS) ? "C" : "F";
  doc["temp_target"]    = toDisplayTemp((g_fermenters[i].floorTemp + g_fermenters[i].ceilingTemp) / 2.0f);
  if (sg > 0.0f)             doc["gravity"]        = sg;
  doc["gravity_unit"]   = "G";
  if (g_fermenters[i].tg > 0.0f) doc["gravity_target"] = g_fermenters[i].tg;
  if (svc.sendOg && g_fermenters[i].og > 0.0f) doc["og"] = g_fermenters[i].og;
  doc["beer"]           = g_fermenters[i].beerName;
  doc["comment"]        = g_fermenters[i].yeastName;
  doc["hysteresis"]     = g_fermenters[i].hysteresis;
  doc[svc.rssiKey]      = WiFi.RSSI();
  uint8_t st = g_fermenters[i].status;
  doc[svc.stateKey]     = (st == STATUS_HEATING) ? "heating" :
                          (st == STATUS_COOLING) ? "cooling" : svc.idleState;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code > 0) {
    logMsg("[RPT] %s F%d (%s): HTTP %d", svc.label, i, g_fermenters[i].fermenterName, code);
  } else {
    logMsg("[RPT] %s F%d (%s): Error %s", svc.label, i, g_fermenters[i].fermenterName, http.errorToString(code).c_str());
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

  JsonDocument doc;
  doc["name"] = "OurBrewbot Test";
  doc["device_source"] = strlen(g_brewServices[svcIndex].deviceName) > 0
    ? (const char*)g_brewServices[svcIndex].deviceName : "OurBrewbot";

  String body;
  serializeJson(doc, body);

  char url[128];
  snprintf(url, sizeof(url), kBrewServiceDefs[svcIndex].urlFormat,
    g_brewServices[svcIndex].serviceId);

  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  logMsg("[RPT] Test POST %s", url);
  logMsg("[RPT] Body: %s", body.c_str());
  int result = http.POST(body);
  String response = http.getString();
  http.end();

  logMsg("[RPT] Test service %d: HTTP %d", svcIndex, result);
  logMsg("[RPT] Response: %s", response.c_str());
  return result;
}