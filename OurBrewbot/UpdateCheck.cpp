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
 * UpdateCheck.cpp — Daily check for a newer firmware release
 * See UpdateCheck.h for what it does and the file it reads.
 */

#include "UpdateCheck.h"
#include "Config.h"
#include "Version.h"
#include "Log.h"
#include "Mqtt.h"
#include "Crash.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

// ============================================================
// TIMING AND LIMITS
// ============================================================
#define UPDATE_FIRST_CHECK_MS  (10UL * 60UL * 1000UL)        // first check 10 min after boot...
#define UPDATE_SPREAD_MIN      60                            // ...plus 0-59 min taken from the chip ID
#define UPDATE_INTERVAL_MS     (24UL * 60UL * 60UL * 1000UL) // then once a day
#define UPDATE_RETRY_MS        (60UL * 60UL * 1000UL)        // after a failure, retry in 1 hour...
#define UPDATE_MAX_RETRIES     3                             // ...up to 3 times, then wait for the next day
#define UPDATE_HTTP_TIMEOUT_MS 5000                          // same as the brew-service reports
#define UPDATE_MAX_BODY        512                           // version.json is ~100 bytes

UpdateStatus g_updateStatus;

// Scheduler state (see updateCheckLoop)
static uint32_t s_waitStartMs  = 0;   // when the current wait began
static uint32_t s_waitMs       = 0;   // how long to wait - 0 means "not scheduled yet"
static uint8_t  s_retriesLeft  = UPDATE_MAX_RETRIES;

// The newest version already logged as a Notice, so "update available" is
// announced once per release rather than every day.
static char s_announcedVersion[16] = "";

// ============================================================
// VERSION HELPERS
// ============================================================

bool parseVersion(const char* text, uint16_t parts[3]) {
  if (text == nullptr) return false;
  if (*text == 'v' || *text == 'V') text++;

  for (int i = 0; i < 3; i++) {
    if (*text < '0' || *text > '9') return false;   // each part needs at least one digit
    uint32_t number = 0;
    while (*text >= '0' && *text <= '9') {
      number = number * 10 + (uint32_t)(*text - '0');
      if (number > 65535) return false;
      text++;
    }
    parts[i] = (uint16_t)number;

    if (i < 2) {
      if (*text != '.') return false;
      text++;
    }
  }
  return *text == '\0';   // nothing may follow the third number
}

bool isNewerVersion(const char* candidate, const char* current) {
  uint16_t a[3], b[3];
  if (!parseVersion(candidate, a) || !parseVersion(current, b)) return false;
  for (int i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;   // the same version
}

bool parseVersionJson(const String& body,
                      char* version, size_t versionSize,
                      char* notes, size_t notesSize) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  // is<const char*>() rather than a plain read: a missing field or a number
  // must be rejected, not quietly turned into an empty string or "0".
  if (!doc["version"].is<const char*>()) return false;
  const char* v = doc["version"];
  uint16_t parts[3];
  if (!parseVersion(v, parts)) return false;
  if (*v == 'v' || *v == 'V') v++;   // store "0.4.16", not "v0.4.16"
  if (strlen(v) >= versionSize) return false;
  strlcpy(version, v, versionSize);

  // Optional. Only keep a complete web link - a truncated URL would be broken.
  notes[0] = '\0';
  if (doc["notes"].is<const char*>()) {
    const char* n = doc["notes"];
    bool isLink = strncmp(n, "https://", 8) == 0 || strncmp(n, "http://", 7) == 0;
    if (isLink && strlen(n) < notesSize) strlcpy(notes, n, notesSize);
  }
  return true;
}

// ============================================================
// THE CHECK
// ============================================================

// Record and log a failed check. Always returns false so callers can
// `return checkFailed(...)`.
static bool checkFailed(const char* reason) {
  strlcpy(g_updateStatus.lastError, reason, sizeof(g_updateStatus.lastError));
  logMsgL(SYSLOG_WARNING, "[UPDATE] Check failed: %s", reason);
  return false;
}

bool runUpdateCheck() {
  g_updateStatus.attempted   = true;
  g_updateStatus.lastCheckMs = millis();

  if (!WiFi.isConnected()) return checkFailed("WiFi not connected");

  // The chip ID and firmware version let the site count how many controllers
  // (and which versions) are in use. Nothing else about the controller is sent.
  char url[96];
  snprintf(url, sizeof(url), "%s?id=%06x&v=%s", UPDATE_CHECK_URL, ESP.getChipId(), FW_VERSION);

  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(UPDATE_HTTP_TIMEOUT_MS);

  int code = http.GET();
  if (code != 200) {
    char reason[40];
    if (code > 0) {
      snprintf(reason, sizeof(reason), "HTTP %d", code);   // includes a redirect to https
    } else {
      snprintf(reason, sizeof(reason), "%s", http.errorToString(code).c_str());
    }
    http.end();
    return checkFailed(reason);
  }

  // getSize() is -1 when the server doesn't say; the length is checked again below
  if (http.getSize() > UPDATE_MAX_BODY) {
    http.end();
    return checkFailed("reply too large");
  }
  String body = http.getString();
  http.end();
  if (body.length() > UPDATE_MAX_BODY) return checkFailed("reply too large");

  char version[sizeof(g_updateStatus.latestVersion)];
  char notes[sizeof(g_updateStatus.notesUrl)];
  if (!parseVersionJson(body, version, sizeof(version), notes, sizeof(notes))) {
    return checkFailed("unreadable reply");
  }

  // A good reply - store it
  g_updateStatus.checked         = true;
  g_updateStatus.lastError[0]    = '\0';
  g_updateStatus.updateAvailable = isNewerVersion(version, FW_VERSION);
  strlcpy(g_updateStatus.latestVersion, version, sizeof(g_updateStatus.latestVersion));
  strlcpy(g_updateStatus.notesUrl,      notes,   sizeof(g_updateStatus.notesUrl));

  if (g_updateStatus.updateAvailable) {
    if (strcmp(version, s_announcedVersion) != 0) {
      logMsgL(SYSLOG_NOTICE, "[UPDATE] Firmware %s is available (running %s)", version, FW_VERSION);
      strlcpy(s_announcedVersion, version, sizeof(s_announcedVersion));
    } else {
      logMsg("[UPDATE] Firmware %s is still available (running %s)", version, FW_VERSION);
    }
  } else if (isNewerVersion(FW_VERSION, version)) {
    logMsg("[UPDATE] Running %s, newer than the latest release %s", FW_VERSION, version);
  } else {
    logMsg("[UPDATE] Firmware is up to date (%s)", FW_VERSION);
  }

  mqttPublishUpdateStatus();
  return true;
}

// ============================================================
// SCHEDULER
// ============================================================

void updateCheckLoop() {
  uint32_t now = millis();

  // First call after boot: schedule the first check. The chip-ID spread
  // stops every controller on the internet checking at the same moment.
  if (s_waitMs == 0) {
    s_waitStartMs = now;
    s_waitMs = UPDATE_FIRST_CHECK_MS + (ESP.getChipId() % UPDATE_SPREAD_MIN) * 60000UL;
    return;
  }

  if (now - s_waitStartMs < s_waitMs) return;   // not due yet
  s_waitStartMs = now;

  // Switched off: skip this one and look again tomorrow
  if (!g_globalConfig.updateCheck) {
    s_waitMs = UPDATE_INTERVAL_MS;
    return;
  }

  checkpoint(CP_UPDATE);   // so a crash during the check is logged against it
  if (runUpdateCheck()) {
    s_waitMs      = UPDATE_INTERVAL_MS;
    s_retriesLeft = UPDATE_MAX_RETRIES;
  } else if (s_retriesLeft > 0) {
    s_retriesLeft--;
    s_waitMs = UPDATE_RETRY_MS;
  } else {
    s_waitMs      = UPDATE_INTERVAL_MS;   // out of retries - try again tomorrow
    s_retriesLeft = UPDATE_MAX_RETRIES;
  }
}
