/*
 * Log.cpp — Centralised serial logging with timestamps + optional syslog
 */

#include "Log.h"
#include "Config.h"
#include "Mqtt.h"
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

  uint8_t pri = (g_syslogConfig.facility * 8) + level;
  char pkt[256];
  snprintf_P(pkt, sizeof(pkt), PSTR("<%u>ourbrewbot ourbrewbot: %s"), pri, msg);
  s_udp.beginPacket(s_syslogIP, g_syslogConfig.port);
  s_udp.write((const uint8_t*)pkt, strlen(pkt));
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

  // Format message — vsnprintf_P reads the format string from flash
  char buf[192];
  vsnprintf_P(buf, sizeof(buf), fmt, args);

  Serial.print(buf);
  Serial.print("\r\n");

  // Syslog output. RFC 5424 levels: lower number = more critical.
  // minLevel = 7 (DEBUG) → allow everything; minLevel = 4 (WARNING) → only
  // WARNING and worse.
  if (g_syslogConfig.enabled && s_ipResolved &&
      WiFi.status() == WL_CONNECTED &&
      level <= g_syslogConfig.minLevel) {

    sendSyslog(level, buf);
  }

  // MQTT log topic output: timestamp + message in a single payload.
  // mqttPublishLog() is a no-op when MQTT is disabled / not connected, and
  // self-guards against re-entry to prevent publish-from-within-publish loops.
  if (g_mqttConfig.enabled && g_mqttConfig.logEnabled) {
    char line[208];
    snprintf(line, sizeof(line), "%s%s", ts, buf);
    mqttPublishLog(level, line);
  }

}

void logMsgImpl(uint8_t level, PGM_P fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlogMsg(level, fmt, args);
  va_end(args);
}
