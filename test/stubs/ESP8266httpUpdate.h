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
// Stand-in for the OTA updater. WebAPI.h includes <ESP8266httpUpdate.h>, and
// WebAPI.cpp's upload handler drives the global `Update` object through
// begin/write/end.
//
// No native test exercises a firmware upload - flashing is inherently a device
// operation - so this exists to let the rest of WebAPI.cpp compile, with just
// enough state that the handler's success and failure branches are reachable
// if a future test wants them.

#include <cstdint>
#include <cstddef>

class UpdaterStub {
public:
  bool   failBegin = false;   // make begin() refuse, for the "no space" branch
  int    error     = 0;
  size_t written   = 0;

  bool begin(size_t /*size*/) { return !failBegin; }
  size_t write(uint8_t* /*data*/, size_t len) { written += len; return len; }
  bool end(bool /*evenIfRemaining*/ = false) { return error == 0; }
  bool hasError() { return error != 0; }
  int  getError() { return error; }
  template <typename T> void printError(T&) {}
};

static UpdaterStub Update;
