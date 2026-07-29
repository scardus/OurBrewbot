#pragma once
// Stand-in for the ESP8266 WiFi class, covering only what Reports.cpp reads:
// whether the link is up (a report cycle is skipped without one) and the RSSI
// it puts in the payload. Both are settable so tests can drive the
// with-WiFi / without-WiFi paths.
#include <cstdint>

class ESP8266WiFiStub {
public:
  bool    connected = true;
  int32_t rssi      = -60;

  bool    isConnected() { return connected; }
  int32_t RSSI()        { return rssi; }
};

// One shared instance, mirroring the global `WiFi` the real header provides.
static ESP8266WiFiStub WiFi;
