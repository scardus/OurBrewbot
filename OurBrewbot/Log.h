#pragma once
/*
 * Log.h — Centralised serial logging with timestamps
 *
 * Usage:
 *   logMsg("[TAG] Something happened: %d", value);
 *
 * Output format:
 *   [HH:MM:SS] [TAG] Something happened: 42\r\n
 *
 * - Automatically prepends uptime timestamp
 * - Automatically appends \r\n (do NOT include \n in format strings)
 * - Uses a 192-byte stack buffer per call (no heap allocation)
 * - Mirrors output to a syslog server when syslogConfig.enabled is true
 */

#include <Arduino.h>
#include <WiFiUDP.h>

// Syslog severity levels (RFC 5424)
#define SYSLOG_EMERG   0
#define SYSLOG_ALERT   1
#define SYSLOG_CRIT    2
#define SYSLOG_ERR     3
#define SYSLOG_WARNING 4
#define SYSLOG_NOTICE  5
#define SYSLOG_INFO    6
#define SYSLOG_DEBUG   7

// Initialise the log system — call once after Serial.begin(), and again after
// WiFi connects (or after syslog config is saved) to resolve the syslog host.
void logInit();

// Log a formatted message with timestamp + \r\n
void logMsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
