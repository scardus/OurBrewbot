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
