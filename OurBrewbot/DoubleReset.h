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
 * DoubleReset.h — Pressing the reset button twice clears the saved WiFi,
 * as on the original MyBrewbot. See DoubleReset.cpp.
 */

#include <stdint.h>

// The second press must come this soon after the first one restarted the device
#define DOUBLE_RESET_WINDOW_MS  10000

// Call first thing in setup(). True if this start-up is the second press of
// the reset button within DOUBLE_RESET_WINDOW_MS - the caller then clears the
// saved WiFi.
bool doubleResetDetected();

// Call every loop() pass. Forgets the first press once the window has passed.
void doubleResetLoop();
