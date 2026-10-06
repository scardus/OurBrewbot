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
 * StackProbe.h — Measure how much loop stack one section of code uses.
 *
 * A diagnostic for the loop stack investigation, built only when STACK_PROBE
 * is defined (see build_src_flags in platformio.ini). Without it every macro
 * below compiles to nothing, so the probes can stay in the code and the
 * native tests never see them. See StackProbe.cpp for how it works.
 *
 * Usage - wrap the section in a BEGIN/END pair with the same variable name:
 *
 *   STACK_PROBE_BEGIN(syslogProbe);
 *   sendSyslog(level, buf);
 *   STACK_PROBE_END(PROBE_LOG_SYSLOG, syslogProbe);
 */

#include <stdint.h>

// The sections being measured. Keep in step with kProbeNames in StackProbe.cpp.
//
// Keep each probe around a small section, never a whole web handler: inside
// a probe ESP.getFreeContStack() only sees the section so far, so /health
// would report the wrong "freeStack".
enum StackProbeId : uint8_t {
  PROBE_ISPINDEL_PARSE = 0,  // deserializeJson() of an iSpindel POST
  PROBE_ISPINDEL_LOG,        // the "[ISPINDEL] Slot ..." logMsg() call, start to end
  PROBE_ISPINDEL_REPLY,      // sending the reply to an iSpindel POST
  PROBE_LOG_FORMAT,          // vsnprintf_P() in vlogMsg() - formatting any log line
  PROBE_LOG_SYSLOG,          // sendSyslog() - the UDP send of any log line
  PROBE_LOG_MQTT,            // mqttPublishLog() - the MQTT copy of any log line
  PROBE_IRQ_ONLY,            // a 2 ms busy wait - only interrupts use stack here
  PROBE_COUNT
};

#ifdef STACK_PROBE

struct StackProbe {
  uint32_t startFree;  // stack left below the stack pointer at the start, 0 = not probing
  uint32_t oldFree;    // the boot-time high-water mark before the probe repainted it
};

StackProbe stackProbeBegin();
void stackProbeEnd(uint8_t id, const StackProbe& probe);
void stackProbeSetWhere(const char* where);
void stackProbeIrqSample();
void stackProbeLogNew();

#define STACK_PROBE_BEGIN(var)         StackProbe var = stackProbeBegin()
#define STACK_PROBE_END(id, var)       stackProbeEnd(id, var)
#define STACK_PROBE_SET_WHERE(where)   stackProbeSetWhere(where)
#define STACK_PROBE_IRQ_SAMPLE()       stackProbeIrqSample()
#define STACK_PROBE_LOG_NEW()          stackProbeLogNew()

#else

#define STACK_PROBE_BEGIN(var)
#define STACK_PROBE_END(id, var)
#define STACK_PROBE_SET_WHERE(where)
#define STACK_PROBE_IRQ_SAMPLE()
#define STACK_PROBE_LOG_NEW()

#endif
