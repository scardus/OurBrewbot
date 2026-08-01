#pragma once
// Scripted stand-in for the DallasTemperature library.
//
// This started life as a compile-only shim: Temperatures.h declares
// "extern DallasTemperature g_sensors1/2" and Temperatures.cpp calls real
// methods on them, so the signatures had to exist even though no native test
// could reach the functions that used them. With getDeviceCount() hardcoded to
// 0 and getTempC() hardcoded to -127, every probe-facing function in that file
// (scanBuses, readTempResults, periodicProbeScan, getTempQuick) compiled but
// never executed a meaningful line.
//
// It now carries a per-instance device table instead - the same idea as the
// scripted receive queue in SoftwareSerial.h. A test populates a bus with
// testAddDevice() and the code under test enumerates and reads it exactly as
// it would real DS18B20s. Two behaviours beyond plain recording matter:
//
//   1. getTempC() answers by ADDRESS, not index, and returns
//      DEVICE_DISCONNECTED_C for an address this bus doesn't carry. That is
//      what makes readTempResults()'s bus-1-then-bus-2 fallback real: the
//      probe genuinely isn't on bus 1.
//   2. testSetFailures() fails the next N reads of one address before
//      answering normally. readTempResults() has a blocking first-failure
//      retry that is otherwise unreachable - a probe that always works never
//      enters it, and one that never works can't be told apart from one the
//      retry failed to recover.
//
// begin() deliberately clears the resolution. The real library resets its
// internal resolution there, which is why periodicProbeScan() re-applies it
// after scanBuses(); modelling that means the re-apply is actually asserted
// rather than assumed.

#include "OneWire.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#define DEVICE_DISCONNECTED_C -127.0f

// Matches MAX_PROBES - the firmware can't track more than this anyway.
#define DS_TEST_MAX_DEVICES 8

class DallasTemperature {
public:
  DallasTemperature(OneWire* /*bus*/) {}

  // ---- surface the production code calls ----

  void begin() {
    beginCount_++;
    resolution_ = 0;  // see header comment: the real library resets this
  }

  int getDeviceCount() { return count_; }

  bool getAddress(uint8_t* addr, int index) {
    if (!addr || index < 0 || index >= count_) return false;
    memcpy(addr, devices_[index].addr, 8);
    return true;
  }

  void setWaitForConversion(bool wait) { waitForConversion_ = wait; }

  void requestTemperatures() {
    requestCount_++;
    lastRequestBlocking_ = waitForConversion_;
  }

  float getTempC(const uint8_t* addr) {
    readCount_++;
    Device* d = find(addr);
    if (!d) return DEVICE_DISCONNECTED_C;      // not on this bus
    if (d->failsRemaining > 0) {
      d->failsRemaining--;
      return DEVICE_DISCONNECTED_C;            // transient read error
    }
    return d->temp;
  }

  void setResolution(uint8_t r) { resolution_ = r; }

  // ---- test-only helpers ----

  // Attach a probe to this bus. The address is the 16-char lowercase hex
  // string the firmware persists, i.e. what addressToString() produces.
  bool testAddDevice(const char* addrHex, float tempC) {
    if (count_ >= DS_TEST_MAX_DEVICES) return false;
    if (!parseAddr(addrHex, devices_[count_].addr)) return false;
    devices_[count_].temp           = tempC;
    devices_[count_].failsRemaining = 0;
    count_++;
    return true;
  }

  void testSetTemp(const char* addrHex, float tempC) {
    uint8_t a[8];
    if (!parseAddr(addrHex, a)) return;
    Device* d = find(a);
    if (d) d->temp = tempC;
  }

  // Fail the next `n` reads of this address, then answer normally.
  void testSetFailures(const char* addrHex, int n) {
    uint8_t a[8];
    if (!parseAddr(addrHex, a)) return;
    Device* d = find(a);
    if (d) d->failsRemaining = n;
  }

  // Drop every probe and all recorded activity - call from setUp().
  void testReset() {
    count_ = 0;
    beginCount_ = requestCount_ = readCount_ = 0;
    resolution_ = 0;
    waitForConversion_ = true;   // library default
    lastRequestBlocking_ = false;
  }

  int     testBeginCount() const        { return beginCount_; }
  int     testRequestCount() const      { return requestCount_; }
  int     testReadCount() const         { return readCount_; }
  uint8_t testResolution() const        { return resolution_; }
  bool    testWaitForConversion() const { return waitForConversion_; }
  // Whether the most recent requestTemperatures() was a blocking one. The
  // async poll must be non-blocking; only the retry may block.
  bool    testLastRequestBlocking() const { return lastRequestBlocking_; }

private:
  struct Device {
    uint8_t addr[8];
    float   temp;
    int     failsRemaining;
  };

  Device  devices_[DS_TEST_MAX_DEVICES];
  int     count_               = 0;
  int     beginCount_          = 0;
  int     requestCount_        = 0;
  int     readCount_           = 0;
  uint8_t resolution_          = 0;
  bool    waitForConversion_   = true;
  bool    lastRequestBlocking_ = false;

  Device* find(const uint8_t* addr) {
    for (int i = 0; i < count_; i++) {
      if (memcmp(devices_[i].addr, addr, 8) == 0) return &devices_[i];
    }
    return nullptr;
  }

  // 16 hex chars -> 8 bytes. Mirrors stringToAddress() in Temperatures.cpp,
  // deliberately written out rather than calling it: the harness must not
  // depend on a function the tests exist to check.
  static bool parseAddr(const char* hex, uint8_t* out) {
    if (!hex || strlen(hex) < 16) return false;
    char pair[3] = {0};
    for (int i = 0; i < 8; i++) {
      pair[0] = hex[i * 2];
      pair[1] = hex[i * 2 + 1];
      out[i] = (uint8_t)strtol(pair, nullptr, 16);
    }
    return true;
  }
};

typedef uint8_t DeviceAddress[8];
