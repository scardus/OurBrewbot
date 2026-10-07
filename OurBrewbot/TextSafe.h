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
 * TextSafe.h — checks for text that comes from outside the firmware
 *
 * Names, hosts and IDs arrive from the WebUI, from devices such as iSpindels
 * and from imported config files. They end up in JSON (the web API, MQTT, Home
 * Assistant discovery) and in MQTT topic names, which have rules of their own.
 * These helpers keep that text valid. They have no Arduino dependencies, so
 * they are unit tested natively.
 */

#include <stddef.h>

// Length of the valid UTF-8 character starting at s (2 to 4 bytes), or 0 if
// the bytes there are not valid UTF-8 - a garbled Tilt reading, for example.
// Stops at the first bad byte, so it never reads past the string's NUL.
size_t utf8CharLength(const unsigned char* s);
