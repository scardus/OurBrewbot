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

#pragma once
/*
 * Crash.h — Persist exception detail + last-known subsystem across reboot.
 *
 * Two complementary post-mortem mechanisms:
 *
 *  1. Exception trap (cause 2 / REASON_EXCEPTION_RST). The ESP8266 Arduino
 *     core invokes `custom_crash_callback` from the exception handler before
 *     resetting. Crash.cpp captures the saved register frame plus a slice of
 *     the stack into RTC user memory.
 *
 *  2. Hardware-watchdog trap (cause 1 / REASON_WDT_RST). The exception
 *     callback does NOT fire for hw-watchdog resets — the chip just resets
 *     when the loop fails to service the WDT for ~6s. To recover *some*
 *     post-mortem info, loop() drops a 1-byte "I'm currently in subsystem X"
 *     breadcrumb into a separate RTC slot via checkpoint(); after a hw-wdt
 *     reset we log that on the way back up.
 *
 * Both records are mirrored via the standard DEFERRED syslog path by
 * crashLogPendingDeferred(), gated on an unexpected reset reason so clean
 * power/software reboots stay quiet.
 *
 * Decode stack words offline with:
 *   xtensa-lx106-elf-addr2line -e firmware.elf -pfiaC <addr>...
 * passing any STACK value in the code range 0x40100000-0x40300000.
 */

#include <stdint.h>

// Subsystem IDs for the main-loop checkpoint breadcrumb. New entries MUST
// be appended (existing values are persisted in RTC across a reset and
// referenced by post-mortem logs).
enum : uint8_t {
  CP_INIT       = 0,
  CP_WEB        = 1,
  CP_BLE        = 2,
  CP_MDNS       = 3,
  CP_MQTT       = 4,
  CP_MQTT_PEND  = 5,
  CP_HOOK       = 6,
  CP_TEMP_REQ   = 7,
  CP_TEMP_READ  = 8,
  CP_PROBE_SCAN = 9,
  CP_TILT       = 10,
  CP_FERM       = 11,
  CP_CLOUD      = 12,
  CP_MQTT_PUB   = 13,
  CP_TEN_MIN    = 14,
  CP_UPDATE     = 15,
  CP_CRASH_RPT  = 16,
};

#define CRASH_STACK_WORDS 24   // stack words captured by the crash handler

// A crash found at boot, kept for the crash report upload (CrashReport.cpp).
// Filled in by crashLogPendingDeferred(). `valid` stays false after a normal
// boot or an intentional restart (OTA, /reboot), so only real faults are sent.
struct CrashInfo {
  bool     valid;
  bool     haveRegisters;   // false when only the checkpoint is known (e.g. hardware watchdog)
  uint8_t  resetCode;       // the boot's reset reason (REASON_*)
  uint8_t  lastModule;      // CP_* the loop was in, 0xFF if unknown
  uint32_t reason;          // what the crash handler saw (254 = panic / heap-check hit)
  uint32_t exccause;
  uint32_t epc1, epc2, epc3, excvaddr, depc;
  uint32_t sp, spEnd;
  uint32_t stack[CRASH_STACK_WORDS];
  uint32_t stackFree;        // least loop stack left before the crash, in bytes
  char     stackAt[40];      // where that happened, e.g. "WEB /iSpindel"
};

extern CrashInfo g_lastCrash;

// Mark `module` as the currently-running subsystem. Cheap: skips the RTC
// write when the module hasn't changed since the previous call. Call before
// each subsystem invocation in loop().
void checkpoint(uint8_t module);

// Name of a checkpoint id, e.g. "MQTT_PEND" - "?" if unknown.
const char* checkpointName(uint32_t module);

// Mirror any pending crash/checkpoint detail via the DEFERRED syslog path,
// and copy it into g_lastCrash for the crash report. Call once from setup()
// after WiFi + syslog are up - and also when syslog is off, so a crash is
// still read and reported. Quiet on clean reboots (power-on, external reset);
// logs detail only when the reset reason indicates a fault (exception, hw
// watchdog, soft watchdog) or a software restart. Clears the crash magic on
// success so the next boot does not re-log it.
void crashLogPendingDeferred();
