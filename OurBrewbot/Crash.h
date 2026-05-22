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
};

// Mark `module` as the currently-running subsystem. Cheap: skips the RTC
// write when the module hasn't changed since the previous call. Call before
// each subsystem invocation in loop().
void checkpoint(uint8_t module);

// Mirror any pending crash/checkpoint detail via the DEFERRED syslog path.
// Call once from setup() after WiFi + syslog are up. Quiet on clean reboots
// (power-on, software restart, external reset); logs detail only when the
// reset reason indicates a fault (exception, hw watchdog, soft watchdog).
// Clears the crash magic on success so the next boot does not re-log it.
void crashLogPendingDeferred();
