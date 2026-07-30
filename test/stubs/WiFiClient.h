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
