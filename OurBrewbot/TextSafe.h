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

// Copy text from outside the firmware into a fixed-size field. Use it instead
// of strlcpy() for names, hosts and IDs. It keeps the copy valid UTF-8, so
// the JSON it ends up in can be read by strict parsers (Python, Home
// Assistant), not just browsers:
//  - any byte that is not part of a valid UTF-8 character becomes '?';
//  - when the text is too long for the field, it is cut before the first
//    character that does not fit whole - strlcpy() would cut "Biere" with an
//    accented e half way through the e, leaving an invalid byte at the end.
// The result is always NUL terminated. dst and src must not overlap.
void copyText(char* dst, const char* src, size_t size);

// True if topic can be used as the MQTT base topic, the start of every topic
// the firmware publishes and subscribes to. fieldSize is the size of the field
// it will be stored in (the NUL needs one byte of it). It must be:
//  - not empty, and short enough to fit the field whole - cutting it would
//    quietly move every topic;
//  - free of '+' and '#': they are wildcards, and a publish to a topic holding
//    one makes the broker drop the connection;
//  - not starting with '$', which is reserved for the broker's own topics;
//  - free of control characters, and valid UTF-8, which MQTT requires.
// '/' is allowed (e.g. "home/brewery"), and so are spaces.
bool isValidBaseTopic(const char* topic, size_t fieldSize);
