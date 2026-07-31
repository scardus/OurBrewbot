#pragma once
// Recording stand-in for PubSubClient, the MQTT client Mqtt.cpp publishes
// through.
//
// Mqtt.cpp instantiates a WiFiClient and a PubSubClient at file scope, which is
// why Round 5 extracted the pure topic parsing into MqttParse.cpp rather than
// pulling the whole file into a test. This stub removes that blocker: every
// publish, subscribe and connect lands in g_mqttTest instead of on a socket, so
// the discovery payloads, the state topics, the log mirror and the connect
// handshake can all be asserted on natively.
//
// Three things here are not just recording, and each unlocks something that is
// otherwise unreachable on the host:
//
//   1. The 4-argument publish() overload. removeOneEntity() clears a retained
//      HA config by publishing a ZERO-LENGTH payload through it - without this
//      overload the entire discovery-removal half of Mqtt.cpp can't be called.
//
//   2. The captured callback. mqttMessageCallback() is static inside Mqtt.cpp,
//      so a test cannot call it directly; setCallback() keeps the pointer and
//      mqttTestInject() drives it. That is the only route to the inbound
//      command path and the Home Assistant birth message.
//
//   3. Scripted failure (mqttTestSetPublishFail / mqttTestSetConnectOk). A
//      publish that fails is what reaches logPublishFailure(), and a connect
//      that fails is what drives the backoff ladder; neither happens against a
//      stub that always succeeds.

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <WiFiClient.h>

// A full publishAllHaDiscovery() on a populated config emits 240 entities
// (12 device + 24x4 fermenter + 5x8 probe + 6x8 tilt + 11x4 iSpindel), and a
// cleanup run adds the legacy fermenter table on top. 512 records leaves
// headroom for a publish-then-remove sequence in a single test.
//
// Payloads are held at the full 1024 B the firmware configures via
// setBufferSize(), so a discovery payload is never silently shortened here -
// the String stub has twice been the hidden cause of a bogus failure by
// truncating quietly, and overflowCount below exists so this one fails loudly
// instead.
#define MQTT_TEST_MAX_RECORDS  512
#define MQTT_TEST_MAX_TOPIC    128
#define MQTT_TEST_MAX_PAYLOAD  1024
#define MQTT_TEST_MAX_SUBS     32

struct MqttPublishRecord {
  char   topic[MQTT_TEST_MAX_TOPIC];
  char   payload[MQTT_TEST_MAX_PAYLOAD];  // NUL terminated for convenience
  size_t payloadLen;                      // true length, payload may contain NULs
  bool   retained;
};

// What the last connect() was called with. The LWT arguments matter: the
// broker publishes willMessage to willTopic if the device drops without a
// clean disconnect, so a regression there is invisible until a real outage.
struct MqttConnectRecord {
  char    clientId[32];
  char    username[40];
  char    password[56];
  char    willTopic[96];
  char    willMessage[32];
  uint8_t willQos;
  bool    willRetain;
  bool    hadCredentials;   // false when Mqtt.cpp passed nullptr user/pass
  int     attempts;         // connect() calls, including failures
};

struct MqttTestState {
  MqttPublishRecord records[MQTT_TEST_MAX_RECORDS];
  int  recordCount   = 0;
  int  overflowCount = 0;   // publishes dropped for want of a record slot

  char subs[MQTT_TEST_MAX_SUBS][MQTT_TEST_MAX_TOPIC];
  int  subCount = 0;
  char unsubs[MQTT_TEST_MAX_SUBS][MQTT_TEST_MAX_TOPIC];
  int  unsubCount = 0;

  MqttConnectRecord lastConnect;

  // ---- knobs ----
  bool connected  = false;
  bool connectOk  = true;   // whether the next connect() succeeds
  int  state      = 0;      // rc reported by state()
  bool failPublishes = false;
  char failTopicContains[64] = {0};  // fail only publishes whose topic matches

  // ---- what the client was configured with ----
  char     serverHost[64] = {0};
  uint16_t serverPort     = 0;
  uint16_t bufferSize     = 0;
  uint8_t  socketTimeout  = 0;
  int      loopCount      = 0;

  void (*callback)(char*, uint8_t*, unsigned int) = nullptr;

  // Called after each successful publish. The log mirror's re-entry guard can
  // only be reached by logging from inside a publish, which is exactly what
  // this hook lets a test do.
  void (*onPublish)(const char* topic) = nullptr;
};

static MqttTestState g_mqttTest;

class PubSubClient {
public:
  PubSubClient(WiFiClient&) {}

  PubSubClient& setServer(const char* host, uint16_t port) {
    strncpy(g_mqttTest.serverHost, host ? host : "", sizeof(g_mqttTest.serverHost) - 1);
    g_mqttTest.serverHost[sizeof(g_mqttTest.serverHost) - 1] = '\0';
    g_mqttTest.serverPort = port;
    return *this;
  }

  PubSubClient& setCallback(void (*cb)(char*, uint8_t*, unsigned int)) {
    g_mqttTest.callback = cb;
    return *this;
  }

  bool setBufferSize(uint16_t size) { g_mqttTest.bufferSize = size; return true; }
  void setSocketTimeout(uint8_t t)  { g_mqttTest.socketTimeout = t; }

  bool connected() { return g_mqttTest.connected; }
  int  state()     { return g_mqttTest.state; }
  bool loop()      { g_mqttTest.loopCount++; return g_mqttTest.connected; }

  bool connect(const char* id, const char* user, const char* pass,
               const char* willTopic, uint8_t willQos, bool willRetain,
               const char* willMessage) {
    MqttConnectRecord& r = g_mqttTest.lastConnect;
    copyField(r.clientId,    sizeof(r.clientId),    id);
    copyField(r.username,    sizeof(r.username),    user);
    copyField(r.password,    sizeof(r.password),    pass);
    copyField(r.willTopic,   sizeof(r.willTopic),   willTopic);
    copyField(r.willMessage, sizeof(r.willMessage), willMessage);
    r.willQos        = willQos;
    r.willRetain     = willRetain;
    r.hadCredentials = (user != nullptr);
    r.attempts++;

    g_mqttTest.connected = g_mqttTest.connectOk;
    return g_mqttTest.connected;
  }

  bool subscribe(const char* topic) {
    if (g_mqttTest.subCount < MQTT_TEST_MAX_SUBS) {
      copyField(g_mqttTest.subs[g_mqttTest.subCount], MQTT_TEST_MAX_TOPIC, topic);
      g_mqttTest.subCount++;
    }
    return true;
  }

  bool unsubscribe(const char* topic) {
    if (g_mqttTest.unsubCount < MQTT_TEST_MAX_SUBS) {
      copyField(g_mqttTest.unsubs[g_mqttTest.unsubCount], MQTT_TEST_MAX_TOPIC, topic);
      g_mqttTest.unsubCount++;
    }
    return true;
  }

  bool publish(const char* topic, const char* payload) {
    return publish(topic, payload, false);
  }

  bool publish(const char* topic, const char* payload, bool retained) {
    return record(topic, (const uint8_t*)(payload ? payload : ""),
                  payload ? strlen(payload) : 0, retained);
  }

  // The overload removeOneEntity() uses to clear a retained discovery config
  // with an empty payload.
  bool publish(const char* topic, const uint8_t* payload, unsigned int len, bool retained) {
    return record(topic, payload, len, retained);
  }

private:
  static void copyField(char* dst, size_t size, const char* src) {
    strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
  }

  bool record(const char* topic, const uint8_t* payload, size_t len, bool retained) {
    if (!g_mqttTest.connected) return false;
    if (g_mqttTest.failPublishes) return false;
    if (g_mqttTest.failTopicContains[0] && topic &&
        strstr(topic, g_mqttTest.failTopicContains) != nullptr) {
      return false;
    }
    if (g_mqttTest.recordCount >= MQTT_TEST_MAX_RECORDS) {
      g_mqttTest.overflowCount++;
      return false;
    }

    MqttPublishRecord& r = g_mqttTest.records[g_mqttTest.recordCount++];
    copyField(r.topic, MQTT_TEST_MAX_TOPIC, topic);
    size_t n = (len < MQTT_TEST_MAX_PAYLOAD - 1) ? len : MQTT_TEST_MAX_PAYLOAD - 1;
    if (payload && n) memcpy(r.payload, payload, n);
    r.payload[n]  = '\0';
    r.payloadLen  = len;   // the TRUE length, so an over-long payload is visible
    r.retained    = retained;

    if (g_mqttTest.onPublish) g_mqttTest.onPublish(r.topic);
    return true;
  }
};

// ============================================================
// TEST HELPERS
// ============================================================

static void mqttTestReset() {
  g_mqttTest = MqttTestState{};
}

// Clears the recorded traffic but keeps the connection, the registered callback
// and the knobs. A fixture needs this because the only way to populate
// Mqtt.cpp's static s_availTopic (and reset its backoff) is to drive a real
// connect, which itself publishes and subscribes - traffic no test wants to see.
static void mqttTestResetRecords() {
  g_mqttTest.recordCount   = 0;
  g_mqttTest.overflowCount = 0;
  g_mqttTest.subCount      = 0;
  g_mqttTest.unsubCount    = 0;
}

static int mqttTestPublishCount() { return g_mqttTest.recordCount; }

// Index of the first / last publish to `topic`, or -1. Both are needed: a
// cleanup test publishes discovery and then removes it, so the same topic
// appears twice with different payloads.
static int mqttTestIndexOf(const char* topic) {
  for (int i = 0; i < g_mqttTest.recordCount; i++) {
    if (strcmp(g_mqttTest.records[i].topic, topic) == 0) return i;
  }
  return -1;
}

static int mqttTestLastIndexOf(const char* topic) {
  for (int i = g_mqttTest.recordCount - 1; i >= 0; i--) {
    if (strcmp(g_mqttTest.records[i].topic, topic) == 0) return i;
  }
  return -1;
}

static bool mqttTestPublished(const char* topic) { return mqttTestIndexOf(topic) >= 0; }

// Payload of the first publish to `topic`, or nullptr when it was never
// published - deliberately not "" so a missing topic can't be mistaken for one
// carrying an empty payload, which is a meaningful state here (it is how a
// retained discovery config is cleared).
static const char* mqttTestPayloadFor(const char* topic) {
  const int i = mqttTestIndexOf(topic);
  return (i < 0) ? nullptr : g_mqttTest.records[i].payload;
}

static const char* mqttTestLastPayloadFor(const char* topic) {
  const int i = mqttTestLastIndexOf(topic);
  return (i < 0) ? nullptr : g_mqttTest.records[i].payload;
}

static size_t mqttTestPayloadLenFor(const char* topic) {
  const int i = mqttTestIndexOf(topic);
  return (i < 0) ? 0 : g_mqttTest.records[i].payloadLen;
}

static bool mqttTestRetainedFor(const char* topic) {
  const int i = mqttTestIndexOf(topic);
  return (i < 0) ? false : g_mqttTest.records[i].retained;
}

static const char* mqttTestTopicAt(int i) {
  return (i >= 0 && i < g_mqttTest.recordCount) ? g_mqttTest.records[i].topic : "";
}

// Number of publishes whose topic contains `needle` - how a test counts the
// entities published for one device without naming all of them.
static int mqttTestCountContaining(const char* needle) {
  int n = 0;
  for (int i = 0; i < g_mqttTest.recordCount; i++) {
    if (strstr(g_mqttTest.records[i].topic, needle) != nullptr) n++;
  }
  return n;
}

static bool mqttTestWasSubscribed(const char* topic) {
  for (int i = 0; i < g_mqttTest.subCount; i++) {
    if (strcmp(g_mqttTest.subs[i], topic) == 0) return true;
  }
  return false;
}

static bool mqttTestWasUnsubscribed(const char* topic) {
  for (int i = 0; i < g_mqttTest.unsubCount; i++) {
    if (strcmp(g_mqttTest.unsubs[i], topic) == 0) return true;
  }
  return false;
}

static void mqttTestSetConnected(bool c)   { g_mqttTest.connected = c; }
static void mqttTestSetConnectOk(bool ok)  { g_mqttTest.connectOk = ok; }
static void mqttTestSetState(int rc)       { g_mqttTest.state = rc; }
static void mqttTestSetPublishFail(bool f) { g_mqttTest.failPublishes = f; }

static void mqttTestSetPublishFailTopic(const char* needle) {
  strncpy(g_mqttTest.failTopicContains, needle ? needle : "",
          sizeof(g_mqttTest.failTopicContains) - 1);
  g_mqttTest.failTopicContains[sizeof(g_mqttTest.failTopicContains) - 1] = '\0';
}

// Deliver an inbound message to the registered callback.
//
// PubSubClient hands the callback a payload that is NOT NUL terminated, and
// mqttMessageCallback() has to copy it out using the length. The buffer here is
// pre-filled with 'X' so anything reading past `length` sees junk rather than a
// convenient terminator - a handler that forgets the length fails here the way
// it would against a real broker.
static void mqttTestInject(const char* topic, const char* payload) {
  if (!g_mqttTest.callback) return;
  static char topicBuf[MQTT_TEST_MAX_TOPIC];
  static uint8_t payloadBuf[MQTT_TEST_MAX_PAYLOAD];

  strncpy(topicBuf, topic ? topic : "", sizeof(topicBuf) - 1);
  topicBuf[sizeof(topicBuf) - 1] = '\0';

  memset(payloadBuf, 'X', sizeof(payloadBuf));
  const size_t len = payload ? strlen(payload) : 0;
  const size_t n   = (len < sizeof(payloadBuf)) ? len : sizeof(payloadBuf);
  if (payload && n) memcpy(payloadBuf, payload, n);

  g_mqttTest.callback(topicBuf, payloadBuf, (unsigned int)n);
}
