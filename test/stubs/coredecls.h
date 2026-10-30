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
// Stand-in for the core's coredecls.h: just crc32(), which FirmwareCheck.cpp
// uses. The same code as the core's crc32.cpp, less its pgm_read_byte(), so a
// CRC worked out here is the one the device works out.

#include <cstdint>
#include <cstddef>

static uint32_t crc32(const void* data, size_t length, uint32_t crc = 0xffffffff) {
  const uint8_t* ldata = (const uint8_t*)data;
  while (length--) {
    uint8_t c = *ldata++;
    for (uint32_t i = 0x80; i > 0; i >>= 1) {
      bool bit = crc & 0x80000000;
      if (c & i)
        bit = !bit;
      crc <<= 1;
      if (bit)
        crc ^= 0x04c11db7;
    }
  }
  return crc;
}
