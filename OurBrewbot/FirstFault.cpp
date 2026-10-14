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
 * FirstFault.cpp — Remember the FIRST CPU exception since boot.
 *
 * How it works:
 *   - firstFaultInstall() registers firstFaultHandler() as the exception
 *     handler for the causes a crash comes from. Whatever handler was there
 *     before is kept.
 *   - When one of those exceptions happens, firstFaultHandler() copies the
 *     processor's view of it (cause, faulting address, caller, stack pointer)
 *     into s_fault, puts the previous handlers back and returns. The CPU then
 *     runs the faulting instruction again: a real fault simply happens again
 *     and now takes the normal crash path, so the crash report is unchanged -
 *     it just also carries s_fault, via custom_crash_callback() in Crash.cpp.
 *   - If the retried instruction works (a one-off glitch, e.g. a bad read from
 *     flash), the device carries on. firstFaultLoop() then logs what happened,
 *     forgets it and starts watching again.
 *
 * Cause 3 (LoadStoreError) is deliberately NOT watched: that one happens in
 * normal running, whenever code reads single bytes from flash or IRAM.
 *
 * Device-only code - the native tests replace firstFaultGet() with a stub.
 */

#include "FirstFault.h"
#include "Log.h"
#include <Arduino.h>
#include <esp8266_undocumented.h>   // struct __exception_frame, _xtos_set_exception_handler

namespace {

// The exception causes a crash comes from (the EXCCAUSE_* numbers). Written
// out rather than taken from <xtensa/corebits.h>: PlatformIO finds a copy of
// that header in the core's GDBStub library and links the whole debugger in.
// Not const: the handler runs from IRAM and reads this, so it must be in RAM,
// never in flash.
int s_watchedCauses[] = {
   0,   // EXCCAUSE_ILLEGAL           - illegal instruction
   2,   // EXCCAUSE_INSTR_ERROR       - instruction fetch error
   9,   // EXCCAUSE_UNALIGNED         - unaligned load or store
  20,   // EXCCAUSE_INSTR_PROHIBITED  - instruction fetch from a bad address
  28,   // EXCCAUSE_LOAD_PROHIBITED   - load from a bad address
  29,   // EXCCAUSE_STORE_PROHIBITED  - store to a bad address
};
constexpr size_t WATCHED_COUNT = sizeof(s_watchedCauses) / sizeof(s_watchedCauses[0]);

// The handlers that were registered before ours, put back after a fault
fn_c_exception_handler_t s_previousHandlers[WATCHED_COUNT];

// Size of the frame the exception entry code builds below the stack pointer
// (UserFrameTotalSize in the core's xtruntime-frames.h)
constexpr uint32_t EXCEPTION_FRAME_SIZE = 0x100;

FirstFault    s_fault;
volatile bool s_haveFault = false;

// Runs inside the exception, so it lives in IRAM and calls nothing in flash:
// the fault being handled might itself be a failed read from flash.
IRAM_ATTR void firstFaultHandler(struct __exception_frame* ef, int cause) {
  uint32_t excvaddr;
  asm volatile ("rsr %0, excvaddr" : "=r"(excvaddr));

  if (!s_haveFault) {
    // One field at a time - a struct copy could become a memcpy() call
    s_fault.cause    = (uint32_t)cause;
    s_fault.epc      = ef->epc;
    s_fault.ps       = ef->ps;
    s_fault.a0       = ef->a0;
    s_fault.sp       = (uint32_t)ef + EXCEPTION_FRAME_SIZE;
    s_fault.excvaddr = excvaddr;
    asm volatile ("" ::: "memory");   // all of s_fault is written before the flag
    s_haveFault      = true;
  }

  // Put the previous handlers back (_xtos_set_exception_handler is in ROM)
  // and return, so the instruction is retried - see the top of the file.
  for (size_t i = 0; i < WATCHED_COUNT; i++) {
    _xtos_set_exception_handler(s_watchedCauses[i], s_previousHandlers[i]);
  }
}

void installHandler() {
  for (size_t i = 0; i < WATCHED_COUNT; i++) {
    s_previousHandlers[i] = _xtos_set_exception_handler(s_watchedCauses[i], firstFaultHandler);
  }
}

} // namespace

// Called from setup() in OurBrewbot.cpp, which cppcheck does not connect to
// this definition.
// cppcheck-suppress unusedFunction
void firstFaultInstall() {
  installHandler();
}

// Called from custom_crash_callback() in Crash.cpp - same cross-file blind
// spot as firstFaultInstall()
// cppcheck-suppress unusedFunction
bool firstFaultGet(FirstFault& out) {
  if (!s_haveFault) return false;
  out = s_fault;
  return true;
}

// Called from loop() in OurBrewbot.cpp - see firstFaultInstall()
// cppcheck-suppress unusedFunction
void firstFaultLoop() {
  if (!s_haveFault) return;
  // Still running, so the retried instruction worked. Report it here, then
  // clear it so a later crash reports its own first fault, not this one.
  logMsgL(SYSLOG_ERR,
          "[CRASH] Survived a CPU exception: cause %u at 0x%08x, a0 0x%08x, SP 0x%08x, EXCVADDR 0x%08x, PS 0x%08x",
          s_fault.cause, s_fault.epc, s_fault.a0, s_fault.sp, s_fault.excvaddr, s_fault.ps);
  s_haveFault = false;
  installHandler();   // watch again
}
