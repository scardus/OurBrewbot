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
void mqttPublishLog(const char* line); // mirror a log line to <baseTopic>/Device/log (non-retained); re-entry guarded
