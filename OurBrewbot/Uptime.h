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
 * Uptime.h — Seconds since boot, without the 49.7-day wrap of millis().
 * See Uptime.cpp.
 */

#include <stdint.h>

// Fold the time since the last call into the running total. Call once at the
// top of every loop() pass; it must run at least once every 49.7 days, which
// loop() always does.
void uptimeTick();

// Seconds since boot. Never wraps in practice (a uint32_t of seconds lasts
// 136 years). Only reads, so it is safe to call from anywhere, including the
// crash handler.
uint32_t uptimeSeconds();

// uptimeSeconds(), but never 0. For timestamps where 0 means "never" (an
// iSpindel not heard from since boot, a fermenter in band): a stamp taken in
// the very first second after boot would otherwise read as "never".
uint32_t uptimeStamp();
