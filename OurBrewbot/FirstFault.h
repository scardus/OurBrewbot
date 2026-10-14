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
 * FirstFault.h — Remember the FIRST CPU exception since boot, exactly as the
 * processor reported it. See FirstFault.cpp.
 *
 * Why: when a crash is followed by a second fault inside the SDK's own crash
 * handling, the registers the core reports are the second fault's, and the
 * real cause is lost (two of the four v0.6.2 crash reports from one device
 * on ourbrewbot.com looked like that). This keeps the first one.
 */

#include <stdint.h>

struct FirstFault {
  uint32_t cause;      // EXCCAUSE, e.g. 0 = illegal instruction, 29 = bad store
  uint32_t epc;        // address of the instruction that faulted
  uint32_t ps;         // processor state - INTLEVEL (bits 0-3) above 0 means an
                       // interrupt was running or interrupts were switched off
  uint32_t a0;         // return address - usually inside the faulting function's caller
  uint32_t sp;         // stack pointer at the fault
  uint32_t excvaddr;   // the bad address, for a load or store fault
};

// Start watching for CPU exceptions. Call once, early in setup().
void firstFaultInstall();

// Copy the first exception since boot (or since the last one the device
// survived) into `out`. Returns false if there hasn't been one. Safe to call
// from the crash handler.
bool firstFaultGet(FirstFault& out);

// Call from loop(). If an exception happened but the retried instruction then
// worked (so the device didn't crash), logs it once and starts watching again.
void firstFaultLoop();
