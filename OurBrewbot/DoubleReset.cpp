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
 * DoubleReset.cpp — Detects two presses of the reset button in quick succession
 *
 * The first press leaves a marker in RTC memory, which keeps its contents
 * through a reset but not through a power cut. If the device is reset again
 * while the marker is there, that is the second press. loop() removes the
 * marker once the device has been running for DOUBLE_RESET_WINDOW_MS.
 *
 * Only a press of the reset button counts (the chip reports it as "External
 * System"). A crash, a watchdog reset, a firmware update or a reboot from the
 * admin page never does, and also removes any marker. RTC memory, rather than
 * a file, means nothing is written to flash on every start-up, and unplugging
 * the power twice can never clear the WiFi.
 *
 * The original firmware also counted 5 presses to erase the flash - that is
 * not copied.
 */

#include "DoubleReset.h"
#include <Arduino.h>
#include <user_interface.h>   // struct rst_info, REASON_EXT_SYS_RST

// RTC memory word for the marker. Crash.cpp uses words 0-54 and
// OriginalConfig.cpp words 101-102; RTC user memory has 128 words.
static const uint32_t DOUBLE_RESET_OFFSET = 100;
// Any other value - including whatever RTC memory holds after a power cut -
// means "no first press"
static const uint32_t DOUBLE_RESET_MAGIC  = 0xD0B1E5E7;

static bool s_markerSet = false;   // this start-up left the marker

static void writeMarker(uint32_t value) {
  ESP.rtcUserMemoryWrite(DOUBLE_RESET_OFFSET, &value, sizeof(value));
}

bool doubleResetDetected() {
  uint32_t marker = 0;
  ESP.rtcUserMemoryRead(DOUBLE_RESET_OFFSET, &marker, sizeof(marker));
  bool firstPressSeen = (marker == DOUBLE_RESET_MAGIC);

  if (ESP.getResetInfoPtr()->reason != REASON_EXT_SYS_RST) {
    if (firstPressSeen) writeMarker(0);
    return false;
  }
  if (firstPressSeen) {
    writeMarker(0);   // a third press starts counting again
    return true;
  }
  writeMarker(DOUBLE_RESET_MAGIC);
  s_markerSet = true;
  return false;
}

void doubleResetLoop() {
  if (s_markerSet && millis() >= DOUBLE_RESET_WINDOW_MS) {
    writeMarker(0);
    s_markerSet = false;
  }
}
