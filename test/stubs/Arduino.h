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

// PROGMEM is a no-op on the host: there's no separate flash address space, so
// a "flash" string is just a const char* and the _P variants are the plain
// functions. FPSTR() therefore yields a const char*, which is what
// Config.cpp's doc[FPSTR(f.key)] lookups need - no ArduinoJson PROGMEM
// support required.
#define PSTR(s) (s)
typedef const char* PGM_P;
#define PROGMEM
#define FPSTR(p)   (reinterpret_cast<const char*>(p))
#define memcpy_P    memcpy
#define strlen_P    strlen
#define snprintf_P  snprintf
#define vsnprintf_P vsnprintf

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

// Serial. Log.cpp writes every line here before deciding whether to also send
// it to syslog or MQTT, so the captured text is how a test sees what was
// logged. Accumulates into one buffer rather than per-line, because Log.cpp
// builds a line from three separate print() calls (timestamp, body, CRLF).
#define SERIAL_TEST_MAX 4096

class SerialStub {
public:
  char   buf[SERIAL_TEST_MAX] = {0};
  size_t len = 0;

  void begin(unsigned long) {}
  void print(const char* s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n > SERIAL_TEST_MAX - 1 - len) n = SERIAL_TEST_MAX - 1 - len;
    memcpy(buf + len, s, n);
    len += n;
    buf[len] = '\0';
  }
  void println(const char* s) { print(s); print("\r\n"); }
  void reset() { len = 0; buf[0] = '\0'; }
};

static SerialStub Serial;

// The real core's Arduino.h pulls in Esp.h for the global ESP object; mirror
// that so production files referencing ESP need no extra include. It has to
// come after String above, not before it: EspClass::getResetReason() returns a
// String, which is what Crash.cpp calls .c_str() on.
#include <Esp.h>

// Pin helpers. Pins.h assigns the pressure sensor to A0, and SmartPlugs.cpp
// wraps its receive pin in digitalPinToInterrupt() - on the host neither
// means anything, but both have to resolve for those files to compile.
#define A0 17
#define digitalPinToInterrupt(p) (p)

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
