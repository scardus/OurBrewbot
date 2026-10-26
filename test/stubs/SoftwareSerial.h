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
// Stand-in for ESP8266 SoftwareSerial, matching the methods Tilt.cpp calls on
// g_bleSerial. Unlike a pure no-op stub, this one carries a scripted receive
// queue: a test pushes the bytes an HM-10 would have sent with feed(), and the
// code under test drains them through available()/read() exactly as it would
// real serial data. That's what makes initBLE()/startTiltScan()/serviceTilt()
// reachable from native tests instead of hardware-only.
#include <cstdint>
#include <cstring>

#define SWSERIAL_8N1 0

class SoftwareSerial {
  // 1 KB is ample: the longest real AT+DISI? response observed is ~280 chars,
  // and the overflow test deliberately feeds more than Tilt.cpp's 320 B buffer.
  char rx_[1024];
  int  rxLen_  = 0;   // bytes queued
  int  rxRead_ = 0;   // bytes already handed to read()
  char lastPrint_[64] = {0};
  char prints_[256]   = {0};  // every command written, "|"-separated
  uint32_t baud_ = 0;  // what begin() was last given
  bool open_            = false;  // begin() called and end() not since
  bool beganWhileOpen_  = false;  // begin() without end() first
  // A scripted HM-10 (see testModuleAt()): answers AT with OK only when the
  // port is opened at the module's own rate, and changes that rate on
  // AT+BAUD<n> followed by AT+RESET, as the real module does.
  uint32_t moduleBaud_  = 0;      // 0 = no scripted module
  uint32_t pendingBaud_ = 0;      // set by AT+BAUD<n>, applied by AT+RESET

  static uint32_t baudForCode(char code) {
    switch (code) {
      case '0': return 9600;  case '1': return 19200; case '2': return 38400;
      case '3': return 57600; case '5': return 4800;  case '6': return 2400;
      case '7': return 1200;  default:  return 0;
    }
  }

public:
  SoftwareSerial(int /*rxPin*/, int /*txPin*/) {}
  void begin(uint32_t baud, int /*config*/, int /*rxPin*/, int /*txPin*/,
             bool /*invert*/, unsigned int /*bufCapacity*/,
             unsigned int /*isrBufCapacity*/) {
    if (open_) beganWhileOpen_ = true;
    open_ = true;
    baud_ = baud;
  }

  void end() { open_ = false; }

  void print(const char* s) {
    if (!s) s = "";
    strncpy(lastPrint_, s, sizeof(lastPrint_) - 1);
    lastPrint_[sizeof(lastPrint_) - 1] = '\0';
    if (strlen(prints_) + strlen(s) + 2 < sizeof(prints_)) {
      if (prints_[0]) strcat(prints_, "|");
      strcat(prints_, s);
    }
    if (moduleBaud_ == 0 || baud_ != moduleBaud_) return;   // module can't hear us
    if (strcmp(s, "AT") == 0) feed("OK");
    else if (strncmp(s, "AT+BAUD", 7) == 0) pendingBaud_ = baudForCode(s[7]);
    else if (strcmp(s, "AT+RESET") == 0 && pendingBaud_ != 0) {
      moduleBaud_  = pendingBaud_;
      pendingBaud_ = 0;
    }
  }

  int available() { return rxLen_ - rxRead_; }
  int read() { return (rxRead_ < rxLen_) ? (unsigned char)rx_[rxRead_++] : -1; }

  // ---- test-only helpers ----

  // Queue bytes as if the module had sent them. Call more than once to
  // deliver a response in chunks across separate service passes.
  void feed(const char* s) {
    if (!s) return;
    size_t n = strlen(s);
    if (rxLen_ + (int)n > (int)sizeof(rx_)) n = sizeof(rx_) - rxLen_;
    memcpy(rx_ + rxLen_, s, n);
    rxLen_ += (int)n;
  }

  // Drop anything queued/recorded - call from setUp().
  void reset() {
    rxLen_ = rxRead_ = 0;
    lastPrint_[0] = '\0';
    prints_[0] = '\0';
    baud_ = 0;
    open_ = beganWhileOpen_ = false;
    moduleBaud_ = pendingBaud_ = 0;
  }

  // Script an HM-10 that is running at `baud` (see moduleBaud_).
  void testModuleAt(uint32_t baud) { moduleBaud_ = baud; }
  uint32_t moduleBaud() const { return moduleBaud_; }

  // Every command written since reset(), "|"-separated.
  const char* prints() const { return prints_; }

  // Whether begin() was ever called on a port that was still open.
  bool beganWhileOpen() const { return beganWhileOpen_; }

  // The baud rate passed to the last begin(), so tests can check what the
  // module would actually be driven at.
  uint32_t baud() const { return baud_; }

  // The last AT command written, so tests can tell "started a scan" from
  // "decided not to".
  const char* lastPrint() const { return lastPrint_; }
};
