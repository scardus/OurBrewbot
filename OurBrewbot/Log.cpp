/*
 * Log.cpp — Centralised serial logging with timestamps
 */

#include "Log.h"
#include <stdarg.h>

void logInit() {
  // Reserved for future use (e.g. log level, remote syslog)
}

void logMsg(const char* fmt, ...) {
  // Timestamp: [HHH:MM:SS] using millis() uptime
  unsigned long ms = millis();
  unsigned long totalSec = ms / 1000;
  unsigned long hours = totalSec / 3600;
  unsigned long mins  = (totalSec % 3600) / 60;
  unsigned long secs  = totalSec % 60;

  char ts[16];
  snprintf(ts, sizeof(ts), "[%03lu:%02lu:%02lu] ", hours, mins, secs);
  Serial.print(ts);

  // Format and print the message
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  Serial.print(buf);
  Serial.print("\r\n");
}
