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
/*
 * MqttParse.h — MQTT command-topic parsing
 *
 * Pulled out of Mqtt.cpp's mqttMessageCallback() so the pure parsing logic can
 * be unit tested without pulling in WiFi/PubSubClient. Splits
 * "<baseTopic>/<scope>/<key>/set" into scope/key strings, and maps a
 * "FermenterN" scope to a fermenter index.
 */

#include "Config.h"

// Split "<baseTopic>/<scope>/<key>/set" into scope/key. Returns false, leaving
// scope/key untouched, if: topic doesn't start with baseTopic + '/', there's no
// second '/' (no scope), scope is empty or >= scopeSize, the key segment isn't
// followed by exactly "/set", or key is empty or >= keySize.
bool parseMqttCommandTopic(const char* topic, const char* baseTopic,
                            char* scope, size_t scopeSize,
                            char* key, size_t keySize);

// Extract the fermenter index from a "FermenterN" scope string. Returns -1 if
// scope doesn't start with "Fermenter" or N is outside [0, MAX_FERMENTERS).
int fermenterIndexFromScope(const char* scope);
