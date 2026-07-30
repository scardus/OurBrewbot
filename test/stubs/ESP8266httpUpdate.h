#pragma once
// Stand-in for the OTA updater. WebAPI.h includes <ESP8266httpUpdate.h>, and
// WebAPI.cpp's upload handler drives the global `Update` object through
// begin/write/end.
//
// No native test exercises a firmware upload - flashing is inherently a device
// operation - so this exists to let the rest of WebAPI.cpp compile, with just
// enough state that the handler's success and failure branches are reachable
// if a future test wants them.

#include <cstdint>
#include <cstddef>

class UpdaterStub {
public:
  bool   failBegin = false;   // make begin() refuse, for the "no space" branch
  int    error     = 0;
  size_t written   = 0;

  bool begin(size_t /*size*/) { return !failBegin; }
  size_t write(uint8_t* /*data*/, size_t len) { written += len; return len; }
  bool end(bool /*evenIfRemaining*/ = false) { return error == 0; }
  bool hasError() { return error != 0; }
  int  getError() { return error; }
  template <typename T> void printError(T&) {}
};

static UpdaterStub Update;
