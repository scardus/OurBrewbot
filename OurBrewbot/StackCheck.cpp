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
 * in the [HEALTH] log says whether that has come close; this file says where.
 *
 * stackCheck() is called from logMsg(), because logging sits on top of most
 * of our deepest call chains (a web handler that logs, then sends the line
 * over syslog). Each time it finds less stack left than ever before, it notes
 * the amount and the current checkpoint. The 10-minute timer logs any new
 * record, so the logging itself never happens while the stack is deep.
 */

#include "StackCheck.h"
#include "Crash.h"
#include <cont.h>   // g_pcont - the loop's stack

static uint32_t s_lowestFree  = UINT32_MAX;  // least stack left seen so far
static uint8_t  s_lowestModule = 0xFF;       // checkpoint at that moment
static bool     s_newLow       = false;      // not yet reported

// Called from logMsgImpl() in Log.cpp - cppcheck doesn't follow the call
// cppcheck-suppress unusedFunction
void stackCheck() {
  // The stack grows down, so what's left is the distance from the current
  // stack position down to the bottom of the loop stack.
  uint32_t sp     = (uint32_t)__builtin_frame_address(0);
  uint32_t bottom = (uint32_t)g_pcont->stack;
  uint32_t top    = (uint32_t)g_pcont->stack_end;

  // A WiFi event handler can log from the SDK's own stack - ignore those.
  if (sp < bottom || sp >= top) return;

  uint32_t freeBytes = sp - bottom;
  if (freeBytes < s_lowestFree) {
    s_lowestFree   = freeBytes;
    s_lowestModule = currentCheckpoint();
    s_newLow       = true;
  }
}

// Called from onTenMinuteTimer() in OurBrewbot.cpp
// cppcheck-suppress unusedFunction
bool stackTakeNewLow(uint32_t& freeBytes, uint8_t& module) {
  if (!s_newLow) return false;
  s_newLow  = false;
  freeBytes = s_lowestFree;
  module    = s_lowestModule;
  return true;
}
