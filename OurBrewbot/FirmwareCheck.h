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
 * FirmwareCheck.h — Checks that an uploaded firmware file is complete and
 * undamaged before it is installed. See FirmwareCheck.cpp.
 */

#include <stdint.h>
#include <stddef.h>

struct FirmwareCheck {
  uint32_t crc;        // CRC of the bytes received so far
  uint32_t received;   // bytes received so far
  uint8_t  field[8];   // the image's own length and CRC, as received
};

// Call when an upload starts
void firmwareCheckStart(FirmwareCheck& check);

// Call with every chunk of the upload, in order
void firmwareCheckAdd(FirmwareCheck& check, const uint8_t* data, size_t len);

// True if the whole file has arrived and matches the length and CRC the build
// wrote into it
bool firmwareCheckPassed(const FirmwareCheck& check);
