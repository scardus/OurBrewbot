#pragma once
// Minimal stand-in for the Arduino core, just enough for OurBrewbot/*.cpp
// to compile under PlatformIO's native (host) test environment.
// Not a full Arduino emulation — extend only as new native tests need it.

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <strings.h>
#include <algorithm>

using std::fabs;
using std::min;
using std::max;

// PROGMEM is a no-op on the host: there's no separate flash address space.
#define PSTR(s) (s)
typedef const char* PGM_P;
#define PROGMEM

// Minimal real String — Temperatures.cpp's addressToString()/scanBuses()
// construct one from a char*, compare, and read it back, even though those
// functions aren't under test (see test/test_native_temperatures). The fixed
// buffer also has to hold the largest string this codebase ever builds in a
// String: Reports.cpp serializes a whole brew-service JSON payload into one
// (~300 B) before POSTing it, and a short buffer would silently truncate it
// via concat() rather than fail.
class String {
  char buf_[512];
public:
  String() { buf_[0] = '\0'; }
  String(const char* s) {
    strncpy(buf_, s ? s : "", sizeof(buf_) - 1);
    buf_[sizeof(buf_) - 1] = '\0';
  }
  String(const String&) = default;
  String& operator=(const char* s) {
    strncpy(buf_, s ? s : "", sizeof(buf_) - 1);
    buf_[sizeof(buf_) - 1] = '\0';
    return *this;
  }
  const char* c_str() const { return buf_; }
  size_t length() const { return strlen(buf_); }
  // Only needed so ArduinoJson's ArduinoStringWriter (serializeJson into a
  // String) compiles once ARDUINOJSON_ENABLE_ARDUINO_STRING is on - no
  // native test currently exercises serialization into a String.
  bool concat(const char* s) {
    size_t curLen = strlen(buf_);
    if (curLen >= sizeof(buf_) - 1) return false;
    strncat(buf_, s, sizeof(buf_) - curLen - 1);
    return true;
  }
  bool equalsIgnoreCase(const char* other) const { return strcasecmp(buf_, other) == 0; }
  bool startsWith(const char* prefix) const { return strncasecmp(buf_, prefix, strlen(prefix)) == 0; }
};

// millis() — settable by tests via test_setMillis() so time-based profile
// steps can be exercised deterministically.
uint32_t millis();
void test_setMillis(uint32_t ms);

// delay() is a real busy-wait on hardware; native tests never need to
// actually block, so this is a no-op.
inline void delay(uint32_t) {}

// strlcpy() is a BSD extension the ESP8266 Arduino core provides, but
// mingw's C library doesn't - minimal standard implementation.
inline size_t strlcpy(char* dst, const char* src, size_t dstsize) {
  size_t srclen = strlen(src);
  if (dstsize > 0) {
    size_t n = (srclen < dstsize - 1) ? srclen : dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return srclen;
}
