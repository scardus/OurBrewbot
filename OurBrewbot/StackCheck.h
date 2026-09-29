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
 * StackCheck.h — Record the deepest point the loop stack reaches, and which
 * subsystem took it there. See StackCheck.cpp.
 */

#include <stdint.h>

// Call just AFTER `module` (a CP_* id, 0xFF for setup) has run. If the loop
// stack has gone deeper than ever before, notes the new low against `module`
// and `where` (e.g. the web URL - may be nullptr).
void stackCheck(uint8_t module, const char* where);

// If a new deepest point has been noted since the last call, fills in the
// free bytes left, the subsystem and the detail, and returns true.
// For the 10-minute health log.
bool stackTakeNewLow(uint32_t& freeBytes, uint8_t& module, const char*& where);
