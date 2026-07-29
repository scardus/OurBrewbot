#pragma once
// Stand-in for ESP8266HTTPClient that captures instead of sends: the URL and
// the serialized JSON body of each POST are recorded, so tests can assert on
// exactly what would have gone to Brewfather / Brewer's Friend without any
// network. POST()'s return value is settable to exercise the error branch.
#include <cstdint>
#include <cstring>
#include <Arduino.h>   // String
#include <WiFiClient.h>

struct HttpTestRecord {
  char url[192];
  char body[1024];
  int  postCount;
  int  nextStatus;
};

static HttpTestRecord g_httpTest = { {0}, {0}, 0, 200 };

static void httpTestReset() {
  g_httpTest.url[0]    = '\0';
  g_httpTest.body[0]   = '\0';
  g_httpTest.postCount = 0;
  g_httpTest.nextStatus = 200;
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

  String getString()            { return String(""); }
  String errorToString(int)     { return String("stub error"); }
  void   end()                  {}
};
