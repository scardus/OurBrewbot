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
 * FirmwareCheck.cpp — Is an uploaded firmware file complete and undamaged?
 *
 * The OTA updater only checks the first byte of an upload, so a file cut
 * short by a failed download, or with damaged bytes, could be installed and
 * leave the controller needing a USB cable to recover.
 *
 * Every image the ESP8266 Arduino build makes already says how long it should
 * be and what its CRC is: the core's elf2bin.py writes the length at byte 4112
 * and the CRC at byte 4116. The CRC covers the whole file, with those 8 bytes
 * counted as zeros. It is the same check the core's ESP.checkFlashCRC() makes
 * on the running firmware, so this uses the core's own crc32().
 *
 * An image built some other way (the original MyBrewbot firmware, an old core)
 * fails the check and is refused. Those go on by USB anyway.
 */

#include "FirmwareCheck.h"
#include <coredecls.h>   // crc32()

// Where the image's length and CRC are (see elf2bin.py in the core's tools)
static const uint32_t CRC_FIELD_OFFSET = 4096 + 16;
static const uint32_t CRC_FIELD_SIZE   = 8;

void firmwareCheckStart(FirmwareCheck& check) {
  check.crc      = 0xFFFFFFFF;   // crc32()'s starting value
  check.received = 0;
  for (uint32_t i = 0; i < CRC_FIELD_SIZE; i++) check.field[i] = 0;
}

void firmwareCheckAdd(FirmwareCheck& check, const uint8_t* data, size_t len) {
  static const uint8_t zero = 0;
  size_t i = 0;
  while (i < len) {
    uint32_t pos = check.received + i;
    if (pos >= CRC_FIELD_OFFSET && pos < CRC_FIELD_OFFSET + CRC_FIELD_SIZE) {
      // Part of the length and CRC: keep it, and count it as a zero
      check.field[pos - CRC_FIELD_OFFSET] = data[i];
      check.crc = crc32(&zero, 1, check.crc);
      i++;
      continue;
    }
    // Everything up to the field, or to the end of this chunk
    size_t run = len - i;
    if (pos < CRC_FIELD_OFFSET && pos + run > CRC_FIELD_OFFSET) run = CRC_FIELD_OFFSET - pos;
    check.crc = crc32(data + i, run, check.crc);
    i += run;
  }
  check.received += len;
}

// A 4-byte number stored lowest byte first, as the ESP8266 does
static uint32_t readWord(const uint8_t* bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool firmwareCheckPassed(const FirmwareCheck& check) {
  if (check.received < CRC_FIELD_OFFSET + CRC_FIELD_SIZE) return false;
  uint32_t expectedLength = readWord(check.field);
  uint32_t expectedCrc    = readWord(check.field + 4);
  return check.received == expectedLength && check.crc == expectedCrc;
}
