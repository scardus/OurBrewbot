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
 * Log.cpp — Centralised serial logging with timestamps + optional syslog
 */

#include "Log.h"
#include "Config.h"
#include "Mqtt.h"
#include "StackProbe.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <stdarg.h>

static WiFiUDP  s_udp;
static IPAddress s_syslogIP;    // cached resolved IP (0.0.0.0 = unresolved)
static bool      s_ipResolved = false;

// How long to wait after the warm-up packet below for the ARP lookup of the
// syslog host to finish. A LAN lookup answers in well under 10 ms; 250 ms is
// generous and only ever spent once per logInit().
#define SYSLOG_ARP_WARMUP_MS 250

// Shortest gap allowed between two syslog datagrams. Handed packets faster
// than it can put them on the wire, lwIP drops them rather than queueing.
// Every log line outside setup() is followed by the MQTT mirror below, which
// spaces them out for free - but MQTT is not connected yet while setup() runs,
// so the boot block went out back to back and about half of it never arrived,
// including the DEFERRED crash checkpoint. Steady-state lines are already
// further apart than this, so enforcing it costs nothing once running.
#define SYSLOG_MIN_GAP_MS 10

static uint32_t s_lastSendMs = 0;

// Put one RFC 3164 line on the wire: <PRI>TIMESTAMP HOSTNAME TAG: MESSAGE.
// Callers must have checked the enabled/resolved/connected gates first.
static void sendSyslog(uint8_t level, const char* msg) {
  // Unsigned arithmetic, so this stays correct across the millis() rollover.
  uint32_t since = millis() - s_lastSendMs;
  if (since < SYSLOG_MIN_GAP_MS) delay(SYSLOG_MIN_GAP_MS - since);

  // The header and the message are written separately rather than being
  // joined in one big buffer first. Every log line passes through here, deep
  // in the call stack, so a 256-byte local buffer cost loop stack on the
  // deepest path in the firmware. WiFiUDP collects both writes into the same
  // packet, so what goes on the wire is unchanged.
  uint8_t pri = (g_syslogConfig.facility * 8) + level;
  char header[40];   // longest is "<191>ourbrewbot ourbrewbot: " = 28 chars
  snprintf_P(header, sizeof(header), PSTR("<%u>ourbrewbot ourbrewbot: "), pri);
  s_udp.beginPacket(s_syslogIP, g_syslogConfig.port);
  s_udp.write((const uint8_t*)header, strlen(header));
  s_udp.write((const uint8_t*)msg, strlen(msg));
  s_udp.endPacket();
  s_lastSendMs = millis();
}

void logInit() {
  s_ipResolved = false;
  s_syslogIP   = IPAddress(0, 0, 0, 0);

  if (!g_syslogConfig.enabled || g_syslogConfig.host[0] == '\0') return;
  if (WiFi.status() != WL_CONNECTED) return;

  // Try to resolve hostname. hostByName is synchronous; cap at 2 s to avoid
  // blocking setup/reconnect for an unreachable or misconfigured syslog host.
  IPAddress resolved;
  if (!WiFi.hostByName(g_syslogConfig.host, resolved, 2000)) return;

  s_syslogIP   = resolved;
  s_ipResolved = true;

  // lwIP does not queue datagrams while it looks up the MAC address of a peer
  // it has not talked to yet - it drops them. That silently ate the first four
  // or five syslog lines of every boot, which is exactly the block that says
  // what the firmware is and why it restarted ([MDNS], [WEB] and the DEFERRED
  // group). Send one throwaway line to start the lookup, then wait for it to
  // finish. delay() yields to the WiFi task so the ARP reply is actually
  // processed during the wait; a busy loop would not work.
  sendSyslog(SYSLOG_INFO, "[LOG] Syslog ready");
  delay(SYSLOG_ARP_WARMUP_MS);
}

// Send one finished log line to every output: serial, syslog and the MQTT
// log topic. ts is the "[HHH:MM:SS] " timestamp, which serial has already
// printed by the time this runs.
static void sendLogLine(uint8_t level, const char* ts, const char* msg) {
  Serial.print(msg);
  Serial.print("\r\n");

  // Syslog output. RFC 5424 levels: lower number = more critical.
  // minLevel = 7 (DEBUG) → allow everything; minLevel = 4 (WARNING) → only
  // WARNING and worse.
  if (g_syslogConfig.enabled && s_ipResolved &&
      WiFi.status() == WL_CONNECTED &&
      level <= g_syslogConfig.minLevel) {

    STACK_PROBE_BEGIN(syslogProbe);
    sendSyslog(level, msg);
    STACK_PROBE_END(PROBE_LOG_SYSLOG, syslogProbe);
  }

  // MQTT log topic output: timestamp + message in a single payload.
  // mqttPublishLog() is a no-op when MQTT is disabled / not connected, and
  // self-guards against re-entry to prevent publish-from-within-publish loops.
  // The timestamp and message are passed separately and joined inside
  // mqttPublishLog(), which already has a static buffer to build them in -
  // joining them here would need another local buffer on the loop stack.
  if (g_mqttConfig.enabled && g_mqttConfig.logEnabled) {
    STACK_PROBE_BEGIN(mqttProbe);
    mqttPublishLog(level, ts, msg);
    STACK_PROBE_END(PROBE_LOG_MQTT, mqttProbe);
  }
}

// The size of a formatted log message, including its terminating NUL.
#define LOG_MSG_SIZE 192

// The formatted message lives in one shared static buffer rather than on the
// stack. Every log line is sent from deep in the call stack, and a 192-byte
// local buffer stayed on the loop stack for the whole of the syslog and MQTT
// sends - the deepest point measured in the firmware.
//
// A shared buffer is only safe while one log line is being handled at a time.
// Sending a line briefly hands control to the WiFi system (the syslog gap
// delay(), the MQTT network write), so a log call made from anything that runs
// during those moments would overwrite the line still being sent. Nothing
// does that today, but s_logBufferInUse catches it if a later change ever
// does: the second line is then formatted by logMsgNested() instead.
static char s_logBuffer[LOG_MSG_SIZE];
static bool s_logBufferInUse = false;

// Handles a log call made while another one is still using s_logBuffer. It
// formats into its own local buffer, so neither line is lost or mixed up.
// noinline keeps that buffer in this function's stack frame, so the stack is
// only used on the rare occasion this actually runs - never on the normal path.
static void __attribute__((noinline))
logMsgNested(uint8_t level, const char* ts, PGM_P fmt, va_list args) {
  char buf[LOG_MSG_SIZE];
  vsnprintf_P(buf, sizeof(buf), fmt, args);
  sendLogLine(level, ts, buf);
}

static void vlogMsg(uint8_t level, PGM_P fmt, va_list args) {
  // Timestamp: [HHH:MM:SS]
  unsigned long ms = millis();
  unsigned long totalSec = ms / 1000;
  unsigned long hours = totalSec / 3600;
  unsigned long mins  = (totalSec % 3600) / 60;
  unsigned long secs  = totalSec % 60;

  char ts[16];
  snprintf(ts, sizeof(ts), "[%03lu:%02lu:%02lu] ", hours, mins, secs);
  Serial.print(ts);

  if (s_logBufferInUse) {
    logMsgNested(level, ts, fmt, args);
    return;
  }
  s_logBufferInUse = true;

  // Format message — vsnprintf_P reads the format string from flash
  STACK_PROBE_BEGIN(formatProbe);
  vsnprintf_P(s_logBuffer, sizeof(s_logBuffer), fmt, args);
  STACK_PROBE_END(PROBE_LOG_FORMAT, formatProbe);

  sendLogLine(level, ts, s_logBuffer);
  s_logBufferInUse = false;
}

void logMsgImpl(uint8_t level, PGM_P fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogMsg(level, fmt, args);
  va_end(args);
}
