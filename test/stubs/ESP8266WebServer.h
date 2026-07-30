#pragma once
// Recording stand-in for ESP8266WebServer, the piece that makes WebAPI.cpp
// testable natively.
//
// Two halves:
//   - REQUEST, scripted by the test: setMethod()/setArg()/setUri() decide what
//     the handler sees. arg("plain") is the POST body, which is how every JSON
//     endpoint receives its payload.
//   - RESPONSE, recorded for the test: send(), sendContent(), sendContent_P(),
//     setContentLength(), sendHeader() and streamFile() all land in the shared
//     g_httpResp recorder, alongside anything serialised into the WiFiClient
//     from server.client() (see WiFiClient.h).
//
// on()/onNotFound() are accepted and ignored: route registration happens in
// setupWebServer(), which native tests never call - they invoke handlers
// directly, so nothing depends on the callbacks being stored.

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <IPAddress.h>
#include <WiFiClient.h>
#include <LittleFS.h>
#include <HttpResponseRecorder.h>
// The real ESP8266WebServer.h pulls in ESP8266WiFi.h, and WebAPI.cpp relies on
// that: it references WiFi (SSID, localIP, RSSI) without including the header
// itself. Mirror the layering so it compiles unchanged.
#include <ESP8266WiFi.h>

enum HTTPMethod {
  HTTP_ANY, HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT,
  HTTP_PATCH, HTTP_DELETE, HTTP_OPTIONS
};

enum HTTPUploadStatus {
  UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED
};

#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)

struct HTTPUpload {
  HTTPUploadStatus status      = UPLOAD_FILE_START;
  String           filename;
  size_t           currentSize = 0;
  size_t           totalSize   = 0;
  uint8_t*         buf         = nullptr;
};

#define WEB_TEST_MAX_ARGS 8

class ESP8266WebServer {
  struct Arg { char name[24]; char value[2048]; bool used; };
  Arg        args_[WEB_TEST_MAX_ARGS] = {};
  HTTPMethod method_ = HTTP_GET;
  char       uri_[64] = {0};
  HTTPUpload upload_{};

public:
  ESP8266WebServer() {}
  explicit ESP8266WebServer(int /*port*/) {}

  // ---- request scripting (test-facing) ----

  void setMethod(HTTPMethod m) { method_ = m; }
  void setUri(const char* u) {
    strncpy(uri_, u ? u : "", sizeof(uri_) - 1);
    uri_[sizeof(uri_) - 1] = '\0';
  }
  void setArg(const char* name, const char* value) {
    for (int i = 0; i < WEB_TEST_MAX_ARGS; i++) {
      if (args_[i].used && strcmp(args_[i].name, name) == 0) {
        strncpy(args_[i].value, value, sizeof(args_[i].value) - 1);
        args_[i].value[sizeof(args_[i].value) - 1] = '\0';
        return;
      }
    }
    for (int i = 0; i < WEB_TEST_MAX_ARGS; i++) {
      if (!args_[i].used) {
        args_[i].used = true;
        strncpy(args_[i].name, name, sizeof(args_[i].name) - 1);
        args_[i].name[sizeof(args_[i].name) - 1] = '\0';
        strncpy(args_[i].value, value, sizeof(args_[i].value) - 1);
        args_[i].value[sizeof(args_[i].value) - 1] = '\0';
        return;
      }
    }
  }
  // The POST body, as every JSON handler reads it.
  void setBody(const char* json) { setArg("plain", json); }
  void clearArgs() { for (int i = 0; i < WEB_TEST_MAX_ARGS; i++) args_[i].used = false; }

  HTTPUpload& testUpload() { return upload_; }

  // ---- request surface (production-facing) ----

  // A missing argument returns an empty String, matching the real class.
  String arg(const char* name) {
    for (int i = 0; i < WEB_TEST_MAX_ARGS; i++) {
      if (args_[i].used && strcmp(args_[i].name, name) == 0) return String(args_[i].value);
    }
    return String("");
  }
  String arg(const String& name) { return arg(name.c_str()); }

  HTTPMethod  method() { return method_; }
  String      uri()    { return String(uri_); }
  WiFiClient  client() { return WiFiClient(); }
  HTTPUpload& upload() { return upload_; }

  // ---- response surface (recorded) ----

  void send(int code, const char* contentType, const String& body) {
    recordSend(code, contentType);
    httpRespAppend(body.c_str(), body.length());
  }
  void send(int code, const char* contentType, const char* body) {
    recordSend(code, contentType);
    httpRespAppend(body, body ? strlen(body) : 0);
  }
  void send(int code, const char* contentType, const __FlashStringHelper* body) {
    const char* s = reinterpret_cast<const char*>(body);
    recordSend(code, contentType);
    httpRespAppend(s, s ? strlen(s) : 0);
  }

  void sendContent(const String& body) { httpRespAppend(body.c_str(), body.length()); }
  void sendContent(const char* body)   { httpRespAppend(body, body ? strlen(body) : 0); }
  void sendContent(const char* body, size_t len) { httpRespAppend(body, len); }
  // PROGMEM is a no-op on the host, so a flash string is just a char*.
  void sendContent_P(const char* body) { httpRespAppend(body, body ? strlen(body) : 0); }

  void setContentLength(size_t len) {
    g_httpResp.declaredContentLength = len;
    g_httpResp.contentLengthSet      = true;
  }

  void sendHeader(const char* name, const char* value, bool /*first*/ = false) {
    if (g_httpResp.headerCount >= HTTP_RESP_MAX_HEADERS) return;
    HttpRespHeader& h = g_httpResp.headers[g_httpResp.headerCount++];
    strncpy(h.name,  name,  sizeof(h.name)  - 1);
    h.name[sizeof(h.name) - 1] = '\0';
    strncpy(h.value, value, sizeof(h.value) - 1);
    h.value[sizeof(h.value) - 1] = '\0';
  }

  // Streams a file body. Records the fact plus the contents, so the
  // path-guard tests can tell a served file from a rejection.
  size_t streamFile(File& f, const char* contentType) {
    recordSend(200, contentType);
    g_httpResp.fileStreamed = true;
    char buf[512];
    size_t total = 0, n;
    while ((n = f.readBytes(buf, sizeof(buf))) > 0) {
      httpRespAppend(buf, n);
      total += n;
      if (n < sizeof(buf)) break;
    }
    return total;
  }

  // ---- route registration: accepted, never stored ----

  template <typename Fn> void on(const char*, Fn) {}
  template <typename Fn> void on(const char*, HTTPMethod, Fn) {}
  template <typename Fn1, typename Fn2> void on(const char*, HTTPMethod, Fn1, Fn2) {}
  template <typename Fn> void onNotFound(Fn) {}
  void begin() {}
  void handleClient() {}

private:
  void recordSend(int code, const char* contentType) {
    g_httpResp.code = code;
    g_httpResp.sendCount++;
    strncpy(g_httpResp.contentType, contentType ? contentType : "",
            sizeof(g_httpResp.contentType) - 1);
    g_httpResp.contentType[sizeof(g_httpResp.contentType) - 1] = '\0';
  }
};
