#pragma once
// Minimal stand-in for the core's Esp.h / the global ESP object. Included from
// test/stubs/Arduino.h, mirroring the real core's layering so production files
// that reference ESP compile natively without an extra include.
//
// The reset info is settable so tests can present a specific reset reason -
// Config.cpp's recordReboot() records extra register detail only for
// REASON_EXCEPTION_RST, which is otherwise unreachable on the host.

#include <cstdint>
#include <cstring>
#include <user_interface.h>

static rst_info g_espResetInfo = { REASON_DEFAULT_RST, 0, 0, 0, 0, 0, 0 };
static uint32_t g_espFreeHeap  = 24000;

// RTC user memory: 512 bytes that survive a reset. Crash.cpp keeps its crash
// record and checkpoint breadcrumb here, so post-mortem behaviour can only be
// tested with real backing store. Offsets are in 4-byte blocks and sizes in
// bytes, matching the core's API.
#define RTC_USER_MEM_BYTES 512
static uint8_t g_espRtcMem[RTC_USER_MEM_BYTES];

class EspClass {
public:
  struct rst_info* getResetInfoPtr() { return &g_espResetInfo; }
  uint32_t getFreeHeap()             { return g_espFreeHeap; }
  uint32_t getMaxFreeBlockSize()     { return g_espFreeHeap / 2; }
  uint8_t  getHeapFragmentation()    { return 0; }
  void     wdtFeed()                 {}
  void     restart()                 {}

  // Board identity, reported by WebAPI.cpp's /board_info.json and /status.
  // Fixed values so a payload-shape test has something stable to assert on.
  uint32_t    getChipId()          { return 0x2924FA; }
  uint32_t    getFlashChipSize()   { return 4194304; }
  uint32_t    getFreeSketchSpace() { return 1044464; }
  const char* getSdkVersion()      { return "2.2.2-dev(38a443e)"; }

  // Both reject an out-of-bounds access rather than clamping, which is what
  // the SDK does - a test relying on a truncated write would be testing
  // something the hardware never does.
  bool rtcUserMemoryRead(uint32_t offset, uint32_t* dst, size_t size) {
    if (offset * 4 + size > RTC_USER_MEM_BYTES) return false;
    memcpy(dst, g_espRtcMem + offset * 4, size);
    return true;
  }
  bool rtcUserMemoryWrite(uint32_t offset, uint32_t* src, size_t size) {
    if (offset * 4 + size > RTC_USER_MEM_BYTES) return false;
    memcpy(g_espRtcMem + offset * 4, src, size);
    return true;
  }

  // The human-readable reset reason the core derives from rst_info.reason;
  // Crash.cpp puts this straight into its DEFERRED log lines. Strings match
  // the core's Esp.cpp exactly so a test asserting on a log line checks the
  // text that really reaches syslog.
  String getResetReason() {
    switch (g_espResetInfo.reason) {
      case REASON_DEFAULT_RST:      return String("Power on");
      case REASON_WDT_RST:          return String("Hardware Watchdog");
      case REASON_EXCEPTION_RST:    return String("Exception");
      case REASON_SOFT_WDT_RST:     return String("Software Watchdog");
      case REASON_SOFT_RESTART:     return String("Software/System restart");
      case REASON_DEEP_SLEEP_AWAKE: return String("Deep-Sleep Wake");
      case REASON_EXT_SYS_RST:      return String("External System");
      default:                      return String("Unknown");
    }
  }
};

static EspClass ESP;

static void espTestSetResetReason(uint32_t reason) {
  g_espResetInfo = rst_info{ reason, 0, 0, 0, 0, 0, 0 };
}

static void espTestSetExceptionReset(uint32_t exccause, uint32_t epc1, uint32_t excvaddr) {
  g_espResetInfo = rst_info{ (uint32_t)REASON_EXCEPTION_RST, exccause, epc1, 0, 0, excvaddr, 0 };
}

// Wipe RTC user memory. Real RTC contents survive a reset but not a power
// cycle, so this is the "cold boot" starting state.
static void espTestClearRtc() { memset(g_espRtcMem, 0, RTC_USER_MEM_BYTES); }
