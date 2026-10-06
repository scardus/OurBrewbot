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
 * Mqtt.h -- MQTT client for publishing fermenter data
 *
 * Publishes each value on its own topic:
 *   <baseTopic>/<fermenterName>/beer_temperature
 *   <baseTopic>/<fermenterName>/ambient_temperature
 *   etc.
 */

#include "Config.h"

void initMqtt();
void mqttLoop();
void reportMqtt();
void mqttApplyControlSubscription(); // subscribe or unsubscribe command wildcard based on allowControl
void mqttPendingSaveCheck();          // call from main loop — writes deferred config save after MQTT commands
bool testMqtt();
void publishAllHaDiscovery();        // publish HA discovery for all MQTT-enabled fermenters (requires haDiscovery=true)
bool forcePublishAllHaDiscovery();   // same but ignores haDiscovery flag, connects if needed (for manual/button trigger)
void cleanupAllHaDiscovery();        // remove HA discovery configs (publish empty payloads)
void mqttPublishUpdateStatus();      // publish <baseTopic>/Device/latest_version after an update check
void mqttPublishLog(uint8_t level, const char* timestamp, const char* msg); // mirror a log line (timestamp + msg, joined here) to <baseTopic>/Device/log as JSON with RFC 5424 severity; re-entry guarded
