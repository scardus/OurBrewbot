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
// Stand-in for WiFiClient. Reports.cpp only needs the type to exist (it
// instantiates one to hand to HTTPClient::begin() and never reads it back), but
// WebAPI.cpp's sendJsonDoc() serialises a document straight into the client
// returned by server.client() rather than building an intermediate String - so
// it also has to behave as an ArduinoJson output sink.
//
// The two write() overloads are exactly what ArduinoJson's default Writer looks
// for. Bytes land in the shared response recorder, so a JSON body written this
// way reads back the same as one passed to server.send().
//
// connected() is settable because sendJsonDoc() guards on it: a client that
// drops between the header and the body must produce no payload.

#include <cstdint>
#include <cstddef>
#include <IPAddress.h>
#include <HttpResponseRecorder.h>

static bool      g_clientConnected = true;
static IPAddress g_clientRemoteIP  = IPAddress(192, 168, 0, 99);

class WiFiClient {
public:
  bool connected() { return g_clientConnected; }
  void stop()      {}

  IPAddress remoteIP() { return g_clientRemoteIP; }

  size_t write(uint8_t c) {
    const char ch = (char)c;
    httpRespAppend(&ch, 1);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t size) {
    httpRespAppend((const char*)buf, size);
    return size;
  }
};

static void clientTestSetConnected(bool connected) { g_clientConnected = connected; }
