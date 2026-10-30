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
// A small made-up firmware image that passes FirmwareCheck.cpp, shared by the
// firmware check and web API suites.
//
// Byte i is (i * 7 + 3) & 0xFF, except bytes 4112-4119, which hold the length
// and CRC. TEST_IMAGE_CRC was worked out with the CRC code from the core's
// elf2bin.py (in Python), not with crc32(), so the test isn't checking the
// firmware's CRC against itself. The same script gave our real firmware.bin's
// stored CRC, 0xF6268D95.

#include <cstdint>
#include <cstddef>

#define TEST_IMAGE_SIZE 5000
static const uint32_t TEST_IMAGE_CRC = 0x5B990C87;

static uint8_t g_testImage[TEST_IMAGE_SIZE];

static void storeWord(uint8_t* at, uint32_t value) {
  at[0] = value & 0xFF;
  at[1] = (value >> 8) & 0xFF;
  at[2] = (value >> 16) & 0xFF;
  at[3] = (value >> 24) & 0xFF;
}

static void makeTestImage() {
  for (size_t i = 0; i < TEST_IMAGE_SIZE; i++) g_testImage[i] = (uint8_t)((i * 7 + 3) & 0xFF);
  storeWord(g_testImage + 4112, TEST_IMAGE_SIZE);
  storeWord(g_testImage + 4116, TEST_IMAGE_CRC);
}
