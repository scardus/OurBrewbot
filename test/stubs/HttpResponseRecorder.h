#pragma once
// The HTTP response a handler is building, captured in one place.
//
// WebAPI.cpp emits a response through three different routes, sometimes in the
// same handler: server.send() for a complete small body, server.sendContent()
// for chunked output, and serializeJson(doc, client) writing straight into the
// WiFiClient that server.client() returns (see sendJsonDoc). A test wants to
// assert on the finished payload without caring which route produced it, so all
// three append here in order.
//
// Its own header because both WiFiClient.h and ESP8266WebServer.h write to it
// and neither can include the other.
//
// Note the deliberate httpResp* naming: test/stubs/ESP8266HTTPClient.h already
// owns the httpTest* family for the OUTBOUND direction (what Reports.cpp would
// have POSTed to a brew service). Both end up in the same translation unit, so
// the two recorders must not share names - this one is the INBOUND request's
// response.

#include <cstdint>
#include <cstring>
#include <cstddef>

#define HTTP_RESP_MAX_BODY    8192
#define HTTP_RESP_MAX_HEADERS 16

struct HttpRespHeader {
  char name[48];
  char value[96];
};

struct HttpRespRecord {
  int    code = 0;                  // 0 = nothing sent yet
  char   contentType[48] = {0};
  char   body[HTTP_RESP_MAX_BODY] = {0};
  size_t bodyLen = 0;
  size_t declaredContentLength = 0;
  bool   contentLengthSet = false;
  int    sendCount = 0;             // send() calls, to catch a double-send
  HttpRespHeader headers[HTTP_RESP_MAX_HEADERS];
  int    headerCount = 0;
  bool   fileStreamed = false;
};

static HttpRespRecord g_httpResp;

// Appends to the body, truncating rather than overflowing: the pages this
// firmware serves are far larger than any assertion needs, and a test that
// cares about a long body checks a prefix.
static void httpRespAppend(const char* data, size_t len) {
  if (!data) return;
  size_t room = HTTP_RESP_MAX_BODY - 1 - g_httpResp.bodyLen;
  if (len > room) len = room;
  memcpy(g_httpResp.body + g_httpResp.bodyLen, data, len);
  g_httpResp.bodyLen += len;
  g_httpResp.body[g_httpResp.bodyLen] = '\0';
}

static void httpRespReset() { g_httpResp = HttpRespRecord{}; }

// Value of a header set via sendHeader(), or "" when absent.
static const char* httpRespHeader(const char* name) {
  for (int i = 0; i < g_httpResp.headerCount; i++) {
    if (strcmp(g_httpResp.headers[i].name, name) == 0) {
      return g_httpResp.headers[i].value;
    }
  }
  return "";
}
