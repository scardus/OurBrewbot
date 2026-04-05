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
 */

#include <Arduino.h>

// Initialise the log system — call once in setup() after Serial.begin()
void logInit();

// Log a formatted message with timestamp + \r\n
void logMsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
