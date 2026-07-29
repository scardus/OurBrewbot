/*
 * MqttParse.cpp — MQTT command-topic parsing
 *
 * See MqttParse.h. Extracted from mqttMessageCallback() in Mqtt.cpp — no
 * behaviour change, just made independently testable.
 */

#include "MqttParse.h"

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
  if (strncmp(scope, "Fermenter", 9) != 0) return -1;
  int idx = atoi(scope + 9);
  if (idx < 0 || idx >= MAX_FERMENTERS) return -1;
  return idx;
}
