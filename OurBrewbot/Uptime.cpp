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
 * Uptime.cpp — Seconds since boot, without the 49.7-day wrap of millis().
 *
 * millis() is a 32-bit count of milliseconds, so it goes back to 0 every
 * 49.7 days. "millis() - last >= interval" stays correct across that, as long
 * as the gap is shorter than 49.7 days - fine for the loop's timers. But a gap
 * that can grow without limit ("when was this iSpindel last seen", "when did
 * the fridge last stop") wraps too, and then looks small again. Use this clock
 * for those, and for reporting uptime.
 *
 * How it works: each uptimeTick() adds the milliseconds since the previous
 * tick to a 64-bit total. One step is never more than one loop() pass, so the
 * 32-bit subtraction is always right, and the 64-bit total never wraps.
 */

#include "Uptime.h"
#include <Arduino.h>

static uint32_t s_lastMs  = 0;   // millis() at the last uptimeTick()
static uint64_t s_totalMs = 0;   // milliseconds counted up to s_lastMs

void uptimeTick() {
  uint32_t now = millis();
  s_totalMs += (uint32_t)(now - s_lastMs);   // correct across a millis() wrap
  s_lastMs = now;
}

uint32_t uptimeSeconds() {
  // Adds the part since the last tick, so the value is current between ticks
  return (uint32_t)((s_totalMs + (uint32_t)(millis() - s_lastMs)) / 1000);
}

uint32_t uptimeStamp() {
  uint32_t s = uptimeSeconds();
  return s ? s : 1;
}
