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
// Stand-in for the core's IPAddress. Its own header, as in the real core,
// because both ESP8266WiFi.h and WiFiUdp.h need the type - Log.cpp resolves a
// syslog host into one and then hands it to WiFiUDP.beginPacket().

#include <cstdint>
#include <cstdio>
#include <Arduino.h>   // for String, which toString() returns

class IPAddress {
  uint8_t octets_[4] = {0, 0, 0, 0};
public:
  IPAddress() {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    octets_[0] = a; octets_[1] = b; octets_[2] = c; octets_[3] = d;
  }

  uint8_t operator[](int i) const { return octets_[i & 3]; }

  bool operator==(const IPAddress& o) const {
    for (int i = 0; i < 4; i++) if (octets_[i] != o.octets_[i]) return false;
    return true;
  }
  bool operator!=(const IPAddress& o) const { return !(*this == o); }

  // Points at a per-instance buffer, so two addresses can be compared as
  // strings without one clobbering the other.
  const char* toStringRaw() const {
    snprintf(const_cast<char*>(str_), sizeof(str_), "%u.%u.%u.%u",
             octets_[0], octets_[1], octets_[2], octets_[3]);
    return str_;
  }
  String toString() const { return String(toStringRaw()); }

private:
  char str_[16] = {0};
};
