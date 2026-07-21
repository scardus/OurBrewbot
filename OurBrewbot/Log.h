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

// Implementation — do not call directly; use the logMsg()/logMsgL() macros
// below, which move the format literal to flash via PSTR(). Format strings
// were the largest block of .rodata held in DRAM (~160 call sites).
void logMsgImpl(uint8_t level, PGM_P fmt, ...) __attribute__((format(printf, 2, 3)));

// Log a formatted message with timestamp + \r\n at SYSLOG_INFO severity.
// fmt must be a string literal (PSTR requirement).
#define logMsg(fmt, ...)         logMsgImpl(SYSLOG_INFO, PSTR(fmt), ##__VA_ARGS__)

// Log a formatted message at an explicit RFC 5424 severity level.
// level: one of SYSLOG_EMERG..SYSLOG_DEBUG (lower = more critical).
#define logMsgL(level, fmt, ...) logMsgImpl((level),     PSTR(fmt), ##__VA_ARGS__)
