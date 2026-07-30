#pragma once
// Minimal stand-in for the core's Esp.h / the global ESP object. Included from
// test/stubs/Arduino.h, mirroring the real core's layering so production files
// that reference ESP compile natively without an extra include.
//
// The reset info is settable so tests can present a specific reset reason -
// Config.cpp's recordReboot() records extra register detail only for
// REASON_EXCEPTION_RST, which is otherwise unreachable on the host.

#include <cstdint>
#include <user_interface.h>

static rst_info g_espResetInfo = { REASON_DEFAULT_RST, 0, 0, 0, 0, 0, 0 };
static uint32_t g_espFreeHeap  = 24000;

class EspClass {
public:
  struct rst_info* getResetInfoPtr() { return &g_espResetInfo; }
  uint32_t getFreeHeap()             { return g_espFreeHeap; }
  uint32_t getMaxFreeBlockSize()     { return g_espFreeHeap / 2; }
  uint8_t  getHeapFragmentation()    { return 0; }
  void     wdtFeed()                 {}
  void     restart()                 {}
};

static EspClass ESP;

static void espTestSetResetReason(uint32_t reason) {
  g_espResetInfo = rst_info{ reason, 0, 0, 0, 0, 0, 0 };
}

static void espTestSetExceptionReset(uint32_t exccause, uint32_t epc1, uint32_t excvaddr) {
  g_espResetInfo = rst_info{ (uint32_t)REASON_EXCEPTION_RST, exccause, epc1, 0, 0, excvaddr, 0 };
}
