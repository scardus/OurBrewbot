#pragma once
// Stand-in for the ESP8266 SDK header of the same name. Config.cpp includes it
// for the rst_info struct and the REASON_* codes recordReboot() branches on;
// the values match the SDK so a test asserting on rsn_code checks the real
// number that lands in the reboot log.

#include <cstdint>

struct rst_info {
  uint32_t reason;
  uint32_t exccause;
  uint32_t epc1;
  uint32_t epc2;
  uint32_t epc3;
  uint32_t excvaddr;
  uint32_t depc;
};

enum {
  REASON_DEFAULT_RST      = 0,   // power on
  REASON_WDT_RST          = 1,   // hardware watchdog
  REASON_EXCEPTION_RST    = 2,
  REASON_SOFT_WDT_RST     = 3,
  REASON_SOFT_RESTART     = 4,   // ESP.restart(), and SDK panic()/assert()
  REASON_DEEP_SLEEP_AWAKE = 5,
  REASON_EXT_SYS_RST      = 6,   // external reset pin
};
