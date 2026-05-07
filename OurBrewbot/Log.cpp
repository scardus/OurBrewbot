/*
 * Log.cpp — Centralised serial logging with timestamps + optional syslog
 */

#include "Log.h"
#include "Config.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <stdarg.h>

static WiFiUDP  s_udp;
static IPAddress s_syslogIP;    // cached resolved IP (0.0.0.0 = unresolved)
static bool      s_ipResolved = false;

void logInit() {
  s_ipResolved = false;
  s_syslogIP   = IPAddress(0, 0, 0, 0);

  if (!g_syslogConfig.enabled || g_syslogConfig.host[0] == '\0') return;
  if (WiFi.status() != WL_CONNECTED) return;

  // Try to resolve hostname; WiFi.hostByName is synchronous on ESP8266
  IPAddress resolved;
  if (WiFi.hostByName(g_syslogConfig.host, resolved)) {
    s_syslogIP   = resolved;
    s_ipResolved = true;
  }
}

void logMsg(const char* fmt, ...) {
  // Timestamp: [HHH:MM:SS]
  unsigned long ms = millis();
  unsigned long totalSec = ms / 1000;
  unsigned long hours = totalSec / 3600;
  unsigned long mins  = (totalSec % 3600) / 60;
  unsigned long secs  = totalSec % 60;

  char ts[16];
  snprintf(ts, sizeof(ts), "[%03lu:%02lu:%02lu] ", hours, mins, secs);
  Serial.print(ts);

  // Format message
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  Serial.print(buf);
  Serial.print("\r\n");

  // Syslog output
  if (g_syslogConfig.enabled && s_ipResolved &&
      WiFi.status() == WL_CONNECTED &&
      SYSLOG_DEBUG <= g_syslogConfig.minLevel) {

    // RFC 3164: <PRI>TIMESTAMP HOSTNAME TAG: MESSAGE
    uint8_t pri = (g_syslogConfig.facility * 8) + SYSLOG_DEBUG;
    char pkt[256];
    // Use zero-padded millis-derived time as a simple timestamp (no RTC)
    snprintf(pkt, sizeof(pkt), "<%u>ourbrewbot ourbrewbot: %s", pri, buf);
    s_udp.beginPacket(s_syslogIP, g_syslogConfig.port);
    s_udp.write((const uint8_t*)pkt, strlen(pkt));
    s_udp.endPacket();
  }
}
