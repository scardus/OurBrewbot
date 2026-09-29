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
 * subsystem was running at the time. See StackCheck.cpp.
 */

#include <stdint.h>

// Note how much loop stack is left right now. Cheap enough to call often;
// it only compares two numbers unless a new deepest point has been reached.
void stackCheck();

// If a new deepest point has been reached since the last call, fills in the
// free bytes left and the subsystem that was running, and returns true.
// For the 10-minute health log.
bool stackTakeNewLow(uint32_t& freeBytes, uint8_t& module);
