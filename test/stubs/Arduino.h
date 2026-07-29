#pragma once
// Minimal stand-in for the Arduino core, just enough for OurBrewbot/*.cpp
// to compile under PlatformIO's native (host) test environment.
// Not a full Arduino emulation — extend only as new native tests need it.

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <algorithm>

using std::fabs;
using std::min;
using std::max;

// PROGMEM is a no-op on the host: there's no separate flash address space.
#define PSTR(s) (s)
typedef const char* PGM_P;
#define PROGMEM

// Real Arduino String isn't needed by any code path native tests compile
// today (see Config.h's recordReboot() declaration) — this exists only so
// that declaration parses.
class String {
public:
  String() {}
  String(const char*) {}
  String(const String&) = default;
};

// millis() — settable by tests via test_setMillis() so time-based profile
// steps can be exercised deterministically.
uint32_t millis();
void test_setMillis(uint32_t ms);
