#pragma once
// Temperatures.h declares "extern DallasTemperature g_sensors1/2" and uses
// DeviceAddress in a couple of signatures. Temperatures.cpp constructs real
// instances and calls real methods on them from functions that aren't
// under native test (scanBuses, requestTempConversion, readTempResults,
// getTempQuick, periodicProbeScan) - those functions still have to
// compile as part of the same translation unit, so the signatures below
// exist purely to satisfy that; none need real behaviour.
#include "OneWire.h"

#define DEVICE_DISCONNECTED_C -127.0f

class DallasTemperature {
public:
  DallasTemperature(OneWire* /*bus*/) {}
  void begin() {}
  int  getDeviceCount() { return 0; }
  bool getAddress(uint8_t* /*addr*/, int /*index*/) { return false; }
  void setWaitForConversion(bool) {}
  void requestTemperatures() {}
  float getTempC(const uint8_t* /*addr*/) { return DEVICE_DISCONNECTED_C; }
  void setResolution(uint8_t) {}
};

typedef uint8_t DeviceAddress[8];
