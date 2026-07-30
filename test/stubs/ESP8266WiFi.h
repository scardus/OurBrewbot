#pragma once
// Stand-in for the ESP8266 WiFi class. Reports.cpp reads whether the link is up
// (a report cycle is skipped without one) and the RSSI it puts in its payload;
// Log.cpp additionally calls status() and resolves the syslog host through
// hostByName(). All of it is settable so tests can drive both the with-WiFi and
// without-WiFi paths.
#include <cstdint>
#include <cstring>
#include <IPAddress.h>

// wl_status_t values from the real header - only the two Log.cpp compares
// against matter here.
#define WL_IDLE_STATUS   0
#define WL_CONNECTED     3
#define WL_DISCONNECTED  6

class ESP8266WiFiStub {
public:
  bool    connected = true;
  int32_t rssi      = -60;

  // Scripted DNS: resolveOk decides whether hostByName succeeds, resolveTo is
  // the address it hands back. A real lookup blocks for up to the timeout, so
  // the failure path is otherwise slow and non-deterministic to reach.
  bool      resolveOk = true;
  IPAddress resolveTo = IPAddress(192, 168, 0, 50);

  bool    isConnected() { return connected; }
  int32_t RSSI()        { return rssi; }
  int     status()      { return connected ? WL_CONNECTED : WL_DISCONNECTED; }

  bool hostByName(const char* /*host*/, IPAddress& result, uint32_t /*timeoutMs*/) {
    if (!resolveOk) return false;
    result = resolveTo;
    return true;
  }
};

// One shared instance, mirroring the global `WiFi` the real header provides.
static ESP8266WiFiStub WiFi;
