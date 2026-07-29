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
