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
#include <LittleFS.h>

// ============================================================
// TIMING AND LIMITS
// ============================================================
#define CRASH_REPORT_FIRST_MS    (2UL * 60UL * 1000UL)    // first try once WiFi has been up 2 min, so it has settled
#define CRASH_REPORT_NEXT_MS     (30UL * 1000UL)          // between saved reports - each POST can hold up the loop for 5 s
#define CRASH_REPORT_RETRY_MS    (10UL * 60UL * 1000UL)   // after a failed try...
#define CRASH_REPORT_ATTEMPTS    4                        // ...for up to 4 failed tries per boot
#define CRASH_REPORT_BUSY_MS     (60UL * 60UL * 1000UL)   // after the website's daily limit (HTTP 429)
#define CRASH_REPORT_TIMEOUT_MS  5000                     // same as the update check
#define CRASH_REPORT_MAX_BYTES   800                      // longest possible report 785 + NUL, rounded to 16 (see test)
#define CRASH_QUEUE_MAX          5                        // reports kept waiting; saving another deletes the oldest

static bool     s_started       = false;  // this boot's crash (if any) has been saved to the queue
static unsigned s_queued        = 0;      // reports waiting in the queue file
static unsigned s_failures      = 0;      // failed tries this boot
static uint32_t s_lastAttemptMs = 0;      // millis() of the last try
static uint32_t s_waitMs        = 0;      // how long after the last try the next one is due
// setup() has already tried WiFi, so if it is up on the first pass it has
// been up since boot
static bool     s_wifiWasUp     = true;
static uint32_t s_wifiUpSinceMs = 0;      // millis() when WiFi last connected
static bool     s_finished      = false;  // queue empty, or given up until the next boot

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

    // How close the loop stack came to overflowing before the crash
    doc["stack_free"] = crash.stackFree;
    doc["stack_at"]   = crash.stackAt;

    // The device's state at the crash, to tell device-specific problems
    // (weak WiFi, low memory) from firmware ones
    doc["uptime"] = crash.uptimeSec;
    doc["heap"]   = crash.freeHeap;
    if (crash.rssi < 0) doc["rssi"] = crash.rssi;   // 0 = not known

    // The exception that started the crash (FirstFault.h), when it was caught
    if (crash.haveFirstFault) {
      const FirstFault& ff = crash.firstFault;
      doc["ff_cause"] = ff.cause;
      const struct { const char* key; uint32_t value; } firstFault[] = {
        { "ff_epc", ff.epc }, { "ff_ps", ff.ps }, { "ff_a0", ff.a0 },
        { "ff_sp", ff.sp }, { "ff_excvaddr", ff.excvaddr },
      };
      for (const auto& r : firstFault) {
        snprintf(hex, sizeof(hex), "%x", r.value);
        doc[r.key] = hex;
      }
    }
  }

  if (measureJson(doc) + 1 > outSize) return false;
  serializeJson(doc, out, outSize);
  return true;
}

// ============================================================
// THE QUEUE FILE
// ============================================================
// One report per line, oldest first. Lines rather than one big JSON array, so
// a report can be read straight into the send buffer without parsing the
// whole file (a JsonDocument holding 5 reports would need about 8 KB of heap).

// How many reports the queue file holds
static unsigned countQueuedReports() {
  File f = LittleFS.open(FILE_CRASH_QUEUE, "r");
  if (!f) return 0;
  unsigned count = 0;
  int last = '\n';   // an empty file holds no reports
  int c;
  while ((c = f.read()) >= 0) {
    if (c == '\n') count++;
    last = c;
  }
  f.close();
  if (last != '\n') count++;   // a last line cut short (power lost while saving it)
  return count;
}

// Delete the oldest `count` reports. The rest are copied to a temp file that
// is then renamed over the queue, so losing power part way through leaves the
// old queue as it was rather than half a file. Returns false if it failed.
static bool dropOldestReports(unsigned count) {
  File in = LittleFS.open(FILE_CRASH_QUEUE, "r");
  if (!in) return false;
  File out = LittleFS.open(FILE_CRASH_QUEUE_TMP, "w");
  if (!out) {
    in.close();
    return false;
  }

  unsigned skipped = 0;
  size_t   kept    = 0;
  bool     ok      = true;
  int c;
  while ((c = in.read()) >= 0) {
    if (skipped < count) {
      if (c == '\n') skipped++;
      continue;
    }
    if (out.write((uint8_t)c) != 1) ok = false;
    kept++;
  }
  in.close();
  out.close();

  if (!ok) {
    LittleFS.remove(FILE_CRASH_QUEUE_TMP);
    return false;
  }
  if (kept == 0) {
    // Nothing left - remove the file rather than keep an empty one
    LittleFS.remove(FILE_CRASH_QUEUE_TMP);
    LittleFS.remove(FILE_CRASH_QUEUE);
  } else if (!LittleFS.rename(FILE_CRASH_QUEUE_TMP, FILE_CRASH_QUEUE)) {
    return false;
  }
  s_queued = (s_queued > count) ? s_queued - count : 0;
  return true;
}

// Add a report for `crash` to the end of the queue, deleting the oldest first
// if the queue is full
static void saveCrashToQueue(const CrashInfo& crash) {
  char body[CRASH_REPORT_MAX_BYTES];
  if (!buildCrashReportJson(crash, body, sizeof(body))) {
    logMsgL(SYSLOG_ERR, "[CRASH] Report too large to save");
    return;
  }

  if (s_queued >= CRASH_QUEUE_MAX) {
    logMsgL(SYSLOG_WARNING, "[CRASH] %u reports already waiting - deleting the oldest", s_queued);
    dropOldestReports(s_queued - CRASH_QUEUE_MAX + 1);
  }

  File f = LittleFS.open(FILE_CRASH_QUEUE, "a");
  if (!f) {
    logMsgL(SYSLOG_ERR, "[CRASH] Cannot save the report");
    return;
  }
  size_t length  = strlen(body);
  size_t written = f.write((const uint8_t*)body, length);
  written += f.write((uint8_t)'\n');
  f.close();

  // A report only partly written is still in the file, so it is counted - it
  // is deleted unsent when its turn comes (see readOldestReport)
  if (written > 0) s_queued++;
  if (written != length + 1) {
    logMsgL(SYSLOG_ERR, "[CRASH] Report only partly saved (%u of %u bytes)",
            (unsigned)written, (unsigned)(length + 1));
    return;
  }
  logMsg("[CRASH] Crash report saved (%u waiting to be sent)", s_queued);
}

// Read the oldest report into `out`. Returns false when it can't be sent as
// it is: cut short (power lost while saving it) or too long for the buffer.
static bool readOldestReport(char* out, size_t outSize) {
  File f = LittleFS.open(FILE_CRASH_QUEUE, "r");
  if (!f) return false;
  size_t length  = 0;
  bool   tooLong = false;
  int c;
  while ((c = f.read()) >= 0 && c != '\n') {
    if (length < outSize - 1) {
      out[length++] = (char)c;
    } else {
      tooLong = true;
    }
  }
  f.close();
  out[length] = '\0';
  return !tooLong && length >= 2 && out[0] == '{' && out[length - 1] == '}';
}

// ============================================================
// SENDING
// ============================================================

// What became of one try to send the oldest report
enum SendResult {
  SEND_FINISHED,   // accepted, or refused for good - take it off the queue
  SEND_LATER,      // failed (no connection, server error) - try again later
  SEND_BUSY,       // the website's daily limit is reached - try again in an hour
};

static SendResult sendOldestReport() {
  char body[CRASH_REPORT_MAX_BYTES];
  if (!readOldestReport(body, sizeof(body))) {
    logMsgL(SYSLOG_WARNING, "[CRASH] A saved report was damaged - deleted it unsent");
    return SEND_FINISHED;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, CRASH_REPORT_URL);
  http.setTimeout(CRASH_REPORT_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  // Posted straight from the stack buffer - copying it into a String first
  // would add a ~550-byte heap allocation for no benefit.
  int code = http.POST((const uint8_t*)body, strlen(body));
  http.end();

  if (code == 200 || code == 204) {
    logMsg("[CRASH] Crash report sent (%u more waiting)", s_queued - 1);
    return SEND_FINISHED;
  }

  // 429 = this controller has already sent the website's daily maximum
  if (code == 429) {
    logMsgL(SYSLOG_WARNING, "[CRASH] Report not sent: the website's daily limit is reached - trying again in an hour");
    return SEND_BUSY;
  }

  // 400 = the website rejected the report, 413 = too big. Retrying won't help.
  if (code == 400 || code == 413) {
    logMsgL(SYSLOG_WARNING, "[CRASH] Report refused: HTTP %d - deleted it", code);
    return SEND_FINISHED;
  }

  if (code > 0) {
    logMsgL(SYSLOG_WARNING, "[CRASH] Report failed: HTTP %d", code);   // e.g. 503, or a redirect
  } else {
    logMsgL(SYSLOG_WARNING, "[CRASH] Report failed: %s", http.errorToString(code).c_str());
  }
  return SEND_LATER;
}

// ============================================================
// SCHEDULER
// ============================================================

void crashReportLoop() {
  if (s_finished) return;

  // First pass after boot: count the reports earlier boots left waiting, then
  // add this boot's crash (there is none after a normal boot or an
  // intentional restart)
  if (!s_started) {
    s_started = true;
    s_queued  = countQueuedReports();
    if (g_lastCrash.valid && g_globalConfig.crashReports) saveCrashToQueue(g_lastCrash);
  }

  // Switched off: nothing is sent, and nothing is kept to send later
  if (!g_globalConfig.crashReports) {
    if (s_queued > 0) {
      LittleFS.remove(FILE_CRASH_QUEUE);
      logMsg("[CRASH] Crash reports are switched off - deleted %u saved reports", s_queued);
      s_queued = 0;
    } else if (g_lastCrash.valid) {
      logMsg("[CRASH] Report not sent: crash reports are switched off");
    }
    s_finished = true;
    return;
  }

  if (s_queued == 0) {
    s_finished = true;
    return;
  }

  // Wait until WiFi has been up for 2 minutes. A lost connection doesn't
  // count as a failed try - the reports just wait for it to come back.
  uint32_t now    = millis();
  bool     wifiUp = WiFi.isConnected();
  if (wifiUp && !s_wifiWasUp) s_wifiUpSinceMs = now;
  s_wifiWasUp = wifiUp;
  if (!wifiUp || now - s_wifiUpSinceMs < CRASH_REPORT_FIRST_MS) return;

  if (now - s_lastAttemptMs < s_waitMs) return;   // not due yet
  s_lastAttemptMs = now;
  checkpoint(CP_CRASH_RPT);   // so a crash while sending is logged against it

  switch (sendOldestReport()) {
    case SEND_FINISHED:
      if (!dropOldestReports(1)) {
        // Stop rather than send the same report again and again
        logMsgL(SYSLOG_ERR, "[CRASH] Cannot update the saved reports - trying again after the next boot");
        s_finished = true;
        return;
      }
      s_waitMs = CRASH_REPORT_NEXT_MS;
      break;

    case SEND_BUSY:
      s_waitMs = CRASH_REPORT_BUSY_MS;
      break;

    case SEND_LATER:
      s_failures++;
      s_waitMs = CRASH_REPORT_RETRY_MS;
      if (s_failures >= CRASH_REPORT_ATTEMPTS) {
        logMsgL(SYSLOG_WARNING, "[CRASH] Report not sent after %u tries - trying again after the next boot", s_failures);
        s_finished = true;
      }
      break;
  }

  if (s_queued == 0) s_finished = true;
}
