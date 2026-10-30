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
 * MqttParse.cpp — MQTT command-topic parsing
 *
 * See MqttParse.h. Extracted from mqttMessageCallback() in Mqtt.cpp — no
 * behaviour change, just made independently testable.
 */

#include "MqttParse.h"
#include <cmath>
#include <cstdlib>

bool parseMqttCommandTopic(const char* topic, const char* baseTopic,
                            char* scope, size_t scopeSize,
                            char* key, size_t keySize) {
  size_t baseLen = strlen(baseTopic);
  if (strncmp(topic, baseTopic, baseLen) != 0 || topic[baseLen] != '/') return false;

  const char* rest   = topic + baseLen + 1;
  const char* slash1 = strchr(rest, '/');
  if (!slash1) return false;
  size_t scopeLen = slash1 - rest;
  if (scopeLen == 0 || scopeLen >= scopeSize) return false;
  memcpy(scope, rest, scopeLen);
  scope[scopeLen] = '\0';

  const char* keyStart = slash1 + 1;
  const char* slash2   = strchr(keyStart, '/');
  if (!slash2 || strcmp(slash2, "/set") != 0) return false;
  size_t keyLen = slash2 - keyStart;
  if (keyLen == 0 || keyLen >= keySize) return false;
  memcpy(key, keyStart, keyLen);
  key[keyLen] = '\0';

  return true;
}

int fermenterIndexFromScope(const char* scope) {
  // Exactly "Fermenter" and one digit. atoi() used to read "FermenterX" and
  // "Fermenter0abc" as fermenter 0.
  if (strncmp(scope, "Fermenter", 9) != 0) return -1;
  char digit = scope[9];
  if (digit < '0' || digit > '9' || scope[10] != '\0') return -1;
  int idx = digit - '0';
  if (idx >= MAX_FERMENTERS) return -1;
  return idx;
}

bool parseMqttNumber(const char* text, float* out) {
  // strtof() reads as much of the text as it can, so check where it stopped:
  // only spaces may follow the number. atof() turned "abc" into 0, a valid
  // hysteresis or compressor delay.
  char* end = nullptr;
  float value = strtof(text, &end);
  if (end == text) return false;          // no number at all
  while (*end == ' ') end++;
  if (*end != '\0') return false;         // something after the number
  // strtof() also reads "nan" and "inf". NaN passes every range check, as any
  // comparison with it is false, and a NaN floor or ceiling meant heating or
  // cooling never stopped.
  if (!std::isfinite(value)) return false;
  *out = value;
  return true;
}
