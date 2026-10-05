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
 * StackProbe.cpp — Measure how much loop stack one section of code uses.
 *
 * Why: StackCheck.cpp says the deepest point is reached during a POST to
 * /iSpindel, but not which part of that request goes deep, and the depth
 * varies from one identical request to the next. Each probe answers that for
 * one section: the deepest it has gone below where it started, and the least
 * stack left at that moment.
 *
 * How: the core fills the unused loop stack with a marker value at boot, and
 * ESP.getFreeContStack() counts how much of it has never been overwritten.
 * stackProbeBegin() repaints the marker below the current stack pointer, so
 * once the section has run, that count shows how deep the section itself
 * went - its own calls plus any interrupt that arrived while it was running
 * (interrupts use whatever stack is current). PROBE_IRQ_ONLY measures the
 * interrupts on their own, so they can be told apart.
 *
 * The repaint would wipe out the boot-time high-water mark that StackCheck
 * and the [HEALTH] log rely on. stackProbeEnd() puts it back by writing one
 * non-marker word where the old deepest point was, so those keep reporting
 * exactly as they would without the probes. Probes can be nested.
 *
 * The figures include ~100 bytes of the probe's own overhead (the core's
 * repaint leaves 64 bytes below the stack pointer alone). Built only with
 * STACK_PROBE defined - see StackProbe.h.
 */

#ifdef STACK_PROBE

#include "StackProbe.h"
#include "StackCheck.h"
#include "Crash.h"
#include "Log.h"
#include <Arduino.h>
#include <cont.h>
#include <string.h>

// Names for the [PROBE] log lines, in StackProbeId order.
static const char* const kProbeNames[PROBE_COUNT] = {
  "iSpindel parse",
  "iSpindel log line",
  "iSpindel reply",
  "log format",
  "log syslog send",
  "log MQTT copy",
  "interrupts only",
};

struct ProbeRecord {
  uint32_t deepest;                  // most stack the section has used, bytes
  uint32_t leastFree;                // least stack left while it ran, bytes
  uint8_t  module;                   // CP_* running at the leastFree moment
  char     where[STACK_WHERE_LEN];   // web URL at that moment, "" if none
  bool     isNew;                    // changed since it was last logged
};

static ProbeRecord s_records[PROBE_COUNT];
static bool        s_recordsReady = false;
static char        s_where[STACK_WHERE_LEN] = "";   // URL being handled now

// Called from dispatchApiRequest() in WebAPI.cpp - nullptr when it's done.
// cppcheck-suppress unusedFunction
void stackProbeSetWhere(const char* where) {
  strlcpy(s_where, where ? where : "", sizeof(s_where));
}

StackProbe stackProbeBegin() {
  StackProbe probe = { 0, 0 };

  const unsigned* sp;
  asm volatile("mov %0, a1" : "=r"(sp));

  // Only the loop stack is painted. A log line written from the WiFi SDK's
  // own context (a WiFi event) runs on a different stack - skip those.
  if (sp <= g_pcont->stack || sp >= g_pcont->stack_end) return probe;

  probe.oldFree   = ESP.getFreeContStack();
  ESP.resetFreeContStack();   // repaint everything below the stack pointer
  probe.startFree = (uint32_t)(sp - g_pcont->stack) * 4;
  return probe;
}

void stackProbeEnd(uint8_t id, const StackProbe& probe) {
  if (probe.startFree == 0 || id >= PROBE_COUNT) return;

  uint32_t freeNow = ESP.getFreeContStack();
  uint32_t used    = (probe.startFree > freeNow) ? probe.startFree - freeNow : 0;

  if (!s_recordsReady) {
    for (int i = 0; i < PROBE_COUNT; i++) s_records[i].leastFree = UINT32_MAX;
    s_recordsReady = true;
  }

  ProbeRecord& rec = s_records[id];
  if (used > rec.deepest) {
    rec.deepest = used;
    rec.isNew   = true;
  }
  if (freeNow < rec.leastFree) {
    rec.leastFree = freeNow;
    rec.module    = checkpointCurrent();
    strlcpy(rec.where, s_where, sizeof(rec.where));
    rec.isNew     = true;
  }

  // Put back the boot-time high-water mark the repaint wiped out, if this
  // section did not go deeper than it. Everything below the stack pointer is
  // unused now, so the word is free to write.
  if (probe.oldFree < freeNow) {
    g_pcont->stack[probe.oldFree / 4] = 0;
  }
}

// Called from loop() in OurBrewbot.cpp. Once a second, busy-wait for 2 ms
// without yielding: nothing of ours runs in that time, so whatever stack the
// probe sees used was used by interrupts (WiFi, timers, serial).
// cppcheck-suppress unusedFunction
void stackProbeIrqSample() {
  static uint32_t s_lastSampleMs = 0;
  if (millis() - s_lastSampleMs < 1000) return;
  s_lastSampleMs = millis();

  STACK_PROBE_BEGIN(irqProbe);
  delayMicroseconds(2000);
  STACK_PROBE_END(PROBE_IRQ_ONLY, irqProbe);
}

// Called from onTenMinuteTimer() in OurBrewbot.cpp. One line per section
// that has a new record since the last call.
// cppcheck-suppress unusedFunction
void stackProbeLogNew() {
  for (int i = 0; i < PROBE_COUNT; i++) {
    if (!s_records[i].isNew) continue;
    s_records[i].isNew = false;

    // Copied first: logging runs the log probes, which can update the records.
    ProbeRecord rec = s_records[i];
    logMsg("[PROBE] %s: deepest %u bytes, least stack left %u bytes, in %s %s",
           kProbeNames[i], rec.deepest, rec.leastFree,
           rec.module == 0xFF ? "setup" : checkpointName(rec.module), rec.where);
  }
}

#endif  // STACK_PROBE
