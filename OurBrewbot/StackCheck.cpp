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

/*
 * StackCheck.cpp — Record the deepest point the loop stack reaches.
 *
 * Why: loop() runs on a 4 KB stack that the ESP8266 core (3.1.2) carves out
 * of the top of the WiFi SDK's own stack. If ours runs out it doesn't stop -
 * it carries on writing into the SDK's stack below it, and the SDK crashes
 * later on a corrupted pointer, far from the cause. The "Stack free" figure
 * in the [HEALTH] log says whether that has come close; this file says
 * which subsystem (and for the web server, which URL) took it there.
 *
 * How: at boot the core fills the whole loop stack with a marker value, and
 * ESP.getFreeContStack() counts how much of it has never been overwritten -
 * the high-water mark. stackCheck() reads that after each subsystem has run
 * (from checkpoint(), and after each web request) and, when it has dropped,
 * notes who was just running. The 10-minute timer logs any new record, so
 * nothing is logged while the stack is deep.
 *
 * Cost: getFreeContStack() walks the unused part of the stack, a few
 * microseconds per call - less the tighter the stack gets.
 */

#include "StackCheck.h"
#include <Arduino.h>
#include <string.h>

static uint32_t s_lowestFree   = UINT32_MAX;  // least stack left seen so far
static uint8_t  s_lowestModule = 0xFF;        // who was running at the time
static char     s_lowestWhere[24] = "";       // e.g. the web URL, "" if none
static bool     s_newLow       = false;       // not yet reported

// Called from checkpoint() in Crash.cpp and dispatchApiRequest() in WebAPI.cpp
// cppcheck-suppress unusedFunction
void stackCheck(uint8_t module, const char* where) {
  uint32_t freeBytes = ESP.getFreeContStack();
  if (freeBytes < s_lowestFree) {
    s_lowestFree   = freeBytes;
    s_lowestModule = module;
    strlcpy(s_lowestWhere, where ? where : "", sizeof(s_lowestWhere));
    s_newLow       = true;
  }
}

// Called from onTenMinuteTimer() in OurBrewbot.cpp
// cppcheck-suppress unusedFunction
bool stackTakeNewLow(uint32_t& freeBytes, uint8_t& module, const char*& where) {
  if (!s_newLow) return false;
  s_newLow  = false;
  freeBytes = s_lowestFree;
  module    = s_lowestModule;
  where     = s_lowestWhere;
  return true;
}
