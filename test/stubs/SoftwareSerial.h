#pragma once
// Tilt.cpp declares "extern SoftwareSerial g_bleSerial" and exercises a
// handful of its methods inside initBLE()/startTiltScan()/serviceTilt() -
// none of those are called by native tests (which target the file's
// standalone parsing functions), but the whole file compiles as one TU so
// the class needs matching no-op signatures.
#include <cstdint>

#define SWSERIAL_8N1 0

class SoftwareSerial {
public:
  SoftwareSerial(int /*rxPin*/, int /*txPin*/) {}
  void begin(uint32_t /*baud*/, int /*config*/, int /*rxPin*/, int /*txPin*/,
             bool /*invert*/, unsigned int /*bufCapacity*/,
             unsigned int /*isrBufCapacity*/) {}
  void print(const char*) {}
  int available() { return 0; }
  int read() { return -1; }
};
