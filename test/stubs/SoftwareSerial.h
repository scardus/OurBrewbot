#pragma once
// Stand-in for ESP8266 SoftwareSerial, matching the methods Tilt.cpp calls on
// g_bleSerial. Unlike a pure no-op stub, this one carries a scripted receive
// queue: a test pushes the bytes an HM-10 would have sent with feed(), and the
// code under test drains them through available()/read() exactly as it would
// real serial data. That's what makes initBLE()/startTiltScan()/serviceTilt()
// reachable from native tests instead of hardware-only.
#include <cstdint>
#include <cstring>

#define SWSERIAL_8N1 0

class SoftwareSerial {
  // 1 KB is ample: the longest real AT+DISI? response observed is ~280 chars,
  // and the overflow test deliberately feeds more than Tilt.cpp's 320 B buffer.
  char rx_[1024];
  int  rxLen_  = 0;   // bytes queued
  int  rxRead_ = 0;   // bytes already handed to read()
  char lastPrint_[64] = {0};

public:
  SoftwareSerial(int /*rxPin*/, int /*txPin*/) {}
  void begin(uint32_t /*baud*/, int /*config*/, int /*rxPin*/, int /*txPin*/,
             bool /*invert*/, unsigned int /*bufCapacity*/,
             unsigned int /*isrBufCapacity*/) {}

  void print(const char* s) {
    strncpy(lastPrint_, s ? s : "", sizeof(lastPrint_) - 1);
    lastPrint_[sizeof(lastPrint_) - 1] = '\0';
  }

  int available() { return rxLen_ - rxRead_; }
  int read() { return (rxRead_ < rxLen_) ? (unsigned char)rx_[rxRead_++] : -1; }

  // ---- test-only helpers ----

  // Queue bytes as if the module had sent them. Call more than once to
  // deliver a response in chunks across separate service passes.
  void feed(const char* s) {
    if (!s) return;
    size_t n = strlen(s);
    if (rxLen_ + (int)n > (int)sizeof(rx_)) n = sizeof(rx_) - rxLen_;
    memcpy(rx_ + rxLen_, s, n);
    rxLen_ += (int)n;
  }

  // Drop anything queued/recorded - call from setUp().
  void reset() {
    rxLen_ = rxRead_ = 0;
    lastPrint_[0] = '\0';
  }

  // The last AT command written, so tests can tell "started a scan" from
  // "decided not to".
  const char* lastPrint() const { return lastPrint_; }
};
