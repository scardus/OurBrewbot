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

#include <ESP8266WebServer.h>

// Pages are compressed at build time and sent directly from flash. Always
// supply the binary length: gzip data contains NUL bytes, unlike HTML text.
static inline void sendWebAsset(ESP8266WebServer& server, const uint8_t* data, size_t length, PGM_P type, bool immutable) {
  server.sendHeader(F("Content-Encoding"), F("gzip"));
  server.sendHeader(F("Cache-Control"), immutable ? F("public, max-age=31536000, immutable") : F("no-store"));
  server.send_P(200, type, (PGM_P)data, length);
}

static inline void sendWebPage(ESP8266WebServer& server, const uint8_t* page, size_t length) {
  sendWebAsset(server, page, length, PSTR("text/html; charset=utf-8"), false);
}
