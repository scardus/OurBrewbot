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
 * CrashReport.cpp — Send a crash report to ourbrewbot.com after a crash
 * See CrashReport.h for what is sent and when.
 */

#include "CrashReport.h"
#include "Config.h"
#include "Version.h"
#include "Log.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

// ============================================================
// TIMING AND LIMITS
// ============================================================
#define CRASH_REPORT_FIRST_MS    (2UL * 60UL * 1000UL)    // first try 2 min after boot, once WiFi has settled
#define CRASH_REPORT_RETRY_MS    (10UL * 60UL * 1000UL)   // then every 10 min...
#define CRASH_REPORT_ATTEMPTS    4                        // ...for up to 4 tries in all
#define CRASH_REPORT_TIMEOUT_MS  5000                     // same as the update check
#define CRASH_REPORT_MAX_BYTES   768                      // a full report is ~550 bytes

static uint8_t  s_attempts      = 0;      // tries so far
static uint32_t s_lastAttemptMs = 0;      // millis() of the last try (0 = boot)
static bool     s_finished      = false;  // sent, given up, or nothing to send

// ============================================================
// THE REPORT
// ============================================================

bool buildCrashReportJson(const CrashInfo& crash, char* out, size_t outSize) {
  JsonDocument doc;
  char hex[9];

  char chipId[7];
  snprintf(chipId, sizeof(chipId), "%06x", ESP.getChipId());
  doc["id"]    = chipId;
  doc["v"]     = FW_VERSION;
  doc["build"] = FW_BUILD_DATE;
  doc["reset"] = crash.resetCode;
  doc["last"]  = checkpointName(crash.lastModule);

  if (crash.haveRegisters) {
    doc["reason"]   = crash.reason;
    doc["exccause"] = crash.exccause;

    // Register values as hex without "0x", which is what the website expects
    const struct { const char* key; uint32_t value; } registers[] = {
      { "epc1", crash.epc1 }, { "epc2", crash.epc2 }, { "epc3", crash.epc3 },
      { "excvaddr", crash.excvaddr }, { "depc", crash.depc },
      { "sp", crash.sp }, { "sp_end", crash.spEnd },
    };
    for (const auto& r : registers) {
      snprintf(hex, sizeof(hex), "%x", r.value);
      doc[r.key] = hex;   // a char array, so ArduinoJson copies it
    }

    JsonArray stack = doc["stack"].to<JsonArray>();
    for (size_t i = 0; i < CRASH_STACK_WORDS; i++) {
      snprintf(hex, sizeof(hex), "%x", crash.stack[i]);
      stack.add(hex);
    }
  }

  if (measureJson(doc) + 1 > outSize) return false;
  serializeJson(doc, out, outSize);
  return true;
}

// Try to send the report once. Returns true when there's no point trying
// again: it was accepted, or the website refused it for good.
static bool sendCrashReport() {
  if (!WiFi.isConnected()) {
    logMsgL(SYSLOG_WARNING, "[CRASH] Report not sent: WiFi not connected");
    return false;
  }

  char body[CRASH_REPORT_MAX_BYTES];
  if (!buildCrashReportJson(g_lastCrash, body, sizeof(body))) {
    logMsgL(SYSLOG_ERR, "[CRASH] Report too large to send");
    return true;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, CRASH_REPORT_URL);
  http.setTimeout(CRASH_REPORT_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  String payload(body);
  int code = http.POST(payload);

  if (code == 200 || code == 204) {
    http.end();
    logMsg("[CRASH] Crash report sent (reset %u in %s)",
           g_lastCrash.resetCode, checkpointName(g_lastCrash.lastModule));
    return true;
  }

  // 400 = the website rejected the report, 413 = too big, 429 = this
  // controller has already sent its daily maximum. Retrying won't help.
  if (code == 400 || code == 413 || code == 429) {
    http.end();
    logMsgL(SYSLOG_WARNING, "[CRASH] Report refused: HTTP %d", code);
    return true;
  }

  if (code > 0) {
    logMsgL(SYSLOG_WARNING, "[CRASH] Report failed: HTTP %d", code);   // e.g. 503, or a redirect
  } else {
    logMsgL(SYSLOG_WARNING, "[CRASH] Report failed: %s", http.errorToString(code).c_str());
  }
  http.end();
  return false;
}

// ============================================================
// SCHEDULER
// ============================================================

void crashReportLoop() {
  if (s_finished) return;

  // Nothing to send after a normal boot or an intentional restart
  if (!g_lastCrash.valid) {
    s_finished = true;
    return;
  }

  uint32_t now  = millis();
  uint32_t wait = (s_attempts == 0) ? CRASH_REPORT_FIRST_MS : CRASH_REPORT_RETRY_MS;
  if (now - s_lastAttemptMs < wait) return;   // not due yet

  if (!g_globalConfig.crashReports) {
    logMsg("[CRASH] Report not sent: crash reports are switched off");
    s_finished = true;
    return;
  }

  s_lastAttemptMs = now;
  s_attempts++;
  checkpoint(CP_CRASH_RPT);   // so a crash while sending is logged against it

  if (sendCrashReport()) {
    s_finished = true;
  } else if (s_attempts >= CRASH_REPORT_ATTEMPTS) {
    logMsgL(SYSLOG_WARNING, "[CRASH] Report not sent after %u tries - giving up", (unsigned)s_attempts);
    s_finished = true;
  }
}
