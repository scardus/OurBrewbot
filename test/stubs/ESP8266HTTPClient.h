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
// Stand-in for ESP8266HTTPClient that captures instead of sends: the URL and
// the serialized JSON body of each POST are recorded, so tests can assert on
// exactly what would have gone to Brewfather / Brewer's Friend without any
// network. POST()'s return value is settable to exercise the error branch.
//
// GET() (UpdateCheck.cpp's version.json download) returns nextStatus too, and
// the reply body and its advertised size are settable, so the update-check
// tests can script a good reply, an HTTP error or a malformed file.
#include <cstdint>
#include <cstring>
#include <Arduino.h>   // String
#include <WiFiClient.h>

struct HttpTestRecord {
  char url[192];
  char body[1024];
  int  postCount;
  int  nextStatus;
  int  getCount;
  char response[1024];   // what getString() returns
  int  responseSize;     // what getSize() returns (-1 = "not given", as for a chunked reply)
};

static HttpTestRecord g_httpTest = { {0}, {0}, 0, 200, 0, {0}, -1 };

static void httpTestReset() {
  g_httpTest.url[0]       = '\0';
  g_httpTest.body[0]      = '\0';
  g_httpTest.postCount    = 0;
  g_httpTest.nextStatus   = 200;
  g_httpTest.getCount     = 0;
  g_httpTest.response[0]  = '\0';
  g_httpTest.responseSize = -1;
}

// Script the reply to the next GET: status code and body. The size is taken
// from the body, as a real server's Content-Length would be.
static void httpTestSetReply(int status, const char* body) {
  g_httpTest.nextStatus = status;
  strncpy(g_httpTest.response, body ? body : "", sizeof(g_httpTest.response) - 1);
  g_httpTest.response[sizeof(g_httpTest.response) - 1] = '\0';
  g_httpTest.responseSize = (int)strlen(g_httpTest.response);
}

class HTTPClient {
public:
  void begin(WiFiClient& /*client*/, const char* url) {
    strncpy(g_httpTest.url, url ? url : "", sizeof(g_httpTest.url) - 1);
    g_httpTest.url[sizeof(g_httpTest.url) - 1] = '\0';
  }
  void setTimeout(uint16_t /*ms*/) {}
  void addHeader(const char* /*name*/, const char* /*value*/) {}

  int POST(String& body) {
    strncpy(g_httpTest.body, body.c_str(), sizeof(g_httpTest.body) - 1);
    g_httpTest.body[sizeof(g_httpTest.body) - 1] = '\0';
    g_httpTest.postCount++;
    return g_httpTest.nextStatus;
  }

  // The real client's buffer overload - posts exactly `size` bytes.
  int POST(const uint8_t* payload, size_t size) {
    if (size > sizeof(g_httpTest.body) - 1) size = sizeof(g_httpTest.body) - 1;
    memcpy(g_httpTest.body, payload, size);
    g_httpTest.body[size] = '\0';
    g_httpTest.postCount++;
    return g_httpTest.nextStatus;
  }

  int GET() {
    g_httpTest.getCount++;
    return g_httpTest.nextStatus;
  }

  int    getSize()              { return g_httpTest.responseSize; }
  String getString()            { return String(g_httpTest.response); }
  String errorToString(int)     { return String("stub error"); }
  void   end()                  {}
};
